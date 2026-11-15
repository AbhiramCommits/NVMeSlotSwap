/*
 * device_sim.c - in-process emulated NVMe controller.
 *
 * The host driver talks to this controller only through two surfaces:
 *
 *   1. a byte-addressable BAR0 window (registers + doorbells), and
 *   2. page-aligned shared memory whose IOVAs are programmed into ASQ/ACQ or
 *      passed in SQEs as PRP1/PRP2.
 *
 * A controller thread owns the register state machine and executes the admin
 * queue.  Each I/O queue pair is drained by an executor thread (or by a small
 * round-robin pool when io_worker_threads is set), which makes per-core queue
 * pairs genuinely concurrent instead of a single serialised loop.
 *
 * Memory-ordering contract:
 *   - the host writes SQEs, release-fences, then release-stores the SQ tail
 *     doorbell; executors acquire-load that doorbell before reading SQEs;
 *   - the controller writes all CQE fields, then release-stores the status
 *     word (which carries the phase tag); the host acquire-loads it before
 *     trusting the rest of the CQE.
 * Both sides use __atomic builtins, so the same code is race-free under
 * ThreadSanitizer while still modelling MMIO ordering.
 */
#define _POSIX_C_SOURCE 200809L

#include "nvme/device.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nvme/fw.h"

#define NVMESS_MAX_QUEUES 64
#define NVMESS_PAGE 4096u
#define NVMESS_DMA_IOVA_BASE 0x10000000ull
#define NVMESS_MAX_ADMIN_DWORDS 1024u
#define NVMESS_IDLE_SPINS 4000u

struct dev_q {
    uint64_t sq_iova;
    uint64_t cq_iova;
    uint16_t sq_depth;
    uint16_t cq_depth;
    uint16_t sq_head; /* free-running 16-bit SQ head (memory index = head % depth) */
    uint16_t cq_tail; /* free-running 16-bit CQ tail */
    uint8_t cq_phase; /* expected phase of the next CQE to write */
    int configured;
    int active;
    int in_service;
    uint64_t commands;
};

struct nvme_dev {
    struct nvme_dev_cfg cfg;

    uint8_t *bar0;
    size_t bar0_size;
    uint32_t stride;

    uint8_t *dma_base;
    size_t dma_size;
    size_t dma_used;
    pthread_mutex_t dma_mu;

    uint8_t *ns;
    uint64_t ns_lbas;

    struct fw_mgr fw;

    struct dev_q q[NVMESS_MAX_QUEUES];
    uint32_t max_qid;
    uint32_t nworkers;

    pthread_t ctrl;
    pthread_t workers[NVMESS_MAX_QUEUES];
    unsigned worker_index[NVMESS_MAX_QUEUES];
    int workers_started;

    int running;
    int cc_enabled;
    int rdy;
    int q0_configured;

    uint64_t stall_ns;   /* quiesce window used by the next immediate commit */
    int pause_io;        /* IO executors stop fetching new SQEs while set */
    int self_test_fail;  /* inject post-activation self-test failure */

    uint64_t ready_count;
    uint64_t reset_count;
    uint64_t activation_count;
    uint64_t last_activation_ns;

    uint32_t cq_errors;
};

/* ------------------------------------------------------------------ */
/* Small utilities                                                     */
/* ------------------------------------------------------------------ */
static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void sleep_ns(uint64_t ns)
{
    struct timespec ts;
    if (ns == 0) {
        return;
    }
    ts.tv_sec = (time_t)(ns / 1000000000ull);
    ts.tv_nsec = (long)(ns % 1000000000ull);
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    }
}

static void cpu_relax(void)
{
#if defined(__aarch64__) || defined(__arm64__)
    __asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    /* nothing */
#endif
}

static void idle_wait(unsigned *spins)
{
    if (*spins < NVMESS_IDLE_SPINS) {
        (*spins)++;
        cpu_relax();
    } else {
        *spins = 0;
        sleep_ns(20000); /* 20 us */
    }
}

/* Busy-wait for a short service time; long waits go through nanosleep. */
static void service_delay(uint64_t ns)
{
    if (ns == 0) {
        return;
    }
    if (ns > 200000ull) {
        sleep_ns(ns);
        return;
    }
    uint64_t end = now_ns() + ns;
    for (;;) {
        uint64_t t = now_ns();
        if (t >= end) {
            return;
        }
        if (end - t > 2000ull) {
            cpu_relax();
        }
    }
}

static uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static uint64_t service_time_ns(nvme_dev_t *dev, uint32_t *rng)
{
    uint64_t base = dev->cfg.io_latency_base_ns;
    uint32_t jitter = dev->cfg.io_latency_jitter_ns;
    if (jitter == 0 || dev->cfg.latency_model == NVME_LAT_NONE) {
        return base;
    }
    uint32_t r = xorshift32(rng);
    if (dev->cfg.latency_model == NVME_LAT_EXPONENTIAL) {
        /* Exponential with mean jitter/2, capped at 8x jitter. */
        double u = (double)((r >> 8) | 1u) / 16777216.0;
        double v = -log(u) * ((double)jitter * 0.5);
        if (v > 8.0 * (double)jitter) {
            v = 8.0 * (double)jitter;
        }
        return base + (uint64_t)v;
    }
    return base + (uint64_t)(r % (jitter + 1u));
}

/* Translate a device-visible IOVA into a host pointer, bounds-checked
 * against the DMA arena. */
static uint8_t *dev_ptr(nvme_dev_t *dev, uint64_t iova, size_t len)
{
    uint64_t base = NVMESS_DMA_IOVA_BASE;
    uint64_t end = base + (uint64_t)dev->dma_used;
    if (iova < base || iova > end || len > (size_t)(end - iova)) {
        return NULL;
    }
    return dev->dma_base + (size_t)(iova - base);
}

static uint32_t mmio_rd32(nvme_dev_t *dev, uint32_t off)
{
    return nvme_mmio_read32(dev->bar0, off);
}
static void mmio_wr32(nvme_dev_t *dev, uint32_t off, uint32_t val)
{
    nvme_mmio_write32(dev->bar0, off, val);
}
static uint64_t mmio_rd64(nvme_dev_t *dev, uint32_t off)
{
    return nvme_mmio_read64(dev->bar0, off);
}

static void set_csts(nvme_dev_t *dev, uint32_t bits)
{
    mmio_wr32(dev, NVME_REG_CSTS, bits);
}

/* ------------------------------------------------------------------ */
/* PRP walking                                                         */
/* ------------------------------------------------------------------ */
/* Copies between a device-local buffer and the shared DMA pages described by
 * PRP1/PRP2.  device_to_host=1 moves data from the device-local buffer into
 * host memory; 0 moves host memory into the device-local buffer. */
static int prp_copy(nvme_dev_t *dev, uint64_t prp1, uint64_t prp2, uint8_t *buf, size_t len,
                    int device_to_host)
{
    size_t done = 0;
    size_t page = NVMESS_PAGE;
    size_t entries_per_page = page / sizeof(uint64_t);

    if (len == 0) {
        return 0;
    }
    if (prp1 == 0) {
        return -1;
    }

    size_t off = (size_t)(prp1 & (page - 1));
    size_t first = page - off;
    if (first > len) {
        first = len;
    }
    {
        uint8_t *p = dev_ptr(dev, prp1, first);
        if (p == NULL) {
            return -1;
        }
        if (device_to_host) {
            memcpy(p, buf, first);
        } else {
            memcpy(buf, p, first);
        }
        done = first;
    }
    if (done == len) {
        return 0;
    }

    /* Exactly one page remains: PRP2 is the second data page. */
    if (len - done <= page) {
        uint8_t *p = dev_ptr(dev, prp2, len - done);
        if (p == NULL) {
            return -1;
        }
        if (device_to_host) {
            memcpy(p, buf + done, len - done);
        } else {
            memcpy(buf + done, p, len - done);
        }
        return 0;
    }

    /* PRP2 points at a PRP list; the last entry chains to the next list. */
    if (prp2 == 0) {
        return -1;
    }
    uint64_t list_iova = prp2;
    for (;;) {
        uint8_t *raw = dev_ptr(dev, list_iova, page);
        if (raw == NULL) {
            return -1;
        }
        uint64_t entry;
        for (size_t i = 0; i < entries_per_page; ++i) {
            size_t remaining = len - done;
            if (remaining == 0) {
                return 0;
            }
            memcpy(&entry, raw + i * sizeof(uint64_t), sizeof(entry));
            if (remaining > page && i + 1 == entries_per_page) {
                list_iova = entry;
                break;
            }
            size_t chunk = remaining > page ? page : remaining;
            uint8_t *p = dev_ptr(dev, entry, chunk);
            if (p == NULL) {
                return -1;
            }
            if (device_to_host) {
                memcpy(p, buf + done, chunk);
            } else {
                memcpy(buf + done, p, chunk);
            }
            done += chunk;
        }
        if (done >= len) {
            return 0;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Completion posting                                                  */
/* ------------------------------------------------------------------ */
static void post_cqe(nvme_dev_t *dev, struct dev_q *q, uint16_t qid, uint16_t cid,
                     uint32_t cdw0, uint16_t sc, uint16_t sct, int dnr)
{
    uint16_t index = (uint16_t)(q->cq_tail % q->cq_depth);
    uint8_t *raw = dev_ptr(dev, q->cq_iova + (uint64_t)index * NVME_CQ_ENTRY_SIZE,
                           NVME_CQ_ENTRY_SIZE);
    if (raw == NULL) {
        __atomic_fetch_add(&dev->cq_errors, 1u, __ATOMIC_RELAXED);
        return;
    }
    struct nvme_cqe *cqe = (struct nvme_cqe *)raw;
    uint16_t status = nvme_cqe_status_encode(q->cq_phase, sc, sct, dnr);

    cqe->cdw0 = cdw0;
    cqe->rsvd = 0;
    cqe->sqhd = q->sq_head;
    cqe->sqid = qid;
    cqe->cid = cid;
    __atomic_store_n(&cqe->status, status, __ATOMIC_RELEASE);

    q->cq_tail++;
    if (q->cq_tail % q->cq_depth == 0) {
        q->cq_phase ^= 1u;
    }
}

static void zero_ring_memory(nvme_dev_t *dev, struct dev_q *q)
{
    if (q->cq_iova != 0 && q->cq_depth != 0) {
        uint8_t *p = dev_ptr(dev, q->cq_iova, (size_t)q->cq_depth * NVME_CQ_ENTRY_SIZE);
        if (p != NULL) {
            memset(p, 0, (size_t)q->cq_depth * NVME_CQ_ENTRY_SIZE);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Identify / log page payloads                                        */
/* ------------------------------------------------------------------ */
static void pad_field(char *dst, size_t size, const char *src)
{
    size_t n = strlen(src);
    if (n > size) {
        n = size;
    }
    memcpy(dst, src, n);
    for (size_t i = n; i < size; ++i) {
        dst[i] = ' ';
    }
}

static void fill_id_ctrl(nvme_dev_t *dev, struct nvme_id_ctrl *id)
{
    memset(id, 0, sizeof(*id));
    id->vid = 0x1B36;
    id->ssvid = 0x1B36;
    pad_field(id->sn, sizeof(id->sn), "NVMESS0000000000001");
    pad_field(id->mn, sizeof(id->mn), "NVMeSlotSwap Emulated Controller");
    pad_field(id->fr, sizeof(id->fr), fw_mgr_active_revision(&dev->fw));
    id->rab = 6;
    id->ieee[0] = 0x00;
    id->ieee[1] = 0x25;
    id->ieee[2] = 0x90;
    id->cmic = 0;
    id->mdts = 7; /* 2^12 * 32 = 128 KiB max transfer */
    id->cntlid = 1;
    id->ver = 0x00010400u;
    id->oacs = NVME_OACS_GET_LOG_PAGE | NVME_OACS_FW_DOWNLOAD | NVME_OACS_FW_COMMIT;
    id->acl = 3;
    id->aerl = 3;
    id->frmw = (uint8_t)(NVME_FRMW_SLOT1_RO |
                         ((NVME_FW_SLOT_COUNT - 1u) << NVME_FRMW_NUM_SLOTS_SHIFT));
    id->lpa = 0;
    id->elpe = 63;
    id->npss = 0;
    id->mtfa = 0;
    id->nn = 1;
    id->oncs = 0x0004; /* write zeroes */
    id->vwc = 0;
    id->awun = 1;
    id->awupf = 1;
}

static void fill_id_ns(nvme_dev_t *dev, struct nvme_id_ns *id)
{
    memset(id, 0, sizeof(*id));
    id->nsze = dev->ns_lbas;
    id->ncap = dev->ns_lbas;
    id->nuse = dev->ns_lbas;
    id->nsfeat = 0;
    id->nlbaf = 0;
    id->flbas = 0;
    id->dlfeat = 0;
    id->npwg = 1;
    id->npwa = 1;
    id->npdg = 0;
    id->npda = 0;
    id->nows = 1;
    id->lbaf[0].ms = 0;
    id->lbaf[0].ds = 12; /* 4096-byte LBA */
    id->lbaf[0].rp = 0;
    {
        uint64_t bytes = dev->ns_lbas * NVME_LBA_SIZE;
        for (unsigned i = 0; i < 8; ++i) {
            id->nvmcap[i] = (uint8_t)(bytes >> (8u * i));
        }
    }
}

static void fill_smart_log(struct nvme_smart_log *log)
{
    memset(log, 0, sizeof(*log));
    log->temperature[0] = 0x2C;
    log->temperature[1] = 0x01;
    log->avail_spare = 100;
    log->spare_thresh = 10;
    log->percent_used = 0;
}

/* ------------------------------------------------------------------ */
/* Admin command execution                                             */
/* ------------------------------------------------------------------ */
struct cmd_result {
    uint16_t sc;
    uint16_t sct;
    int dnr;
    uint32_t cdw0;
};

static struct cmd_result ok_result(uint32_t cdw0)
{
    struct cmd_result r = {NVME_SC_SUCCESS, NVME_SCT_GENERIC, 0, cdw0};
    return r;
}

static struct cmd_result err_result(uint16_t sc)
{
    struct cmd_result r = {sc, NVME_SCT_GENERIC, 1, 0};
    return r;
}

static struct cmd_result exec_create_cq(nvme_dev_t *dev, const struct nvme_sqe *sqe)
{
    uint16_t qid = (uint16_t)(sqe->cdw10 >> 16);
    uint16_t qsize = (uint16_t)(sqe->cdw10 & 0xFFFFu);
    uint16_t depth = (uint16_t)(qsize + 1u);
    if (qid == 0 || qid > dev->max_qid || depth < 2 || depth > 4096 ||
        (depth & (depth - 1u)) != 0) {
        return err_result(NVME_SC_INVALID_FIELD);
    }
    if ((sqe->cdw11 & 1u) == 0) {
        return err_result(NVME_SC_INVALID_FIELD); /* physically contiguous required */
    }
    struct dev_q *q = &dev->q[qid];
    if (q->configured) {
        return err_result(NVME_SC_INVALID_FIELD);
    }
    q->cq_iova = sqe->prp1;
    q->cq_depth = depth;
    q->cq_tail = 0;
    q->cq_phase = 1;
    if (dev_ptr(dev, q->cq_iova, (size_t)depth * NVME_CQ_ENTRY_SIZE) == NULL) {
        q->cq_iova = 0;
        return err_result(NVME_SC_INVALID_FIELD);
    }
    return ok_result(0);
}

static struct cmd_result exec_create_sq(nvme_dev_t *dev, const struct nvme_sqe *sqe)
{
    uint16_t qid = (uint16_t)(sqe->cdw10 >> 16);
    uint16_t qsize = (uint16_t)(sqe->cdw10 & 0xFFFFu);
    uint16_t depth = (uint16_t)(qsize + 1u);
    uint16_t cqid = (uint16_t)(sqe->cdw11 >> 16);
    if (qid == 0 || qid > dev->max_qid || depth < 2 || depth > 4096 ||
        (depth & (depth - 1u)) != 0) {
        return err_result(NVME_SC_INVALID_FIELD);
    }
    if (cqid != qid) {
        return err_result(NVME_SC_INVALID_FIELD); /* one CQ per SQ in this model */
    }
    struct dev_q *q = &dev->q[qid];
    if (q->cq_iova == 0) {
        return err_result(NVME_SC_INVALID_FIELD);
    }
    if (dev_ptr(dev, sqe->prp1, (size_t)depth * NVME_SQ_ENTRY_SIZE) == NULL) {
        return err_result(NVME_SC_INVALID_FIELD);
    }
    q->sq_iova = sqe->prp1;
    q->sq_depth = depth;
    q->sq_head = 0;
    q->commands = 0;
    __atomic_store_n(&q->active, 0, __ATOMIC_RELEASE);
    q->configured = 1;
    __atomic_store_n(&q->active, 1, __ATOMIC_RELEASE);
    return ok_result(0);
}

/* Waits for the executor to leave the queue, then clears host-visible state. */
static void teardown_queue(nvme_dev_t *dev, struct dev_q *q)
{
    uint64_t deadline = now_ns() + 200000000ull; /* 200 ms */
    __atomic_store_n(&q->active, 0, __ATOMIC_RELEASE);
    while (__atomic_load_n(&q->in_service, __ATOMIC_ACQUIRE) != 0 && now_ns() < deadline) {
        cpu_relax();
    }
    zero_ring_memory(dev, q);
    q->sq_iova = 0;
    q->cq_iova = 0;
    q->sq_depth = 0;
    q->cq_depth = 0;
    q->sq_head = 0;
    q->cq_tail = 0;
    q->cq_phase = 1;
    q->configured = 0;
}

static struct cmd_result exec_delete_sq(nvme_dev_t *dev, const struct nvme_sqe *sqe)
{
    uint16_t qid = (uint16_t)(sqe->cdw10 >> 16);
    if (qid == 0 || qid > dev->max_qid) {
        return err_result(NVME_SC_INVALID_FIELD);
    }
    struct dev_q *q = &dev->q[qid];
    __atomic_store_n(&q->active, 0, __ATOMIC_RELEASE);
    q->sq_iova = 0;
    q->sq_depth = 0;
    q->sq_head = 0;
    return ok_result(0);
}

static struct cmd_result exec_delete_cq(nvme_dev_t *dev, const struct nvme_sqe *sqe)
{
    uint16_t qid = (uint16_t)(sqe->cdw10 >> 16);
    if (qid == 0 || qid > dev->max_qid) {
        return err_result(NVME_SC_INVALID_FIELD);
    }
    struct dev_q *q = &dev->q[qid];
    __atomic_store_n(&q->active, 0, __ATOMIC_RELEASE);
    q->cq_iova = 0;
    q->cq_depth = 0;
    q->cq_tail = 0;
    q->cq_phase = 1;
    q->configured = 0;
    return ok_result(0);
}

static struct cmd_result exec_identify(nvme_dev_t *dev, const struct nvme_sqe *sqe)
{
    uint32_t cns = sqe->cdw10 & 0xFFu;
    uint32_t nsid = sqe->nsid;
    uint8_t data[NVMESS_PAGE];

    memset(data, 0, sizeof(data));
    switch (cns) {
    case NVME_ID_CNS_CONTROLLER:
        fill_id_ctrl(dev, (struct nvme_id_ctrl *)data);
        break;
    case NVME_ID_CNS_NAMESPACE: {
        if (nsid != 1) {
            return err_result(NVME_SC_INVALID_FIELD);
        }
        fill_id_ns(dev, (struct nvme_id_ns *)data);
        break;
    }
    default:
        return err_result(NVME_SC_INVALID_FIELD);
    }
    if (prp_copy(dev, sqe->prp1, sqe->prp2, data, sizeof(data), 1) != 0) {
        return err_result(NVME_SC_INVALID_FIELD);
    }
    return ok_result(0);
}

static struct cmd_result exec_get_log_page(nvme_dev_t *dev, const struct nvme_sqe *sqe)
{
    uint32_t lid = sqe->cdw10 & 0xFFu;
    uint32_t numd = ((sqe->cdw10 >> 16) & 0xFFFu) + 1u; /* dwords */
    uint32_t ofst = sqe->cdw11 * 4u;                    /* bytes */
    uint8_t data[NVMESS_MAX_ADMIN_DWORDS * 4u];
    uint8_t page[512];

    if (numd > NVMESS_MAX_ADMIN_DWORDS || ofst > 4096u) {
        return err_result(NVME_SC_INVALID_FIELD);
    }
    memset(data, 0, sizeof(data));
    memset(page, 0, sizeof(page));

    switch (lid) {
    case NVME_LOG_FW_SLOT:
        fw_mgr_read_log(&dev->fw, (struct nvme_fw_slot_log *)page);
        break;
    case NVME_LOG_SMART:
        fill_smart_log((struct nvme_smart_log *)page);
        break;
    default:
        return err_result(NVME_SC_INVALID_FIELD);
    }

    size_t bytes = (size_t)numd * 4u;
    for (size_t i = 0; i < bytes; ++i) {
        size_t src = (size_t)ofst + i;
        data[i] = src < sizeof(page) ? page[src] : 0;
    }
    if (prp_copy(dev, sqe->prp1, sqe->prp2, data, bytes, 1) != 0) {
        return err_result(NVME_SC_INVALID_FIELD);
    }
    return ok_result(0);
}

static struct cmd_result exec_features(nvme_dev_t *dev, const struct nvme_sqe *sqe, int set)
{
    uint32_t fid = sqe->cdw10 & 0xFFu;
    if (fid != NVME_FEAT_NUM_QUEUES) {
        return err_result(NVME_SC_INVALID_FIELD);
    }
    uint32_t nsqr = set ? (sqe->cdw11 & 0xFFFFu) : dev->max_qid;
    uint32_t ncqr = set ? (sqe->cdw11 >> 16) : dev->max_qid;
    if (nsqr > dev->max_qid || ncqr > dev->max_qid) {
        nsqr = dev->max_qid;
        ncqr = dev->max_qid;
    }
    return ok_result((ncqr << 16) | nsqr);
}

static struct cmd_result exec_fw_download(nvme_dev_t *dev, const struct nvme_sqe *sqe)
{
    uint32_t numd = (sqe->cdw10 & 0xFFFFu) + 1u; /* dwords */
    uint32_t ofst = sqe->cdw11 * 4u;             /* bytes */
    uint32_t len = numd * 4u;
    uint8_t chunk[NVME_FW_MAX_CHUNK_BYTES];

    if (len == 0 || len > NVME_FW_MAX_CHUNK_BYTES || ofst > NVME_FW_MAX_IMAGE_BYTES) {
        return err_result(NVME_SC_INVALID_FIELD);
    }
    if (prp_copy(dev, sqe->prp1, sqe->prp2, chunk, len, 0) != 0) {
        return err_result(NVME_SC_INVALID_FIELD);
    }
    uint16_t sc = fw_mgr_download_chunk(&dev->fw, ofst, chunk, len);
    if (sc != NVME_SC_SUCCESS) {
        return err_result(sc);
    }
    return ok_result(0);
}

static struct cmd_result exec_fw_commit(nvme_dev_t *dev, const struct nvme_sqe *sqe)
{
    uint8_t slot = (uint8_t)(sqe->cdw10 & 0x7u);
    uint8_t action = (uint8_t)((sqe->cdw10 >> 3) & 0x7u);
    struct fw_commit_result res;
    int quiesce;

    /*
     * Validate the image and write the idle slot first.  The payload SHA-256
     * and the 1 MiB copy happen here, while I/O is still flowing normally.
     */
    uint16_t st = fw_mgr_prepare(&dev->fw, slot, action);
    if (st != NVME_SC_SUCCESS) {
        return err_result(st);
    }

    quiesce = (action == (uint8_t)NVME_FW_CA_REPLACE_ACTIVATE_IMMEDIATE && slot > NVME_FW_SLOT1 &&
               slot <= NVME_FW_SLOT_COUNT);
    if (quiesce) {
        /*
         * Quiesce: stop fetching new SQEs for the activation window, but do
         * not touch commands that are already in flight.  Executors finish
         * their current command and then park until pause_io clears.
         */
        __atomic_store_n(&dev->pause_io, 1, __ATOMIC_RELEASE);
        sleep_ns(__atomic_load_n(&dev->stall_ns, __ATOMIC_ACQUIRE));
    }

    res = fw_mgr_apply(&dev->fw, slot, action,
                       __atomic_load_n(&dev->self_test_fail, __ATOMIC_ACQUIRE));

    if (quiesce) {
        __atomic_store_n(&dev->pause_io, 0, __ATOMIC_RELEASE);
    }

    if (res.activated) {
        __atomic_add_fetch(&dev->activation_count, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&dev->last_activation_ns, now_ns(), __ATOMIC_RELEASE);
    }
    if (res.rolled_back) {
        /* Post-activation self test failed: CFS is asserted briefly while the
         * controller returns to the previous slot. */
        set_csts(dev, mmio_rd32(dev, NVME_REG_CSTS) | NVME_CSTS_CFS);
        sleep_ns(1500000);
        set_csts(dev, mmio_rd32(dev, NVME_REG_CSTS) & ~NVME_CSTS_CFS);
    }
    return err_result(res.status);
}

static struct cmd_result exec_admin(nvme_dev_t *dev, const struct nvme_sqe *sqe)
{
    switch (sqe->opc) {
    case NVME_ADMIN_CREATE_CQ:
        return exec_create_cq(dev, sqe);
    case NVME_ADMIN_CREATE_SQ:
        return exec_create_sq(dev, sqe);
    case NVME_ADMIN_DELETE_SQ:
        return exec_delete_sq(dev, sqe);
    case NVME_ADMIN_DELETE_CQ:
        return exec_delete_cq(dev, sqe);
    case NVME_ADMIN_IDENTIFY:
        return exec_identify(dev, sqe);
    case NVME_ADMIN_GET_LOG_PAGE:
        return exec_get_log_page(dev, sqe);
    case NVME_ADMIN_SET_FEATURES:
        return exec_features(dev, sqe, 1);
    case NVME_ADMIN_GET_FEATURES:
        return exec_features(dev, sqe, 0);
    case NVME_ADMIN_FW_IMAGE_DOWNLOAD:
        return exec_fw_download(dev, sqe);
    case NVME_ADMIN_FW_COMMIT:
        return exec_fw_commit(dev, sqe);
    default:
        return err_result(NVME_SC_INVALID_OPCODE);
    }
}

/* ------------------------------------------------------------------ */
/* I/O command execution                                               */
/* ------------------------------------------------------------------ */
static struct cmd_result exec_io(nvme_dev_t *dev, const struct nvme_sqe *sqe)
{
    uint64_t slba = ((uint64_t)sqe->cdw11 << 32) | sqe->cdw10;
    uint32_t nlb = (sqe->cdw12 & 0xFFFFu) + 1u;
    uint64_t bytes;

    switch (sqe->opc) {
    case NVME_IO_FLUSH:
        return ok_result(0);
    case NVME_IO_READ:
    case NVME_IO_WRITE:
        break;
    default:
        return err_result(NVME_SC_INVALID_OPCODE);
    }

    if (nlb == 0 || nlb > 256u) {
        return err_result(NVME_SC_INVALID_FIELD);
    }
    if (slba >= dev->ns_lbas || (uint64_t)nlb > dev->ns_lbas - slba) {
        return err_result(NVME_SC_INVALID_FIELD);
    }
    bytes = (uint64_t)nlb * NVME_LBA_SIZE;
    if (bytes > 1024u * 1024u) {
        return err_result(NVME_SC_INVALID_FIELD);
    }

    uint8_t *host = (uint8_t *)dev->ns + (size_t)slba * NVME_LBA_SIZE;
    if (sqe->opc == NVME_IO_WRITE) {
        if (prp_copy(dev, sqe->prp1, sqe->prp2, host, (size_t)bytes, 0) != 0) {
            return err_result(NVME_SC_INVALID_FIELD);
        }
    } else {
        if (prp_copy(dev, sqe->prp1, sqe->prp2, host, (size_t)bytes, 1) != 0) {
            return err_result(NVME_SC_INVALID_FIELD);
        }
    }
    return ok_result(0);
}

/* ------------------------------------------------------------------ */
/* Queue execution                                                     */
/* ------------------------------------------------------------------ */
static int q_enter(nvme_dev_t *dev, struct dev_q *q)
{
    if (__atomic_load_n(&q->active, __ATOMIC_ACQUIRE) == 0) {
        return 0;
    }
    __atomic_store_n(&q->in_service, 1, __ATOMIC_RELAXED);
    if (__atomic_load_n(&q->active, __ATOMIC_ACQUIRE) == 0 ||
        __atomic_load_n(&dev->pause_io, __ATOMIC_ACQUIRE) != 0) {
        __atomic_store_n(&q->in_service, 0, __ATOMIC_RELEASE);
        return 0;
    }
    return 1;
}

static void q_leave(struct dev_q *q)
{
    __atomic_store_n(&q->in_service, 0, __ATOMIC_RELEASE);
}

/* Processes up to max commands from a queue; returns number executed. */
static int run_queue(nvme_dev_t *dev, uint16_t qid, uint32_t *rng, int max)
{
    struct dev_q *q = &dev->q[qid];
    int done = 0;

    while (done < max) {
        uint32_t tail;
        struct cmd_result res;
        uint8_t *raw;

        if (!q_enter(dev, q)) {
            break;
        }
        tail = nvme_dbl_read(dev->bar0, NVME_SQ_DBL(qid, dev->stride));
        if (q->sq_depth == 0 || q->sq_head == (uint16_t)tail) {
            q_leave(q);
            break;
        }
        raw = dev_ptr(dev,
                      q->sq_iova +
                          (uint64_t)(q->sq_head % q->sq_depth) * NVME_SQ_ENTRY_SIZE,
                      NVME_SQ_ENTRY_SIZE);
        if (raw == NULL) {
            q_leave(q);
            break;
        }
        /* The doorbell acquire-load above orders the SQE read after the
         * host's release store. */
        struct nvme_sqe sqe_copy;
        memcpy(&sqe_copy, raw, sizeof(sqe_copy));
        uint16_t cid = sqe_copy.cid;
        q->sq_head++;

        if (qid == 0) {
            res = exec_admin(dev, &sqe_copy);
        } else {
            res = exec_io(dev, &sqe_copy);
            if (res.sc == NVME_SC_SUCCESS) {
                service_delay(service_time_ns(dev, rng));
            }
        }
        q->commands++;
        post_cqe(dev, q, qid, cid, res.cdw0, res.sc, res.sct, res.dnr);
        q_leave(q);
        done++;
    }
    return done;
}

/* Admin queue polling when the controller is not ready: commands are
 * completed with INVALID_FIELD instead of being executed. */
static int run_admin_reject(nvme_dev_t *dev)
{
    struct dev_q *q = &dev->q[NVME_ADMIN_QID];
    int done = 0;
    for (;;) {
        uint32_t tail;
        uint8_t *raw;
        if (q->sq_depth == 0) {
            break;
        }
        tail = nvme_dbl_read(dev->bar0, NVME_SQ_DBL(0, dev->stride));
        if (q->sq_head == (uint16_t)tail) {
            break;
        }
        raw = dev_ptr(dev,
                      q->sq_iova +
                          (uint64_t)(q->sq_head % q->sq_depth) * NVME_SQ_ENTRY_SIZE,
                      NVME_SQ_ENTRY_SIZE);
        if (raw == NULL) {
            break;
        }
        struct nvme_sqe sqe_copy;
        memcpy(&sqe_copy, raw, sizeof(sqe_copy));
        q->sq_head++;
        post_cqe(dev, q, 0, sqe_copy.cid, 0, NVME_SC_INVALID_FIELD, NVME_SCT_GENERIC, 1);
        done++;
    }
    return done;
}

/* ------------------------------------------------------------------ */
/* Controller state machine                                            */
/* ------------------------------------------------------------------ */
static int setup_admin_queue(nvme_dev_t *dev)
{
    uint32_t aqa = mmio_rd32(dev, NVME_REG_AQA);
    uint64_t asq = mmio_rd64(dev, NVME_REG_ASQ);
    uint64_t acq = mmio_rd64(dev, NVME_REG_ACQ);
    uint32_t asqs = (aqa & NVME_AQA_ASQS_MASK) + 1u;
    uint32_t acqs = ((aqa >> NVME_AQA_ACQS_SHIFT) & NVME_AQA_ACQS_MASK) + 1u;

    if (asq == 0 || acq == 0 || asqs < 2 || asqs > 4096 || acqs < 2 || acqs > 4096) {
        return -1;
    }
    if (dev_ptr(dev, asq, (size_t)asqs * NVME_SQ_ENTRY_SIZE) == NULL ||
        dev_ptr(dev, acq, (size_t)acqs * NVME_CQ_ENTRY_SIZE) == NULL) {
        return -1;
    }
    struct dev_q *q = &dev->q[NVME_ADMIN_QID];
    q->sq_iova = asq;
    q->cq_iova = acq;
    q->sq_depth = (uint16_t)asqs;
    q->cq_depth = (uint16_t)acqs;
    q->sq_head = 0;
    q->cq_tail = 0;
    q->cq_phase = 1;
    q->configured = 1;
    memset(dev_ptr(dev, acq, (size_t)acqs * NVME_CQ_ENTRY_SIZE), 0,
           (size_t)acqs * NVME_CQ_ENTRY_SIZE);
    __atomic_store_n(&q->active, 1, __ATOMIC_RELEASE);
    dev->q0_configured = 1;
    return 0;
}

static void do_enable(nvme_dev_t *dev, uint32_t cc)
{
    uint32_t iosqes = (cc & NVME_CC_IOSQES_MASK) >> NVME_CC_IOSQES_SHIFT;
    uint32_t iocqes = (cc & NVME_CC_IOCQES_MASK) >> NVME_CC_IOCQES_SHIFT;

    dev->cc_enabled = 1;
    dev->rdy = 0;
    set_csts(dev, 0);

    sleep_ns((uint64_t)dev->cfg.enable_delay_us * 1000ull);

    if (iosqes != 6 || iocqes != 4) {
        set_csts(dev, NVME_CSTS_CFS);
        return;
    }

    /* A controller enable starts every queue at index 0 with cleared
     * doorbells, exactly like the reset path. */
    {
        size_t dbl_bytes = (size_t)2u * (dev->max_qid + 1u) * dev->stride;
        memset(dev->bar0 + NVME_DOORBELL_BASE, 0, dbl_bytes);
    }
    if (setup_admin_queue(dev) != 0) {
        set_csts(dev, NVME_CSTS_CFS);
        return;
    }

    /* A pending A/B activation takes effect on reset, including the reset
     * that follows a CC.EN 1->0 transition. */
    fw_mgr_apply_pending(&dev->fw);

    dev->rdy = 1;
    __atomic_add_fetch(&dev->ready_count, 1, __ATOMIC_RELAXED);
    set_csts(dev, NVME_CSTS_RDY);
}

static void do_disable(nvme_dev_t *dev)
{
    dev->cc_enabled = 0;
    dev->rdy = 0;
    __atomic_store_n(&dev->pause_io, 0, __ATOMIC_RELEASE);

    for (uint32_t qid = 1; qid <= dev->max_qid; ++qid) {
        if (dev->q[qid].configured) {
            teardown_queue(dev, &dev->q[qid]);
        }
    }
    teardown_queue(dev, &dev->q[NVME_ADMIN_QID]);
    dev->q0_configured = 0;

    /* Doorbells are cleared on reset. */
    size_t dbl_bytes = (size_t)2u * (dev->max_qid + 1u) * dev->stride;
    memset(dev->bar0 + NVME_DOORBELL_BASE, 0, dbl_bytes);

    __atomic_add_fetch(&dev->reset_count, 1, __ATOMIC_RELAXED);
    set_csts(dev, 0);
}

static void *controller_thread(void *arg)
{
    nvme_dev_t *dev = (nvme_dev_t *)arg;
    unsigned spins = 0;
    uint32_t rng = dev->cfg.seed ^ 0x5BD1E995u;

    while (__atomic_load_n(&dev->running, __ATOMIC_ACQUIRE)) {
        uint32_t cc = mmio_rd32(dev, NVME_REG_CC);
        int en = (cc & NVME_CC_EN) != 0;

        if (en && !dev->cc_enabled) {
            do_enable(dev, cc);
            spins = 0;
            continue;
        }
        if (!en && dev->cc_enabled) {
            do_disable(dev);
            spins = 0;
            continue;
        }

        if (dev->rdy) {
            if (run_queue(dev, NVME_ADMIN_QID, &rng, 64) > 0) {
                spins = 0;
                continue;
            }
        } else {
            if (!dev->q0_configured) {
                uint32_t aqa = mmio_rd32(dev, NVME_REG_AQA);
                if (aqa != 0 && mmio_rd64(dev, NVME_REG_ASQ) != 0 &&
                    mmio_rd64(dev, NVME_REG_ACQ) != 0) {
                    (void)setup_admin_queue(dev);
                }
            }
            if (dev->q0_configured) {
                if (run_admin_reject(dev) > 0) {
                    spins = 0;
                    continue;
                }
            }
        }
        idle_wait(&spins);
    }
    return NULL;
}

static void *io_worker(void *argp)
{
    void **arg = (void **)argp;
    nvme_dev_t *dev = (nvme_dev_t *)arg[0];
    unsigned idx = (unsigned)(uintptr_t)arg[1];
    unsigned spins = 0;
    uint32_t rng = dev->cfg.seed + 0x9E3779B9u * (idx + 1u);

    free(arg);

    for (;;) {
        int did_work = 0;
        for (uint32_t qid = idx + 1u; qid <= dev->max_qid; qid += dev->nworkers) {
            struct dev_q *q = &dev->q[qid];
            if (__atomic_load_n(&q->active, __ATOMIC_ACQUIRE) == 0) {
                continue;
            }
            if (__atomic_load_n(&dev->pause_io, __ATOMIC_ACQUIRE) != 0) {
                continue;
            }
            if (run_queue(dev, (uint16_t)qid, &rng, 32) > 0) {
                did_work = 1;
            }
        }
        if (!__atomic_load_n(&dev->running, __ATOMIC_ACQUIRE)) {
            break;
        }
        if (did_work) {
            spins = 0;
        } else {
            idle_wait(&spins);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */
void nvme_dev_cfg_default(struct nvme_dev_cfg *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->ns_lba_count = 16384; /* 64 MiB */
    cfg->doorbell_stride = 4;
    cfg->max_qid = 8;
    cfg->io_latency_base_ns = 2500;
    cfg->io_latency_jitter_ns = 1500;
    cfg->latency_model = NVME_LAT_UNIFORM;
    cfg->seed = 0xC0FFEEu;
    cfg->enable_delay_us = 2000;
    cfg->reset_delay_us = 500;
    cfg->io_worker_threads = 0;
    cfg->dma_arena_bytes = 128u * 1024u * 1024u;
}

nvme_dev_t *nvme_dev_create(const struct nvme_dev_cfg *cfg)
{
    nvme_dev_t *dev;
    uint64_t cap;

    if (cfg == NULL || cfg->ns_lba_count == 0 || cfg->max_qid == 0 ||
        cfg->max_qid >= NVMESS_MAX_QUEUES) {
        return NULL;
    }
    if (cfg->doorbell_stride != 4 && cfg->doorbell_stride != 8 &&
        cfg->doorbell_stride != 16 && cfg->doorbell_stride != 32) {
        return NULL;
    }

    dev = (nvme_dev_t *)calloc(1, sizeof(*dev));
    if (dev == NULL) {
        return NULL;
    }
    dev->cfg = *cfg;
    dev->max_qid = cfg->max_qid;
    dev->ns_lbas = cfg->ns_lba_count;
    dev->stride = cfg->doorbell_stride;
    dev->stall_ns = 1500000ull; /* 1.5 ms default quiesce window */
    pthread_mutex_init(&dev->dma_mu, NULL);

    dev->bar0_size = ((size_t)NVME_DOORBELL_BASE +
                      2u * (dev->max_qid + 1u) * dev->stride + NVMESS_PAGE - 1u) &
                     ~(size_t)(NVMESS_PAGE - 1u);
    if (posix_memalign((void **)&dev->bar0, NVMESS_PAGE, dev->bar0_size) != 0) {
        goto fail;
    }
    memset(dev->bar0, 0, dev->bar0_size);

    dev->dma_size = cfg->dma_arena_bytes;
    if (dev->dma_size < 16u * 1024u * 1024u) {
        dev->dma_size = 16u * 1024u * 1024u;
    }
    if (posix_memalign((void **)&dev->dma_base, NVMESS_PAGE, dev->dma_size) != 0) {
        goto fail;
    }
    memset(dev->dma_base, 0, dev->dma_size);

    dev->ns = (uint8_t *)calloc(1, (size_t)dev->ns_lbas * NVME_LBA_SIZE);
    if (dev->ns == NULL) {
        goto fail;
    }

    fw_mgr_init(&dev->fw);

    /* CAP: 1024 max queue entries, 15 x 500 ms timeout, 4-byte doorbells,
     * NVM command set only, 4 KiB minimum memory page. */
    cap = (uint64_t)(1023u & NVME_CAP_MQES_MASK) |
          ((uint64_t)0x0Fu << NVME_CAP_TO_SHIFT) |
          ((uint64_t)((dev->stride == 4) ? 0u : (dev->stride == 8) ? 1u : (dev->stride == 16) ? 2u : 3u)
           << NVME_CAP_DSTRD_SHIFT);
    nvme_mmio_write64(dev->bar0, NVME_REG_CAP, cap);
    nvme_mmio_write32(dev->bar0, NVME_REG_VS, 0x00010400u);
    nvme_mmio_write32(dev->bar0, NVME_REG_CSTS, 0);

    dev->nworkers = cfg->io_worker_threads == 0 ? dev->max_qid : cfg->io_worker_threads;
    if (dev->nworkers > dev->max_qid) {
        dev->nworkers = dev->max_qid;
    }

    __atomic_store_n(&dev->running, 1, __ATOMIC_RELEASE);
    if (pthread_create(&dev->ctrl, NULL, controller_thread, dev) != 0) {
        __atomic_store_n(&dev->running, 0, __ATOMIC_RELEASE);
        goto fail;
    }
    /* Worker argument pairs (device, index) stored in a stable array. */
    for (uint32_t i = 0; i < dev->nworkers; ++i) {
        void **arg = (void **)calloc(2, sizeof(void *));
        if (arg == NULL) {
            goto fail_threads;
        }
        arg[0] = dev;
        arg[1] = (void *)(uintptr_t)i;
        dev->worker_index[i] = i;
        /* Reuse the arg block: worker_index keeps a copy for teardown. */
        if (pthread_create(&dev->workers[i], NULL, io_worker, arg) != 0) {
            free(arg);
            goto fail_threads;
        }
        dev->workers_started = (int)i + 1;
    }
    return dev;

fail_threads:
    __atomic_store_n(&dev->running, 0, __ATOMIC_RELEASE);
    for (int i = 0; i < dev->workers_started; ++i) {
        pthread_join(dev->workers[i], NULL);
    }
    pthread_join(dev->ctrl, NULL);
    fw_mgr_free(&dev->fw);
    free(dev->ns);
    free(dev->dma_base);
    free(dev->bar0);
    pthread_mutex_destroy(&dev->dma_mu);
    free(dev);
    return NULL;

fail:
    fw_mgr_free(&dev->fw);
    free(dev->ns);
    free(dev->dma_base);
    free(dev->bar0);
    pthread_mutex_destroy(&dev->dma_mu);
    free(dev);
    return NULL;
}

void nvme_dev_destroy(nvme_dev_t *dev)
{
    if (dev == NULL) {
        return;
    }
    __atomic_store_n(&dev->running, 0, __ATOMIC_RELEASE);
    pthread_join(dev->ctrl, NULL);
    for (int i = 0; i < dev->workers_started; ++i) {
        pthread_join(dev->workers[i], NULL);
    }
    /* Worker argument blocks are freed by the worker threads on exit. */
    fw_mgr_free(&dev->fw);
    free(dev->ns);
    free(dev->dma_base);
    free(dev->bar0);
    pthread_mutex_destroy(&dev->dma_mu);
    free(dev);
}

volatile void *nvme_dev_bar0(nvme_dev_t *dev) { return dev->bar0; }

uint32_t nvme_dev_bar0_size(const nvme_dev_t *dev) { return (uint32_t)dev->bar0_size; }

uint32_t nvme_dev_doorbell_stride(const nvme_dev_t *dev) { return dev->stride; }

void *nvme_dev_dma_map(nvme_dev_t *dev, size_t len, uint64_t *out_iova)
{
    size_t aligned = (len + NVMESS_PAGE - 1u) & ~(size_t)(NVMESS_PAGE - 1u);
    void *ptr;

    if (aligned == 0 || out_iova == NULL) {
        return NULL;
    }
    pthread_mutex_lock(&dev->dma_mu);
    if (aligned > dev->dma_size || dev->dma_used > dev->dma_size - aligned) {
        pthread_mutex_unlock(&dev->dma_mu);
        return NULL;
    }
    ptr = dev->dma_base + dev->dma_used;
    *out_iova = NVMESS_DMA_IOVA_BASE + (uint64_t)dev->dma_used;
    dev->dma_used += aligned;
    pthread_mutex_unlock(&dev->dma_mu);
    return ptr;
}

void nvme_dev_dma_unmap(nvme_dev_t *dev, void *ptr) { (void)dev; (void)ptr; }

void nvme_dev_set_stall(nvme_dev_t *dev, uint64_t ns)
{
    __atomic_store_n(&dev->stall_ns, ns, __ATOMIC_RELEASE);
}

void nvme_dev_set_self_test_fail(nvme_dev_t *dev, int fail)
{
    __atomic_store_n(&dev->self_test_fail, fail, __ATOMIC_RELEASE);
    dev->fw.self_test_fail = fail;
}

uint32_t nvme_dev_csts(nvme_dev_t *dev) { return mmio_rd32(dev, NVME_REG_CSTS); }

uint64_t nvme_dev_ready_count(nvme_dev_t *dev)
{
    return __atomic_load_n(&dev->ready_count, __ATOMIC_RELAXED);
}

uint64_t nvme_dev_reset_count(nvme_dev_t *dev)
{
    return __atomic_load_n(&dev->reset_count, __ATOMIC_RELAXED);
}

uint64_t nvme_dev_last_activation_ns(nvme_dev_t *dev)
{
    return __atomic_load_n(&dev->last_activation_ns, __ATOMIC_ACQUIRE);
}

uint64_t nvme_dev_command_count(nvme_dev_t *dev)
{
    uint64_t total = 0;
    for (uint32_t i = 0; i <= dev->max_qid; ++i) {
        total += __atomic_load_n(&dev->q[i].commands, __ATOMIC_RELAXED);
    }
    return total;
}

int nvme_dev_save_fw_state(nvme_dev_t *dev, const char *path)
{
    return fw_mgr_save(&dev->fw, path);
}

int nvme_dev_load_fw_state(nvme_dev_t *dev, const char *path)
{
    return fw_mgr_load(&dev->fw, path);
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nvme/device.h"

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int wait_cqe(nvme_cqe *cq, int idx, unsigned phase, uint32_t timeout_ms)
{
    uint64_t deadline = now_ns() + (uint64_t)timeout_ms * 1000000ull;
    while ((__atomic_load_n(&cq[idx].status, __ATOMIC_ACQUIRE) & 1u) != phase) {
        if (now_ns() > deadline) {
            return -1;
        }
    }
    return 0;
}

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL: %s\n", msg);                                                             \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

int main(void)
{
    nvme_dev_cfg cfg;
    nvme_dev_cfg_default(&cfg);
    nvme_dev_t *dev = nvme_dev_create(&cfg);
    volatile void *bar0 = nvme_dev_bar0(dev);
    uint64_t asq_iova = 0, acq_iova = 0, d1 = 0, d2 = 0, d3 = 0, d4 = 0, list_iova = 0;
    uint64_t idiova = 0, sq_iova = 0, cq_iova = 0;
    void *asq = nvme_dev_dma_map(dev, 32 * 64, &asq_iova);
    void *acq = nvme_dev_dma_map(dev, 32 * 16, &acq_iova);
    void *p1 = nvme_dev_dma_map(dev, 4096, &d1);
    void *p2 = nvme_dev_dma_map(dev, 4096, &d2);
    void *r1 = nvme_dev_dma_map(dev, 4096, &d3);
    void *r2 = nvme_dev_dma_map(dev, 4096, &d4);
    void *list = nvme_dev_dma_map(dev, 4096, &list_iova);
    void *idb = nvme_dev_dma_map(dev, 4096, &idiova);
    void *io_sq = nvme_dev_dma_map(dev, 64 * 64, &sq_iova);
    void *io_cq = nvme_dev_dma_map(dev, 64 * 16, &cq_iova);
    nvme_cqe *cq = (nvme_cqe *)acq;
    nvme_cqe *icq = (nvme_cqe *)io_cq;
    nvme_sqe sqe;

    memset(asq, 0, 32 * 64);
    memset(acq, 0, 32 * 16);
    memset(idb, 0, 4096);
    memset(io_cq, 0, 64 * 16);

    nvme_mmio_write32(bar0, NVME_REG_AQA, 31u | (31u << 16));
    nvme_mmio_write64(bar0, NVME_REG_ASQ, asq_iova);
    nvme_mmio_write64(bar0, NVME_REG_ACQ, acq_iova);

    /* Command before CC.EN: must be rejected with INVALID_FIELD. */
    memset(&sqe, 0, sizeof(sqe));
    sqe.opc = NVME_ADMIN_IDENTIFY;
    sqe.cid = 7;
    sqe.prp1 = idiova;
    sqe.cdw10 = NVME_ID_CNS_CONTROLLER;
    memcpy(asq, &sqe, sizeof(sqe));
    __atomic_thread_fence(__ATOMIC_RELEASE);
    nvme_dbl_write(bar0, NVME_SQ_DBL(0, 4), 1);
    CHECK(wait_cqe(cq, 0, 1, 1000) == 0, "no completion before RDY");
    printf("pre-RDY identify status=%u (expect 2)\n", nvme_cqe_sc(cq[0].status));
    CHECK(nvme_cqe_sc(cq[0].status) == NVME_SC_INVALID_FIELD, "pre-RDY status");

    /* Enable with the admin queue back at index 0. */
    nvme_mmio_write32(bar0, NVME_REG_CC, NVME_CC_EN | (6u << 16) | (4u << 20));
    uint64_t deadline = now_ns() + 2000000000ull;
    while ((nvme_mmio_read32(bar0, NVME_REG_CSTS) & NVME_CSTS_RDY) == 0) {
        CHECK(now_ns() <= deadline, "controller not ready");
    }
    nvme_dbl_write(bar0, NVME_SQ_DBL(0, 4), 0);
    memset(cq, 0, 32 * 16);

    memset(&sqe, 0, sizeof(sqe));
    sqe.opc = NVME_ADMIN_IDENTIFY;
    sqe.cid = 1;
    sqe.prp1 = idiova;
    sqe.cdw10 = NVME_ID_CNS_CONTROLLER;
    memcpy(asq, &sqe, sizeof(sqe));
    __atomic_thread_fence(__ATOMIC_RELEASE);
    nvme_dbl_write(bar0, NVME_SQ_DBL(0, 4), 1);
    CHECK(wait_cqe(cq, 0, 1, 1000) == 0, "identify timeout");
    printf("identify status=%u model='%.40s'\n", nvme_cqe_sc(cq[0].status), (char *)idb);
    CHECK(nvme_cqe_sc(cq[0].status) == 0, "identify status");

    /* Create CQ/SQ for queue 1. */
    memset(&sqe, 0, sizeof(sqe));
    sqe.opc = NVME_ADMIN_CREATE_CQ;
    sqe.cid = 2;
    sqe.prp1 = cq_iova;
    sqe.cdw10 = 63u | (1u << 16);
    sqe.cdw11 = 1;
    memcpy((char *)asq + 64, &sqe, sizeof(sqe));
    __atomic_thread_fence(__ATOMIC_RELEASE);
    nvme_dbl_write(bar0, NVME_SQ_DBL(0, 4), 2);
    if (wait_cqe(cq, 1, 1, 1000) != 0) { printf("create cq timeout\n"); return 1; }
    printf("create cq status=%u cid=%u sqhd=%u\n", nvme_cqe_sc(cq[1].status), cq[1].cid, cq[1].sqhd);
    CHECK(nvme_cqe_sc(cq[1].status) == 0, "create cq");

    memset(&sqe, 0, sizeof(sqe));
    sqe.opc = NVME_ADMIN_CREATE_SQ;
    sqe.cid = 3;
    sqe.prp1 = sq_iova;
    sqe.cdw10 = 63u | (1u << 16);
    sqe.cdw11 = 1 | (1u << 16);
    memcpy((char *)asq + 128, &sqe, sizeof(sqe));
    __atomic_thread_fence(__ATOMIC_RELEASE);
    nvme_dbl_write(bar0, NVME_SQ_DBL(0, 4), 3);
    if (wait_cqe(cq, 2, 1, 1000) != 0) { printf("create sq timeout\n"); return 1; }
    printf("create sq status=%u\n", nvme_cqe_sc(cq[2].status));
    CHECK(nvme_cqe_sc(cq[2].status) == 0, "create sq");

    /* 8 KiB write through PRP1+PRP2 (second page), then read back. */
    for (int i = 0; i < 4096; ++i) {
        ((unsigned char *)p1)[i] = (unsigned char)(i * 7);
        ((unsigned char *)p2)[i] = (unsigned char)(i * 13 + 1);
    }
    memset(&sqe, 0, sizeof(sqe));
    sqe.opc = NVME_IO_WRITE;
    sqe.cid = 100;
    sqe.nsid = 1;
    sqe.prp1 = d1;
    sqe.prp2 = d2;
    sqe.cdw12 = 1; /* 2 LBAs */
    memcpy(io_sq, &sqe, sizeof(sqe));
    __atomic_thread_fence(__ATOMIC_RELEASE);
    nvme_dbl_write(bar0, NVME_SQ_DBL(1, 4), 1);
    CHECK(wait_cqe(icq, 0, 1, 2000) == 0, "write timeout");
    printf("write status=%u\n", nvme_cqe_sc(icq[0].status));

    memset(&sqe, 0, sizeof(sqe));
    sqe.opc = NVME_IO_READ;
    sqe.cid = 101;
    sqe.nsid = 1;
    sqe.prp1 = d3;
    sqe.prp2 = d4;
    sqe.cdw12 = 1;
    memcpy((char *)io_sq + 64, &sqe, sizeof(sqe));
    __atomic_thread_fence(__ATOMIC_RELEASE);
    nvme_dbl_write(bar0, NVME_SQ_DBL(1, 4), 2);
    CHECK(wait_cqe(icq, 1, 1, 2000) == 0, "read timeout");
    printf("read status=%u match=%d\n", nvme_cqe_sc(icq[1].status),
           memcmp(p1, r1, 4096) == 0 && memcmp(p2, r2, 4096) == 0);

    /* 12 KiB via PRP list: prp1 + list[0]=d2 list[1]=d3. */
    ((uint64_t *)list)[0] = d2;
    ((uint64_t *)list)[1] = d3;
    memset(&sqe, 0, sizeof(sqe));
    sqe.opc = NVME_IO_WRITE;
    sqe.cid = 102;
    sqe.nsid = 1;
    sqe.prp1 = d1;
    sqe.prp2 = list_iova;
    sqe.cdw10 = 2;
    sqe.cdw12 = 2; /* 3 LBAs */
    memcpy((char *)io_sq + 128, &sqe, sizeof(sqe));
    __atomic_thread_fence(__ATOMIC_RELEASE);
    nvme_dbl_write(bar0, NVME_SQ_DBL(1, 4), 3);
    CHECK(wait_cqe(icq, 2, 1, 2000) == 0, "prplist timeout");
    printf("prp-list write status=%u\n", nvme_cqe_sc(icq[2].status));

    /* LBA out of range. */
    memset(&sqe, 0, sizeof(sqe));
    sqe.opc = NVME_IO_READ;
    sqe.cid = 103;
    sqe.nsid = 1;
    sqe.prp1 = d1;
    sqe.cdw10 = cfg.ns_lba_count - 1;
    sqe.cdw12 = 3;
    memcpy((char *)io_sq + 192, &sqe, sizeof(sqe));
    __atomic_thread_fence(__ATOMIC_RELEASE);
    nvme_dbl_write(bar0, NVME_SQ_DBL(1, 4), 4);
    CHECK(wait_cqe(icq, 3, 1, 2000) == 0, "oob timeout");
    printf("out-of-range status=%u (expect 2)\n", nvme_cqe_sc(icq[3].status));

    nvme_dev_destroy(dev);
    printf("scratch device test done\n");
    return 0;
}

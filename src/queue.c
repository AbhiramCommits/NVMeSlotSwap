/*
 * queue.c - lock-free submission/completion ring layer.
 *
 * Why the fences are where they are
 * ---------------------------------
 * The SQE bytes and the SQ tail doorbell are two separate stores that the
 * controller observes through different paths (shared memory and MMIO).  x86
 * and arm64 both allow those stores to become visible out of order, so a
 * controller could see the doorbell advance before it sees the SQE it was
 * meant to publish.  The release fence before the doorbell store prevents
 * that; the doorbell itself is a release store for the same reason.
 *
 * On the completion side the phase tag is the only field the host may read
 * before it knows the CQE is valid.  The status word is loaded with acquire
 * semantics (and followed by an acquire fence) so that no other CQE field is
 * read speculatively before the phase check succeeds.
 *
 * The doorbell is never read back by the host: doorbells are write-only on
 * real NVMe controllers, and a read can be a fatal error on some parts.
 */
#include "nvme/queue.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "nvme/device.h"
#include "nvme/regs.h"

int nvme_qp_init(struct nvme_qp* qp, nvme_dev_t* dev, uint16_t qid, uint16_t depth) {
    if (qp == NULL || dev == NULL || depth < 2) {
        return -EINVAL;
    }

    memset(qp, 0, sizeof(*qp));
    qp->depth = depth;
    qp->qid = qid;
    qp->dev = dev;
    qp->sq_bytes = (uint32_t)depth * NVME_SQ_ENTRY_SIZE;
    qp->cq_bytes = (uint32_t)depth * NVME_CQ_ENTRY_SIZE;

    qp->sq = (struct nvme_sqe*)nvme_dev_dma_map(dev, qp->sq_bytes, &qp->sq_iova);
    if (qp->sq == NULL) {
        return -ENOMEM;
    }
    qp->cq = (struct nvme_cqe*)nvme_dev_dma_map(dev, qp->cq_bytes, &qp->cq_iova);
    if (qp->cq == NULL) {
        nvme_dev_dma_unmap(dev, qp->sq);
        memset(qp, 0, sizeof(*qp));
        return -ENOMEM;
    }
    memset(qp->sq, 0, qp->sq_bytes);
    memset(qp->cq, 0, qp->cq_bytes);

    uint32_t stride = nvme_dev_doorbell_stride(dev);
    qp->sq_db = (volatile uint32_t*)((volatile char*)nvme_dev_bar0(dev) + NVME_SQ_DBL(qid, stride));
    qp->cq_db = (volatile uint32_t*)((volatile char*)nvme_dev_bar0(dev) + NVME_CQ_DBL(qid, stride));

    qp->cq_phase = 1;
    qp->sq_tail_resv = 0;
    qp->sq_tail = 0;
    qp->cq_head = 0;
    return 0;
}

void nvme_qp_destroy(struct nvme_qp* qp) {
    if (qp == NULL || qp->dev == NULL) {
        return;
    }
    if (qp->sq != NULL) {
        nvme_dev_dma_unmap(qp->dev, qp->sq);
    }
    if (qp->cq != NULL) {
        nvme_dev_dma_unmap(qp->dev, qp->cq);
    }
    memset(qp, 0, sizeof(*qp));
}

/*
 * Re-arms the host-side indices after a controller reset.  Queue memory and
 * doorbell addresses are kept because the driver re-creates the device-side
 * queues over the same buffers.  The CQ is cleared so that a stale phase-tag
 * match cannot deliver a completion from before the reset.
 */
void nvme_qp_reset(struct nvme_qp* qp) {
    __atomic_store_n(&qp->sq_tail_resv, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&qp->sq_tail, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&qp->cq_head, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&qp->cq_phase, 1, __ATOMIC_RELAXED);
    /* Counters restart with the queues: the in-flight calculation is
     * (sq_tail_resv - completed), so both baselines must be zero after a
     * controller reset. */
    __atomic_store_n(&qp->submitted, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&qp->completed, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&qp->stalls, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&qp->inflight, 0, __ATOMIC_RELAXED);
    if (qp->sq != NULL) {
        memset(qp->sq, 0, qp->sq_bytes);
    }
    if (qp->cq != NULL) {
        memset(qp->cq, 0, qp->cq_bytes);
    }
}

int nvme_qp_submit(struct nvme_qp* qp, const struct nvme_sqe* sqe, uint16_t cid) {
    uint32_t cur = __atomic_load_n(&qp->sq_tail_resv, __ATOMIC_RELAXED);

    /*
     * Reserve a slot with a CAS loop.  A fetch_add would also work but the
     * loop lets us test for a full ring and return -EAGAIN without needing a
     * rollback that other producers could observe.
     */
    for (;;) {
        /* `completed` is free-running (unlike the masked cq_head index), so it
         * pairs with sq_tail_resv to give an exact in-flight count. */
        uint32_t done = (uint32_t)__atomic_load_n(&qp->completed, __ATOMIC_ACQUIRE);
        if ((uint16_t)(cur - done) >= qp->depth) {
            __atomic_fetch_add(&qp->stalls, 1, __ATOMIC_RELAXED);
            return NVME_QP_EAGAIN;
        }
        if (__atomic_compare_exchange_n(&qp->sq_tail_resv, &cur, cur + 1, 0, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {
            break;
        }
    }

    struct nvme_sqe* slot = &qp->sq[cur % qp->depth];
    memcpy(slot, sqe, sizeof(*slot));
    slot->cid = cid;

    /* Publish the SQE before the doorbell store below. */
    __atomic_thread_fence(__ATOMIC_RELEASE);

    /* Doorbells carry a free-running 16-bit index; the controller derives the
     * memory slot with (index % depth).  A masked value would make a full ring
     * indistinguishable from an empty one when depth commands are outstanding. */
    uint16_t tail = (uint16_t)(cur + 1u);
    __atomic_store_n(&qp->sq_tail, tail, __ATOMIC_RELAXED);
    __atomic_store_n(qp->sq_db, tail, __ATOMIC_RELEASE);

    __atomic_add_fetch(&qp->submitted, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&qp->inflight, 1, __ATOMIC_RELAXED);
    return 0;
}

int nvme_qp_reap(struct nvme_qp* qp, struct nvme_cqe* out, int max) {
    int n = 0;
    uint32_t head = qp->cq_head;
    uint8_t phase = qp->cq_phase;

    if (max <= 0) {
        return 0;
    }

    while (n < max) {
        struct nvme_cqe* cqe = &qp->cq[head];
        uint16_t status = __atomic_load_n(&cqe->status, __ATOMIC_ACQUIRE);
        if (nvme_cqe_phase(status) != phase) {
            break;
        }
        /* Phase matched: the rest of this CQE was written before the release
         * store that published the status word. */
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        out[n] = *cqe;
        out[n].status = status;

        head++;
        if (head == qp->depth) {
            head = 0;
            phase ^= 1u;
        }
        n++;
        __atomic_fetch_add(&qp->completed, 1, __ATOMIC_RELAXED);
        __atomic_fetch_sub(&qp->inflight, 1, __ATOMIC_RELAXED);
    }

    if (n > 0) {
        qp->cq_head = head;
        qp->cq_phase = phase;
        /* One doorbell update per batch, not per completion. */
        __atomic_store_n(qp->cq_db, head, __ATOMIC_RELEASE);
    }
    return n;
}

bool nvme_qp_full(const struct nvme_qp* qp) {
    uint32_t resv = __atomic_load_n(&qp->sq_tail_resv, __ATOMIC_RELAXED);
    uint32_t done = (uint32_t)__atomic_load_n(&qp->completed, __ATOMIC_ACQUIRE);
    return (uint16_t)(resv - done) >= qp->depth;
}

uint64_t nvme_qp_submitted(const struct nvme_qp* qp) {
    return __atomic_load_n(&qp->submitted, __ATOMIC_RELAXED);
}

uint64_t nvme_qp_completed(const struct nvme_qp* qp) {
    return __atomic_load_n(&qp->completed, __ATOMIC_RELAXED);
}

uint64_t nvme_qp_stalls(const struct nvme_qp* qp) {
    return __atomic_load_n(&qp->stalls, __ATOMIC_RELAXED);
}

uint64_t nvme_qp_inflight(const struct nvme_qp* qp) {
    return __atomic_load_n(&qp->inflight, __ATOMIC_RELAXED);
}

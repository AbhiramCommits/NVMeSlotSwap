/*
 * queue.h - lock-free host-side submission/completion ring layer.
 *
 * Invariants (see docs/design.md):
 *   - exactly one thread owns the completion queue head (the reaper), so
 *     phase-tag flipping never races;
 *   - producers reserve a submission slot with an atomic compare/exchange on
 *     sq_tail_resv and never overwrite an SQE that is still in flight;
 *   - the 64-byte SQE is written before a release fence and the doorbell
 *     store that publishes it;
 *   - the CQE status word is loaded with acquire semantics before any other
 *     field of the completion is trusted.
 *
 * All atomic operations use the compiler's __atomic builtins so that the
 * same header can be consumed by C11 and C++17 translation units.
 */
#ifndef NVME_QUEUE_H
#define NVME_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

#include "nvme/spec.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nvme_dev nvme_dev_t;

/* Return codes for submit/reap. */
#define NVME_QP_OK 0
#define NVME_QP_EAGAIN (-11)  /* ring full: back off and reap */
#define NVME_QP_EINVAL (-22)

struct nvme_qp {
    struct nvme_sqe *sq; /* host virtual address of the SQ */
    struct nvme_cqe *cq; /* host virtual address of the CQ */
    uint64_t sq_iova;    /* device-visible address programmed into ASQ / PRP1 */
    uint64_t cq_iova;
    uint32_t sq_bytes;
    uint32_t cq_bytes;

    uint16_t depth; /* queue entries */
    uint16_t qid;

    /* Free-running reservation counter; index = value % depth. */
    uint32_t sq_tail_resv;
    /* Last tail value published through the doorbell. */
    uint32_t sq_tail;
    /* Completion head; only the owner thread advances it. */
    uint32_t cq_head;
    /* Expected phase tag of the next completion (starts at 1). */
    uint8_t cq_phase;

    volatile uint32_t *sq_db; /* BAR0 SQ tail doorbell */
    volatile uint32_t *cq_db; /* BAR0 CQ head doorbell */

    nvme_dev_t *dev;

    /* Statistics (atomic). */
    uint64_t submitted;
    uint64_t completed;
    uint64_t stalls;   /* submit attempts that returned -EAGAIN */
    uint64_t inflight; /* submitted - reaped */
};

int nvme_qp_init(struct nvme_qp *qp, nvme_dev_t *dev, uint16_t qid, uint16_t depth);
void nvme_qp_destroy(struct nvme_qp *qp);

/* Re-arm host-side indices after a controller reset.  Queue memory and
 * doorbell addresses are preserved; the device restarts at head=0. */
void nvme_qp_reset(struct nvme_qp *qp);

/*
 * Reserve a submission slot, copy the 64-byte SQE, release-fence, then write
 * the SQ tail doorbell.  Returns NVME_QP_EAGAIN when the ring is full.
 */
int nvme_qp_submit(struct nvme_qp *qp, const struct nvme_sqe *sqe, uint16_t cid);

/*
 * Reap up to max completions.  Checks the phase tag before trusting the rest
 * of each CQE, acquires after the phase check, and rings the CQ head doorbell
 * once per batch.  Returns the number of completions copied to out.
 */
int nvme_qp_reap(struct nvme_qp *qp, struct nvme_cqe *out, int max);

bool nvme_qp_full(const struct nvme_qp *qp); /* true => host must back off */

/* Thread-safe statistic snapshots. */
uint64_t nvme_qp_submitted(const struct nvme_qp *qp);
uint64_t nvme_qp_completed(const struct nvme_qp *qp);
uint64_t nvme_qp_stalls(const struct nvme_qp *qp);
uint64_t nvme_qp_inflight(const struct nvme_qp *qp);

#ifdef __cplusplus
}
#endif

#endif /* NVME_QUEUE_H */

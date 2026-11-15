/*
 * queue.c - (step 1 scaffold; implemented in step 3).
 */
#include "nvme/queue.h"

#include <errno.h>

int nvme_qp_init(struct nvme_qp *qp, nvme_dev_t *dev, uint16_t qid, uint16_t depth)
{
    (void)qp;
    (void)dev;
    (void)qid;
    (void)depth;
    return -ENOSYS;
}

void nvme_qp_destroy(struct nvme_qp *qp) { (void)qp; }
void nvme_qp_reset(struct nvme_qp *qp) { (void)qp; }

int nvme_qp_submit(struct nvme_qp *qp, const struct nvme_sqe *sqe, uint16_t cid)
{
    (void)qp;
    (void)sqe;
    (void)cid;
    return -ENOSYS;
}

int nvme_qp_reap(struct nvme_qp *qp, struct nvme_cqe *out, int max)
{
    (void)qp;
    (void)out;
    (void)max;
    return 0;
}

bool nvme_qp_full(const struct nvme_qp *qp)
{
    (void)qp;
    return true;
}

uint64_t nvme_qp_submitted(const struct nvme_qp *qp) { (void)qp; return 0; }
uint64_t nvme_qp_completed(const struct nvme_qp *qp) { (void)qp; return 0; }
uint64_t nvme_qp_stalls(const struct nvme_qp *qp) { (void)qp; return 0; }
uint64_t nvme_qp_inflight(const struct nvme_qp *qp) { (void)qp; return 0; }

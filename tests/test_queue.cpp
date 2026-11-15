#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "nvme/device.h"
#include "nvme/queue.h"

namespace {

class QueueTest : public ::testing::Test {
protected:
    void SetUp() override {
        nvme_dev_cfg_default(&cfg_);
        cfg_.ns_lba_count = 64;
        cfg_.max_qid = 4;
        dev_ = nvme_dev_create(&cfg_);
        ASSERT_NE(dev_, nullptr);
        ASSERT_EQ(0, nvme_qp_init(&qp_, dev_, 1, kDepth));
        /* No CREATE_SQ was issued for qid 1, so the controller never consumes
         * these SQEs; the test plays the controller when posting CQEs. */
    }

    void TearDown() override {
        nvme_qp_destroy(&qp_);
        nvme_dev_destroy(dev_);
    }

    static void post_cqe(nvme_qp& qp, uint16_t index, uint16_t cid, uint8_t phase) {
        nvme_cqe* cqe = &qp.cq[index];
        cqe->cdw0 = 0;
        cqe->rsvd = 0;
        cqe->sqhd = 0;
        cqe->sqid = qp.qid;
        cqe->cid = cid;
        __atomic_store_n(&cqe->status, nvme_cqe_status_encode(phase, 0, 0, 0), __ATOMIC_RELEASE);
    }

    static nvme_sqe make_sqe(uint16_t cid) {
        nvme_sqe sqe;
        std::memset(&sqe, 0, sizeof(sqe));
        sqe.opc = NVME_IO_WRITE;
        sqe.nsid = 1;
        sqe.cid = cid;
        sqe.cdw12 = 0;
        return sqe;
    }

    nvme_dev_cfg cfg_{};
    nvme_dev_t* dev_ = nullptr;
    nvme_qp qp_{};
    static constexpr uint16_t kDepth = 8;
};

TEST_F(QueueTest, FullRingReturnsEagainAndPreservesInFlightSqe) {
    for (uint16_t i = 0; i < kDepth; ++i) {
        nvme_sqe sqe = make_sqe(i);
        ASSERT_EQ(0, nvme_qp_submit(&qp_, &sqe, i)) << "submit " << i;
    }
    EXPECT_TRUE(nvme_qp_full(&qp_));
    EXPECT_EQ(static_cast<uint64_t>(kDepth), nvme_qp_inflight(&qp_));

    nvme_sqe overflow = make_sqe(999);
    EXPECT_EQ(NVME_QP_EAGAIN, nvme_qp_submit(&qp_, &overflow, 999));
    EXPECT_EQ(1u, nvme_qp_stalls(&qp_));

    /* The oldest in-flight SQE must not have been overwritten. */
    EXPECT_EQ(0u, qp_.sq[0].cid);
    EXPECT_EQ(1u, qp_.sq[1].cid);
}

TEST_F(QueueTest, PhaseAndOrderAcrossThreeWraps) {
    uint16_t next_cid = 0;
    uint8_t phase = 1;

    for (int wrap = 0; wrap < 3; ++wrap) {
        for (uint16_t i = 0; i < kDepth; ++i) {
            nvme_sqe sqe = make_sqe(next_cid);
            ASSERT_EQ(0, nvme_qp_submit(&qp_, &sqe, next_cid));
            next_cid++;
        }

        /* Complete out of order on purpose: the ring must hand back exactly
         * the cids the controller posted, in ring order. */
        const uint16_t order[8] = {5, 2, 7, 0, 6, 1, 4, 3};
        for (uint16_t i = 0; i < kDepth; ++i) {
            post_cqe(qp_, i, static_cast<uint16_t>(wrap * kDepth + order[i]), phase);
        }

        nvme_cqe out[16];
        int n = nvme_qp_reap(&qp_, out, 16);
        ASSERT_EQ(static_cast<int>(kDepth), n) << "wrap " << wrap;
        for (uint16_t i = 0; i < kDepth; ++i) {
            EXPECT_EQ(static_cast<uint16_t>(wrap * kDepth + order[i]), out[i].cid);
            EXPECT_EQ(phase, nvme_cqe_phase(out[i].status));
        }
        EXPECT_EQ(static_cast<uint8_t>(phase ^ 1u), qp_.cq_phase)
            << "phase must flip exactly once per wrap";
        EXPECT_EQ(0u, nvme_qp_inflight(&qp_));
        EXPECT_FALSE(nvme_qp_full(&qp_));
        phase ^= 1u;
    }

    /* A further reap with the wrong phase must return nothing. */
    nvme_cqe none[4];
    EXPECT_EQ(0, nvme_qp_reap(&qp_, none, 4));
    EXPECT_EQ(static_cast<uint64_t>(3 * kDepth), nvme_qp_completed(&qp_));
    EXPECT_EQ(static_cast<uint64_t>(3 * kDepth), nvme_qp_submitted(&qp_));
}

TEST_F(QueueTest, PartialBatchOnlyWrapsWhenLastEntryConsumed) {
    for (uint16_t i = 0; i < kDepth; ++i) {
        nvme_sqe sqe = make_sqe(i);
        ASSERT_EQ(0, nvme_qp_submit(&qp_, &sqe, i));
    }
    for (uint16_t i = 0; i < kDepth; ++i) {
        post_cqe(qp_, i, i, 1);
    }
    nvme_cqe out[8];

    int n = nvme_qp_reap(&qp_, out, 3);
    EXPECT_EQ(3, n);
    EXPECT_EQ(1u, qp_.cq_phase);
    EXPECT_EQ(3u, qp_.cq_head);
    EXPECT_EQ(static_cast<uint64_t>(kDepth - 3), nvme_qp_inflight(&qp_));

    n = nvme_qp_reap(&qp_, out, 8);
    EXPECT_EQ(static_cast<int>(kDepth - 3), n);
    EXPECT_EQ(0u, qp_.cq_phase);
    EXPECT_EQ(0u, qp_.cq_head);
}

} // namespace

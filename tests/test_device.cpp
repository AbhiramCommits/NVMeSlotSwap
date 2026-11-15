#include <gtest/gtest.h>

#include <cstring>
#include <ctime>
#include <string>

#include "nvme/device.h"

namespace {

uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

class DeviceTest : public ::testing::Test {
protected:
    void SetUp() override {
        nvme_dev_cfg_default(&cfg_);
        cfg_.ns_lba_count = 256;
        cfg_.max_qid = 4;
        cfg_.enable_delay_us = 300;
        cfg_.reset_delay_us = 100;
        cfg_.io_latency_base_ns = 50;
        cfg_.io_latency_jitter_ns = 50;
        dev_ = nvme_dev_create(&cfg_);
        ASSERT_NE(dev_, nullptr);
        bar0_ = nvme_dev_bar0(dev_);
        stride_ = nvme_dev_doorbell_stride(dev_);
        asq_ = nvme_dev_dma_map(dev_, 32 * 64, &asq_iova_);
        acq_ = nvme_dev_dma_map(dev_, 32 * 16, &acq_iova_);
        data_ = nvme_dev_dma_map(dev_, 4096, &data_iova_);
        ASSERT_NE(asq_, nullptr);
        ASSERT_NE(acq_, nullptr);
        ASSERT_NE(data_, nullptr);
        std::memset(asq_, 0, 32 * 64);
        std::memset(acq_, 0, 32 * 16);
        std::memset(data_, 0, 4096);
    }

    void TearDown() override {
        if (dev_ != nullptr) {
            nvme_dev_destroy(dev_);
        }
    }

    void program_admin_registers() {
        nvme_mmio_write32(bar0_, NVME_REG_AQA, 31u | (31u << 16));
        nvme_mmio_write64(bar0_, NVME_REG_ASQ, asq_iova_);
        nvme_mmio_write64(bar0_, NVME_REG_ACQ, acq_iova_);
    }

    bool enable(uint32_t timeout_ms = 3000) {
        nvme_mmio_write32(bar0_, NVME_REG_CC,
                          NVME_CC_EN | (6u << NVME_CC_IOSQES_SHIFT) | (4u << NVME_CC_IOCQES_SHIFT));
        return wait_ready(true, timeout_ms);
    }

    bool wait_ready(bool want, uint32_t timeout_ms = 3000) {
        uint64_t deadline = now_ns() + static_cast<uint64_t>(timeout_ms) * 1000000ull;
        while (true) {
            bool ready = (nvme_mmio_read32(bar0_, NVME_REG_CSTS) & NVME_CSTS_RDY) != 0;
            if (ready == want) {
                return true;
            }
            if (now_ns() > deadline) {
                return false;
            }
        }
    }

    bool submit_admin(const nvme_sqe& sqe) {
        if (sq_idx_ >= 32) {
            return false;
        }
        std::memcpy(static_cast<char*>(asq_) + sq_idx_ * 64, &sqe, 64);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        nvme_dbl_write(bar0_, NVME_SQ_DBL(0, stride_), static_cast<uint32_t>(sq_idx_ + 1));
        sq_idx_++;
        return true;
    }

    bool wait_cqe(uint32_t timeout_ms = 2000) {
        nvme_cqe* cq = static_cast<nvme_cqe*>(acq_);
        uint64_t deadline = now_ns() + static_cast<uint64_t>(timeout_ms) * 1000000ull;
        while ((__atomic_load_n(&cq[cq_idx_].status, __ATOMIC_ACQUIRE) & 1u) != 1u) {
            if (now_ns() > deadline) {
                return false;
            }
        }
        last_cqe_ = cq[cq_idx_];
        return true;
    }

    unsigned last_sc() const { return nvme_cqe_sc(last_cqe_.status); }

    nvme_sqe identify_sqe(uint64_t prp1, uint32_t cns, uint32_t nsid = 0) const {
        nvme_sqe sqe;
        std::memset(&sqe, 0, sizeof(sqe));
        sqe.opc = NVME_ADMIN_IDENTIFY;
        sqe.nsid = nsid;
        sqe.prp1 = prp1;
        sqe.cdw10 = cns;
        return sqe;
    }

    /* Creates one I/O queue pair and returns its qid. */
    bool create_io_queue(uint16_t qid, uint16_t depth, void* sq, uint64_t sq_iova, void* cq,
                         uint64_t cq_iova) {
        nvme_sqe cq_sqe;
        std::memset(&cq_sqe, 0, sizeof(cq_sqe));
        cq_sqe.opc = NVME_ADMIN_CREATE_CQ;
        cq_sqe.prp1 = cq_iova;
        cq_sqe.cdw10 = (depth - 1u) | (static_cast<uint32_t>(qid) << 16);
        cq_sqe.cdw11 = 1;
        if (!submit_admin(cq_sqe) || !wait_cqe()) {
            return false;
        }
        if (last_sc() != 0) {
            return false;
        }
        cq_idx_++;
        last_cqe_ = nvme_cqe{};

        nvme_sqe sq_sqe;
        std::memset(&sq_sqe, 0, sizeof(sq_sqe));
        sq_sqe.opc = NVME_ADMIN_CREATE_SQ;
        sq_sqe.prp1 = sq_iova;
        sq_sqe.cdw10 = (depth - 1u) | (static_cast<uint32_t>(qid) << 16);
        sq_sqe.cdw11 = 1u | (static_cast<uint32_t>(qid) << 16);
        if (!submit_admin(sq_sqe) || !wait_cqe()) {
            return false;
        }
        if (last_sc() != 0) {
            return false;
        }
        cq_idx_++;
        (void)sq;
        (void)cq;
        return true;
    }

    nvme_dev_cfg cfg_{};
    nvme_dev_t* dev_ = nullptr;
    volatile void* bar0_ = nullptr;
    uint32_t stride_ = 4;
    void* asq_ = nullptr;
    void* acq_ = nullptr;
    void* data_ = nullptr;
    uint64_t asq_iova_ = 0;
    uint64_t acq_iova_ = 0;
    uint64_t data_iova_ = 0;
    uint32_t sq_idx_ = 0;
    uint32_t cq_idx_ = 0;
    nvme_cqe last_cqe_{};
};

TEST_F(DeviceTest, EnableResetReenableSequence) {
    program_admin_registers();
    ASSERT_TRUE(enable());
    EXPECT_TRUE((nvme_mmio_read32(bar0_, NVME_REG_CSTS) & NVME_CSTS_RDY) != 0);
    EXPECT_EQ(1u, nvme_dev_ready_count(dev_));

    nvme_mmio_write32(bar0_, NVME_REG_CC, 0);
    ASSERT_TRUE(wait_ready(false));
    EXPECT_EQ(1u, nvme_dev_reset_count(dev_));

    /* Doorbells are cleared by the reset. */
    EXPECT_EQ(0u, nvme_dbl_read(bar0_, NVME_SQ_DBL(0, stride_)));

    ASSERT_TRUE(enable());
    EXPECT_EQ(2u, nvme_dev_ready_count(dev_));
}

TEST_F(DeviceTest, AdminCommandRejectedBeforeReady) {
    program_admin_registers();
    /* No CC.EN yet.  The controller must complete the command with
     * INVALID_FIELD instead of executing it. */
    nvme_sqe sqe = identify_sqe(data_iova_, NVME_ID_CNS_CONTROLLER);
    sqe.cid = 77;
    ASSERT_TRUE(submit_admin(sqe));
    ASSERT_TRUE(wait_cqe());
    EXPECT_EQ(NVME_SC_INVALID_FIELD, last_sc());
    EXPECT_EQ(77, last_cqe_.cid);
}

TEST_F(DeviceTest, IdentifyControllerAndNamespace) {
    program_admin_registers();
    ASSERT_TRUE(enable());

    ASSERT_TRUE(submit_admin(identify_sqe(data_iova_, NVME_ID_CNS_CONTROLLER)));
    ASSERT_TRUE(wait_cqe());
    ASSERT_EQ(0u, last_sc());
    nvme_id_ctrl id;
    std::memcpy(&id, data_, sizeof(id));
    EXPECT_EQ(std::string(id.mn, 40).substr(0, 12), "NVMeSlotSwap");
    EXPECT_EQ(1u, id.nn);
    EXPECT_EQ(0x00010400u, id.ver);
    EXPECT_NE(0u, id.frmw & NVME_FRMW_SLOT1_RO);
    EXPECT_NE(0u, id.oacs & NVME_OACS_FW_DOWNLOAD);
    EXPECT_NE(0u, id.oacs & NVME_OACS_FW_COMMIT);
    cq_idx_++;

    ASSERT_TRUE(submit_admin(identify_sqe(data_iova_, NVME_ID_CNS_NAMESPACE, 1)));
    ASSERT_TRUE(wait_cqe());
    ASSERT_EQ(0u, last_sc());
    nvme_id_ns ns;
    std::memcpy(&ns, data_, sizeof(ns));
    EXPECT_EQ(cfg_.ns_lba_count, ns.nsze);
    EXPECT_EQ(0u, ns.nlbaf);
    EXPECT_EQ(12u, ns.lbaf[0].ds);
    cq_idx_++;
}

TEST_F(DeviceTest, Prp2AndPrpListTransfers) {
    program_admin_registers();
    ASSERT_TRUE(enable());

    const uint16_t depth = 8;
    uint64_t sq_iova = 0, cq_iova = 0;
    uint64_t p1i = 0, p2i = 0, p3i = 0, r1i = 0, r2i = 0, r3i = 0, listi = 0;
    void* sq = nvme_dev_dma_map(dev_, depth * 64, &sq_iova);
    void* cq = nvme_dev_dma_map(dev_, depth * 16, &cq_iova);
    void* p1 = nvme_dev_dma_map(dev_, 4096, &p1i);
    void* p2 = nvme_dev_dma_map(dev_, 4096, &p2i);
    void* p3 = nvme_dev_dma_map(dev_, 4096, &p3i);
    void* r1 = nvme_dev_dma_map(dev_, 4096, &r1i);
    void* r2 = nvme_dev_dma_map(dev_, 4096, &r2i);
    void* r3 = nvme_dev_dma_map(dev_, 4096, &r3i);
    void* list = nvme_dev_dma_map(dev_, 4096, &listi);
    ASSERT_NE(sq, nullptr);
    ASSERT_NE(cq, nullptr);
    ASSERT_NE(list, nullptr);
    std::memset(sq, 0, depth * 64);
    std::memset(cq, 0, depth * 16);
    std::memset(list, 0, 4096);

    ASSERT_TRUE(create_io_queue(1, depth, sq, sq_iova, cq, cq_iova));

    for (int i = 0; i < 4096; ++i) {
        static_cast<uint8_t*>(p1)[i] = static_cast<uint8_t>(i * 3);
        static_cast<uint8_t*>(p2)[i] = static_cast<uint8_t>(i * 5 + 1);
        static_cast<uint8_t*>(p3)[i] = static_cast<uint8_t>(i * 7 + 2);
    }

    nvme_cqe* io_cq = static_cast<nvme_cqe*>(cq);
    auto io_sqe = [](uint8_t opc, uint64_t slba, uint16_t nlb, uint64_t prp1, uint64_t prp2) {
        nvme_sqe s;
        std::memset(&s, 0, sizeof(s));
        s.opc = opc;
        s.nsid = 1;
        s.prp1 = prp1;
        s.prp2 = prp2;
        s.cdw10 = static_cast<uint32_t>(slba & 0xFFFFFFFFu);
        s.cdw11 = static_cast<uint32_t>(slba >> 32);
        s.cdw12 = nlb - 1u;
        return s;
    };
    auto ring_io = [&](uint16_t slot, const nvme_sqe& s) {
        std::memcpy(static_cast<char*>(sq) + slot * 64, &s, 64);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        nvme_dbl_write(bar0_, NVME_SQ_DBL(1, stride_), slot + 1u);
    };
    auto wait_io = [&](uint16_t slot, uint32_t timeout_ms = 2000) {
        uint64_t deadline = now_ns() + static_cast<uint64_t>(timeout_ms) * 1000000ull;
        while ((__atomic_load_n(&io_cq[slot].status, __ATOMIC_ACQUIRE) & 1u) != 1u) {
            if (now_ns() > deadline) {
                return false;
            }
        }
        return true;
    };

    /* Two pages: PRP2 is the second data page. */
    ring_io(0, io_sqe(NVME_IO_WRITE, 0, 2, p1i, p2i));
    ASSERT_TRUE(wait_io(0));
    ASSERT_EQ(0u, nvme_cqe_sc(io_cq[0].status));

    ring_io(1, io_sqe(NVME_IO_READ, 0, 2, r1i, r2i));
    ASSERT_TRUE(wait_io(1));
    ASSERT_EQ(0u, nvme_cqe_sc(io_cq[1].status));
    EXPECT_EQ(0, std::memcmp(p1, r1, 4096));
    EXPECT_EQ(0, std::memcmp(p2, r2, 4096));

    /* Three pages: PRP2 points at a PRP list. */
    static_cast<uint64_t*>(list)[0] = p2i;
    static_cast<uint64_t*>(list)[1] = p3i;
    ring_io(2, io_sqe(NVME_IO_WRITE, 8, 3, p1i, listi));
    ASSERT_TRUE(wait_io(2));
    ASSERT_EQ(0u, nvme_cqe_sc(io_cq[2].status));

    static_cast<uint64_t*>(list)[0] = r2i;
    static_cast<uint64_t*>(list)[1] = r3i;
    ring_io(3, io_sqe(NVME_IO_READ, 8, 3, r1i, listi));
    ASSERT_TRUE(wait_io(3));
    ASSERT_EQ(0u, nvme_cqe_sc(io_cq[3].status));
    EXPECT_EQ(0, std::memcmp(p1, r1, 4096));
    EXPECT_EQ(0, std::memcmp(p2, r2, 4096));
    EXPECT_EQ(0, std::memcmp(p3, r3, 4096));

    /* LBA out of range must be rejected, not silently truncated. */
    ring_io(4, io_sqe(NVME_IO_READ, cfg_.ns_lba_count - 1, 2, r1i, r2i));
    ASSERT_TRUE(wait_io(4));
    EXPECT_EQ(NVME_SC_INVALID_FIELD, nvme_cqe_sc(io_cq[4].status));
}

} // namespace

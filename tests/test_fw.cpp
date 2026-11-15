#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <thread>
#include <string>
#include <vector>

#include "nvme/backend.hpp"
#include "nvme/driver.hpp"
#include "nvme/fw.h"

namespace nvme {

namespace {

bool wait_for_completion(NvmeDriver& driver, unsigned q, Completion* out,
                         uint32_t timeout_ms = 2000) {
    uint64_t deadline = now_ns() + static_cast<uint64_t>(timeout_ms) * 1000000ull;
    while (now_ns() < deadline) {
        auto c = driver.poll(q);
        if (c) {
            *out = *c;
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

} // namespace

class FwTest : public ::testing::Test {
protected:
    void SetUp() override {
        nvme_dev_cfg_default(&cfg_);
        cfg_.ns_lba_count = 1024;
        cfg_.max_qid = 4;
        cfg_.enable_delay_us = 300;
        backend_ = std::make_unique<SimBackend>(cfg_);
        driver_ = std::make_unique<NvmeDriver>(*backend_);
        driver_->reset_and_enable();
        updater_ = std::make_unique<FirmwareUpdater>(*driver_);
    }

    void reset_controller() {
        driver_->controller_reset();
        driver_->reset_and_enable();
    }

    void stage(unsigned slot, const std::string& rev, size_t size = 65536, size_t chunk = 4096) {
        std::vector<uint8_t> image = FirmwareUpdater::build_image(rev, size, 0xF00Du + slot);
        StageResult r = updater_->stage(slot, image, chunk);
        ASSERT_TRUE(r.ok()) << "stage failed with status " << r.status;
    }

    nvme_dev_cfg cfg_{};
    std::unique_ptr<SimBackend> backend_;
    std::unique_ptr<NvmeDriver> driver_;
    std::unique_ptr<FirmwareUpdater> updater_;
};

TEST_F(FwTest, ChunkedDownloadReassemblesImage) {
    std::vector<uint8_t> image = FirmwareUpdater::build_image("FW2.0.1", 256 * 1024, 9);
    StageResult r = updater_->stage(2, image, 4096);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ((image.size() + 4095) / 4096, r.chunks);

    CommitResult c = updater_->commit(2, CommitAction::Replace);
    EXPECT_EQ(0, c.status);
    EXPECT_EQ(1, c.active_slot);
    FwSlotInfo log = updater_->read_slot_log();
    EXPECT_EQ("FW2.0.1", log.revisions[1]);
    EXPECT_EQ("FW1.0.0", log.revisions[0]);

    /* A different chunk size exercises the same reassembly path. */
    image = FirmwareUpdater::build_image("FW3.0.0", 48 * 1024, 11);
    r = updater_->stage(3, image, 1024);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(0, updater_->commit(3, CommitAction::Replace).status);
    log = updater_->read_slot_log();
    EXPECT_EQ("FW3.0.0", log.revisions[2]);
}

TEST_F(FwTest, BadMagicIsRejectedAndActiveSlotUntouched) {
    std::vector<uint8_t> image = FirmwareUpdater::build_image("FW2.0.1", 8192, 3);
    image[0] = 'X'; /* corrupt the 'NVFW' magic */
    ASSERT_TRUE(updater_->stage(2, image, 4096).ok());

    CommitResult c = updater_->commit(2, CommitAction::ReplaceActivateImmediate);
    EXPECT_EQ(NVME_SC_FW_IMAGE_INVALID, c.status);
    EXPECT_FALSE(c.rolled_back);
    FwSlotInfo log = updater_->read_slot_log();
    EXPECT_EQ(1, log.active_slot);
    EXPECT_EQ("", log.revisions[1]);
}

TEST_F(FwTest, CorruptedPayloadIsRejectedAndActiveSlotUntouched) {
    std::vector<uint8_t> image = FirmwareUpdater::build_image("FW2.0.1", 16384, 4);
    image.back() ^= 0xFF; /* payload bit flip after the SHA-256 was computed */
    ASSERT_TRUE(updater_->stage(2, image, 4096).ok());

    CommitResult c = updater_->commit(2, CommitAction::ReplaceActivateImmediate);
    EXPECT_EQ(NVME_SC_FW_IMAGE_INVALID, c.status);
    FwSlotInfo log = updater_->read_slot_log();
    EXPECT_EQ(1, log.active_slot);
    EXPECT_EQ("", log.revisions[1]);
}

TEST_F(FwTest, CommitActionMatrix) {
    /* CA=0: replace the slot, do not activate. */
    stage(2, "FW2.0.0");
    CommitResult c0 = updater_->commit(2, CommitAction::Replace);
    EXPECT_EQ(0, c0.status);
    FwSlotInfo log = updater_->read_slot_log();
    EXPECT_EQ(1, log.active_slot);
    EXPECT_EQ(1, log.next_slot);
    EXPECT_EQ("FW2.0.0", log.revisions[1]);

    /* CA=1: replace and activate on the next reset. */
    stage(3, "FW3.0.0");
    CommitResult c1 = updater_->commit(3, CommitAction::ReplaceActivate);
    EXPECT_EQ(0, c1.status);
    log = updater_->read_slot_log();
    EXPECT_EQ(1, log.active_slot);
    EXPECT_EQ(3, log.next_slot);
    reset_controller();
    log = updater_->read_slot_log();
    EXPECT_EQ(3, log.active_slot);
    EXPECT_EQ(3, log.next_slot);

    /* CA=2: activate an existing slot on the next reset. */
    stage(4, "FW4.0.0");
    EXPECT_EQ(0, updater_->commit(4, CommitAction::Replace).status);
    CommitResult c2 = updater_->commit(4, CommitAction::Activate);
    EXPECT_EQ(NVME_SC_FW_ACTIVATION_REQUIRES_RESET, c2.status);
    EXPECT_TRUE(c2.requires_reset);
    log = updater_->read_slot_log();
    EXPECT_EQ(3, log.active_slot);
    EXPECT_EQ(4, log.next_slot);
    reset_controller();
    log = updater_->read_slot_log();
    EXPECT_EQ(4, log.active_slot);

    /* CA=3: replace and activate immediately without a controller reset. */
    uint64_t resets_before = nvme_dev_reset_count(backend_->device());
    stage(5, "FW5.0.0");
    CommitResult c3 = updater_->commit(5, CommitAction::ReplaceActivateImmediate);
    EXPECT_EQ(0, c3.status);
    EXPECT_FALSE(c3.rolled_back);
    log = updater_->read_slot_log();
    EXPECT_EQ(5, log.active_slot);
    EXPECT_EQ(5, log.next_slot);
    EXPECT_EQ(resets_before, nvme_dev_reset_count(backend_->device()));
    EXPECT_TRUE(updater_->verify_activation("FW5.0.0"));
}

TEST_F(FwTest, SlotOneIsReadOnly) {
    stage(1, "FW9.0.0");
    CommitResult c = updater_->commit(1, CommitAction::ReplaceActivateImmediate);
    EXPECT_EQ(NVME_SC_FW_ACTIVATION_PROHIBITED, c.status);
    FwSlotInfo log = updater_->read_slot_log();
    EXPECT_EQ(1, log.active_slot);
    EXPECT_EQ("FW1.0.0", log.revisions[0]);
}

TEST_F(FwTest, ActivateEmptySlotFails) {
    CommitResult c = updater_->commit(6, CommitAction::Activate);
    EXPECT_EQ(NVME_SC_FW_SLOT_INVALID, c.status);
}

TEST_F(FwTest, FailedSelfTestRollsBackToPreviousSlot) {
    stage(2, "FW2.0.0");
    ASSERT_EQ(0, updater_->commit(2, CommitAction::ReplaceActivateImmediate).status);
    ASSERT_EQ("FW2.0.0", updater_->active_revision());

    /* Revision containing BAD triggers the post-activation self-test failure. */
    stage(3, "FW3BAD0");
    CommitResult c = updater_->commit(3, CommitAction::ReplaceActivateImmediate);
    EXPECT_TRUE(c.rolled_back);
    EXPECT_EQ(NVME_SC_FW_IMAGE_INVALID, c.status);
    EXPECT_EQ("FW2.0.0", updater_->active_revision());
    FwSlotInfo log = updater_->read_slot_log();
    EXPECT_EQ(2, log.active_slot);
    EXPECT_EQ(2, log.next_slot);
    EXPECT_EQ("FW3BAD0", log.revisions[2]);
}

TEST_F(FwTest, InjectedSelfTestFailureAlsoRollsBack) {
    stage(2, "FW2.0.0");
    ASSERT_EQ(0, updater_->commit(2, CommitAction::ReplaceActivateImmediate).status);
    stage(3, "FW3.0.0");
    driver_->set_self_test_fail(true);
    CommitResult c = updater_->commit(3, CommitAction::ReplaceActivateImmediate);
    EXPECT_TRUE(c.rolled_back);
    EXPECT_EQ("FW2.0.0", updater_->active_revision());
}

TEST_F(FwTest, NamespaceStaysReadableAcrossRollback) {
    driver_->create_io_queues(1, 8);
    DmaBufferPool pool(*backend_, 2 * NVME_LBA_SIZE, 2);
    DmaBuffer* w = pool.acquire();
    DmaBuffer* r = pool.acquire();
    ASSERT_NE(w, nullptr);
    ASSERT_NE(r, nullptr);
    std::memset(w->ptr, 0x5A, 2 * NVME_LBA_SIZE);
    CmdHandle h = driver_->submit_write(0, 4, 2, *w);
    ASSERT_TRUE(h.ok);
    Completion done{};
    ASSERT_TRUE(wait_for_completion(*driver_, 0, &done));
    ASSERT_TRUE(done.ok());

    stage(2, "FW2.0.0");
    ASSERT_EQ(0, updater_->commit(2, CommitAction::ReplaceActivateImmediate).status);
    stage(3, "FW3BAD0");
    EXPECT_TRUE(updater_->commit(3, CommitAction::ReplaceActivateImmediate).rolled_back);

    h = driver_->submit_read(0, 4, 2, *r);
    ASSERT_TRUE(h.ok);
    ASSERT_TRUE(wait_for_completion(*driver_, 0, &done));
    EXPECT_TRUE(done.ok());
    EXPECT_EQ(0, std::memcmp(w->ptr, r->ptr, 2 * NVME_LBA_SIZE));
}

} // namespace nvme

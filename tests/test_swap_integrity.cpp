#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "nvme/backend.hpp"
#include "nvme/driver.hpp"
#include "nvme/fw.h"
#include "nvme/workload.hpp"

namespace nvme {

TEST(SwapIntegrity, Ca3ActivationLosesNothing)
{
    nvme_dev_cfg cfg;
    nvme_dev_cfg_default(&cfg);
    cfg.ns_lba_count = 4096;
    cfg.max_qid = 4;
    cfg.enable_delay_us = 300;

    SimBackend backend(cfg);
    NvmeDriver driver(backend);
    driver.reset_and_enable();
    driver.create_io_queues(4, 16);

    WorkloadConfig wc;
    wc.queues = 4;
    wc.depth = 16;
    wc.read_ratio = 0.7;
    wc.seconds = 10;
    wc.seed = 424242;
    wc.io_blocks = 2;

    Workload workload(driver, wc);
    workload.prefill();
    workload.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    FirmwareUpdater updater(driver);
    std::vector<uint8_t> image =
        FirmwareUpdater::build_image("FW2.0.1", 128 * 1024, 0x5150);
    StageResult staged = updater.stage(2, image, 4096);
    ASSERT_TRUE(staged.ok());

    workload.set_activation_time(now_ns());
    CommitResult committed = updater.commit(2, CommitAction::ReplaceActivateImmediate);
    EXPECT_EQ(0, committed.status);
    EXPECT_FALSE(committed.rolled_back);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    workload.stop();
    WorkloadStats stats = workload.stats();

    EXPECT_EQ(0u, stats.dropped) << "the no-reset path must not lose commands";
    EXPECT_EQ(0u, stats.integrity_errors);
    EXPECT_EQ(0u, stats.failed);
    EXPECT_EQ(stats.submitted, stats.completed);
    EXPECT_GT(stats.completed, 1000u);

    FwSlotInfo log = updater.read_slot_log();
    EXPECT_EQ(2, log.active_slot);
    EXPECT_EQ("FW2.0.1", log.revisions[1]);
    EXPECT_TRUE(updater.verify_activation("FW2.0.1"));
}

TEST(SwapIntegrity, FailedCommitRollsBackAndKeepsServing)
{
    nvme_dev_cfg cfg;
    nvme_dev_cfg_default(&cfg);
    cfg.ns_lba_count = 4096;
    cfg.max_qid = 4;
    cfg.enable_delay_us = 300;

    SimBackend backend(cfg);
    NvmeDriver driver(backend);
    driver.reset_and_enable();
    driver.create_io_queues(2, 16);

    WorkloadConfig wc;
    wc.queues = 2;
    wc.depth = 16;
    wc.read_ratio = 0.5;
    wc.seconds = 10;
    wc.seed = 99;

    Workload workload(driver, wc);
    workload.prefill();
    workload.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    FirmwareUpdater updater(driver);
    ASSERT_TRUE(updater.stage(2, FirmwareUpdater::build_image("FW9BAD0", 64 * 1024, 1), 4096)
                    .ok());
    workload.set_activation_time(now_ns());
    CommitResult committed = updater.commit(2, CommitAction::ReplaceActivateImmediate);
    EXPECT_TRUE(committed.rolled_back);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    workload.stop();
    WorkloadStats stats = workload.stats();
    EXPECT_EQ(0u, stats.dropped);
    EXPECT_EQ(0u, stats.integrity_errors);
    EXPECT_EQ("FW1.0.0", updater.active_revision());
}

} // namespace nvme

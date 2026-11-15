/* workload.hpp - continuous multi-queue I/O workload with integrity checks. */
#ifndef NVME_WORKLOAD_HPP
#define NVME_WORKLOAD_HPP

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "nvme/driver.hpp"
#include "nvme/latency.hpp"

namespace nvme {

struct WorkloadConfig {
    unsigned queues = 4;
    unsigned depth = 32;
    double read_ratio = 0.7; /* 0..1 */
    unsigned seconds = 3;
    uint32_t seed = 12345;
    unsigned io_blocks = 2; /* 4 KiB LBAs per command (2 => 8 KiB, PRP2) */
    double window_half_ms = 200.0;
};

struct WorkloadStats {
    uint64_t submitted = 0;
    uint64_t completed = 0;
    uint64_t failed = 0;
    uint64_t dropped = 0;
    uint64_t eagain_stalls = 0;
    uint64_t integrity_errors = 0;
    double duration_s = 0.0;
    double iops = 0.0;
    double max_gap_us = 0.0;
    /* Longest completion gap whose two ends sit in the 20 ms after the
     * activation timestamp: isolates the activation's own I/O interruption
     * from scheduler noise elsewhere in the run. */
    double activation_gap_us = 0.0;
    std::map<uint16_t, uint64_t> failed_by_status;
    LatencySummary steady;
    LatencySummary window;
    uint64_t window_start_ns = 0;
    uint64_t window_end_ns = 0;
};

/*
 * One queue pair per worker thread, QD commands outstanding per queue,
 * deterministic LBA selection.  Every LBA always holds a pattern derived from
 * its own number, so a read can be verified regardless of which thread wrote
 * it; concurrent writers of the same LBA write identical bytes.
 */
class Workload {
public:
    struct Worker;

    Workload(NvmeDriver& driver, const WorkloadConfig& cfg);
    ~Workload();

    Workload(const Workload&) = delete;
    Workload& operator=(const Workload&) = delete;

    /* Writes the integrity pattern over the whole namespace through the
     * driver, then verifies a sample by reading it back. */
    void prefill();
    uint64_t prefill_lba_count() const { return ns_lbas_; }

    void start();
    /* Stops issuing new commands and waits until all outstanding complete. */
    void pause();
    void resume();
    void stop();

    void set_activation_time(uint64_t ns);

    WorkloadStats stats() const;
    bool integrity_ok() const;

    uint64_t namespace_lbas() const { return ns_lbas_; }
    const WorkloadConfig& config() const { return cfg_; }

    /* Up to 4096 raw latency samples (microseconds) from the reservoir. */
    std::vector<double> latency_samples_us() const;
    std::vector<double> window_samples_us() const;

private:
    void fill_buffer(DmaBuffer* buf, uint64_t slba) const;
    bool verify_buffer(const DmaBuffer* buf, uint64_t slba) const;

    NvmeDriver& driver_;
    WorkloadConfig cfg_;
    uint64_t ns_lbas_ = 0;
    uint16_t io_blocks_ = 2;

    std::vector<std::unique_ptr<Worker>> workers_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> paused_{false};
    std::atomic<uint64_t> activation_ns_{0};

    /* Shared aggregates. */
    std::atomic<uint64_t> submitted_{0};
    std::atomic<uint64_t> completed_{0};
    std::atomic<uint64_t> failed_{0};
    std::atomic<uint64_t> eagain_{0};
    std::atomic<uint64_t> integrity_errors_{0};
    std::atomic<uint64_t> left_in_flight_{0};
    std::atomic<uint64_t> activation_gap_ns_{0};

    LatencyRecorder latency_;
    LatencyRecorder window_latency_;

    mutable std::mutex status_mutex_;
    std::map<uint16_t, uint64_t> failed_by_status_;

    uint64_t started_ns_ = 0;
    uint64_t stopped_ns_ = 0;
    uint64_t window_start_ns_ = 0;
    uint64_t window_end_ns_ = 0;
};

} // namespace nvme

#endif /* NVME_WORKLOAD_HPP */

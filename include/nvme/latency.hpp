/* latency.hpp - log-scale latency histogram and completion-gap tracking. */
#ifndef NVME_LATENCY_HPP
#define NVME_LATENCY_HPP

#include <cstdint>
#include <mutex>
#include <vector>

namespace nvme {

/*
 * Fixed-bucket log-scale histogram: 16 buckets per power of two from 1 ns to
 * ~66 s.  Recording is lock-free (relaxed atomic bucket increments) so that
 * every queue thread can share one recorder without serialising the fast
 * path.  A small reservoir of raw samples is kept for offline inspection;
 * it samples one completion in eight to keep the cost out of the fast path.
 */
class LatencyRecorder {
public:
    static constexpr unsigned kBuckets = 576;
    static constexpr unsigned kReservoirCap = 4096;

    explicit LatencyRecorder(uint32_t seed = 0x9E3779B9u);

    void reset();

    void record(uint64_t latency_ns);
    /* Also feeds the "longest interval with zero completions" metric. */
    void record_completion(uint64_t latency_ns, uint64_t completion_ns);

    uint64_t count() const;
    double mean_ns() const;
    uint64_t min_ns() const;
    uint64_t max_ns() const;

    /* Percentile in [0,100]; returns the histogram bucket's upper bound. */
    uint64_t percentile_ns(double p) const;
    uint64_t p50_ns() const { return percentile_ns(50.0); }
    uint64_t p90_ns() const { return percentile_ns(90.0); }
    uint64_t p99_ns() const { return percentile_ns(99.0); }
    uint64_t p999_ns() const { return percentile_ns(99.9); }

    double max_gap_us() const;
    uint64_t last_completion_ns() const;

    std::vector<uint64_t> reservoir() const;

private:
    static unsigned bucket_of(uint64_t ns);
    static double bucket_upper_ns(unsigned idx);
    void maybe_sample(uint64_t latency_ns);

    uint64_t buckets_[kBuckets];
    uint64_t total_count_;
    uint64_t total_sum_ns_;
    uint64_t min_ns_;
    uint64_t max_ns_;

    uint64_t last_completion_ns_;
    uint64_t max_gap_ns_;
    uint64_t sample_counter_;

    uint32_t rng_state_;
    mutable std::mutex reservoir_mutex_;
    std::vector<uint64_t> reservoir_;
};

struct LatencySummary {
    uint64_t count = 0;
    double mean_us = 0.0;
    double p50_us = 0.0;
    double p90_us = 0.0;
    double p99_us = 0.0;
    double p999_us = 0.0;
    double max_us = 0.0;
    double max_gap_us = 0.0;
};

LatencySummary summarize(const LatencyRecorder &rec);

} // namespace nvme

#endif /* NVME_LATENCY_HPP */

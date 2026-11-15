/*
 * latency.cpp - fixed-bucket log-scale latency histogram.
 *
 * Buckets are 16 per power of two: bucket i covers [2^(i/16), 2^((i+1)/16))
 * nanoseconds, i.e. sub-microsecond through ~66 seconds.  Recording is
 * lock-free: one relaxed atomic increment per sample.  Percentiles report the
 * bucket's upper bound, which is conservative (never understates latency).
 */
#include "nvme/latency.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace nvme {

namespace {
constexpr double kPercentileScale = 100.0;
}

LatencyRecorder::LatencyRecorder(uint32_t seed) : rng_state_(seed ? seed : 0x9E3779B9u)
{
    reset();
}

void LatencyRecorder::reset()
{
    for (unsigned i = 0; i < kBuckets; ++i) {
        __atomic_store_n(&buckets_[i], 0ull, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&total_count_, 0ull, __ATOMIC_RELAXED);
    __atomic_store_n(&total_sum_ns_, 0ull, __ATOMIC_RELAXED);
    __atomic_store_n(&min_ns_, std::numeric_limits<uint64_t>::max(), __ATOMIC_RELAXED);
    __atomic_store_n(&max_ns_, 0ull, __ATOMIC_RELAXED);
    __atomic_store_n(&last_completion_ns_, 0ull, __ATOMIC_RELAXED);
    __atomic_store_n(&max_gap_ns_, 0ull, __ATOMIC_RELAXED);
    __atomic_store_n(&sample_counter_, 0ull, __ATOMIC_RELAXED);
    std::lock_guard<std::mutex> lock(reservoir_mutex_);
    reservoir_.clear();
    reservoir_.reserve(kReservoirCap);
}

unsigned LatencyRecorder::bucket_of(uint64_t ns)
{
    if (ns == 0) {
        return 0;
    }
    unsigned exponent = 63u - static_cast<unsigned>(__builtin_clzll(ns));
    if (exponent > 35u) {
        return kBuckets - 1;
    }
    uint64_t frac = (ns << 4) >> exponent; /* 16..31 */
    unsigned idx = exponent * 16u + static_cast<unsigned>(frac - 16u);
    if (idx >= kBuckets) {
        idx = kBuckets - 1;
    }
    return idx;
}

double LatencyRecorder::bucket_upper_ns(unsigned idx)
{
    unsigned exponent = idx / 16u;
    unsigned frac = 16u + (idx % 16u);
    return std::ldexp(static_cast<double>(frac), static_cast<int>(exponent)) / 16.0;
}

void LatencyRecorder::record(uint64_t latency_ns)
{
    if (latency_ns == 0) {
        latency_ns = 1;
    }
    unsigned idx = bucket_of(latency_ns);
    __atomic_fetch_add(&buckets_[idx], 1ull, __ATOMIC_RELAXED);
    __atomic_fetch_add(&total_count_, 1ull, __ATOMIC_RELAXED);
    __atomic_fetch_add(&total_sum_ns_, latency_ns, __ATOMIC_RELAXED);

    uint64_t prev = __atomic_load_n(&max_ns_, __ATOMIC_RELAXED);
    while (latency_ns > prev &&
           !__atomic_compare_exchange_n(&max_ns_, &prev, latency_ns, 0, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {
    }
    uint64_t minv = __atomic_load_n(&min_ns_, __ATOMIC_RELAXED);
    while (latency_ns < minv &&
           !__atomic_compare_exchange_n(&min_ns_, &minv, latency_ns, 0, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {
    }
    maybe_sample(latency_ns);
}

void LatencyRecorder::record_completion(uint64_t latency_ns, uint64_t completion_ns)
{
    record(latency_ns);
    if (completion_ns == 0) {
        return;
    }
    uint64_t prev = __atomic_load_n(&last_completion_ns_, __ATOMIC_ACQUIRE);
    for (;;) {
        if (completion_ns <= prev) {
            return;
        }
        if (__atomic_compare_exchange_n(&last_completion_ns_, &prev, completion_ns, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            break;
        }
    }
    if (prev != 0) {
        uint64_t gap = completion_ns - prev;
        uint64_t best = __atomic_load_n(&max_gap_ns_, __ATOMIC_RELAXED);
        while (gap > best &&
               !__atomic_compare_exchange_n(&max_gap_ns_, &best, gap, 0, __ATOMIC_RELAXED,
                                            __ATOMIC_RELAXED)) {
        }
    }
}

void LatencyRecorder::maybe_sample(uint64_t latency_ns)
{
    uint64_t n = __atomic_add_fetch(&sample_counter_, 1ull, __ATOMIC_RELAXED);
    if ((n & 7ull) != 0) {
        return; /* 1 in 8 samples */
    }
    std::lock_guard<std::mutex> lock(reservoir_mutex_);
    if (reservoir_.size() < kReservoirCap) {
        reservoir_.push_back(latency_ns);
        return;
    }
    rng_state_ ^= rng_state_ << 13;
    rng_state_ ^= rng_state_ >> 17;
    rng_state_ ^= rng_state_ << 5;
    reservoir_[rng_state_ % reservoir_.size()] = latency_ns;
}

uint64_t LatencyRecorder::count() const
{
    return __atomic_load_n(&total_count_, __ATOMIC_RELAXED);
}

double LatencyRecorder::mean_ns() const
{
    uint64_t n = count();
    if (n == 0) {
        return 0.0;
    }
    return static_cast<double>(__atomic_load_n(&total_sum_ns_, __ATOMIC_RELAXED)) /
           static_cast<double>(n);
}

uint64_t LatencyRecorder::min_ns() const
{
    uint64_t v = __atomic_load_n(&min_ns_, __ATOMIC_RELAXED);
    return v == std::numeric_limits<uint64_t>::max() ? 0 : v;
}

uint64_t LatencyRecorder::max_ns() const
{
    return __atomic_load_n(&max_ns_, __ATOMIC_RELAXED);
}

uint64_t LatencyRecorder::percentile_ns(double p) const
{
    uint64_t n = count();
    if (n == 0) {
        return 0;
    }
    if (p <= 0.0) {
        return min_ns();
    }
    if (p >= 100.0) {
        return max_ns();
    }
    uint64_t target = static_cast<uint64_t>(
        std::ceil(p / kPercentileScale * static_cast<double>(n)));
    if (target == 0) {
        target = 1;
    }
    uint64_t seen = 0;
    for (unsigned i = 0; i < kBuckets; ++i) {
        seen += __atomic_load_n(&buckets_[i], __ATOMIC_RELAXED);
        if (seen >= target) {
            uint64_t ub = static_cast<uint64_t>(std::ceil(bucket_upper_ns(i)));
            return ub ? ub : 1;
        }
    }
    return max_ns();
}

double LatencyRecorder::max_gap_us() const
{
    return static_cast<double>(__atomic_load_n(&max_gap_ns_, __ATOMIC_RELAXED)) / 1000.0;
}

uint64_t LatencyRecorder::last_completion_ns() const
{
    return __atomic_load_n(&last_completion_ns_, __ATOMIC_ACQUIRE);
}

std::vector<uint64_t> LatencyRecorder::reservoir() const
{
    std::lock_guard<std::mutex> lock(reservoir_mutex_);
    return reservoir_;
}

LatencySummary summarize(const LatencyRecorder &rec)
{
    LatencySummary s;
    s.count = rec.count();
    s.mean_us = rec.mean_ns() / 1000.0;
    s.p50_us = static_cast<double>(rec.p50_ns()) / 1000.0;
    s.p90_us = static_cast<double>(rec.p90_ns()) / 1000.0;
    s.p99_us = static_cast<double>(rec.p99_ns()) / 1000.0;
    s.p999_us = static_cast<double>(rec.p999_ns()) / 1000.0;
    s.max_us = static_cast<double>(rec.max_ns()) / 1000.0;
    s.max_gap_us = rec.max_gap_us();
    return s;
}

} // namespace nvme

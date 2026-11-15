/*
 * workload.cpp - continuous multi-queue I/O workload with integrity checking.
 *
 * Each worker thread owns one I/O queue pair and keeps `depth` commands
 * outstanding.  Every LBA always contains a pattern derived from its own
 * number, so a read can be verified no matter which thread (or which earlier
 * run) wrote it; concurrent writers of the same LBA write identical bytes.
 *
 * The workload also records the longest interval with zero completions across
 * all queues, which is the metric that exposes a controller quiesce window.
 */
#include "nvme/workload.hpp"

#include <algorithm>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <thread>

namespace nvme {

namespace {

uint64_t mix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

/*
 * Cheap but strict integrity pattern: a per-LBA base word followed by a
 * constant-stride sequence.  A single changed byte anywhere in the block
 * breaks the sequence, and verification costs one add/compare per 8 bytes
 * instead of a hash, so the measurement harness does not become the
 * bottleneck it is trying to measure.
 */
constexpr uint64_t kStride = 0x9E3779B97F4A7C15ull;
constexpr size_t kWordsPerBlock = NVME_LBA_SIZE / 8;

void fill_block(uint8_t* p, uint64_t lba, uint32_t seed) {
    uint64_t v = mix64(lba ^ (static_cast<uint64_t>(seed) << 32));
    for (size_t i = 0; i < kWordsPerBlock; ++i) {
        std::memcpy(p + i * 8, &v, 8);
        v += kStride;
    }
}

bool check_block(const uint8_t* p, uint64_t lba, uint32_t seed) {
    uint64_t v = mix64(lba ^ (static_cast<uint64_t>(seed) << 32));
    for (size_t i = 0; i < kWordsPerBlock; ++i) {
        uint64_t got;
        std::memcpy(&got, p + i * 8, 8);
        if (got != v) {
            return false;
        }
        v += kStride;
    }
    return true;
}

} // namespace

struct Workload::Worker {
    Workload* w = nullptr;
    unsigned qidx = 0;
    unsigned depth = 0;
    std::unique_ptr<DmaBufferPool> pool;
    std::vector<DmaBuffer*> bufs;
    std::vector<unsigned> free_slots;
    std::vector<uint64_t> slba;
    std::vector<uint64_t> submit_ns;
    std::vector<uint8_t> is_write;
    std::vector<int16_t> cid_slot;
    std::atomic<uint64_t> inflight{0};
    std::atomic<bool> parked{false};
    std::thread thread;
    uint32_t rng = 1;
    uint64_t submitted_local = 0;

    void run();
    bool submit_one(uint64_t deadline);
    void complete(const Completion& c, uint64_t now);
};

void Workload::Worker::complete(const Completion& c, uint64_t now) {
    if (c.cid >= cid_slot.size()) {
        return;
    }
    int slot = cid_slot[c.cid];
    cid_slot[c.cid] = -1;
    if (slot < 0 || static_cast<unsigned>(slot) >= depth) {
        return;
    }
    const unsigned s = static_cast<unsigned>(slot);
    uint64_t lat = (submit_ns[s] != 0 && now > submit_ns[s]) ? now - submit_ns[s] : 1;

    if (c.ok()) {
        w->completed_.fetch_add(1, std::memory_order_relaxed);
        if (!is_write[s]) {
            const uint8_t* p = static_cast<const uint8_t*>(bufs[s]->ptr);
            unsigned blocks = w->io_blocks_;
            for (unsigned b = 0; b < blocks; ++b) {
                if (!check_block(p + b * NVME_LBA_SIZE, slba[s] + b, w->cfg_.seed)) {
                    w->integrity_errors_.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
        }
    } else {
        w->failed_.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(w->status_mutex_);
        w->failed_by_status_[c.status]++;
    }

    uint64_t prev_completion = w->latency_.last_completion_ns();
    w->latency_.record_completion(lat, now);
    uint64_t t_act = w->activation_ns_.load(std::memory_order_acquire);
    if (t_act != 0) {
        uint64_t half = static_cast<uint64_t>(w->cfg_.window_half_ms * 1000000.0);
        if (now + half >= t_act && now <= t_act + half) {
            w->window_latency_.record_completion(lat, now);
        }
        if (now >= t_act && now <= t_act + 20000000ull && prev_completion != 0 &&
            now > prev_completion) {
            uint64_t gap = now - prev_completion;
            uint64_t best = w->activation_gap_ns_.load(std::memory_order_relaxed);
            while (gap > best && !w->activation_gap_ns_.compare_exchange_weak(best, gap)) {
            }
        }
    }

    free_slots.push_back(s);
    inflight.fetch_sub(1, std::memory_order_relaxed);
}

bool Workload::Worker::submit_one(uint64_t deadline) {
    if (free_slots.empty() && !bufs.empty()) {
        return false;
    }
    if (free_slots.empty()) {
        return false;
    }
    const unsigned slot = free_slots.back();
    free_slots.pop_back();

    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    bool write = (rng % 100u) >= static_cast<uint32_t>(w->cfg_.read_ratio * 100.0);
    uint64_t span = w->ns_lbas_ > w->io_blocks_ ? w->ns_lbas_ - w->io_blocks_ + 1 : 1;
    uint64_t lba = rng % span;
    lba &= ~static_cast<uint64_t>(1); /* 2-LBA aligned */
    if (lba + w->io_blocks_ > w->ns_lbas_) {
        lba = w->ns_lbas_ - w->io_blocks_;
    }

    DmaBuffer* buf = bufs[slot];
    if (write) {
        for (unsigned b = 0; b < w->io_blocks_; ++b) {
            fill_block(static_cast<uint8_t*>(buf->ptr) + b * NVME_LBA_SIZE, lba + b, w->cfg_.seed);
        }
    }

    CmdHandle h =
        write ? w->driver_.submit_write(qidx, lba, static_cast<uint16_t>(w->io_blocks_), *buf)
              : w->driver_.submit_read(qidx, lba, static_cast<uint16_t>(w->io_blocks_), *buf);
    if (!h.ok) {
        w->eagain_.fetch_add(1, std::memory_order_relaxed);
        free_slots.push_back(slot);
        return false;
    }
    cid_slot[h.cid] = static_cast<int16_t>(slot);
    slba[slot] = lba;
    submit_ns[slot] = h.submit_ns;
    is_write[slot] = write ? 1 : 0;
    inflight.fetch_add(1, std::memory_order_relaxed);
    w->submitted_.fetch_add(1, std::memory_order_relaxed);
    submitted_local++;
    (void)deadline;
    return true;
}

void Workload::Worker::run() {
    const uint64_t deadline =
        w->started_ns_ + static_cast<uint64_t>(w->cfg_.seconds) * 1000000000ull;
    unsigned idle = 0;

    for (;;) {
        bool paused = w->paused_.load(std::memory_order_acquire);
        if (paused && inflight.load(std::memory_order_relaxed) == 0) {
            /* Do not touch the rings while the controller is being reset. */
            parked.store(true, std::memory_order_release);
            while (w->paused_.load(std::memory_order_acquire) &&
                   !w->stop_.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            parked.store(false, std::memory_order_release);
            if (w->stop_.load(std::memory_order_acquire)) {
                break;
            }
            continue;
        }
        parked.store(false, std::memory_order_release);

        Completion cqe[32];
        int n = w->driver_.poll_batch(qidx, cqe, 32);
        uint64_t now = now_ns();
        for (int i = 0; i < n; ++i) {
            complete(cqe[i], now);
        }

        bool stop = w->stop_.load(std::memory_order_acquire) || now >= deadline;
        paused = w->paused_.load(std::memory_order_acquire);

        if (!stop && !paused && inflight.load(std::memory_order_relaxed) < depth) {
            while (!free_slots.empty()) {
                if (!submit_one(deadline)) {
                    break;
                }
                if (inflight.load(std::memory_order_relaxed) >= depth) {
                    break;
                }
            }
        } else if (stop && inflight.load(std::memory_order_relaxed) == 0) {
            break;
        }

        if (n == 0) {
            if (++idle < 64) {
                /* short spin */
            } else {
                idle = 0;
                std::this_thread::yield();
            }
        } else {
            idle = 0;
        }
    }

    /* Anything still outstanding when the run ends is reported as dropped. */
    w->left_in_flight_.fetch_add(inflight.load(std::memory_order_relaxed));
}

Workload::Workload(NvmeDriver& driver, const WorkloadConfig& cfg)
    : driver_(driver), cfg_(cfg), latency_(cfg.seed), window_latency_(cfg.seed ^ 0xA5A5u) {
    IdentifyNs ns = driver_.identify_ns(1);
    if (ns.nsze < 4) {
        throw std::runtime_error("namespace is too small for a workload");
    }
    ns_lbas_ = ns.nsze;
    io_blocks_ = static_cast<uint16_t>(std::max(1u, std::min(cfg_.io_blocks, 2u)));
    if (ns_lbas_ < io_blocks_ + 2) {
        throw std::runtime_error("namespace is too small for the requested I/O size");
    }
    if (cfg_.queues == 0 || cfg_.queues > driver_.io_queue_count()) {
        throw std::invalid_argument("workload queues exceed the created I/O queues");
    }
    if (cfg_.depth < 2 || cfg_.depth > 4096) {
        throw std::invalid_argument("workload depth must be 2..4096");
    }
}

Workload::~Workload() {
    try {
        stop();
    } catch (...) {
    }
}

void Workload::fill_buffer(DmaBuffer* buf, uint64_t slba) const {
    for (unsigned b = 0; b < io_blocks_; ++b) {
        fill_block(static_cast<uint8_t*>(buf->ptr) + b * NVME_LBA_SIZE, slba + b, cfg_.seed);
    }
}

bool Workload::verify_buffer(const DmaBuffer* buf, uint64_t slba) const {
    for (unsigned b = 0; b < io_blocks_; ++b) {
        if (!check_block(static_cast<const uint8_t*>(buf->ptr) + b * NVME_LBA_SIZE, slba + b,
                         cfg_.seed)) {
            return false;
        }
    }
    return true;
}

void Workload::prefill() {
    const size_t kBatch = 16;
    DmaBufferPool pool(driver_.backend(), 2 * NVME_LBA_SIZE, kBatch);
    std::deque<DmaBuffer*> pending;
    std::vector<DmaBuffer*> idle;
    idle.reserve(kBatch);
    for (size_t i = 0; i < kBatch; ++i) {
        DmaBuffer* b = pool.acquire();
        if (b != nullptr) {
            idle.push_back(b);
        }
    }
    if (idle.empty()) {
        throw std::runtime_error("unable to allocate prefill buffers");
    }
    const size_t max_outstanding =
        std::max<size_t>(2, std::min<size_t>(kBatch, driver_.io_depth()));

    uint64_t lba = 0;
    while (lba < ns_lbas_ || !pending.empty()) {
        bool submitted = false;
        while (!idle.empty() && lba < ns_lbas_ && pending.size() < max_outstanding) {
            DmaBuffer* buf = idle.back();
            idle.pop_back();
            unsigned blocks = static_cast<unsigned>(std::min<uint64_t>(io_blocks_, ns_lbas_ - lba));
            fill_buffer(buf, lba);
            CmdHandle h = driver_.submit_write(0, lba, static_cast<uint16_t>(blocks), *buf);
            if (!h.ok) {
                idle.push_back(buf);
                break;
            }
            pending.push_back(buf);
            lba += blocks;
            submitted = true;
        }

        if (!pending.empty() &&
            (pending.size() >= max_outstanding || !submitted || lba >= ns_lbas_)) {
            auto c = driver_.poll(0);
            if (!c) {
                std::this_thread::yield();
                continue;
            }
            if (!c->ok()) {
                throw std::runtime_error("prefill write failed");
            }
            idle.push_back(pending.front());
            pending.pop_front();
        }
    }

    /* Verify the first and last block pair. */
    DmaBuffer* buf = idle.empty() ? nullptr : idle.back();
    if (buf != nullptr) {
        uint64_t first = 0;
        uint64_t last = ns_lbas_ - io_blocks_;
        for (uint64_t sample : {first, last}) {
            for (int attempt = 0; attempt < 2; ++attempt) {
                CmdHandle h = driver_.submit_read(0, sample, io_blocks_, *buf);
                if (!h.ok) {
                    throw std::runtime_error("prefill verification submit failed");
                }
                for (;;) {
                    auto c = driver_.poll(0);
                    if (c) {
                        if (!c->ok()) {
                            throw std::runtime_error("prefill verification read failed");
                        }
                        break;
                    }
                    std::this_thread::yield();
                }
                if (verify_buffer(buf, sample)) {
                    break;
                }
                if (attempt == 1) {
                    integrity_errors_.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        idle.push_back(buf);
    }
    for (auto* b : idle) {
        pool.release(b);
    }
}

void Workload::start() {
    if (!workers_.empty()) {
        return;
    }
    stopped_ns_ = 0;
    stop_.store(false);
    paused_.store(false);
    latency_.reset();
    window_latency_.reset();
    started_ns_ = now_ns();

    for (unsigned i = 0; i < cfg_.queues; ++i) {
        auto worker = std::make_unique<Worker>();
        worker->w = this;
        worker->qidx = i;
        worker->depth = cfg_.depth;
        worker->rng = 0x9E3779B9u * (i + 1u) + cfg_.seed;
        worker->pool =
            std::make_unique<DmaBufferPool>(driver_.backend(), 2 * NVME_LBA_SIZE, cfg_.depth);
        worker->bufs.reserve(cfg_.depth);
        for (unsigned s = 0; s < cfg_.depth; ++s) {
            DmaBuffer* buf = worker->pool->acquire();
            if (buf == nullptr) {
                throw std::runtime_error("unable to allocate workload buffers");
            }
            worker->bufs.push_back(buf);
            worker->free_slots.push_back(s);
        }
        std::reverse(worker->free_slots.begin(), worker->free_slots.end());
        worker->slba.assign(cfg_.depth, 0);
        worker->submit_ns.assign(cfg_.depth, 0);
        worker->is_write.assign(cfg_.depth, 0);
        worker->cid_slot.assign(65536, -1);
        workers_.push_back(std::move(worker));
    }

    for (auto& w : workers_) {
        Worker* raw = w.get();
        raw->thread = std::thread([raw] { raw->run(); });
    }
}

void Workload::pause() {
    paused_.store(true, std::memory_order_release);
    const uint64_t deadline = now_ns() + 10000000000ull;
    for (;;) {
        uint64_t total = 0;
        bool all_parked = true;
        for (auto& w : workers_) {
            total += w->inflight.load(std::memory_order_relaxed);
            if (!w->parked.load(std::memory_order_acquire)) {
                all_parked = false;
            }
        }
        if (total == 0 && all_parked) {
            return;
        }
        if (now_ns() > deadline) {
            throw std::runtime_error("timed out draining in-flight I/O for pause");
        }
        std::this_thread::yield();
    }
}

void Workload::resume() {
    paused_.store(false, std::memory_order_release);
}

void Workload::stop() {
    if (workers_.empty()) {
        return;
    }
    stop_.store(true, std::memory_order_release);
    for (auto& w : workers_) {
        if (w->thread.joinable()) {
            w->thread.join();
        }
    }
    stopped_ns_ = now_ns();
}

void Workload::set_activation_time(uint64_t ns) {
    uint64_t half = static_cast<uint64_t>(cfg_.window_half_ms * 1000000.0);
    activation_ns_.store(ns, std::memory_order_release);
    window_start_ns_ = ns > half ? ns - half : 0;
    window_end_ns_ = ns + half;
}

WorkloadStats Workload::stats() const {
    WorkloadStats s;
    s.submitted = submitted_.load();
    s.completed = completed_.load();
    s.failed = failed_.load();
    s.eagain_stalls = eagain_.load();
    s.integrity_errors = integrity_errors_.load();
    s.dropped = left_in_flight_.load();
    uint64_t end = stopped_ns_ != 0 ? stopped_ns_ : now_ns();
    s.duration_s = started_ns_ != 0 ? static_cast<double>(end - started_ns_) / 1e9 : 0.0;
    s.iops = s.duration_s > 0 ? static_cast<double>(s.completed) / s.duration_s : 0.0;
    s.max_gap_us = latency_.max_gap_us();
    s.activation_gap_us =
        static_cast<double>(activation_gap_ns_.load(std::memory_order_relaxed)) / 1000.0;
    s.steady = summarize(latency_);
    s.window = summarize(window_latency_);
    s.window_start_ns = window_start_ns_;
    s.window_end_ns = window_end_ns_;
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        s.failed_by_status = failed_by_status_;
    }
    return s;
}

bool Workload::integrity_ok() const {
    return integrity_errors_.load(std::memory_order_relaxed) == 0;
}

std::vector<double> Workload::latency_samples_us() const {
    std::vector<uint64_t> raw = latency_.reservoir();
    std::vector<double> out;
    out.reserve(raw.size());
    for (uint64_t v : raw) {
        out.push_back(static_cast<double>(v) / 1000.0);
    }
    return out;
}

std::vector<double> Workload::window_samples_us() const {
    std::vector<uint64_t> raw = window_latency_.reservoir();
    std::vector<double> out;
    out.reserve(raw.size());
    for (uint64_t v : raw) {
        out.push_back(static_cast<double>(v) / 1000.0);
    }
    return out;
}

} // namespace nvme

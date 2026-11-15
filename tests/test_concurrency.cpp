#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <thread>
#include <vector>

#include "nvme/backend.hpp"
#include "nvme/driver.hpp"

namespace nvme {
namespace {

constexpr uint64_t kStride = 0x9E3779B97F4A7C15ull;

uint64_t mix64(uint64_t x)
{
    x += kStride;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

void fill_block(uint8_t *p, uint64_t lba, uint64_t queue)
{
    uint64_t v = mix64(lba ^ (queue << 48));
    for (size_t i = 0; i < NVME_LBA_SIZE / 8; ++i) {
        std::memcpy(p + i * 8, &v, 8);
        v += kStride;
    }
}

bool check_block(const uint8_t *p, uint64_t lba, uint64_t queue)
{
    uint64_t v = mix64(lba ^ (queue << 48));
    for (size_t i = 0; i < NVME_LBA_SIZE / 8; ++i) {
        uint64_t got;
        std::memcpy(&got, p + i * 8, 8);
        if (got != v) {
            return false;
        }
        v += kStride;
    }
    return true;
}

struct Totals {
    std::atomic<uint64_t> completed{0};
    std::atomic<uint64_t> failed{0};
    std::atomic<uint64_t> integrity{0};
    std::atomic<uint64_t> stalls{0};
};

} // namespace

TEST(Concurrency, FourQueuesTwentyThousandCommandsNoLoss)
{
    nvme_dev_cfg cfg;
    nvme_dev_cfg_default(&cfg);
    cfg.ns_lba_count = 16384;
    cfg.max_qid = 4;
    cfg.enable_delay_us = 300;
    cfg.io_latency_base_ns = 200;
    cfg.io_latency_jitter_ns = 200;

    SimBackend backend(cfg);
    NvmeDriver driver(backend);
    driver.reset_and_enable();
    driver.create_io_queues(4, 32);

    constexpr int kOpsPerThread = 20000;
    constexpr int kPrefillOps = 128;
    constexpr size_t kQd = 16;
    Totals totals;

    std::vector<std::thread> threads;
    for (unsigned t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            struct Pending {
                DmaBuffer *buf;
                uint64_t lba;
                bool write;
            };
            DmaBufferPool pool(backend, 2 * NVME_LBA_SIZE, kQd);
            std::deque<Pending> pending;
            std::vector<DmaBuffer *> idle;
            for (size_t i = 0; i < kQd; ++i) {
                DmaBuffer *b = pool.acquire();
                if (b != nullptr) {
                    idle.push_back(b);
                }
            }
            const uint64_t base = static_cast<uint64_t>(t) * 2048ull;
            uint32_t rng = 0x1234u + t * 7919u;

            auto reap_one = [&]() {
                for (;;) {
                    auto c = driver.poll(t);
                    if (!c) {
                        std::this_thread::yield();
                        continue;
                    }
                    Pending p = pending.front();
                    pending.pop_front();
                    if (!c->ok()) {
                        totals.failed.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        totals.completed.fetch_add(1, std::memory_order_relaxed);
                        if (!p.write) {
                            const auto *bytes = static_cast<const uint8_t *>(p.buf->ptr);
                            if (!check_block(bytes, p.lba, t) ||
                                !check_block(bytes + NVME_LBA_SIZE, p.lba + 1, t)) {
                                totals.integrity.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                    }
                    idle.push_back(p.buf);
                    return;
                }
            };

            uint64_t prefill_pos = 0;
            uint64_t *prefill_cursor = nullptr;
            auto issue = [&](bool write) -> bool {
                if (idle.empty()) {
                    return false;
                }
                DmaBuffer *buf = idle.back();
                uint64_t lba;
                if (prefill_cursor != nullptr) {
                    lba = base + *prefill_cursor;
                    *prefill_cursor += 2;
                } else {
                    lba = base + (rng % 250u);
                    lba &= ~1ull;
                }
                if (write) {
                    fill_block(static_cast<uint8_t *>(buf->ptr), lba, t);
                    fill_block(static_cast<uint8_t *>(buf->ptr) + NVME_LBA_SIZE, lba + 1, t);
                }
                CmdHandle h = write ? driver.submit_write(t, lba, 2, *buf)
                                    : driver.submit_read(t, lba, 2, *buf);
                if (!h.ok) {
                    totals.stalls.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }
                idle.pop_back();
                pending.push_back(Pending{buf, lba, write});
                return true;
            };

            /* Prefill this thread's whole slice with its own pattern. */
            prefill_cursor = &prefill_pos;
            for (int i = 0; i < kPrefillOps; ++i) {
                if (!issue(true)) {
                    if (pending.empty()) {
                        ADD_FAILURE() << "prefill could not make progress";
                        return;
                    }
                    reap_one();
                    --i;
                }
                if (pending.size() >= kQd) {
                    reap_one();
                }
            }
            prefill_cursor = nullptr;
            while (!pending.empty()) {
                reap_one();
            }

            /* Mixed load.  Reads are verified against the deterministic
             * pattern that every write of that LBA produces. */
            for (int i = 0; i < kOpsPerThread; ++i) {
                rng = rng * 1664525u + 1013904223u;
                bool write = (rng % 100u) < 40u;
                if (!issue(write)) {
                    if (pending.empty()) {
                        ADD_FAILURE() << "mixed load could not make progress";
                        return;
                    }
                    reap_one();
                    --i;
                    continue;
                }
                if (pending.size() >= kQd) {
                    reap_one();
                }
            }
            while (!pending.empty()) {
                reap_one();
            }
            for (auto *b : idle) {
                pool.release(b);
            }
        });
    }
    for (auto &th : threads) {
        th.join();
    }

    const uint64_t expected = 4ull * (kOpsPerThread + kPrefillOps);
    EXPECT_EQ(0u, totals.failed.load());
    EXPECT_EQ(0u, totals.integrity.load());
    EXPECT_EQ(0u, totals.stalls.load());
    EXPECT_EQ(expected, totals.completed.load());
    for (unsigned q = 0; q < 4; ++q) {
        EXPECT_EQ(0u, nvme_qp_inflight(&driver.queue(q).qp)) << "queue " << q;
    }
}

} // namespace nvme

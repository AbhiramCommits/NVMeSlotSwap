/*
 * main.cpp - NVMeSlotSwap command line interface.
 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

#include "nvme/backend.hpp"
#include "nvme/device.h"
#include "nvme/driver.hpp"
#include "nvme/fw.h"

namespace {

void print_usage(FILE *out)
{
    std::fprintf(out,
                 "NVMeSlotSwap - non-disruptive dual-image NVMe firmware activation\n"
                 "\n"
                 "Usage: nvmeslotswap <command> [options]\n"
                 "\n"
                 "Commands:\n"
                 "  info                 Show the emulated controller summary\n"
                 "  ident                Identify Controller / Identify Namespace\n"
                 "  io [options]         Run a smoke I/O workload\n"
                 "                         --queues N --depth N --seconds N --rw PCT\n"
                 "  fw list              Read the Firmware Slot Information log (0x03)\n"
                 "  fw stage             Download a firmware image in 4 KiB chunks\n"
                 "                         --slot N --rev STR --size BYTES\n"
                 "  fw commit            Commit a staged image\n"
                 "                         --slot N --action 0|1|2|3 [--fail-commit]\n"
                 "  swap-test [options]  Continuous I/O across activation, JSON result\n"
                 "                         --queues N --depth N --warmup-ms N --post-ms N\n"
                 "                         --action 0|1|2|3|none --rw PCT\n"
                 "                         [--json PATH] [--fail-commit]\n"
                 "                         [--slot N] [--rev STR] [--size BYTES]\n"
                 "  --help               Show this help\n");
}

struct Options {
    unsigned queues = 4;
    unsigned depth = 32;
    unsigned seconds = 3;
    unsigned rw = 70;
    unsigned warmup_ms = 1000;
    unsigned post_ms = 1000;
    int action = -1; /* -1 => no activation */
    unsigned slot = 2;
    std::string rev = "FW2.0.1";
    size_t size = 1024 * 1024;
    std::string json_path;
    std::string state_path;
    bool fail_commit = false;
    bool reset_after_commit = false;
};

bool parse_opts(int argc, char **argv, Options &o, int start)
{
    for (int i = start; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char *name) -> const char * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "nvmeslotswap: %s needs a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--queues") {
            const char *v = need("--queues");
            if (!v) return false;
            o.queues = static_cast<unsigned>(std::strtoul(v, nullptr, 10));
        } else if (a == "--depth") {
            const char *v = need("--depth");
            if (!v) return false;
            o.depth = static_cast<unsigned>(std::strtoul(v, nullptr, 10));
        } else if (a == "--seconds") {
            const char *v = need("--seconds");
            if (!v) return false;
            o.seconds = static_cast<unsigned>(std::strtoul(v, nullptr, 10));
        } else if (a == "--rw") {
            const char *v = need("--rw");
            if (!v) return false;
            o.rw = static_cast<unsigned>(std::strtoul(v, nullptr, 10));
        } else if (a == "--warmup-ms") {
            const char *v = need("--warmup-ms");
            if (!v) return false;
            o.warmup_ms = static_cast<unsigned>(std::strtoul(v, nullptr, 10));
        } else if (a == "--post-ms") {
            const char *v = need("--post-ms");
            if (!v) return false;
            o.post_ms = static_cast<unsigned>(std::strtoul(v, nullptr, 10));
        } else if (a == "--action") {
            const char *v = need("--action");
            if (!v) return false;
            if (std::strcmp(v, "none") == 0) {
                o.action = -1;
            } else {
                o.action = static_cast<int>(std::strtol(v, nullptr, 10));
            }
        } else if (a == "--slot") {
            const char *v = need("--slot");
            if (!v) return false;
            o.slot = static_cast<unsigned>(std::strtoul(v, nullptr, 10));
        } else if (a == "--rev") {
            const char *v = need("--rev");
            if (!v) return false;
            o.rev = v;
        } else if (a == "--size") {
            const char *v = need("--size");
            if (!v) return false;
            o.size = static_cast<size_t>(std::strtoull(v, nullptr, 10));
        } else if (a == "--json") {
            const char *v = need("--json");
            if (!v) return false;
            o.json_path = v;
        } else if (a == "--state") {
            const char *v = need("--state");
            if (!v) return false;
            o.state_path = v;
        } else if (a == "--fail-commit") {
            o.fail_commit = true;
        } else if (a == "--reset") {
            o.reset_after_commit = true;
        } else {
            std::fprintf(stderr, "nvmeslotswap: unknown option %s\n", a.c_str());
            return false;
        }
    }
    return true;
}

nvme_dev_cfg default_cfg()
{
    nvme_dev_cfg cfg;
    nvme_dev_cfg_default(&cfg);
    return cfg;
}

int cmd_info()
{
    nvme::SimBackend backend(default_cfg());
    nvme::NvmeDriver drv(backend);
    drv.reset_and_enable();
    nvme::IdentifyCtrl id = drv.identify_ctrl();
    std::printf("model: %s\n", id.model.c_str());
    std::printf("serial: %s\n", id.serial.c_str());
    std::printf("firmware: %s\n", id.firmware.c_str());
    std::printf("namespaces: %u\n", id.nn);
    return 0;
}

int cmd_ident()
{
    nvme::SimBackend backend(default_cfg());
    nvme::NvmeDriver drv(backend);
    drv.reset_and_enable();
    nvme::IdentifyCtrl id = drv.identify_ctrl();
    nvme::IdentifyNs ns = drv.identify_ns(1);
    std::printf("model:      %s\n", id.model.c_str());
    std::printf("serial:     %s\n", id.serial.c_str());
    std::printf("firmware:   %s\n", id.firmware.c_str());
    std::printf("vendor:     0x%04x subsystem: 0x%04x\n", id.vid, id.ssvid);
    std::printf("namespace:  1, %llu LBAs x %llu bytes (%llu MiB)\n",
                static_cast<unsigned long long>(ns.nsze),
                static_cast<unsigned long long>(ns.lba_size),
                static_cast<unsigned long long>(ns.nsze * ns.lba_size / (1024 * 1024)));
    std::printf("lba format: lbads=%u (%llu-byte LBAs)\n", ns.lbads,
                static_cast<unsigned long long>(ns.lba_size));
    return 0;
}

/* ------------------------------------------------------------------ */
/* Firmware update commands                                            */
/* ------------------------------------------------------------------ */
std::string state_path(const Options &o)
{
    return o.state_path.empty() ? std::string(".nvmess/fw.state") : o.state_path;
}

void ensure_parent_dir(const std::string &path)
{
    size_t pos = path.find_last_of('/');
    if (pos == std::string::npos || pos == 0) {
        return;
    }
    std::string dir = path.substr(0, pos);
    (void)mkdir(dir.c_str(), 0755);
}

void fill_ns_pattern(uint8_t *p, size_t len, uint64_t lba)
{
    for (size_t i = 0; i + 8 <= len; i += 8) {
        uint64_t v = 0x4E564D45534C4F54ull ^ (lba * 0x9E3779B97F4A7C15ull) ^
                     (static_cast<uint64_t>(i / 8) * 0xBF58476D1CE4E5B9ull);
        std::memcpy(p + i, &v, 8);
    }
}

bool wait_one(nvme::NvmeDriver &drv, unsigned q, unsigned *sc)
{
    uint64_t deadline = nvme::now_ns() + 2000000000ull;
    while (nvme::now_ns() < deadline) {
        auto c = drv.poll(q);
        if (c) {
            *sc = c->sc;
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

/* Writes a deterministic pattern to two LBAs before activation and reads it
 * back afterwards, proving the namespace stayed readable across the commit. */
bool namespace_roundtrip(nvme::NvmeDriver &drv, bool write_phase, uint8_t *wbuf, uint8_t *rbuf,
                         const nvme::DmaBuffer &wa, const nvme::DmaBuffer &ra)
{
    unsigned sc = 0;
    if (write_phase) {
        fill_ns_pattern(wbuf, 2 * NVME_LBA_SIZE, 16);
        std::memcpy(wa.ptr, wbuf, 2 * NVME_LBA_SIZE);
        nvme::CmdHandle h = drv.submit_write(0, 16, 2, wa);
        return h.ok && wait_one(drv, 0, &sc) && sc == 0;
    }
    nvme::CmdHandle h = drv.submit_read(0, 16, 2, ra);
    if (!h.ok || !wait_one(drv, 0, &sc) || sc != 0) {
        return false;
    }
    std::memcpy(rbuf, ra.ptr, 2 * NVME_LBA_SIZE);
    uint8_t expect[2 * NVME_LBA_SIZE];
    fill_ns_pattern(expect, sizeof(expect), 16);
    return std::memcmp(expect, rbuf, sizeof(expect)) == 0;
}

void print_slots(const nvme::FwSlotInfo &info)
{
    std::printf("active slot: %u   next activation slot: %u\n", info.active_slot,
                info.next_slot);
    for (unsigned i = 0; i < 7; ++i) {
        std::string rev = info.revisions[i];
        bool active = (info.active_slot == i + 1);
        bool next = (info.next_slot == i + 1);
        std::printf("  slot %u: ", i + 1);
        if (rev.empty()) {
            std::printf("(empty)");
        } else {
            std::printf("%s", rev.c_str());
        }
        if (active) {
            std::printf("  [active]");
        }
        if (next && !active) {
            std::printf("  [next]");
        }
        if (i == 0) {
            std::printf("  [read-only]");
        }
        std::printf("\n");
    }
}

const char *commit_action_name(int action)
{
    switch (action) {
    case 0:
        return "CA=0 replace slot, no activation";
    case 1:
        return "CA=1 replace slot, activate on next reset";
    case 2:
        return "CA=2 activate existing slot on next reset";
    case 3:
        return "CA=3 replace slot and activate immediately";
    default:
        return "unknown";
    }
}

int cmd_fw(int argc, char **argv, const Options &o)
{
    if (argc < 3) {
        std::fprintf(stderr, "nvmeslotswap: fw needs one of: list, stage, commit\n");
        return 2;
    }
    std::string sub = argv[2];
    std::string path = state_path(o);

    nvme::SimBackend backend(default_cfg());
    nvme::NvmeDriver drv(backend);
    drv.reset_and_enable();
    bool loaded = drv.load_fw_state(path);
    nvme::FirmwareUpdater upd(drv);

    if (sub == "list") {
        print_slots(upd.read_slot_log());
        std::printf("emulated device state: %s%s\n", path.c_str(),
                    loaded ? "" : " (not found; fresh controller)");
        return 0;
    }

    if (sub == "stage") {
        if (o.slot < 1 || o.slot > 7) {
            std::fprintf(stderr, "nvmeslotswap: --slot must be 1..7\n");
            return 2;
        }
        std::vector<uint8_t> image =
            nvme::FirmwareUpdater::build_image(o.rev, o.size, 0x5EEDu + o.slot);
        nvme::StageResult r = upd.stage(o.slot, image, 4096);
        if (!r.ok()) {
            std::fprintf(stderr, "stage failed with status 0x%02x (image is %zu bytes)\n",
                         r.status, r.bytes);
            return 1;
        }
        ensure_parent_dir(path);
        drv.save_fw_state(path);
        std::printf("staged %zu bytes in %zu x 4096-byte chunks for slot %u (rev %s) in %llu us\n",
                    r.bytes, r.chunks, o.slot, o.rev.c_str(),
                    static_cast<unsigned long long>(r.elapsed_us));
        print_slots(upd.read_slot_log());
        std::printf("emulated device state: %s\n", path.c_str());
        return 0;
    }

    if (sub == "commit") {
        if (o.slot < 1 || o.slot > 7 || o.action < 0 || o.action > 3) {
            std::fprintf(stderr, "nvmeslotswap: commit needs --slot 1..7 and --action 0..3\n");
            return 2;
        }
        nvme::CommitAction action = static_cast<nvme::CommitAction>(o.action);
        drv.set_self_test_fail(o.fail_commit);

        /* Establish I/O and write a known pattern before the commit. */
        drv.create_io_queues(1, 8);
        nvme::DmaBufferPool pool(backend, 2 * NVME_LBA_SIZE, 2);
        nvme::DmaBuffer *wa = pool.acquire();
        nvme::DmaBuffer *ra = pool.acquire();
        std::vector<uint8_t> wbuf(2 * NVME_LBA_SIZE), rbuf(2 * NVME_LBA_SIZE);
        bool wrote = namespace_roundtrip(drv, true, wbuf.data(), rbuf.data(), *wa, *ra);

        uint64_t resets_before = nvme_dev_reset_count(backend.device());
        nvme::CommitResult r = upd.commit(o.slot, action);
        uint64_t resets_after = nvme_dev_reset_count(backend.device());
        std::printf("commit: %s\n", commit_action_name(o.action));
        std::printf("status: 0x%02x   active slot: %u (%s)   next slot: %u   latency: %llu us\n",
                    r.status, r.active_slot, r.active_revision.c_str(), r.next_slot,
                    static_cast<unsigned long long>(r.latency_us));

        std::printf("controller resets during commit: %llu\n",
                    static_cast<unsigned long long>(resets_after - resets_before));
        if (r.requires_reset) {
            std::printf("activation pending: the new slot takes effect on the next reset\n");
            if (o.reset_after_commit) {
                drv.controller_reset();
                drv.reset_and_enable();
                drv.recreate_io_queues();
                nvme::FwSlotInfo info = upd.read_slot_log();
                std::printf("after reset: active slot %u (%s)\n", info.active_slot,
                            info.revisions[info.active_slot - 1].c_str());
            }
        }
        if (r.rolled_back) {
            std::printf("ROLLBACK: post-activation self test failed; restored slot %u (%s)\n",
                        r.active_slot, r.active_revision.c_str());
        } else if (r.status == 0 && (o.action == 3 || o.reset_after_commit)) {
            bool verified = upd.verify_activation(r.active_revision);
            std::printf("activation verified via log page 0x03: %s\n", verified ? "yes" : "no");
        }

        if (!r.requires_reset || o.reset_after_commit) {
            bool intact = namespace_roundtrip(drv, false, wbuf.data(), rbuf.data(), *wa, *ra);
            std::printf("namespace check: %s%s\n", intact ? "OK" : "FAILED",
                        wrote ? " (2 LBAs written before commit read back intact)" : "");
        }
        ensure_parent_dir(path);
        drv.save_fw_state(path);
        print_slots(upd.read_slot_log());
        std::printf("emulated device state: %s\n", path.c_str());
        return (r.status == 0 || r.rolled_back || r.requires_reset) ? 0 : 1;
    }

    std::fprintf(stderr, "nvmeslotswap: unknown fw subcommand '%s'\n", sub.c_str());
    return 2;
}

struct SmokeCtx {
    nvme::NvmeDriver *drv = nullptr;
    nvme::DmaBufferPool *pool = nullptr;
    unsigned queue = 0;
    unsigned depth = 32;
    double read_ratio = 0.7;
    uint64_t deadline_ns = 0;
    uint64_t ns_lbas = 0;
    unsigned seed = 1;
    std::atomic<uint64_t> *submitted = nullptr;
    std::atomic<uint64_t> *completed = nullptr;
    std::atomic<uint64_t> *failed = nullptr;
    std::atomic<uint64_t> *stalls = nullptr;
    std::atomic<bool> *panic = nullptr;
};

void smoke_worker(SmokeCtx ctx)
{
    std::vector<nvme::DmaBuffer *> idle;
    std::deque<nvme::DmaBuffer *> pending;
    idle.reserve(ctx.depth);
    for (unsigned i = 0; i < ctx.depth; ++i) {
        nvme::DmaBuffer *buf = ctx.pool->acquire();
        if (buf == nullptr) {
            break;
        }
        idle.push_back(buf);
    }
    uint32_t rng = 0x1234u + ctx.seed;
    const uint64_t io_bytes = 2ull * NVME_LBA_SIZE;

    for (;;) {
        nvme::Completion cqe[32];
        int n = ctx.drv->poll_batch(ctx.queue, cqe, 32);
        for (int i = 0; i < n; ++i) {
            if (cqe[i].ok()) {
                ctx.completed->fetch_add(1, std::memory_order_relaxed);
            } else {
                ctx.failed->fetch_add(1, std::memory_order_relaxed);
            }
            if (!pending.empty()) {
                idle.push_back(pending.front());
                pending.pop_front();
            }
        }

        uint64_t now = nvme::now_ns();
        if (now < ctx.deadline_ns) {
            while (!idle.empty() && pending.size() < ctx.depth) {
                nvme::DmaBuffer *buf = idle.back();
                idle.pop_back();
                rng ^= rng << 13;
                rng ^= rng >> 17;
                rng ^= rng << 5;
                bool write = (rng % 100u) >= static_cast<uint32_t>(ctx.read_ratio * 100.0);
                uint64_t slba =
                    ctx.ns_lbas > 2 ? (rng % static_cast<uint32_t>(ctx.ns_lbas - 2)) : 0;
                if (write) {
                    std::memset(buf->ptr, static_cast<int>(rng & 0xFF), io_bytes);
                }
                nvme::CmdHandle h = write
                                        ? ctx.drv->submit_write(ctx.queue, slba, 2, *buf)
                                        : ctx.drv->submit_read(ctx.queue, slba, 2, *buf);
                if (!h.ok) {
                    ctx.stalls->fetch_add(1, std::memory_order_relaxed);
                    idle.push_back(buf);
                    break;
                }
                ctx.submitted->fetch_add(1, std::memory_order_relaxed);
                pending.push_back(buf);
            }
        } else if (pending.empty()) {
            break;
        }
        if (n == 0) {
            std::this_thread::yield();
        }
    }

    /* Drain the queue before returning the buffers. */
    uint64_t drain_deadline = nvme::now_ns() + 2000000000ull;
    while (!pending.empty() && nvme::now_ns() < drain_deadline) {
        nvme::Completion cqe[32];
        int n = ctx.drv->poll_batch(ctx.queue, cqe, 32);
        for (int i = 0; i < n; ++i) {
            if (cqe[i].ok()) {
                ctx.completed->fetch_add(1, std::memory_order_relaxed);
            } else {
                ctx.failed->fetch_add(1, std::memory_order_relaxed);
            }
            if (!pending.empty()) {
                idle.push_back(pending.front());
                pending.pop_front();
            }
        }
        if (n == 0) {
            std::this_thread::yield();
        }
    }
    for (auto *buf : idle) {
        ctx.pool->release(buf);
    }
}

int cmd_io(const Options &o)
{
    nvme_dev_cfg cfg = default_cfg();
    nvme::SimBackend backend(cfg);
    nvme::NvmeDriver drv(backend);
    drv.reset_and_enable();
    nvme::IdentifyNs ns = drv.identify_ns(1);
    drv.create_io_queues(o.queues, static_cast<uint16_t>(o.depth));

    std::vector<std::unique_ptr<nvme::DmaBufferPool>> pools;
    pools.reserve(o.queues);
    for (unsigned i = 0; i < o.queues; ++i) {
        pools.push_back(std::make_unique<nvme::DmaBufferPool>(
            backend, 2 * NVME_LBA_SIZE, o.depth));
    }

    std::atomic<uint64_t> submitted{0}, completed{0}, failed{0}, stalls{0};
    std::atomic<bool> panic{false};
    uint64_t start = nvme::now_ns();
    uint64_t deadline = start + static_cast<uint64_t>(o.seconds) * 1000000000ull;

    std::vector<std::thread> threads;
    std::vector<std::unique_ptr<SmokeCtx>> ctxs;
    ctxs.reserve(o.queues);
    for (unsigned i = 0; i < o.queues; ++i) {
        auto ctx = std::make_unique<SmokeCtx>();
        ctx->drv = &drv;
        ctx->pool = pools[i].get();
        ctx->queue = i;
        ctx->depth = o.depth;
        ctx->read_ratio = o.rw / 100.0;
        ctx->deadline_ns = deadline;
        ctx->ns_lbas = ns.nsze;
        ctx->seed = i + 1;
        ctx->submitted = &submitted;
        ctx->completed = &completed;
        ctx->failed = &failed;
        ctx->stalls = &stalls;
        ctx->panic = &panic;
        ctxs.push_back(std::move(ctx));
    }
    for (unsigned i = 0; i < o.queues; ++i) {
        SmokeCtx *ctx = ctxs[i].get();
        threads.emplace_back([ctx] { smoke_worker(*ctx); });
    }
    for (auto &t : threads) {
        t.join();
    }
    uint64_t elapsed = nvme::now_ns() - start;
    double secs = static_cast<double>(elapsed) / 1e9;
    uint64_t comp = completed.load();
    std::printf("io: queues=%u depth=%u seconds=%.2f rw=%u\n", o.queues, o.depth, secs, o.rw);
    std::printf("iops: %.0f\n", secs > 0 ? static_cast<double>(comp) / secs : 0.0);
    std::printf("submitted: %llu completed: %llu failed: %llu eagain_stalls: %llu\n",
                static_cast<unsigned long long>(submitted.load()),
                static_cast<unsigned long long>(comp),
                static_cast<unsigned long long>(failed.load()),
                static_cast<unsigned long long>(stalls.load()));
    return failed.load() == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        print_usage(stderr);
        return 2;
    }
    const char *cmd = argv[1];
    if (std::strcmp(cmd, "--help") == 0 || std::strcmp(cmd, "-h") == 0 ||
        std::strcmp(cmd, "help") == 0) {
        print_usage(stdout);
        return 0;
    }

    Options opts;
    int opt_start = (std::strcmp(cmd, "fw") == 0) ? 3 : 2;
    if (!parse_opts(argc, argv, opts, opt_start)) {
        return 2;
    }

    try {
        if (std::strcmp(cmd, "info") == 0) {
            return cmd_info();
        }
        if (std::strcmp(cmd, "ident") == 0) {
            return cmd_ident();
        }
        if (std::strcmp(cmd, "io") == 0) {
            return cmd_io(opts);
        }
        if (std::strcmp(cmd, "fw") == 0) {
            return cmd_fw(argc, argv, opts);
        }
    } catch (const std::exception &e) {
        std::fprintf(stderr, "nvmeslotswap: %s\n", e.what());
        return 1;
    }

    std::fprintf(stderr, "nvmeslotswap: '%s' is not implemented yet\n", cmd);
    return 2;
}

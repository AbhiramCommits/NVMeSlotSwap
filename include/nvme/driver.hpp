/* driver.hpp - C++17 NVMe driver facade. */
#ifndef NVME_DRIVER_HPP
#define NVME_DRIVER_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "nvme/backend.hpp"
#include "nvme/queue.h"
#include "nvme/spec.h"

namespace nvme {

struct IdentifyCtrl {
    uint16_t vid = 0;
    uint16_t ssvid = 0;
    std::string serial;
    std::string model;
    std::string firmware;
    uint8_t frmw = 0;
    uint16_t oacs = 0;
    uint32_t nn = 0;
    uint32_t ver = 0;
    uint32_t max_queue_entries = 0;
};

struct IdentifyNs {
    uint64_t nsze = 0;
    uint64_t ncap = 0;
    uint64_t nuse = 0;
    uint8_t lbads = 0;
    uint64_t lba_size = 0;
};

struct Completion {
    uint16_t cid = 0;
    uint16_t status = 0; /* raw status word, phase tag cleared */
    unsigned sc = 0;
    unsigned sct = 0;
    bool dnr = false;
    uint32_t cdw0 = 0;
    bool ok() const { return sc == 0 && sct == 0; }
};

struct CmdHandle {
    uint16_t cid = 0;
    unsigned qid = 0;
    uint64_t submit_ns = 0;
    bool ok = true;
};

struct FwSlotInfo {
    uint8_t active_slot = 0;
    uint8_t next_slot = 0;
    std::string revisions[7];
};

enum class CommitAction : uint8_t {
    Replace = 0,
    ReplaceActivate = 1,
    Activate = 2,
    ReplaceActivateImmediate = 3,
};

struct StageResult {
    uint16_t status = 0;
    size_t bytes = 0;
    size_t chunks = 0;
    uint64_t elapsed_us = 0;
    bool ok() const { return status == 0; }
};

struct CommitResult {
    uint16_t status = 0;
    bool requires_reset = false;
    bool rolled_back = false;
    uint8_t active_slot = 0;
    uint8_t next_slot = 0;
    std::string active_revision;
    uint64_t latency_us = 0;
};

uint64_t now_ns();
uint64_t boot_relative_ns();

/* 4096-byte aligned, device-visible "DMA" buffer. */
struct DmaBuffer {
    void *ptr = nullptr;
    uint64_t iova = 0;
    size_t size = 0;
};

class DmaBufferPool {
public:
    DmaBufferPool(Backend &backend, size_t buffer_size, size_t count);
    ~DmaBufferPool();

    DmaBufferPool(const DmaBufferPool &) = delete;
    DmaBufferPool &operator=(const DmaBufferPool &) = delete;

    DmaBuffer *acquire();
    void release(DmaBuffer *buf);
    size_t free_count() const;

private:
    Backend &backend_;
    std::vector<std::unique_ptr<DmaBuffer>> buffers_;
    std::vector<DmaBuffer *> free_;
    mutable std::mutex mutex_;
};

struct QueuePair {
    nvme_qp qp{};
    uint16_t next_cid = 1;
    uint16_t depth = 0;
};

class NvmeDriver {
public:
    explicit NvmeDriver(Backend &backend);
    ~NvmeDriver();

    NvmeDriver(const NvmeDriver &) = delete;
    NvmeDriver &operator=(const NvmeDriver &) = delete;

    Backend &backend() { return backend_; }

    void reset_and_enable();
    void controller_reset();
    bool wait_ready(uint32_t timeout_us);
    bool is_ready();

    IdentifyCtrl identify_ctrl();
    IdentifyNs identify_ns(uint32_t nsid);
    FwSlotInfo read_slot_log();
    void set_number_of_queues(unsigned count);

    void create_io_queues(unsigned count, uint16_t depth);
    void recreate_io_queues();
    void destroy_io_queues();
    unsigned io_queue_count() const { return static_cast<unsigned>(queues_.size()); }
    unsigned io_depth() const { return io_depth_; }

    QueuePair &queue(unsigned idx);
    CmdHandle submit_read(unsigned q, uint64_t slba, uint16_t nlb, const DmaBuffer &buf);
    CmdHandle submit_write(unsigned q, uint64_t slba, uint16_t nlb, const DmaBuffer &buf);
    CmdHandle submit_flush(unsigned q);
    std::optional<Completion> poll(unsigned q);
    int poll_batch(unsigned q, Completion *out, int max);

    Completion sync_admin(const nvme_sqe &sqe);
    Completion sync_admin_checked(const nvme_sqe &sqe, const char *what);
    void set_self_test_fail(bool fail);
    void save_fw_state(const std::string &path);
    bool load_fw_state(const std::string &path);

    static constexpr unsigned kAdminDepth = 32;

private:
    void init_admin_queue();
    CmdHandle submit_io(unsigned qidx, uint8_t opc, uint64_t slba, uint16_t nlb,
                        const DmaBuffer &buf);
    Completion admin_wait(uint16_t cid);
    int submit_admin_async(const nvme_sqe &sqe, uint16_t cid);
    DmaBuffer *admin_buf();

    Backend &backend_;
    volatile void *bar0_ = nullptr;
    uint32_t doorbell_stride_ = 0;

    QueuePair admin_{};
    DmaBufferPool admin_pool_;
    unsigned next_admin_cid_ = 1;

    std::vector<std::unique_ptr<QueuePair>> queues_;
    unsigned io_depth_ = 0;
    bool enabled_ = false;
};

class FirmwareUpdater {
public:
    explicit FirmwareUpdater(NvmeDriver &driver) : driver_(driver) {}

    /* Builds header + deterministic payload with the payload SHA-256 filled in. */
    static std::vector<uint8_t> build_image(const std::string &revision, size_t payload_len,
                                            uint32_t seed);

    StageResult stage(unsigned slot, const std::vector<uint8_t> &image, size_t chunk_bytes);
    CommitResult commit(unsigned slot, CommitAction action);
    FwSlotInfo read_slot_log();
    bool verify_activation(const std::string &revision);
    std::string active_revision();

    static std::string normalize_revision(const std::string &rev);

private:
    NvmeDriver &driver_;
};

/* Reads a raw Image Download / Commit payload from disk (host-side cache). */
std::vector<uint8_t> read_file_bytes(const std::string &path);
bool write_file_bytes(const std::string &path, const std::vector<uint8_t> &bytes);

} // namespace nvme

#endif /* NVME_DRIVER_HPP */

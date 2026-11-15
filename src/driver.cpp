/*
 * driver.cpp - C++17 NVMe driver facade.
 *
 * The driver issues real NVMe commands: it programs AQA/ASQ/ACQ and CC.EN
 * through BAR0, creates I/O queue pairs with Create I/O Submission/Completion
 * Queue, and moves data with PRPs.  Nothing here reaches into device state.
 */
#include "nvme/driver.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <thread>

#include "nvme/fw.h"
#include "nvme/regs.h"
#include "sha256.h"

namespace nvme {

uint64_t now_ns()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

uint64_t boot_relative_ns() { return now_ns(); }

namespace {

Completion to_completion(const nvme_cqe &cqe)
{
    Completion c;
    c.cid = cqe.cid;
    c.status = cqe.status;
    c.sc = nvme_cqe_sc(cqe.status);
    c.sct = nvme_cqe_sct(cqe.status);
    c.dnr = nvme_cqe_dnr(cqe.status) != 0;
    c.cdw0 = cqe.cdw0;
    return c;
}

std::string trim_field(const char *data, size_t len)
{
    size_t end = len;
    while (end > 0 && (data[end - 1] == ' ' || data[end - 1] == '\0')) {
        --end;
    }
    return std::string(data, end);
}

std::string status_text(unsigned sc, unsigned sct)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "status=0x%02x/0x%x", sc, sct);
    return std::string(buf);
}

} // namespace

/* ------------------------------------------------------------------ */
/* DMA buffer pool                                                     */
/* ------------------------------------------------------------------ */
DmaBufferPool::DmaBufferPool(Backend &backend, size_t buffer_size, size_t count)
    : backend_(backend)
{
    if (buffer_size == 0 || count == 0) {
        throw std::invalid_argument("DmaBufferPool needs a non-zero size and count");
    }
    buffers_.reserve(count);
    free_.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        auto buf = std::make_unique<DmaBuffer>();
        buf->size = buffer_size;
        buf->ptr = backend_.dma_alloc(buffer_size, &buf->iova);
        if (buf->ptr == nullptr) {
            throw std::runtime_error("out of emulated DMA memory");
        }
        std::memset(buf->ptr, 0, buffer_size);
        free_.push_back(buf.get());
        buffers_.push_back(std::move(buf));
    }
}

DmaBufferPool::~DmaBufferPool()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &buf : buffers_) {
        if (buf->ptr != nullptr) {
            backend_.dma_free(buf->ptr);
        }
    }
}

DmaBuffer *DmaBufferPool::acquire()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (free_.empty()) {
        return nullptr;
    }
    DmaBuffer *buf = free_.back();
    free_.pop_back();
    return buf;
}

void DmaBufferPool::release(DmaBuffer *buf)
{
    if (buf == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    free_.push_back(buf);
}

size_t DmaBufferPool::free_count() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return free_.size();
}

/* ------------------------------------------------------------------ */
/* Driver                                                              */
/* ------------------------------------------------------------------ */
NvmeDriver::NvmeDriver(Backend &backend)
    : backend_(backend), admin_pool_(backend, 4096, 4)
{
    bar0_ = backend_.bar0();
    doorbell_stride_ = backend_.doorbell_stride();
    if (backend_.sim_device() == nullptr) {
        throw std::runtime_error(
            "this driver build requires an in-process controller backend; the VFIO "
            "backend is compile-only in this repository");
    }
    if (bar0_ == nullptr || doorbell_stride_ < 4) {
        throw std::runtime_error("backend did not provide a BAR0 window");
    }
}

NvmeDriver::~NvmeDriver()
{
    try {
        if (enabled_) {
            for (auto &q : queues_) {
                nvme_sqe sqe{};
                sqe.opc = NVME_ADMIN_DELETE_SQ;
                sqe.nsid = 1;
                sqe.cdw10 = static_cast<uint32_t>(q->qp.qid) << 16;
                (void)sync_admin(sqe);
                nvme_sqe cqe_sqe{};
                cqe_sqe.opc = NVME_ADMIN_DELETE_CQ;
                cqe_sqe.nsid = 1;
                cqe_sqe.cdw10 = static_cast<uint32_t>(q->qp.qid) << 16;
                (void)sync_admin(cqe_sqe);
            }
        }
    } catch (...) {
        /* best effort during teardown */
    }
    for (auto &q : queues_) {
        nvme_qp_destroy(&q->qp);
    }
    queues_.clear();
    if (admin_.qp.sq != nullptr) {
        nvme_qp_destroy(&admin_.qp);
    }
}

bool NvmeDriver::is_ready()
{
    return (nvme_mmio_read32(bar0_, NVME_REG_CSTS) & NVME_CSTS_RDY) != 0;
}

bool NvmeDriver::wait_ready(uint32_t timeout_us)
{
    const uint64_t deadline = now_ns() + static_cast<uint64_t>(timeout_us) * 1000ull;
    while (!is_ready()) {
        if (now_ns() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    return true;
}

void NvmeDriver::controller_reset()
{
    if (!enabled_) {
        return;
    }
    nvme_mmio_write32(bar0_, NVME_REG_CC, 0);
    const uint64_t deadline = now_ns() + 1000000000ull;
    while (is_ready()) {
        if (now_ns() > deadline) {
            throw std::runtime_error("controller did not clear CSTS.RDY on reset");
        }
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    enabled_ = false;
    io_depth_ = 0;
}

void NvmeDriver::reset_and_enable()
{
    if (enabled_) {
        controller_reset();
    } else if (is_ready()) {
        /* Belt and braces: leave nothing programmed from a prior life. */
        nvme_mmio_write32(bar0_, NVME_REG_CC, 0);
        (void)wait_ready(1000);
    }

    if (admin_.qp.sq == nullptr) {
        if (nvme_qp_init(&admin_.qp, backend_.sim_device(), NVME_ADMIN_QID, kAdminDepth) != 0) {
            throw std::runtime_error("failed to allocate the admin queue pair");
        }
        admin_.depth = kAdminDepth;
    } else {
        nvme_qp_reset(&admin_.qp);
    }
    next_admin_cid_ = 1;

    const uint32_t aqa = (kAdminDepth - 1u) | ((kAdminDepth - 1u) << NVME_AQA_ACQS_SHIFT);
    nvme_mmio_write32(bar0_, NVME_REG_AQA, aqa);
    nvme_mmio_write64(bar0_, NVME_REG_ASQ, admin_.qp.sq_iova);
    nvme_mmio_write64(bar0_, NVME_REG_ACQ, admin_.qp.cq_iova);
    nvme_mmio_write32(bar0_, NVME_REG_CC,
                      NVME_CC_EN | (6u << NVME_CC_IOSQES_SHIFT) |
                          (4u << NVME_CC_IOCQES_SHIFT));

    if (!wait_ready(3000000)) {
        throw std::runtime_error("controller failed to set CSTS.RDY");
    }
    uint32_t csts = nvme_mmio_read32(bar0_, NVME_REG_CSTS);
    if ((csts & NVME_CSTS_CFS) != 0) {
        throw std::runtime_error("controller reported a fatal status during enable");
    }
    enabled_ = true;
}

int NvmeDriver::submit_admin_async(const nvme_sqe &sqe, uint16_t cid)
{
    nvme_sqe copy = sqe;
    copy.cid = cid;
    return nvme_qp_submit(&admin_.qp, &copy, cid);
}

Completion NvmeDriver::admin_wait(uint16_t cid)
{
    const uint64_t deadline = now_ns() + 5000000000ull;
    for (;;) {
        nvme_cqe raw[8];
        int n = nvme_qp_reap(&admin_.qp, raw, 8);
        for (int i = 0; i < n; ++i) {
            if (raw[i].cid == cid) {
                return to_completion(raw[i]);
            }
        }
        if (now_ns() > deadline) {
            throw std::runtime_error("admin command timed out");
        }
        std::this_thread::sleep_for(std::chrono::microseconds(10));
    }
}

Completion NvmeDriver::sync_admin(const nvme_sqe &sqe)
{
    uint16_t cid = next_admin_cid_++;
    if (next_admin_cid_ == 0) {
        next_admin_cid_ = 1;
    }
    int rc = submit_admin_async(sqe, cid);
    if (rc == NVME_QP_EAGAIN) {
        /* Nothing else is ever outstanding on the admin queue in this tool;
         * drain whatever is there and retry once. */
        nvme_cqe raw[8];
        while (nvme_qp_reap(&admin_.qp, raw, 8) > 0) {
        }
        cid = next_admin_cid_++;
        rc = submit_admin_async(sqe, cid);
    }
    if (rc != 0) {
        throw std::runtime_error("failed to submit an admin command");
    }
    return admin_wait(cid);
}

Completion NvmeDriver::sync_admin_checked(const nvme_sqe &sqe, const char *what)
{
    Completion c = sync_admin(sqe);
    if (!c.ok()) {
        throw std::runtime_error(std::string(what) + " failed: " + status_text(c.sc, c.sct));
    }
    return c;
}

DmaBuffer *NvmeDriver::admin_buf()
{
    DmaBuffer *buf = admin_pool_.acquire();
    if (buf == nullptr) {
        throw std::runtime_error("admin DMA buffer pool exhausted");
    }
    return buf;
}

IdentifyCtrl NvmeDriver::identify_ctrl()
{
    DmaBuffer *buf = admin_buf();
    nvme_sqe sqe{};
    sqe.opc = NVME_ADMIN_IDENTIFY;
    sqe.nsid = 0;
    sqe.prp1 = buf->iova;
    sqe.cdw10 = NVME_ID_CNS_CONTROLLER;
    sync_admin_checked(sqe, "Identify Controller");

    nvme_id_ctrl id;
    std::memcpy(&id, buf->ptr, sizeof(id));
    admin_pool_.release(buf);

    IdentifyCtrl out;
    out.vid = id.vid;
    out.ssvid = id.ssvid;
    out.serial = trim_field(id.sn, sizeof(id.sn));
    out.model = trim_field(id.mn, sizeof(id.mn));
    out.firmware = trim_field(id.fr, sizeof(id.fr));
    out.frmw = id.frmw;
    out.oacs = id.oacs;
    out.nn = id.nn;
    out.ver = id.ver;
    uint64_t cap = nvme_mmio_read64(bar0_, NVME_REG_CAP);
    out.max_queue_entries = nvme_cap_mqes(cap) + 1u;
    return out;
}

IdentifyNs NvmeDriver::identify_ns(uint32_t nsid)
{
    DmaBuffer *buf = admin_buf();
    nvme_sqe sqe{};
    sqe.opc = NVME_ADMIN_IDENTIFY;
    sqe.nsid = nsid;
    sqe.prp1 = buf->iova;
    sqe.cdw10 = NVME_ID_CNS_NAMESPACE;
    sync_admin_checked(sqe, "Identify Namespace");

    nvme_id_ns id;
    std::memcpy(&id, buf->ptr, sizeof(id));
    admin_pool_.release(buf);

    IdentifyNs out;
    out.nsze = id.nsze;
    out.ncap = id.ncap;
    out.nuse = id.nuse;
    out.lbads = id.lbaf[0].ds;
    out.lba_size = 1ull << id.lbaf[0].ds;
    return out;
}

FwSlotInfo NvmeDriver::read_slot_log()
{
    DmaBuffer *buf = admin_buf();
    std::memset(buf->ptr, 0, 512);
    nvme_sqe sqe{};
    sqe.opc = NVME_ADMIN_GET_LOG_PAGE;
    sqe.nsid = 0xFFFFFFFFu;
    sqe.prp1 = buf->iova;
    sqe.cdw10 = NVME_LOG_FW_SLOT | (127u << 16); /* 128 dwords = 512 bytes */
    sqe.cdw11 = 0;
    sync_admin_checked(sqe, "Get Log Page 0x03");

    nvme_fw_slot_log log;
    std::memcpy(&log, buf->ptr, sizeof(log));
    admin_pool_.release(buf);

    FwSlotInfo info;
    info.active_slot = static_cast<uint8_t>(nvme_afi_active(log.afi));
    info.next_slot = static_cast<uint8_t>(nvme_afi_next(log.afi));
    for (unsigned i = 0; i < 7; ++i) {
        info.revisions[i] = std::string(log.frs[i], 8);
        while (!info.revisions[i].empty() && info.revisions[i].back() == ' ') {
            info.revisions[i].pop_back();
        }
    }
    return info;
}

void NvmeDriver::set_number_of_queues(unsigned count)
{
    nvme_sqe sqe{};
    sqe.opc = NVME_ADMIN_SET_FEATURES;
    sqe.cdw10 = NVME_FEAT_NUM_QUEUES;
    sqe.cdw11 = ((count - 1u) << 16) | (count - 1u);
    (void)sync_admin(sqe);
}

void NvmeDriver::create_io_queues(unsigned count, uint16_t depth)
{
    if (!enabled_) {
        throw std::runtime_error("create_io_queues called before the controller was enabled");
    }
    if (count == 0 || count > 32 || depth < 2) {
        throw std::invalid_argument("invalid queue count/depth");
    }
    if (!queues_.empty()) {
        destroy_io_queues();
    }
    queues_.reserve(count);
    for (unsigned i = 1; i <= count; ++i) {
        auto qp = std::make_unique<QueuePair>();
        if (nvme_qp_init(&qp->qp, backend_.sim_device(), static_cast<uint16_t>(i), depth) != 0) {
            throw std::runtime_error("failed to allocate an I/O queue pair");
        }
        qp->depth = depth;
        qp->next_cid = 1;
        queues_.push_back(std::move(qp));
    }

    for (unsigned i = 0; i < count; ++i) {
        QueuePair &q = *queues_[i];
        const uint16_t qid = q.qp.qid;

        nvme_sqe cq{};
        cq.opc = NVME_ADMIN_CREATE_CQ;
        cq.nsid = 1;
        cq.prp1 = q.qp.cq_iova;
        cq.cdw10 = (static_cast<uint32_t>(depth) - 1u) | (static_cast<uint32_t>(qid) << 16);
        cq.cdw11 = 1u; /* physically contiguous */
        sync_admin_checked(cq, "Create I/O Completion Queue");

        nvme_sqe sq{};
        sq.opc = NVME_ADMIN_CREATE_SQ;
        sq.nsid = 1;
        sq.prp1 = q.qp.sq_iova;
        sq.cdw10 = (static_cast<uint32_t>(depth) - 1u) | (static_cast<uint32_t>(qid) << 16);
        sq.cdw11 = 1u | (static_cast<uint32_t>(qid) << 16);
        sync_admin_checked(sq, "Create I/O Submission Queue");
    }
    io_depth_ = depth;
    set_number_of_queues(count);
}

void NvmeDriver::recreate_io_queues()
{
    if (!enabled_ || queues_.empty()) {
        throw std::runtime_error("recreate_io_queues requires an enabled controller");
    }
    for (auto &q : queues_) {
        nvme_qp_reset(&q->qp);
        q->next_cid = 1;
        nvme_sqe cq{};
        cq.opc = NVME_ADMIN_CREATE_CQ;
        cq.nsid = 1;
        cq.prp1 = q->qp.cq_iova;
        cq.cdw10 = (static_cast<uint32_t>(q->depth) - 1u) |
                   (static_cast<uint32_t>(q->qp.qid) << 16);
        cq.cdw11 = 1u;
        sync_admin_checked(cq, "Create I/O Completion Queue");
        nvme_sqe sq{};
        sq.opc = NVME_ADMIN_CREATE_SQ;
        sq.nsid = 1;
        sq.prp1 = q->qp.sq_iova;
        sq.cdw10 = (static_cast<uint32_t>(q->depth) - 1u) |
                   (static_cast<uint32_t>(q->qp.qid) << 16);
        sq.cdw11 = 1u | (static_cast<uint32_t>(q->qp.qid) << 16);
        sync_admin_checked(sq, "Create I/O Submission Queue");
    }
    set_number_of_queues(static_cast<unsigned>(queues_.size()));
}

void NvmeDriver::destroy_io_queues()
{
    if (enabled_) {
        for (auto &q : queues_) {
            nvme_sqe sq{};
            sq.opc = NVME_ADMIN_DELETE_SQ;
            sq.nsid = 1;
            sq.cdw10 = static_cast<uint32_t>(q->qp.qid) << 16;
            (void)sync_admin(sq);
            nvme_sqe cq{};
            cq.opc = NVME_ADMIN_DELETE_CQ;
            cq.nsid = 1;
            cq.cdw10 = static_cast<uint32_t>(q->qp.qid) << 16;
            (void)sync_admin(cq);
        }
    }
    for (auto &q : queues_) {
        nvme_qp_destroy(&q->qp);
    }
    queues_.clear();
    io_depth_ = 0;
}

QueuePair &NvmeDriver::queue(unsigned idx)
{
    if (idx >= queues_.size()) {
        throw std::out_of_range("I/O queue index out of range");
    }
    return *queues_[idx];
}

CmdHandle NvmeDriver::submit_read(unsigned q, uint64_t slba, uint16_t nlb, const DmaBuffer &buf)
{
    return submit_io(q, NVME_IO_READ, slba, nlb, buf);
}

CmdHandle NvmeDriver::submit_write(unsigned q, uint64_t slba, uint16_t nlb, const DmaBuffer &buf)
{
    return submit_io(q, NVME_IO_WRITE, slba, nlb, buf);
}

CmdHandle NvmeDriver::submit_flush(unsigned q)
{
    QueuePair &qp = queue(q);
    CmdHandle h;
    h.qid = qp.qp.qid;
    uint16_t cid = qp.next_cid++;
    if (qp.next_cid == 0) {
        qp.next_cid = 1;
    }
    h.cid = cid;
    nvme_sqe sqe{};
    sqe.opc = NVME_IO_FLUSH;
    sqe.nsid = 1;
    if (nvme_qp_submit(&qp.qp, &sqe, cid) != 0) {
        h.ok = false;
        return h;
    }
    h.submit_ns = now_ns();
    return h;
}

CmdHandle NvmeDriver::submit_io(unsigned qidx, uint8_t opc, uint64_t slba, uint16_t nlb,
                                const DmaBuffer &buf)
{
    QueuePair &qp = queue(qidx);
    const uint64_t bytes = static_cast<uint64_t>(nlb) * NVME_LBA_SIZE;
    CmdHandle h;
    h.qid = qp.qp.qid;
    if (nlb == 0 || bytes > buf.size || bytes > 2ull * NVME_LBA_SIZE) {
        h.ok = false;
        return h;
    }

    uint16_t cid = qp.next_cid++;
    if (qp.next_cid == 0) {
        qp.next_cid = 1;
    }
    h.cid = cid;

    nvme_sqe sqe{};
    sqe.opc = opc;
    sqe.nsid = 1;
    sqe.cid = cid;
    sqe.prp1 = buf.iova;
    sqe.prp2 = bytes > NVME_LBA_SIZE ? buf.iova + NVME_LBA_SIZE : 0;
    sqe.cdw10 = static_cast<uint32_t>(slba & 0xFFFFFFFFu);
    sqe.cdw11 = static_cast<uint32_t>(slba >> 32);
    sqe.cdw12 = static_cast<uint32_t>(nlb - 1u);

    if (nvme_qp_submit(&qp.qp, &sqe, cid) != 0) {
        h.ok = false;
        return h;
    }
    h.submit_ns = now_ns();
    return h;
}

int NvmeDriver::poll_batch(unsigned q, Completion *out, int max)
{
    QueuePair &qp = queue(q);
    nvme_cqe raw[32];
    if (max > 32) {
        max = 32;
    }
    int n = nvme_qp_reap(&qp.qp, raw, max);
    for (int i = 0; i < n; ++i) {
        out[i] = to_completion(raw[i]);
    }
    return n;
}

std::optional<Completion> NvmeDriver::poll(unsigned q)
{
    Completion c;
    if (poll_batch(q, &c, 1) == 1) {
        return c;
    }
    return std::nullopt;
}

void NvmeDriver::set_self_test_fail(bool fail)
{
    nvme_dev_set_self_test_fail(backend_.sim_device(), fail ? 1 : 0);
}

void NvmeDriver::save_fw_state(const std::string &path)
{
    if (nvme_dev_save_fw_state(backend_.sim_device(), path.c_str()) != 0) {
        throw std::runtime_error("failed to save emulated firmware state to " + path);
    }
}

bool NvmeDriver::load_fw_state(const std::string &path)
{
    return nvme_dev_load_fw_state(backend_.sim_device(), path.c_str()) == 0;
}

/* ------------------------------------------------------------------ */
/* FirmwareUpdater                                                     */
/* ------------------------------------------------------------------ */
std::string FirmwareUpdater::normalize_revision(const std::string &rev)
{
    std::string r = rev.substr(0, 8);
    r.resize(8, ' ');
    return r;
}

std::vector<uint8_t> FirmwareUpdater::build_image(const std::string &revision,
                                                  size_t payload_len, uint32_t seed)
{
    /* Keep the image a whole number of dwords: FW Image Download counts data
     * in dwords. */
    payload_len = (payload_len + 3u) & ~static_cast<size_t>(3u);
    if (payload_len > NVME_FW_MAX_IMAGE_BYTES - sizeof(nvme_fw_image_hdr)) {
        throw std::invalid_argument("firmware image is larger than the slot limit");
    }

    nvme_fw_image_hdr hdr;
    std::memset(&hdr, 0, sizeof(hdr));
    std::memcpy(hdr.magic, "NVFW", 4);
    hdr.version = NVME_FW_IMAGE_VERSION;
    std::string rev8 = normalize_revision(revision);
    std::memcpy(hdr.revision, rev8.data(), 8);
    hdr.payload_len = static_cast<uint32_t>(payload_len);

    std::vector<uint8_t> image(sizeof(hdr) + payload_len);
    uint8_t *payload = image.data() + sizeof(hdr);
    uint64_t x = 0x9E3779B97F4A7C15ull ^ static_cast<uint64_t>(seed);
    for (size_t i = 0; i < payload_len; ++i) {
        x += 0x9E3779B97F4A7C15ull;
        uint64_t z = x;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        payload[i] = static_cast<uint8_t>(z);
    }
    sha256(payload, payload_len, hdr.sha256);
    std::memcpy(image.data(), &hdr, sizeof(hdr));
    return image;
}

StageResult FirmwareUpdater::stage(unsigned slot, const std::vector<uint8_t> &image,
                                   size_t chunk_bytes)
{
    if (slot < 1 || slot > NVME_FW_SLOT_COUNT) {
        throw std::invalid_argument("firmware slot must be 1..7");
    }
    if (chunk_bytes == 0 || chunk_bytes > NVME_LBA_SIZE || (chunk_bytes % 4) != 0) {
        throw std::invalid_argument("chunk size must be a non-zero multiple of 4 up to 4096");
    }
    if (image.size() < sizeof(nvme_fw_image_hdr) || (image.size() % 4) != 0) {
        throw std::invalid_argument("firmware image must be header plus whole dwords");
    }

    DmaBufferPool pool(driver_.backend(), NVME_LBA_SIZE, 1);
    DmaBuffer *buf = pool.acquire();
    if (buf == nullptr) {
        throw std::runtime_error("unable to allocate a firmware download buffer");
    }

    StageResult result;
    result.bytes = image.size();
    const uint64_t start = now_ns();
    size_t off = 0;
    while (off < image.size()) {
        size_t n = image.size() - off;
        if (n > chunk_bytes) {
            n = chunk_bytes;
        }
        std::memcpy(buf->ptr, image.data() + off, n);

        nvme_sqe sqe{};
        sqe.opc = NVME_ADMIN_FW_IMAGE_DOWNLOAD;
        sqe.nsid = 0;
        sqe.prp1 = buf->iova;
        sqe.cdw10 = static_cast<uint32_t>(n / 4u) - 1u;           /* NUMD, 0-based */
        sqe.cdw11 = static_cast<uint32_t>(off / 4u);              /* OFST in dwords */
        Completion c = driver_.sync_admin(sqe);
        if (!c.ok()) {
            result.status = static_cast<uint16_t>(c.sc);
            result.elapsed_us = (now_ns() - start) / 1000;
            pool.release(buf);
            return result;
        }
        off += n;
        result.chunks++;
    }
    result.status = 0;
    result.elapsed_us = (now_ns() - start) / 1000;
    pool.release(buf);
    return result;
}

CommitResult FirmwareUpdater::commit(unsigned slot, CommitAction action)
{
    FwSlotInfo before = read_slot_log();

    nvme_sqe sqe{};
    sqe.opc = NVME_ADMIN_FW_COMMIT;
    sqe.nsid = 0;
    sqe.cdw10 = (static_cast<uint32_t>(slot) & 0x7u) |
                ((static_cast<uint32_t>(action) & 0x7u) << 3);
    sqe.cdw11 = 0; /* BPID 0 */

    const uint64_t start = now_ns();
    Completion c = driver_.sync_admin(sqe);
    const uint64_t elapsed = now_ns() - start;

    FwSlotInfo after = read_slot_log();
    CommitResult r;
    r.status = static_cast<uint16_t>(c.sc);
    r.requires_reset = (c.sc == NVME_SC_FW_ACTIVATION_REQUIRES_RESET);
    r.active_slot = after.active_slot;
    r.next_slot = after.next_slot;
    if (after.active_slot >= 1 && after.active_slot <= 7) {
        r.active_revision = after.revisions[after.active_slot - 1];
    }
    if (action == CommitAction::ReplaceActivateImmediate && !c.ok() && slot >= 1 &&
        slot <= 7 && before.revisions[slot - 1] != after.revisions[slot - 1] &&
        after.active_slot == before.active_slot) {
        /* The target slot now carries the new revision, but the active slot
         * did not change: the controller installed the image, failed its
         * post-activation self test and rolled back. */
        r.rolled_back = true;
    }
    r.latency_us = elapsed / 1000;
    return r;
}

FwSlotInfo FirmwareUpdater::read_slot_log() { return driver_.read_slot_log(); }

bool FirmwareUpdater::verify_activation(const std::string &revision)
{
    FwSlotInfo info = read_slot_log();
    if (info.active_slot < 1 || info.active_slot > 7) {
        return false;
    }
    std::string active = info.revisions[info.active_slot - 1];
    std::string want = revision;
    while (!active.empty() && active.back() == ' ') {
        active.pop_back();
    }
    while (!want.empty() && want.back() == ' ') {
        want.pop_back();
    }
    return active == want;
}

std::string FirmwareUpdater::active_revision()
{
    FwSlotInfo info = read_slot_log();
    if (info.active_slot >= 1 && info.active_slot <= 7) {
        return info.revisions[info.active_slot - 1];
    }
    return {};
}

/* ------------------------------------------------------------------ */
/* Utility file helpers                                                */
/* ------------------------------------------------------------------ */
std::vector<uint8_t> read_file_bytes(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

bool write_file_bytes(const std::string &path, const std::vector<uint8_t> &bytes)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

} // namespace nvme

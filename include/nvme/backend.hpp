/* backend.hpp - host/device transport seam: BAR0 + DMA. */
#ifndef NVME_BACKEND_HPP
#define NVME_BACKEND_HPP

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "nvme/device.h"

namespace nvme {

/*
 * A Backend is the only place platform knowledge lives.  The driver above it
 * speaks NVMe (BAR0 register writes, doorbells, PRPs) and never calls into
 * device internals.
 */
class Backend {
public:
    virtual ~Backend() = default;

    virtual volatile void* bar0() = 0;
    virtual uint32_t doorbell_stride() const = 0;
    virtual void* dma_alloc(size_t len, uint64_t* iova) = 0;
    virtual void dma_free(void* ptr) = 0;
    virtual const char* name() const = 0;
    virtual bool is_simulated() const = 0;
    /* Non-null only for backends that host the controller in-process; the
     * VFIO backend reaches the same contract through mapped BAR0/DMA. */
    virtual nvme_dev_t* sim_device() { return nullptr; }
};

/* In-process emulated controller (default on every host, including macOS). */
class SimBackend final : public Backend {
public:
    explicit SimBackend(const nvme_dev_cfg& cfg) {
        dev_ = nvme_dev_create(&cfg);
        if (dev_ == nullptr) {
            throw std::runtime_error("nvme_dev_create failed");
        }
    }
    ~SimBackend() override {
        if (dev_ != nullptr) {
            nvme_dev_destroy(dev_);
        }
    }

    SimBackend(const SimBackend&) = delete;
    SimBackend& operator=(const SimBackend&) = delete;

    volatile void* bar0() override { return nvme_dev_bar0(dev_); }
    uint32_t doorbell_stride() const override { return nvme_dev_doorbell_stride(dev_); }
    void* dma_alloc(size_t len, uint64_t* iova) override {
        return nvme_dev_dma_map(dev_, len, iova);
    }
    void dma_free(void* ptr) override { nvme_dev_dma_unmap(dev_, ptr); }
    const char* name() const override { return "sim"; }
    bool is_simulated() const override { return true; }
    nvme_dev_t* sim_device() override { return dev_; }

    nvme_dev_t* device() const { return dev_; }

private:
    nvme_dev_t* dev_ = nullptr;
};

/*
 * VFIO backend factory (Linux + -DNVMESS_BACKEND_VFIO=ON only).  The
 * implementation lives in src/backend_vfio.cpp and is documented as
 * unvalidated on the macOS build host; no results in this repository come
 * from it.
 */
Backend* make_vfio_backend(const std::string& device_path, const std::string& group_path);

} // namespace nvme

#endif /* NVME_BACKEND_HPP */

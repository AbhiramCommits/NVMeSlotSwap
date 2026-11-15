/*
 * backend_vfio.cpp - Linux VFIO transport backend.
 *
 * Compiled only with -DNVMESS_BACKEND_VFIO=ON on Linux.  This file is the
 * platform seam: it maps a real (or QEMU-emulated) NVMe controller's BAR0 and
 * registers shared DMA memory with the IOMMU.  The NVMe protocol code above
 * the seam is unchanged.
 *
 * STATUS: compile-verified only.  The development host for the measurements in
 * this repository is macOS, so nothing in this file has been executed against
 * a device.  No result in results/ comes from this backend.
 *
 * The current in-tree queue layer still binds to the in-process controller
 * (see NvmeDriver's constructor), so `make_vfio_backend()` gives you a working
 * BAR0/DMA transport that a Linux driver port can build on, but the CLI does
 * not select it yet.  docs/qemu.md has the bind and QEMU steps.
 */
#include "nvme/backend.hpp"
#include "nvme/regs.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include <linux/vfio.h>

namespace nvme {

namespace {

constexpr uint64_t kVfioIovaBase = 0x10000000ull;
constexpr uint64_t kVfioPage = 4096ull;

uint64_t align_up(uint64_t value, uint64_t align)
{
    return (value + align - 1u) & ~(align - 1u);
}

std::string errno_text(const char *what)
{
    return std::string(what) + ": " + std::strerror(errno);
}

struct DmaRegion {
    void *ptr;
    size_t len;
    uint64_t iova;
};

class VfioBackend final : public Backend {
public:
    VfioBackend(const std::string &group_path, const std::string &device_path)
    {
        container_ = open("/dev/vfio/vfio", O_RDWR | O_CLOEXEC);
        if (container_ < 0) {
            throw std::runtime_error(errno_text("open /dev/vfio/vfio"));
        }
        if (ioctl(container_, VFIO_GET_API_VERSION) != VFIO_API_VERSION) {
            throw std::runtime_error("unexpected VFIO API version");
        }
        if (ioctl(container_, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU) <= 0) {
            throw std::runtime_error("VFIO type1 IOMMU is not available");
        }
        if (ioctl(container_, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU) != 0) {
            throw std::runtime_error(errno_text("VFIO_SET_IOMMU"));
        }

        group_ = open(group_path.c_str(), O_RDWR | O_CLOEXEC);
        if (group_ < 0) {
            throw std::runtime_error(errno_text(("open " + group_path).c_str()));
        }
        struct vfio_group_status status;
        std::memset(&status, 0, sizeof(status));
        status.argsz = sizeof(status);
        if (ioctl(group_, VFIO_GROUP_GET_STATUS, &status) != 0) {
            throw std::runtime_error(errno_text("VFIO_GROUP_GET_STATUS"));
        }
        if ((status.flags & VFIO_GROUP_FLAGS_VIABLE) == 0) {
            throw std::runtime_error("VFIO group is not viable (bind every device in the group)");
        }
        if (ioctl(group_, VFIO_GROUP_SET_CONTAINER, &container_) != 0) {
            throw std::runtime_error(errno_text("VFIO_GROUP_SET_CONTAINER"));
        }
        device_ = ioctl(group_, VFIO_GROUP_GET_DEVICE_FD, device_path.c_str());
        if (device_ < 0) {
            throw std::runtime_error(errno_text("VFIO_GROUP_GET_DEVICE_FD"));
        }

        struct vfio_region_info region;
        std::memset(&region, 0, sizeof(region));
        region.argsz = sizeof(region);
        region.index = VFIO_PCI_BAR0_REGION_INDEX;
        if (ioctl(device_, VFIO_DEVICE_GET_REGION_INFO, &region) != 0) {
            throw std::runtime_error(errno_text("VFIO_DEVICE_GET_REGION_INFO(BAR0)"));
        }
        if ((region.flags & VFIO_REGION_INFO_FLAG_MMAP) == 0) {
            throw std::runtime_error("BAR0 is not mappable through VFIO");
        }
        bar_size_ = region.size;
        bar_ = mmap(nullptr, region.size, PROT_READ | PROT_WRITE, MAP_SHARED, device_,
                    static_cast<off_t>(region.offset));
        if (bar_ == MAP_FAILED) {
            bar_ = nullptr;
            throw std::runtime_error(errno_text("mmap BAR0"));
        }
        uint64_t cap = nvme_mmio_read64(bar_, NVME_REG_CAP);
        stride_ = 4u << nvme_cap_dstrd(cap);
        dma_next_iova_ = kVfioIovaBase;
    }

    ~VfioBackend() override
    {
        for (auto &entry : regions_) {
            if (entry.second.ptr != nullptr) {
                (void)unmap(entry.second);
            }
        }
        if (bar_ != nullptr) {
            munmap(bar_, bar_size_);
        }
        if (device_ >= 0) {
            close(device_);
        }
        if (group_ >= 0) {
            close(group_);
        }
        if (container_ >= 0) {
            close(container_);
        }
    }

    VfioBackend(const VfioBackend &) = delete;
    VfioBackend &operator=(const VfioBackend &) = delete;

    volatile void *bar0() override { return bar_; }
    uint32_t doorbell_stride() const override { return stride_; }

    void *dma_alloc(size_t len, uint64_t *iova) override
    {
        if (len == 0 || iova == nullptr) {
            return nullptr;
        }
        size_t aligned = static_cast<size_t>(align_up(len, kVfioPage));
        void *ptr = mmap(nullptr, aligned, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
        if (ptr == MAP_FAILED) {
            /* Hugepages are optional; 4 KiB anonymous memory works too. */
            ptr = mmap(nullptr, aligned, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        }
        if (ptr == MAP_FAILED) {
            return nullptr;
        }
        uint64_t addr = align_up(dma_next_iova_, kVfioPage);
        struct vfio_iommu_type1_dma_map map;
        std::memset(&map, 0, sizeof(map));
        map.argsz = sizeof(map);
        map.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;
        map.vaddr = reinterpret_cast<uint64_t>(ptr);
        map.iova = addr;
        map.size = aligned;
        if (ioctl(container_, VFIO_IOMMU_MAP_DMA, &map) != 0) {
            munmap(ptr, aligned);
            return nullptr;
        }
        dma_next_iova_ = addr + aligned;
        regions_[ptr] = DmaRegion{ptr, aligned, addr};
        *iova = addr;
        return ptr;
    }

    void dma_free(void *ptr) override
    {
        auto it = regions_.find(ptr);
        if (it == regions_.end()) {
            return;
        }
        (void)unmap(it->second);
        regions_.erase(it);
    }

    const char *name() const override { return "vfio"; }
    bool is_simulated() const override { return false; }

private:
    int unmap(const DmaRegion &region)
    {
        struct vfio_iommu_type1_dma_unmap unmap_req;
        std::memset(&unmap_req, 0, sizeof(unmap_req));
        unmap_req.argsz = sizeof(unmap_req);
        unmap_req.iova = region.iova;
        unmap_req.size = region.len;
        int rc = ioctl(container_, VFIO_IOMMU_UNMAP_DMA, &unmap_req);
        munmap(region.ptr, region.len);
        return rc;
    }

    int container_ = -1;
    int group_ = -1;
    int device_ = -1;
    void *bar_ = nullptr;
    size_t bar_size_ = 0;
    uint32_t stride_ = 4;
    uint64_t dma_next_iova_ = kVfioIovaBase;
    std::unordered_map<void *, DmaRegion> regions_;
};

} // namespace

Backend *make_vfio_backend(const std::string &device_path, const std::string &group_path)
{
    return new VfioBackend(group_path, device_path);
}

} // namespace nvme

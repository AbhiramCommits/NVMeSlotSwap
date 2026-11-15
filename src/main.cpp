/*
 * main.cpp - NVMeSlotSwap command line interface.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "nvme/device.h"
#include "nvme/driver.hpp"

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

uint64_t now_ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
           static_cast<uint64_t>(ts.tv_nsec);
}

/*
 * Step-2 bootstrap: bring the controller to RDY and issue one hand-built
 * IDENTIFY through BAR0.  The full C++ driver facade replaces this in step 3;
 * the same sequence becomes tests/test_device.cpp.
 */
int bootstrap_identify(nvme_id_ctrl &out)
{
    nvme_dev_cfg cfg;
    nvme_dev_cfg_default(&cfg);
    nvme_dev_t *dev = nvme_dev_create(&cfg);
    if (dev == nullptr) {
        std::fprintf(stderr, "failed to create emulated controller\n");
        return 1;
    }
    volatile void *bar0 = nvme_dev_bar0(dev);
    uint64_t asq_iova = 0, acq_iova = 0, data_iova = 0;
    void *asq = nvme_dev_dma_map(dev, 32 * 64, &asq_iova);
    void *acq = nvme_dev_dma_map(dev, 32 * 16, &acq_iova);
    void *data = nvme_dev_dma_map(dev, 4096, &data_iova);
    std::memset(data, 0, 4096);

    nvme_mmio_write32(bar0, NVME_REG_AQA, (31u) | (31u << 16));
    nvme_mmio_write64(bar0, NVME_REG_ASQ, asq_iova);
    nvme_mmio_write64(bar0, NVME_REG_ACQ, acq_iova);
    nvme_mmio_write32(bar0, NVME_REG_CC,
                      NVME_CC_EN | (6u << NVME_CC_IOSQES_SHIFT) |
                          (4u << NVME_CC_IOCQES_SHIFT));

    uint64_t deadline = now_ns() + 2000000000ull;
    while ((nvme_mmio_read32(bar0, NVME_REG_CSTS) & NVME_CSTS_RDY) == 0) {
        if (now_ns() > deadline) {
            std::fprintf(stderr, "controller did not become ready\n");
            nvme_dev_destroy(dev);
            return 1;
        }
    }

    nvme_sqe sqe;
    std::memset(&sqe, 0, sizeof(sqe));
    sqe.opc = NVME_ADMIN_IDENTIFY;
    sqe.cid = 1;
    sqe.nsid = 0;
    sqe.prp1 = data_iova;
    sqe.cdw10 = NVME_ID_CNS_CONTROLLER;
    std::memcpy(static_cast<nvme_sqe *>(asq), &sqe, sizeof(sqe));
    __atomic_thread_fence(__ATOMIC_RELEASE);
    nvme_dbl_write(bar0, NVME_SQ_DBL(0, nvme_dev_doorbell_stride(dev)), 1);

    nvme_cqe *cq = static_cast<nvme_cqe *>(acq);
    while ((__atomic_load_n(&cq->status, __ATOMIC_ACQUIRE) & 1u) == 0) {
        if (now_ns() > deadline + 2000000000ull) {
            std::fprintf(stderr, "identify timed out\n");
            nvme_dev_destroy(dev);
            return 1;
        }
    }
    unsigned sc = nvme_cqe_sc(cq->status);
    if (sc != 0) {
        std::fprintf(stderr, "identify failed with status %u\n", sc);
        nvme_dev_destroy(dev);
        return 1;
    }
    std::memcpy(&out, data, sizeof(out));
    nvme_dev_destroy(dev);
    return 0;
}

int cmd_info()
{
    nvme_id_ctrl id;
    if (bootstrap_identify(id) != 0) {
        return 1;
    }
    std::printf("model: %.*s\n", 40, id.mn);
    std::printf("serial: %.*s\n", 20, id.sn);
    std::printf("firmware: %.*s\n", 8, id.fr);
    std::printf("namespaces: %u\n", id.nn);
    return 0;
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
    if (std::strcmp(cmd, "info") == 0) {
        return cmd_info();
    }
    std::fprintf(stderr, "nvmeslotswap: '%s' is not implemented yet\n", cmd);
    return 2;
}

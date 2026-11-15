/*
 * device.h - emulated NVMe controller: BAR0 window, DMA arena, admin/IO
 * command execution.
 *
 * The host driver only ever touches this object through:
 *   - nvme_dev_bar0()  : a byte-addressable register/doorbell window, and
 *   - nvme_dev_dma_map(): page-aligned shared memory whose IOVA is handed to
 *                         the controller in SQEs.
 * There is no back door into controller state from the driver.
 */
#ifndef NVME_DEVICE_H
#define NVME_DEVICE_H

#include <stddef.h>
#include <stdint.h>

#include "nvme/regs.h"
#include "nvme/spec.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nvme_dev nvme_dev_t;
struct fw_mgr;

enum nvme_latency_model {
    NVME_LAT_NONE = 0,
    NVME_LAT_UNIFORM = 1,     /* base + uniform(0, jitter)  */
    NVME_LAT_EXPONENTIAL = 2, /* base + exponential(jitter) */
};

struct nvme_dev_cfg {
    uint64_t ns_lba_count;      /* namespace size in 4 KiB LBAs          */
    uint32_t doorbell_stride;   /* bytes: 4 << CAP.DSTRD                 */
    uint32_t max_qid;           /* highest I/O queue id supported        */
    uint32_t io_latency_base_ns;
    uint32_t io_latency_jitter_ns;
    enum nvme_latency_model latency_model;
    uint32_t seed;              /* deterministic jitter/PRNG seed        */
    uint32_t enable_delay_us;   /* CC.EN 1 -> CSTS.RDY delay             */
    uint32_t reset_delay_us;    /* CC.EN 1 -> 0 completion delay         */
    uint32_t io_worker_threads; /* 0 => one executor per I/O queue pair  */
    size_t dma_arena_bytes;     /* shared "DMA" arena size               */
};

void nvme_dev_cfg_default(struct nvme_dev_cfg *cfg);

nvme_dev_t *nvme_dev_create(const struct nvme_dev_cfg *cfg);
void nvme_dev_destroy(nvme_dev_t *dev);

volatile void *nvme_dev_bar0(nvme_dev_t *dev);
uint32_t nvme_dev_bar0_size(const nvme_dev_t *dev);
uint32_t nvme_dev_doorbell_stride(const nvme_dev_t *dev);

/* Page-aligned shared memory; out_iova receives the device-visible address. */
void *nvme_dev_dma_map(nvme_dev_t *dev, size_t len, uint64_t *out_iova);
void nvme_dev_dma_unmap(nvme_dev_t *dev, void *ptr);

/* Quiesce window: stop fetching new SQEs for the given duration but let
 * every already-queued command complete. */
void nvme_dev_set_stall(nvme_dev_t *dev, uint64_t ns);

/* Fault injection for the post-activation self-test (CLI --fail-commit). */
void nvme_dev_set_self_test_fail(nvme_dev_t *dev, int fail);

uint32_t nvme_dev_csts(nvme_dev_t *dev);
uint64_t nvme_dev_ready_count(nvme_dev_t *dev); /* CC.EN 0->1 transitions */
uint64_t nvme_dev_reset_count(nvme_dev_t *dev);
uint64_t nvme_dev_last_activation_ns(nvme_dev_t *dev);

/* Persist/restore only the emulated firmware subsystem.  The CLI uses this so
 * that `fw stage`, `fw commit` and `fw list` observe the same emulated part
 * across process invocations.  Returns 0 on success, negative errno on error. */
int nvme_dev_save_fw_state(nvme_dev_t *dev, const char *path);
int nvme_dev_load_fw_state(nvme_dev_t *dev, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* NVME_DEVICE_H */

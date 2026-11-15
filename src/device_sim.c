/*
 * device_sim.c - (step 1 scaffold; implemented in step 2).
 */
#include "nvme/device.h"

#include <string.h>

void nvme_dev_cfg_default(struct nvme_dev_cfg *cfg) { memset(cfg, 0, sizeof(*cfg)); }

nvme_dev_t *nvme_dev_create(const struct nvme_dev_cfg *cfg)
{
    (void)cfg;
    return NULL;
}

void nvme_dev_destroy(nvme_dev_t *dev) { (void)dev; }

volatile void *nvme_dev_bar0(nvme_dev_t *dev)
{
    (void)dev;
    return NULL;
}

uint32_t nvme_dev_bar0_size(const nvme_dev_t *dev)
{
    (void)dev;
    return 0;
}

uint32_t nvme_dev_doorbell_stride(const nvme_dev_t *dev)
{
    (void)dev;
    return 4;
}

void *nvme_dev_dma_map(nvme_dev_t *dev, size_t len, uint64_t *out_iova)
{
    (void)dev;
    (void)len;
    (void)out_iova;
    return NULL;
}

void nvme_dev_dma_unmap(nvme_dev_t *dev, void *ptr)
{
    (void)dev;
    (void)ptr;
}

void nvme_dev_set_stall(nvme_dev_t *dev, uint64_t ns) { (void)dev; (void)ns; }
void nvme_dev_set_self_test_fail(nvme_dev_t *dev, int fail) { (void)dev; (void)fail; }

uint32_t nvme_dev_csts(nvme_dev_t *dev)
{
    (void)dev;
    return 0;
}

uint64_t nvme_dev_ready_count(nvme_dev_t *dev) { (void)dev; return 0; }
uint64_t nvme_dev_reset_count(nvme_dev_t *dev) { (void)dev; return 0; }
uint64_t nvme_dev_last_activation_ns(nvme_dev_t *dev) { (void)dev; return 0; }

int nvme_dev_save_fw_state(nvme_dev_t *dev, const char *path)
{
    (void)dev;
    (void)path;
    return -1;
}

int nvme_dev_load_fw_state(nvme_dev_t *dev, const char *path)
{
    (void)dev;
    (void)path;
    return -1;
}

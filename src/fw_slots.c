/*
 * fw_slots.c - (step 1 scaffold; implemented in step 4).
 */
#include "nvme/fw.h"

#include <stdlib.h>
#include <string.h>

void fw_mgr_init(struct fw_mgr *fw) { memset(fw, 0, sizeof(*fw)); }
void fw_mgr_free(struct fw_mgr *fw) { (void)fw; }

uint16_t fw_mgr_download_chunk(struct fw_mgr *fw, uint32_t ofst, const void *data,
                               uint32_t len)
{
    (void)fw;
    (void)ofst;
    (void)data;
    (void)len;
    return NVME_SC_INVALID_FIELD;
}

struct fw_commit_result fw_mgr_commit(struct fw_mgr *fw, uint8_t slot, uint8_t action,
                                      int self_test_fail)
{
    struct fw_commit_result r;
    (void)fw;
    (void)slot;
    (void)action;
    (void)self_test_fail;
    memset(&r, 0, sizeof(r));
    r.status = NVME_SC_INVALID_FIELD;
    return r;
}

void fw_mgr_apply_pending(struct fw_mgr *fw) { (void)fw; }
void fw_mgr_read_log(const struct fw_mgr *fw, struct nvme_fw_slot_log *out)
{
    (void)fw;
    memset(out, 0, sizeof(*out));
}
const char *fw_mgr_active_revision(const struct fw_mgr *fw) { (void)fw; return ""; }
void fw_mgr_revision(const struct nvme_fw_slot *slot, char out[9])
{
    (void)slot;
    out[0] = '\0';
}

int fw_mgr_save(const struct fw_mgr *fw, const char *path)
{
    (void)fw;
    (void)path;
    return -1;
}

int fw_mgr_load(struct fw_mgr *fw, const char *path)
{
    (void)fw;
    (void)path;
    return -1;
}

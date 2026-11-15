/* fw.h - device-side firmware slot manager (A/B slots, sha256, rollback). */
#ifndef NVME_FW_H
#define NVME_FW_H

#include <stdint.h>

#include "nvme/spec.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NVME_FW_SLOT_COUNT 7                /* slots 1..7 */
#define NVME_FW_SLOT1 1u
#define NVME_FW_MAX_IMAGE_BYTES (4u * 1024u * 1024u)
#define NVME_FW_MAX_CHUNK_BYTES (64u * 1024u)

/* Image header written by FirmwareUpdater::build_image (little endian). */
struct nvme_fw_image_hdr {
    uint8_t magic[4];    /* 'N','V','F','W'            */
    uint32_t version;    /* header format version (1)  */
    char revision[8];    /* space padded ASCII         */
    uint32_t payload_len;
    uint8_t sha256[32];  /* over the payload only      */
};
NVME_STATIC_ASSERT(sizeof(struct nvme_fw_image_hdr) == 52, "firmware image header layout");

#define NVME_FW_IMAGE_VERSION 1u

struct nvme_fw_slot {
    int occupied;
    int read_only;
    char revision[8];
    uint8_t sha256[32];
    uint32_t size;
    uint8_t *image; /* payload bytes, malloc'd */
};

struct fw_mgr {
    struct nvme_fw_slot slots[NVME_FW_SLOT_COUNT];
    uint8_t active_slot; /* 1..7 */
    uint8_t next_slot;   /* 1..7 */
    uint8_t *staging;
    uint32_t staging_len;
    uint32_t staging_cap;
    int self_test_fail; /* injected post-activation self-test failure */
    uint64_t activation_count;
    uint64_t rollback_count;
};

struct fw_commit_result {
    uint16_t status;
    int activated;   /* active slot changed */
    int rolled_back; /* post-activation self test failed */
    int pending;     /* activation will happen on the next reset */
    uint8_t active_slot;
    uint8_t next_slot;
};

void fw_mgr_init(struct fw_mgr *fw);
void fw_mgr_free(struct fw_mgr *fw);

/* FW Image Download: ofst is in bytes, len must be a multiple of 4. */
uint16_t fw_mgr_download_chunk(struct fw_mgr *fw, uint32_t ofst, const void *data,
                               uint32_t len);

/* FW Commit.  action is NVME_FW_CA_*; slot is 1..7. */
struct fw_commit_result fw_mgr_commit(struct fw_mgr *fw, uint8_t slot, uint8_t action,
                                      int self_test_fail);

/* Applies a pending activation (next_slot) as part of a controller reset. */
void fw_mgr_apply_pending(struct fw_mgr *fw);

void fw_mgr_read_log(const struct fw_mgr *fw, struct nvme_fw_slot_log *out);
const char *fw_mgr_active_revision(const struct fw_mgr *fw);
void fw_mgr_revision(const struct nvme_fw_slot *slot, char out[9]);

/* Emulated-device state persistence, used by the CLI so that separate
 * invocations observe the same emulated controller.  Returns 0 on success. */
int fw_mgr_save(const struct fw_mgr *fw, const char *path);
int fw_mgr_load(struct fw_mgr *fw, const char *path);

typedef struct nvme_fw_image_hdr nvme_fw_image_hdr;
typedef struct nvme_fw_slot nvme_fw_slot;
typedef struct fw_mgr fw_mgr;

#ifdef __cplusplus
}
#endif

#endif /* NVME_FW_H */

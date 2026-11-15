/*
 * fw_slots.c - device-side firmware slot manager.
 *
 * Seven slots, slot 1 read-only (mirrors the FRMW bit real parts report).
 * Images arrive through FW Image Download (0x11) in strictly sequential
 * chunks; FW Commit (0x10) validates the image header and payload SHA-256
 * before touching a slot.  CA=3 activates immediately inside a quiesce window
 * supplied by the controller; a failed post-activation self test restores the
 * previous active slot.
 */
#include "nvme/fw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sha256.h"

#define FW_STATE_MAGIC 0x5346574Eu /* "NFWS" little endian */
#define FW_STATE_VERSION 1u

static void revision_pad(char dst[8], const char* src) {
    size_t n = strlen(src);
    if (n > 8) {
        n = 8;
    }
    memcpy(dst, src, n);
    for (size_t i = n; i < 8; ++i) {
        dst[i] = ' ';
    }
}

void fw_mgr_revision(const struct nvme_fw_slot* slot, char out[9]) {
    size_t n = 8;
    if (slot == NULL || !slot->occupied) {
        out[0] = '\0';
        return;
    }
    while (n > 0 && (slot->revision[n - 1] == ' ' || slot->revision[n - 1] == '\0')) {
        --n;
    }
    memcpy(out, slot->revision, n);
    out[n] = '\0';
}

static void refresh_active_revision(struct fw_mgr* fw) {
    struct nvme_fw_slot* s = &fw->slots[fw->active_slot - 1];
    fw_mgr_revision(s, fw->active_revision);
}

void fw_mgr_init(struct fw_mgr* fw) {
    memset(fw, 0, sizeof(*fw));
    fw->active_slot = NVME_FW_SLOT1;
    fw->next_slot = NVME_FW_SLOT1;
    fw->self_test_fail = 0;

    struct nvme_fw_slot* s = &fw->slots[NVME_FW_SLOT1 - 1];
    s->occupied = 1;
    s->read_only = 1;
    revision_pad(s->revision, "FW1.0.0");
    s->size = 4096;
    s->image = (uint8_t*)calloc(1, s->size);
    if (s->image != NULL) {
        sha256(s->image, s->size, s->sha256);
    }
    refresh_active_revision(fw);
}

void fw_mgr_free(struct fw_mgr* fw) {
    if (fw == NULL) {
        return;
    }
    for (unsigned i = 0; i < NVME_FW_SLOT_COUNT; ++i) {
        free(fw->slots[i].image);
        fw->slots[i].image = NULL;
    }
    free(fw->staging);
    fw->staging = NULL;
    fw->staging_len = 0;
    fw->staging_cap = 0;
}

static int fw_mgr_reserve(struct fw_mgr* fw, uint32_t need) {
    if (need <= fw->staging_cap) {
        return 0;
    }
    uint32_t cap = fw->staging_cap ? fw->staging_cap : 65536u;
    while (cap < need && cap < NVME_FW_MAX_IMAGE_BYTES) {
        cap *= 2u;
    }
    if (cap > NVME_FW_MAX_IMAGE_BYTES) {
        cap = NVME_FW_MAX_IMAGE_BYTES;
    }
    if (cap < need) {
        return -1;
    }
    uint8_t* p = (uint8_t*)realloc(fw->staging, cap);
    if (p == NULL) {
        return -1;
    }
    fw->staging = p;
    fw->staging_cap = cap;
    return 0;
}

uint16_t fw_mgr_download_chunk(struct fw_mgr* fw, uint32_t ofst, const void* data, uint32_t len) {
    if (len == 0 || (len & 3u) != 0 || data == NULL) {
        return NVME_SC_INVALID_FIELD;
    }
    /* An offset of 0 starts a fresh image, discarding any partial download.
     * Other offsets must follow on exactly: overlapping or out-of-order
     * chunks are rejected. */
    if (ofst == 0) {
        fw->staging_len = 0;
    }
    if (ofst != fw->staging_len) {
        return NVME_SC_INVALID_FIELD;
    }
    if ((uint64_t)ofst + len > NVME_FW_MAX_IMAGE_BYTES) {
        return NVME_SC_INVALID_FIELD;
    }
    if (fw_mgr_reserve(fw, ofst + len) != 0) {
        return NVME_SC_INTERNAL;
    }
    memcpy(fw->staging + ofst, data, len);
    fw->staging_len = ofst + len;
    return NVME_SC_SUCCESS;
}

static void discard_staging(struct fw_mgr* fw) {
    fw->staging_len = 0;
}

static int payload_contains(const uint8_t* p, size_t len, const char* needle) {
    size_t n = strlen(needle);
    if (n == 0 || len < n) {
        return 0;
    }
    for (size_t i = 0; i + n <= len; ++i) {
        if (memcmp(p + i, needle, n) == 0) {
            return 1;
        }
    }
    return 0;
}

static int validate_staged_image(struct fw_mgr* fw, struct nvme_fw_image_hdr* hdr) {
    if (fw->staging == NULL || fw->staging_len < sizeof(*hdr)) {
        return 0;
    }
    memcpy(hdr, fw->staging, sizeof(*hdr));
    if (memcmp(hdr->magic, "NVFW", 4) != 0 || hdr->version != NVME_FW_IMAGE_VERSION) {
        return 0;
    }
    if (hdr->payload_len > NVME_FW_MAX_IMAGE_BYTES - sizeof(*hdr)) {
        return 0;
    }
    if ((uint64_t)fw->staging_len != (uint64_t)sizeof(*hdr) + hdr->payload_len) {
        return 0;
    }
    uint8_t digest[32];
    sha256(fw->staging + sizeof(*hdr), hdr->payload_len, digest);
    if (memcmp(digest, hdr->sha256, 32) != 0) {
        return 0;
    }
    return 1;
}

static int install_slot(struct fw_mgr* fw, uint8_t slot, const struct nvme_fw_image_hdr* hdr) {
    if (slot < 1 || slot > NVME_FW_SLOT_COUNT) {
        return -1;
    }
    struct nvme_fw_slot* s = &fw->slots[slot - 1];
    uint8_t* image = NULL;
    if (hdr->payload_len > 0) {
        image = (uint8_t*)malloc(hdr->payload_len);
        if (image == NULL) {
            return -1;
        }
        memcpy(image, fw->staging + sizeof(*hdr), hdr->payload_len);
    }
    free(s->image);
    s->image = image;
    s->occupied = 1;
    s->read_only = 0;
    s->size = hdr->payload_len;
    memcpy(s->revision, hdr->revision, 8);
    memcpy(s->sha256, hdr->sha256, 32);
    return 0;
}

uint16_t fw_mgr_prepare(struct fw_mgr* fw, uint8_t slot, uint8_t action) {
    if (slot < 1 || slot > NVME_FW_SLOT_COUNT) {
        return NVME_SC_FW_SLOT_INVALID;
    }
    if (action > (uint8_t)NVME_FW_CA_REPLACE_ACTIVATE_IMMEDIATE) {
        return NVME_SC_INVALID_FIELD;
    }

    if (action == (uint8_t)NVME_FW_CA_ACTIVATE) {
        return fw->slots[slot - 1].occupied ? NVME_SC_SUCCESS : NVME_SC_FW_SLOT_INVALID;
    }

    if (slot == NVME_FW_SLOT1) {
        /* Slot 1 is read-only: FRMW bit 0 advertises it and the controller
         * refuses to replace it. */
        return NVME_SC_FW_ACTIVATION_PROHIBITED;
    }

    struct nvme_fw_image_hdr hdr;
    if (!validate_staged_image(fw, &hdr)) {
        discard_staging(fw);
        return NVME_SC_FW_IMAGE_INVALID;
    }
    if (install_slot(fw, slot, &hdr) != 0) {
        discard_staging(fw);
        return NVME_SC_INTERNAL;
    }
    discard_staging(fw);
    return NVME_SC_SUCCESS;
}

struct fw_commit_result fw_mgr_apply(struct fw_mgr* fw, uint8_t slot, uint8_t action,
                                     int self_test_fail) {
    struct fw_commit_result r;
    memset(&r, 0, sizeof(r));
    r.active_slot = fw->active_slot;
    r.next_slot = fw->next_slot;

    if (slot < 1 || slot > NVME_FW_SLOT_COUNT) {
        r.status = NVME_SC_FW_SLOT_INVALID;
        return r;
    }
    if (action > (uint8_t)NVME_FW_CA_REPLACE_ACTIVATE_IMMEDIATE) {
        r.status = NVME_SC_INVALID_FIELD;
        return r;
    }
    if (action == (uint8_t)NVME_FW_CA_ACTIVATE) {
        if (!fw->slots[slot - 1].occupied) {
            r.status = NVME_SC_FW_SLOT_INVALID;
            return r;
        }
        fw->next_slot = slot;
        r.next_slot = slot;
        r.pending = 1;
        /* Activation waits for a reset; contrast with CA=3. */
        r.status = NVME_SC_FW_ACTIVATION_REQUIRES_RESET;
        return r;
    }

    if (action == (uint8_t)NVME_FW_CA_REPLACE) {
        r.status = NVME_SC_SUCCESS;
        return r;
    }
    if (action == (uint8_t)NVME_FW_CA_REPLACE_ACTIVATE) {
        fw->next_slot = slot;
        r.next_slot = slot;
        r.pending = 1;
        r.status = NVME_SC_SUCCESS;
        return r;
    }

    /* CA=3: activate immediately.  This is the only step that runs inside the
     * controller's quiesce window, and it is a pointer/string update. */
    uint8_t prev = fw->active_slot;
    fw->active_slot = slot;
    fw->next_slot = slot;
    refresh_active_revision(fw);

    {
        const struct nvme_fw_slot* s = &fw->slots[slot - 1];
        if (self_test_fail || payload_contains((const uint8_t*)s->revision, 8, "BAD")) {
            /* Post-activation self test failed: put the previous slot back. */
            fw->active_slot = prev;
            fw->next_slot = prev;
            refresh_active_revision(fw);
            fw->rollback_count++;
            r.active_slot = fw->active_slot;
            r.next_slot = fw->next_slot;
            r.rolled_back = 1;
            r.status = NVME_SC_FW_IMAGE_INVALID;
            return r;
        }
    }

    fw->activation_count++;
    r.activated = 1;
    r.active_slot = fw->active_slot;
    r.next_slot = fw->next_slot;
    r.status = NVME_SC_SUCCESS;
    return r;
}

struct fw_commit_result fw_mgr_commit(struct fw_mgr* fw, uint8_t slot, uint8_t action,
                                      int self_test_fail) {
    uint16_t st = fw_mgr_prepare(fw, slot, action);
    if (st != NVME_SC_SUCCESS) {
        struct fw_commit_result r;
        memset(&r, 0, sizeof(r));
        r.status = st;
        r.active_slot = fw->active_slot;
        r.next_slot = fw->next_slot;
        return r;
    }
    return fw_mgr_apply(fw, slot, action, self_test_fail);
}

void fw_mgr_apply_pending(struct fw_mgr* fw) {
    if (fw->next_slot >= 1 && fw->next_slot <= NVME_FW_SLOT_COUNT &&
        fw->next_slot != fw->active_slot && fw->slots[fw->next_slot - 1].occupied) {
        fw->active_slot = fw->next_slot;
        refresh_active_revision(fw);
        fw->activation_count++;
    }
    fw->next_slot = fw->active_slot;
}

void fw_mgr_read_log(const struct fw_mgr* fw, struct nvme_fw_slot_log* out) {
    memset(out, 0, sizeof(*out));
    out->afi = nvme_afi_encode(fw->active_slot, fw->next_slot);
    for (unsigned i = 0; i < NVME_FW_SLOT_COUNT; ++i) {
        const struct nvme_fw_slot* s = &fw->slots[i];
        for (unsigned j = 0; j < 8; ++j) {
            out->frs[i][j] = s->occupied ? s->revision[j] : ' ';
        }
    }
}

const char* fw_mgr_active_revision(const struct fw_mgr* fw) {
    return fw->active_revision;
}

/* ------------------------------------------------------------------ */
/* Emulated-device state persistence (CLI convenience)                  */
/* ------------------------------------------------------------------ */
static int write_u32(FILE* f, uint32_t v) {
    return fwrite(&v, sizeof(v), 1, f) == 1 ? 0 : -1;
}
static int read_u32(FILE* f, uint32_t* v) {
    return fread(v, sizeof(*v), 1, f) == 1 ? 0 : -1;
}

int fw_mgr_save(const struct fw_mgr* fw, const char* path) {
    FILE* f = fopen(path, "wb");
    if (f == NULL) {
        return -1;
    }
    uint32_t magic = FW_STATE_MAGIC;
    uint32_t version = FW_STATE_VERSION;
    uint8_t active = fw->active_slot;
    uint8_t next = fw->next_slot;
    uint8_t fail = (uint8_t)(fw->self_test_fail ? 1 : 0);
    uint8_t pad[5] = {0, 0, 0, 0, 0};
    int ok = 1;

    ok &= write_u32(f, magic) == 0;
    ok &= write_u32(f, version) == 0;
    ok &= fwrite(&active, 1, 1, f) == 1;
    ok &= fwrite(&next, 1, 1, f) == 1;
    ok &= fwrite(&fail, 1, 1, f) == 1;
    ok &= fwrite(pad, 1, sizeof(pad), f) == sizeof(pad);
    ok &= fwrite(&fw->activation_count, sizeof(uint64_t), 1, f) == 1;
    ok &= fwrite(&fw->rollback_count, sizeof(uint64_t), 1, f) == 1;
    ok &= write_u32(f, fw->staging_len) == 0;

    for (unsigned i = 0; i < NVME_FW_SLOT_COUNT; ++i) {
        const struct nvme_fw_slot* s = &fw->slots[i];
        uint8_t flags = (uint8_t)((s->occupied ? 1 : 0) | (s->read_only ? 2 : 0));
        uint32_t size = s->size;
        ok &= fwrite(&flags, 1, 1, f) == 1;
        ok &= fwrite(s->revision, 1, 8, f) == 8;
        ok &= fwrite(s->sha256, 1, 32, f) == 32;
        ok &= write_u32(f, size) == 0;
    }
    for (unsigned i = 0; i < NVME_FW_SLOT_COUNT; ++i) {
        const struct nvme_fw_slot* s = &fw->slots[i];
        if (s->occupied && s->size > 0) {
            ok &= fwrite(s->image, 1, s->size, f) == s->size;
        }
    }
    if (fw->staging_len > 0) {
        ok &= fwrite(fw->staging, 1, fw->staging_len, f) == fw->staging_len;
    }
    if (fclose(f) != 0) {
        ok = 0;
    }
    return ok ? 0 : -1;
}

int fw_mgr_load(struct fw_mgr* fw, const char* path) {
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
    uint32_t magic = 0, version = 0;
    uint8_t active = 0, next = 0, fail = 0;
    uint8_t pad[5];
    uint64_t activations = 0, rollbacks = 0;
    uint32_t staging_len = 0;

    if (read_u32(f, &magic) != 0 || read_u32(f, &version) != 0 || magic != FW_STATE_MAGIC ||
        version != FW_STATE_VERSION) {
        fclose(f);
        return -1;
    }
    if (fread(&active, 1, 1, f) != 1 || fread(&next, 1, 1, f) != 1 || fread(&fail, 1, 1, f) != 1 ||
        fread(pad, 1, sizeof(pad), f) != sizeof(pad) ||
        fread(&activations, sizeof(uint64_t), 1, f) != 1 ||
        fread(&rollbacks, sizeof(uint64_t), 1, f) != 1 || read_u32(f, &staging_len) != 0) {
        fclose(f);
        return -1;
    }
    if (active < 1 || active > NVME_FW_SLOT_COUNT || next < 1 || next > NVME_FW_SLOT_COUNT ||
        staging_len > NVME_FW_MAX_IMAGE_BYTES) {
        fclose(f);
        return -1;
    }

    struct nvme_fw_slot slots[NVME_FW_SLOT_COUNT];
    memset(slots, 0, sizeof(slots));
    for (unsigned i = 0; i < NVME_FW_SLOT_COUNT; ++i) {
        uint8_t flags = 0;
        uint32_t size = 0;
        if (fread(&flags, 1, 1, f) != 1 || fread(slots[i].revision, 1, 8, f) != 8 ||
            fread(slots[i].sha256, 1, 32, f) != 32 || read_u32(f, &size) != 0) {
            fclose(f);
            return -1;
        }
        slots[i].occupied = (flags & 1u) != 0;
        slots[i].read_only = (flags & 2u) != 0;
        slots[i].size = size;
        if (size > NVME_FW_MAX_IMAGE_BYTES) {
            fclose(f);
            return -1;
        }
    }
    for (unsigned i = 0; i < NVME_FW_SLOT_COUNT; ++i) {
        if (slots[i].occupied && slots[i].size > 0) {
            slots[i].image = (uint8_t*)malloc(slots[i].size);
            if (slots[i].image == NULL ||
                fread(slots[i].image, 1, slots[i].size, f) != slots[i].size) {
                for (unsigned j = 0; j < NVME_FW_SLOT_COUNT; ++j) {
                    free(slots[j].image);
                }
                fclose(f);
                return -1;
            }
        }
    }

    uint8_t* staging = NULL;
    if (staging_len > 0) {
        staging = (uint8_t*)malloc(staging_len);
        if (staging == NULL || fread(staging, 1, staging_len, f) != staging_len) {
            free(staging);
            for (unsigned j = 0; j < NVME_FW_SLOT_COUNT; ++j) {
                free(slots[j].image);
            }
            fclose(f);
            return -1;
        }
    }
    fclose(f);

    for (unsigned i = 0; i < NVME_FW_SLOT_COUNT; ++i) {
        free(fw->slots[i].image);
        fw->slots[i] = slots[i];
    }
    free(fw->staging);
    fw->staging = staging;
    fw->staging_len = staging_len;
    fw->staging_cap = staging_len;
    fw->active_slot = active;
    fw->next_slot = next;
    fw->self_test_fail = fail ? 1 : 0;
    fw->activation_count = activations;
    fw->rollback_count = rollbacks;
    refresh_active_revision(fw);
    return 0;
}

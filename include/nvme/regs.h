/*
 * regs.h - NVMe controller register map (BAR0) and MMIO access helpers.
 *
 * BAR0 layout (NVMe 1.4 section 3.1):
 *   0x0000 CAP   0x0008 VS    0x000C INTMS  0x0010 INTMC
 *   0x0014 CC    0x001C CSTS  0x0020 NSSR  0x0024 AQA
 *   0x0028 ASQ   0x0030 ACQ
 *   0x1000.. doorbell array, 2 doorbells per queue id:
 *           SQ0TDBL, CQ0HDBL, SQ1TDBL, CQ1HDBL, ...
 *
 * The emulated controller keeps BAR0 in ordinary host memory.  The driver
 * therefore accesses registers with the atomic helpers below, which gives
 * the same single-copy semantics the MMIO path provides on real silicon:
 * every access goes through the BAR0 window and nothing reads controller
 * private state directly.  on the VFIO path these helpers become plain
 * loads/stores to a mapped BAR.
 */
#ifndef NVME_REGS_H
#define NVME_REGS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Register offsets. */
#define NVME_REG_CAP 0x0000u
#define NVME_REG_VS 0x0008u
#define NVME_REG_INTMS 0x000Cu
#define NVME_REG_INTMC 0x0010u
#define NVME_REG_CC 0x0014u
#define NVME_REG_CSTS 0x001Cu
#define NVME_REG_NSSR 0x0020u
#define NVME_REG_AQA 0x0024u
#define NVME_REG_ASQ 0x0028u
#define NVME_REG_ACQ 0x0030u

#define NVME_DOORBELL_BASE 0x1000u

/* Doorbell offsets: 2 doorbells per queue id, stride = 4 << CAP.DSTRD. */
#define NVME_SQ_DBL(qid, stride) (0x1000u + (2u * (qid)) * (stride))
#define NVME_CQ_DBL(qid, stride) (0x1000u + (2u * (qid) + 1u) * (stride))

/* CAP fields. */
#define NVME_CAP_MQES_SHIFT 0u
#define NVME_CAP_MQES_MASK 0xFFFFu
#define NVME_CAP_CQR (1u << 16)
#define NVME_CAP_TO_SHIFT 24u
#define NVME_CAP_TO_MASK 0xFF000000u
#define NVME_CAP_DSTRD_SHIFT 32u
#define NVME_CAP_DSTRD_MASK 0x0000000Fu
#define NVME_CAP_NSSRS (1u << 36)
#define NVME_CAP_CSS_SHIFT 37u
#define NVME_CAP_MPSMIN_SHIFT 48u

/* CC bits. */
#define NVME_CC_EN (1u << 0)
#define NVME_CC_CSS_SHIFT 4u
#define NVME_CC_CSS_MASK 0x00000070u
#define NVME_CC_MPS_SHIFT 7u
#define NVME_CC_MPS_MASK 0x00000780u
#define NVME_CC_AMS_SHIFT 11u
#define NVME_CC_AMS_MASK 0x00003800u
#define NVME_CC_SHN_SHIFT 14u
#define NVME_CC_SHN_MASK 0x0000C000u
#define NVME_CC_IOSQES_SHIFT 16u
#define NVME_CC_IOSQES_MASK 0x000F0000u
#define NVME_CC_IOCQES_SHIFT 20u
#define NVME_CC_IOCQES_MASK 0x00F00000u

#define NVME_CC_SHN_NONE 0u
#define NVME_CC_SHN_NORMAL 1u
#define NVME_CC_SHN_ABRUPT 2u

/* CSTS bits. */
#define NVME_CSTS_RDY (1u << 0)
#define NVME_CSTS_CFS (1u << 1)
#define NVME_CSTS_SHST_SHIFT 2u
#define NVME_CSTS_SHST_MASK 0x0000000Cu
#define NVME_CSTS_SHST_NORMAL 0u
#define NVME_CSTS_SHST_COMPLETE 2u
#define NVME_CSTS_PP (1u << 4)

/* NSSR bits. */
#define NVME_NSSR_NSSR (1u << 0)

/*
 * MMIO helpers.
 *
 * The host side must never read a doorbell back: doorbells are write-only
 * registers on real hardware and reading them can raise a fatal error in
 * some controllers.  Only the device model polls them.
 */
static inline uint32_t nvme_mmio_read32(const volatile void* bar0, uint32_t off) {
    const uint32_t* p = (const uint32_t*)((const volatile char*)bar0 + off);
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static inline void nvme_mmio_write32(volatile void* bar0, uint32_t off, uint32_t val) {
    uint32_t* p = (uint32_t*)((volatile char*)bar0 + off);
    __atomic_store_n(p, val, __ATOMIC_RELEASE);
}

static inline uint64_t nvme_mmio_read64(const volatile void* bar0, uint32_t off) {
    const uint64_t* p = (const uint64_t*)((const volatile char*)bar0 + off);
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static inline void nvme_mmio_write64(volatile void* bar0, uint32_t off, uint64_t val) {
    uint64_t* p = (uint64_t*)((volatile char*)bar0 + off);
    __atomic_store_n(p, val, __ATOMIC_RELEASE);
}

/* Doorbell helpers.  The store must follow a release fence (see queue.c). */
static inline void nvme_dbl_write(volatile void* bar0, uint32_t off, uint32_t val) {
    uint32_t* p = (uint32_t*)((volatile char*)bar0 + off);
    __atomic_store_n(p, val, __ATOMIC_RELEASE);
}

static inline uint32_t nvme_dbl_read(const volatile void* bar0, uint32_t off) {
    const uint32_t* p = (const uint32_t*)((const volatile char*)bar0 + off);
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

/* CAP decoding helpers. */
static inline uint32_t nvme_cap_dstrd(uint64_t cap) {
    return (uint32_t)((cap >> NVME_CAP_DSTRD_SHIFT) & NVME_CAP_DSTRD_MASK);
}
static inline uint32_t nvme_cap_mqes(uint64_t cap) {
    return (uint32_t)((cap >> NVME_CAP_MQES_SHIFT) & NVME_CAP_MQES_MASK);
}

#ifdef __cplusplus
}
#endif

#endif /* NVME_REGS_H */

/*
 * spec.h - NVMe base-specification shaped types used by NVMeSlotSwap.
 *
 * The structures below mirror the on-the-wire/on-the-bar layout of the NVMe
 * 1.4 base specification closely enough to exercise real mechanics: 64-byte
 * submission queue entries, 16-byte completion queue entries with the phase
 * tag in bit 0 of DW3, the admin/IO opcodes used by the project, and the
 * Identify Controller / Identify Namespace / Firmware Slot Information log
 * page payloads.
 */
#ifndef NVME_SPEC_H
#define NVME_SPEC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__cplusplus)
#define NVME_STATIC_ASSERT(cond, msg) static_assert((cond), msg)
#else
#define NVME_STATIC_ASSERT(cond, msg) _Static_assert((cond), msg)
#endif

#define NVME_ADMIN_QID 0u
#define NVME_MAX_QID 0xFFFFu

#define NVME_LBA_SHIFT 12u
#define NVME_LBA_SIZE 4096u

/* ------------------------------------------------------------------ */
/* Submission Queue Entry - 64 bytes (NVMe base spec, Figure 78).       */
/* ------------------------------------------------------------------ */
struct nvme_sqe {
    uint8_t opc;      /* bits 7:0   opcode                       */
    uint8_t flags;    /* bits 1:0 fuse, bits 7:6 psdt            */
    uint16_t cid;     /* command identifier                      */
    uint32_t nsid;    /* namespace identifier                    */
    uint32_t cdw2;    /* command dword 2                         */
    uint32_t cdw3;    /* command dword 3                         */
    uint64_t mptr;    /* metadata pointer                        */
    uint64_t prp1;    /* physical region page 1                  */
    uint64_t prp2;    /* physical region page 2 (or PRP list)    */
    uint32_t cdw10;   /* command dword 10                        */
    uint32_t cdw11;   /* command dword 11                        */
    uint32_t cdw12;   /* command dword 12                        */
    uint32_t cdw13;   /* command dword 13                        */
    uint32_t cdw14;   /* command dword 14                        */
    uint32_t cdw15;   /* command dword 15                        */
};
NVME_STATIC_ASSERT(sizeof(struct nvme_sqe) == 64, "NVMe SQE must be 64 bytes");

#define NVME_SQE_FUSE_MASK 0x03u
#define NVME_SQE_PSDT_MASK 0xC0u

/* ------------------------------------------------------------------ */
/* Completion Queue Entry - 16 bytes (NVMe base spec, Figure 79).       */
/*                                                                      */
/* DW3 layout: bit 0 phase tag, bits 8:1 status code, bits 11:9 status  */
/* code type, bits 13:12 CRD, bit 14 more, bit 15 do-not-retry.         */
/* ------------------------------------------------------------------ */
struct nvme_cqe {
    uint32_t cdw0;   /* command specific                        */
    uint32_t rsvd;   /* reserved                                */
    uint16_t sqhd;   /* submission queue head pointer           */
    uint16_t sqid;   /* submission queue identifier             */
    uint16_t cid;    /* command identifier                      */
    uint16_t status; /* phase tag + status field                */
};
NVME_STATIC_ASSERT(sizeof(struct nvme_cqe) == 16, "NVMe CQE must be 16 bytes");

#define NVME_CQE_PHASE_MASK 0x0001u
#define NVME_CQE_SC_MASK 0x00FEu
#define NVME_CQE_SCT_MASK 0x0E00u
#define NVME_CQE_DNR 0x4000u

static inline uint16_t nvme_cqe_status_encode(unsigned phase, unsigned sc, unsigned sct,
                                              int dnr)
{
    return (uint16_t)(((phase & 1u) << 0) | ((sc & 0xFFu) << 1) | ((sct & 0x7u) << 9) |
                      (dnr ? NVME_CQE_DNR : 0u));
}
static inline unsigned nvme_cqe_phase(uint16_t status) { return status & 1u; }
static inline unsigned nvme_cqe_sc(uint16_t status) { return (status >> 1) & 0xFFu; }
static inline unsigned nvme_cqe_sct(uint16_t status) { return (status >> 9) & 0x7u; }
static inline int nvme_cqe_dnr(uint16_t status) { return (status & NVME_CQE_DNR) != 0; }

/* ------------------------------------------------------------------ */
/* Admin opcodes (subset exercised by this project).                    */
/* ------------------------------------------------------------------ */
enum nvme_admin_opc {
    NVME_ADMIN_DELETE_SQ = 0x00,
    NVME_ADMIN_CREATE_SQ = 0x01,
    NVME_ADMIN_GET_LOG_PAGE = 0x02,
    NVME_ADMIN_DELETE_CQ = 0x04,
    NVME_ADMIN_CREATE_CQ = 0x05,
    NVME_ADMIN_IDENTIFY = 0x06,
    NVME_ADMIN_ABORT = 0x08,
    NVME_ADMIN_SET_FEATURES = 0x09,
    NVME_ADMIN_GET_FEATURES = 0x0A,
    NVME_ADMIN_ASYNC_EVENT = 0x0C,
    NVME_ADMIN_FW_COMMIT = 0x10,
    NVME_ADMIN_FW_IMAGE_DOWNLOAD = 0x11,
};

enum nvme_io_opc {
    NVME_IO_FLUSH = 0x00,
    NVME_IO_WRITE = 0x01,
    NVME_IO_READ = 0x02,
};

/* Identify CNS (Controller or Namespace Structure) values. */
enum nvme_identify_cns {
    NVME_ID_CNS_NAMESPACE = 0x00,
    NVME_ID_CNS_CONTROLLER = 0x01,
    NVME_ID_CNS_ACTIVE_NS_LIST = 0x02,
    NVME_ID_CNS_NS_ID_DESCRIPTORS = 0x03,
};

/* Log page identifiers. */
enum nvme_log_lid {
    NVME_LOG_ERROR = 0x01,
    NVME_LOG_SMART = 0x02,
    NVME_LOG_FW_SLOT = 0x03,
};

/* Feature identifiers. */
enum nvme_feature_id {
    NVME_FEAT_ARBITRATION = 0x01,
    NVME_FEAT_POWER_MGMT = 0x02,
    NVME_FEAT_TEMP_THRESH = 0x04,
    NVME_FEAT_NUM_QUEUES = 0x07,
};

/* Firmware commit actions (CDW10.CA). */
enum nvme_fw_commit_action {
    NVME_FW_CA_REPLACE = 0,
    NVME_FW_CA_REPLACE_ACTIVATE = 1,
    NVME_FW_CA_ACTIVATE = 2,
    NVME_FW_CA_REPLACE_ACTIVATE_IMMEDIATE = 3,
};

/* ------------------------------------------------------------------ */
/* Status codes actually returned by the device model.                  */
/* ------------------------------------------------------------------ */
enum nvme_status {
    NVME_SC_SUCCESS = 0x00,
    NVME_SC_INVALID_OPCODE = 0x01,
    NVME_SC_INVALID_FIELD = 0x02,
    NVME_SC_INTERNAL = 0x06,
    NVME_SC_FW_SLOT_INVALID = 0x06,
    NVME_SC_FW_IMAGE_INVALID = 0x07,
    NVME_SC_FW_ACTIVATION_REQUIRES_RESET = 0x0B,
    NVME_SC_FW_ACTIVATION_REQUIRES_CONV_RESET = 0x10,
    NVME_SC_FW_ACTIVATION_PROHIBITED = 0x13,
};

/* Status code types (SCT). 0x0 = generic, 0x1 = command specific. */
enum nvme_status_type {
    NVME_SCT_GENERIC = 0x0,
    NVME_SCT_COMMAND_SPECIFIC = 0x1,
};

/* ------------------------------------------------------------------ */
/* Identify Controller - 4096 bytes.                                    */
/* ------------------------------------------------------------------ */
struct nvme_id_ctrl {
    uint16_t vid;          /* 0   PCI vendor id                      */
    uint16_t ssvid;        /* 2   PCI subsystem vendor id            */
    char sn[20];           /* 4   serial number                      */
    char mn[40];           /* 24  model number                       */
    char fr[8];            /* 64  firmware revision                  */
    uint8_t rab;           /* 72  recommended arbitration burst      */
    uint8_t ieee[3];       /* 73  IEEE OUI                           */
    uint8_t cmic;          /* 76  controller multi-path I/O          */
    uint8_t mdts;          /* 77  max data transfer size (2^mdts pg) */
    uint16_t cntlid;       /* 78  controller id                      */
    uint32_t ver;          /* 80  NVMe version                       */
    uint32_t rtd3r;        /* 84  RTD3 resume latency                */
    uint32_t rtd3e;        /* 88  RTD3 entry latency                 */
    uint32_t oaes;         /* 92  optional async event support       */
    uint32_t ctratt;       /* 96  controller attributes              */
    uint8_t rsvd100[156];  /* 100..255                               */
    uint16_t oacs;         /* 256 optional admin command support     */
    uint8_t acl;           /* 258 abort command limit                */
    uint8_t aerl;          /* 259 async event request limit          */
    uint8_t frmw;          /* 260 firmware updates (bit0: slot1 RO)  */
    uint8_t lpa;           /* 261 log page attributes                */
    uint8_t elpe;          /* 262 error log page entries             */
    uint8_t npss;          /* 263 number of power states             */
    uint8_t avscc;         /* 264 admin vendor specific config       */
    uint8_t apsta;         /* 265 autonomous power state transition  */
    uint16_t wctemp;       /* 266 warning composite temperature      */
    uint16_t cctemp;       /* 268 critical composite temperature     */
    uint16_t mtfa;         /* 270 max time for fw activation         */
    uint32_t hmpre;        /* 272 host memory buffer preferred size  */
    uint32_t hmmin;        /* 276 host memory buffer min size        */
    uint8_t tnvmcap[16];   /* 280 total NVM capacity                 */
    uint8_t unvmcap[16];   /* 296 unallocated NVM capacity           */
    uint32_t rpmbs;        /* 312 reclaimable capacity / bitmap      */
    uint16_t edstt;        /* 316 extended device self test time     */
    uint8_t dsto;          /* 318 device self test options           */
    uint8_t fwug;          /* 319 firmware update granularity        */
    uint16_t kas;          /* 320 keep alive support                 */
    uint16_t hctma;        /* 322 host controlled thermal mgmt       */
    uint16_t mntmt;        /* 324 minimum thermal management temp    */
    uint16_t mxtmt;        /* 326 maximum thermal management temp    */
    uint32_t sanicap;      /* 328 sanitize capabilities              */
    uint32_t hmminds;      /* 332 host memory max IDT                */
    uint16_t hmmaxd;       /* 336 host memory max descriptors        */
    uint16_t nsetidmax;    /* 338 NVM set identifier max             */
    uint16_t endgidmax;    /* 340 endurance group identifier max     */
    uint8_t anatt;         /* 342 ANA transition time                */
    uint8_t anacap;        /* 343 ANA capabilities                   */
    uint32_t anagrpmax;    /* 344 ANA group max                      */
    uint32_t nanagrpid;    /* 348 number of ANA group ids            */
    uint32_t pels;         /* 352 persistent event log size          */
    uint8_t rsvd356[156];  /* 356..511                              */
    uint32_t nn;           /* 512 number of namespaces               */
    uint16_t oncs;         /* 516 optional NVM command support       */
    uint16_t fuses;        /* 518 fused operation support            */
    uint8_t fna;           /* 520 format NVM attributes              */
    uint8_t vwc;           /* 521 volatile write cache               */
    uint16_t awun;         /* 522 atomic write unit normal           */
    uint16_t awupf;        /* 524 atomic write unit power fail       */
    uint8_t nvscc;         /* 526 NVM vendor specific config         */
    uint8_t nwpc;          /* 527 namespace write protection capability */
    uint16_t acwu;         /* 528 atomic compare & write unit        */
    uint16_t rsvd530;      /* 530                                   */
    uint32_t sgls;         /* 532 SGL support                       */
    uint8_t rsvd536[3560]; /* 536..4095                             */
};
NVME_STATIC_ASSERT(sizeof(struct nvme_id_ctrl) == 4096, "Identify Controller is 4096 bytes");

/* frmw (Firmware Updates) bits. */
#define NVME_FRMW_SLOT1_RO 0x01u
#define NVME_FRMW_NUM_SLOTS_SHIFT 1u
#define NVME_FRMW_NUM_SLOTS_MASK 0x0Eu
#define NVME_FRMW_FW_SLOT_READ_ONLY 0x10u

/* oacs bits. */
#define NVME_OACS_FW_DOWNLOAD 0x0004u
#define NVME_OACS_FW_COMMIT 0x0008u
#define NVME_OACS_GET_LOG_PAGE 0x0002u

/* ------------------------------------------------------------------ */
/* Identify Namespace - 4096 bytes.                                     */
/* ------------------------------------------------------------------ */
struct nvme_lbaf {
    uint16_t ms;  /* metadata size                       */
    uint8_t ds;   /* LBA data size, log2 (12 => 4096 B)  */
    uint8_t rp;   /* relative performance                */
};
NVME_STATIC_ASSERT(sizeof(struct nvme_lbaf) == 4, "LBA format descriptor is 4 bytes");

struct nvme_id_ns {
    uint64_t nsze;         /* 0   namespace size in LBAs            */
    uint64_t ncap;         /* 8   namespace capacity in LBAs        */
    uint64_t nuse;         /* 16  namespace utilization in LBAs     */
    uint8_t nsfeat;        /* 24  namespace features                */
    uint8_t nlbaf;         /* 25  number of LBA formats (0-based)   */
    uint8_t flbas;         /* 26  formatted LBA size                */
    uint8_t mc;            /* 27  metadata capabilities             */
    uint8_t dpc;           /* 28  end-to-end data protection caps   */
    uint8_t dps;           /* 29  end-to-end data protection type   */
    uint8_t nmic;          /* 30  namespace multi-path I/O          */
    uint8_t rescap;        /* 31  reservation capabilities          */
    uint8_t fpi;           /* 32  format progress indicator         */
    uint8_t dlfeat;        /* 33  deallocate logical block features */
    uint8_t rsvd34[2];     /* 34..35                                */
    uint16_t nawun;        /* 36  namespace atomic write unit normal */
    uint16_t nawupf;       /* 38  namespace atomic write unit pf    */
    uint16_t nacwu;        /* 40  namespace atomic compare & write  */
    uint16_t nabsn;        /* 42  namespace atomic boundary size    */
    uint16_t nabo;         /* 44  namespace atomic boundary offset  */
    uint16_t nabspf;       /* 46  namespace atomic boundary pf      */
    uint16_t noiob;        /* 48  namespace optimal I/O boundary    */
    uint8_t nvmcap[16];    /* 50  NVM capacity                      */
    uint16_t npwg;         /* 66  namespace preferred write granularity */
    uint16_t npwa;         /* 68  namespace preferred write alignment   */
    uint16_t npdg;         /* 70  namespace preferred deallocate gran.  */
    uint16_t npda;         /* 72  namespace preferred deallocate align. */
    uint16_t nows;         /* 74  namespace optimal write size      */
    uint16_t mssrl;        /* 76  max single sector read latency    */
    uint8_t mcl[4];        /* 78  max command latency (unaligned)   */
    uint8_t msrc;          /* 82  max single sector read latency    */
    uint8_t rsvd83[1];     /* 83                                    */
    uint32_t anagrpid;     /* 84  ANA group identifier              */
    uint8_t rsvd88[3];     /* 88..90                                */
    uint8_t nsattr;        /* 91  namespace attributes              */
    uint16_t nvmsetid;     /* 92  NVM set identifier                */
    uint16_t endgid;       /* 94  endurance group identifier        */
    uint8_t nguid[16];     /* 96  namespace globally unique id      */
    uint8_t eui64[8];      /* 112 EUI-64                            */
    uint8_t rsvd120[8];    /* 120..127                              */
    struct nvme_lbaf lbaf[64]; /* 128..383                          */
    uint8_t rsvd384[3712]; /* 384..4095                             */
};
NVME_STATIC_ASSERT(sizeof(struct nvme_id_ns) == 4096, "Identify Namespace is 4096 bytes");
NVME_STATIC_ASSERT(offsetof(struct nvme_id_ns, lbaf) == 128, "lbaf must be at offset 128");

/* ------------------------------------------------------------------ */
/* Firmware Slot Information log page (LID 0x03) - 512 bytes.           */
/* afi bits 2:0 = active slot, bits 6:4 = next activation slot.         */
/* ------------------------------------------------------------------ */
struct nvme_fw_slot_log {
    uint8_t afi;      /* 0                                       */
    uint8_t rsvd1[7]; /* 1..7                                    */
    char frs[7][8];   /* 8..63  firmware revision for slot 1..7   */
    uint8_t rsvd2[448]; /* 64..511                                */
};
NVME_STATIC_ASSERT(sizeof(struct nvme_fw_slot_log) == 512,
                   "Firmware Slot Information log page is 512 bytes");

#define NVME_AFI_ACTIVE_MASK 0x07u
#define NVME_AFI_NEXT_SHIFT 4u
#define NVME_AFI_NEXT_MASK 0x70u

static inline uint8_t nvme_afi_encode(unsigned active_slot, unsigned next_slot)
{
    return (uint8_t)((active_slot & NVME_AFI_ACTIVE_MASK) |
                     ((next_slot << NVME_AFI_NEXT_SHIFT) & NVME_AFI_NEXT_MASK));
}
static inline unsigned nvme_afi_active(uint8_t afi) { return afi & NVME_AFI_ACTIVE_MASK; }
static inline unsigned nvme_afi_next(uint8_t afi) { return (afi & NVME_AFI_NEXT_MASK) >> NVME_AFI_NEXT_SHIFT; }

/* ------------------------------------------------------------------ */
/* SMART / Health Information log page (LID 0x02) - stub, 512 bytes.    */
/* ------------------------------------------------------------------ */
struct nvme_smart_log {
    uint8_t critical_warning;
    uint8_t temperature[2]; /* unaligned in the spec layout */
    uint8_t avail_spare;
    uint8_t spare_thresh;
    uint8_t percent_used;
    uint8_t rsvd6[26];
    uint8_t data_units_read[16];
    uint8_t data_units_written[16];
    uint8_t host_reads[16];
    uint8_t host_writes[16];
    uint8_t ctrl_busy_time[16];
    uint8_t power_cycles[16];
    uint8_t power_on_hours[16];
    uint8_t unsafe_shutdowns[16];
    uint8_t media_errors[16];
    uint8_t num_err_log_entries[16];
    uint32_t warning_temp_time;
    uint32_t critical_comp_time;
    uint16_t temp_sensor[8];
    uint32_t thm_temp1_trans_count;
    uint32_t thm_temp2_trans_count;
    uint32_t thm_temp1_total_time;
    uint32_t thm_temp2_total_time;
    uint8_t rsvd240[280];
};
NVME_STATIC_ASSERT(sizeof(struct nvme_smart_log) == 512, "SMART log is 512 bytes");

/* ------------------------------------------------------------------ */
/* Admin command helpers (CDW encodings used by both driver and model). */
/* ------------------------------------------------------------------ */
#define NVME_AQA_ASQS_SHIFT 0u
#define NVME_AQA_ACQS_SHIFT 16u
#define NVME_AQA_ASQS_MASK 0x0FFFu
#define NVME_AQA_ACQS_MASK 0x0FFFu

#define NVME_SQ_ENTRY_SIZE 64u
#define NVME_CQ_ENTRY_SIZE 16u

#ifdef __cplusplus
}
#endif

#endif /* NVME_SPEC_H */

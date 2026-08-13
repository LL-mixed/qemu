/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2023-2024. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef UB_UBC_H
#define UB_UBC_H

#include "hw/sysbus.h"
#include "qom/object.h"
#include "hw/ub/hisi/ubc.h"
#include "hw/ub/ub_bus.h"
#include "hw/ub/ub_link.h"
#include "hw/ub/gsva_key.h"
#include "hw/ub/ub_obmm_remote.h"
#include "hw/ub/ub_obmm_remote_model.h"
#include "hw/ub/ub_scc_device.h"
#include "qemu/timer.h"
#include "qapi/error.h"

#define TYPE_BUS_CONTROLLER_DEV "ubc"
OBJECT_DECLARE_TYPE(BusControllerDev, BusControllerDevClass, BUS_CONTROLLER_DEV)

/* Entity table support (matches guest pool.h entity_base_info layout) */
#define UB_ENTITY_GUID_DW_NUM  4
#define UB_ENTITY_MAX_RES_NUM  3
#define UB_MAX_ENTITIES        8

typedef enum UBEntityState {
    UB_ENTITY_STATE_ABSENT,
    UB_ENTITY_STATE_PENDING,   /* injected but not yet confirmed by guest */
    UB_ENTITY_STATE_PRESENT,
    UB_ENTITY_STATE_ERROR,
} UBEntityState;

typedef struct UBEntityErs {
    uint32_t ss;    /* segment size */
    uint32_t sa_l;  /* start address low */
    uint32_t sa_h;  /* start address high */
} UBEntityErs;

typedef struct UBEntityDesc {
    uint32_t     entity_idx;
    uint32_t     device_id;       /* 0x0541 for MUE, 0x0542 for UE */
    uint32_t     eid[UB_ENTITY_GUID_DW_NUM];
    uint32_t     ueid[UB_ENTITY_GUID_DW_NUM];
    uint32_t     cna;
    uint32_t     upi;
    uint32_t     guid[UB_ENTITY_GUID_DW_NUM]; /* vendor|device_id|version|type|rsv|seq */
    UBEntityErs  ers[UB_ENTITY_MAX_RES_NUM];
    UBEntityState state;
} UBEntityDesc;

/* Per-Entity Configuration Space */
typedef struct UBEntityCfgSpace {
    uint8_t  *cfg_base;      /* 该实体的配置空间基址 */
    uint32_t cfg_size;       /* 配置空间大小 */
    uint32_t eid;            /* 实体 EID */
    uint32_t cna;            /* 实体 CNA */
    uint16_t upi;            /* 实体 UPI */
    bool     initialized;    /* 是否已初始化 */
} UBEntityCfgSpace;

typedef struct UBCmdQueueState {
    uint64_t base;
    uint16_t depth;
    uint16_t head;
    uint16_t tail;
} UBCmdQueueState;

#define UBC_MAX_JETTIES  1024
#define UBC_MAX_JFCS     128
#define UBC_MAX_JFRS     128
#define UBC_MAX_AEQS     16
#define UBC_MAX_CEQS     16

typedef struct UBCJettyState {
    bool     active;
    uint32_t jetty_id;
    uint64_t sq_buf_addr;    /* SQ buffer guest physical address */
    uint32_t sq_depth;       /* SQ depth in WQEBBs */
    uint32_t sq_pi;          /* producer index (from doorbell) */
    uint32_t sq_ci;          /* consumer index */
    uint32_t tx_jfcn;        /* TX JFC index for completions */
    uint32_t rx_jfcn;        /* RX JFC index */
    uint32_t jfrn;           /* JFR (receive queue) index */
    bool     jfs_mode;       /* true=JFS send-only, false=JETTY send+recv */
    uint32_t seid_idx;       /* EID index */
    uint32_t sqe_bb_shift;   /* log2(sq_depth) */
    uint32_t sqe_token_id;   /* SQ buffer token/tid */
    uint32_t jetty_state;    /* jetty state machine: RESET/READY/ERROR/SUSPEND */
    uint32_t user_data_l;    /* from JFS context DW7 — copied to CQE */
    uint32_t user_data_h;    /* from JFS context DW8 — copied to CQE */
} UBCJettyState;

typedef struct UBCJfcState {
    bool     active;
    uint32_t jfc_id;
    uint64_t cq_buf_addr;    /* CQ buffer guest physical address */
    uint32_t cq_depth;
    uint32_t cqe_token_id;   /* CQ buffer token/tid */
    uint32_t cq_pi;
    uint32_t cq_ci;
    uint32_t ceqn;           /* completion EQ index used by this JFC */
    uint32_t cq_owner_phase; /* tracks owner bit phase for CQE generation */
} UBCJfcState;

typedef struct UBCJfrState {
    bool     active;
    uint32_t jfr_id;
    uint64_t rq_buf_addr;    /* RQ buffer guest physical address */
    uint32_t rq_depth;
    uint32_t rqe_token_id;   /* RQ buffer token/tid */
    uint32_t rq_pi;
    uint32_t rq_ci;
} UBCJfrState;

typedef struct UBCEqState {
    bool     active;
    uint32_t eq_id;
    uint64_t eq_buf_addr;    /* EQ buffer guest physical address */
    uint32_t eq_depth;       /* number of EQEs */
    uint32_t eq_pi;          /* producer index */
    uint32_t eq_ci;          /* consumer index (for query only) */
    uint32_t eq_owner_phase; /* owner bit phase for EQE generation */
    uint32_t irq_num;        /* USI vector number programmed by guest */
} UBCEqState;

typedef struct BusControllerDev {
    UBDevice parent;
    UbGuid bus_instance_guid;
    int bus_instance_lock_fd;

    /* Multi-entity support */
    uint32_t entity_count;  /* Number of entities (FEs), default=1 */
    char *remote_memory_model_manifest;
    char *scheduler_core_model;
    UbObmmRemoteModelState remote_memory_model;
    QEMUTimer *remote_memory_model_timer;
    struct UbcObmmAsyncChild *obmm_async_children;
    struct UbObmmAsyncState *obmm_async;
    struct UbSccDeviceState *obmm_scc;
    UBEntityDesc entities[UB_MAX_ENTITIES]; /* per-entity descriptor table */
    UBEntityCfgSpace entity_cfg_spaces[UB_MAX_ENTITIES]; /* per-entity cfg spaces */

    struct {
        BusControllerDev *owner;
        MemoryRegion region;
        uint8_t idx;
        uint8_t *storage;
        uint64_t storage_size;
    } ers[UB_NUM_REGIONS];
    UBCmdQueueState cmd_csq;
    UBCmdQueueState cmd_crq;
    UBCmdQueueState ctrl_csq;
    UBCmdQueueState ctrl_crq;

    /*
     * CSQ base address recorded after the last successful CMDQ processing.
     * When the guest re-programs CSQ for ctrlq, the base changes.
     * If cmd_csq.base differs from this value (and this value is non-zero),
     * the queue has been repurposed and ubc_process_cmdq must be skipped.
     */
    uint64_t cmdq_last_csq_base;
    /*
     * UMMU fallback gate:
     * - true: ctrl-path DMA may use compatibility fallback (CPU PTW/direct/alias)
     * - false: no fallback (strict IOMMU/UMMU translation only)
     * Data-path DMA is always strict regardless of this flag.
     */
    bool dma_fallback_init_window;

    /* URMA jetty / JFC / JFR state tracking (populated by mailbox cmds) */
    UBCJettyState jetties[UBC_MAX_JETTIES];
    UBCJfcState   jfcs[UBC_MAX_JFCS];
    UBCJfrState   jfrs[UBC_MAX_JFRS];
    UBCEqState    aeqs[UBC_MAX_AEQS];
    UBCEqState    ceqs[UBC_MAX_CEQS];

    /* URMA RX buffering for packets arriving before dst jetty is active */
    struct {
        uint32_t count;
        struct {
            uint32_t dst_jetty;
            uint32_t src_jetty;
            uint32_t src_scna;
            uint32_t data_len;
            uint8_t  src_eid[16];
            uint8_t  data[4096];
        } entries[64];
    } urma_rx_buf;

    /* Associated UMMU for address translation */
    struct UMMUState *ummu;

    /* Pending RDMA READ requests awaiting remote response */
    #define UBC_MAX_PENDING_READS 64  /* Increased from 16 for better concurrency */
    struct {
        uint32_t count;
        struct {
            bool     pending;
            uint32_t jetty_id;    /* originating jetty */
            uint32_t wqe_idx;     /* WQE index in SQ */
            uint32_t jfc_id;      /* TX JFC for completion */
            uint64_t local_va;    /* local SGE VA to write response data */
            uint32_t local_len;   /* local SGE buffer length */
            uint32_t local_token_id; /* local SGE token/tid */
            uint32_t req_id;      /* request ID for matching */
        } entries[UBC_MAX_PENDING_READS];
    } pending_reads;
    uint32_t next_read_req_id;
    struct {
        bool pending;
        bool model_queued;
        uint32_t req_id;
        uint32_t peer_cna;
        uint32_t expect_len;
        uint32_t actual_len;
        int status;
        uint64_t block_version;
        uint64_t block_bytes;
        uint64_t checksum64;
        uint64_t map_id;
        uint64_t map_generation;
        uint64_t remote_offset;
        uint64_t per_range_ordinal;
        uint8_t *buf;
    } sim_dec_sync_read;

    /* OBMM coherence synchronous wait (same pattern as sim_dec_sync_read) */
    struct {
        bool     pending;
        uint32_t req_id;
        uint32_t peer_cna;
        uint32_t msg_type;
        int      status;
        uint32_t data_len;
        uint32_t grant_state;
        uint8_t  data[64];
        /* For multi-ack protocols (INV_ACK collection) */
        uint32_t pending_ack_count;
        uint64_t ack_bitmap;
    } coh_sync_wait;
    uint32_t next_coh_req_id;

    void *linqu_uapi_bridge;
    bool linqu_uapi_bridge_ready;
    uint64_t linqu_uapi_cmdq_iova;
    uint64_t linqu_uapi_cq_iova;
    uint32_t linqu_uapi_cmdq_depth;
    uint32_t linqu_uapi_cq_depth;
    uint32_t linqu_uapi_cmdq_head;
    uint32_t linqu_uapi_cmdq_tail;
    uint32_t linqu_uapi_cq_head;
    uint32_t linqu_uapi_cq_tail;
    uint64_t linqu_uapi_default_segment;
    uint64_t linqu_uapi_segment_data_offset;
    uint64_t linqu_uapi_last_error;
    uint64_t linqu_uapi_irq_status;
    bool linqu_uapi_kick_pending;
    bool linqu_uapi_kick_running;
    uint32_t linqu_uapi_kick_batch;
    uint32_t next_sim_dec_read_req_id;
    uint32_t next_tp_id;
} BusControllerDev;

typedef struct UbcObmmResolvedMap {
    uint64_t map_id;
    uint64_t map_generation;
    uint64_t remote_uba;
    uint64_t length;
    uint32_t token_id;
    uint32_t peer_cna;
} UbcObmmResolvedMap;

typedef void (*UbcObmmAsyncReadCompleteFn)(
    void *opaque, ObmmRemoteToken token, uint16_t child_index,
    ObmmRemoteStatus status, const void *payload, uint32_t bytes_done,
    uint64_t model_publish_ns);

struct BusControllerDevClass {
    UBDeviceClass parent_class;
};

/* Forward declaration for UMMU */
typedef struct UMMUState UMMUState;

#define TYPE_BUS_CONTROLLER "ub-bus-controller"
OBJECT_DECLARE_TYPE(BusControllerState, BusControllerClass, BUS_CONTROLLER)

typedef struct BusControllerState BusControllerState;
struct BusControllerState {
    SysBusDevice busdev;
    qemu_irq irq;

    MemoryRegion msgq_reg_mem; /* ubc msgq */
    uint32_t msgq_reg_size;
    uint8_t *msgq_reg;
    MemoryRegion fm_msgq_reg_mem; /* fm msgq */
    uint32_t fm_msgq_reg_size;
    uint8_t *fm_msgq_reg;
    MemoryRegion io_mmio; /* ub mmio hpa memory region */
    uint32_t mmio_size;
    bool mig_enabled;
    HiMsgqInfo msgq;
    BusControllerDev *ubc_dev;
    UBBus *bus;
    QEMUTimer *notify_retry_timer;
    QLIST_ENTRY(BusControllerState) node;
};

struct BusControllerClass {
    SysBusDeviceClass parent_class;
};

#define UBC_ERS0_SPACE_SIZE 0x80   /* 128 pages = 512 KiB for doorbell range */
#define UBC_ERS1_SPACE_SIZE 0x10001
#define UBC_ERS2_SPACE_SIZE 0x20
#define UBC_ERS0_SPACE_ADDR 0x18000000ULL
#define UBC_ERS1_SPACE_ADDR 0x18100000ULL
#define UBC_ERS2_SPACE_ADDR 0x18200000ULL
#define UBC_EID_UPI_TEN_DEFAULT_VAL 1024
#define UBC_CLASS_CODE 0x0000  /* UB_CLASS_BUS_CONTROLLER — driver requires base_code=0x00 for IBUS_CONTROLLER type */

uint64_t ub_ers_phys_base(void);

void ub_save_ubc_list(BusControllerState *s);
BusControllerState *container_of_ubbus(UBBus *bus);
int ub_inject_remote_cfg_cpl_notify(BusControllerState *s,
                                    const UbGuid *remote_bi_guid,
                                    Error **errp);
void ub_try_inject_remote_cfg_notifies(BusControllerState *s);
void ub_notify_retry_timer_cb(void *opaque);
void ub_link_process_incoming_message(BusControllerState *s, UBLinkState *link);
void ubc_handle_urma_rx_data(BusControllerDev *ubc_dev, uint32_t dst_jetty,
                              uint32_t src_jetty, uint32_t src_scna,
                              const uint8_t src_eid[16],
                              const uint8_t *data, uint32_t data_len);
void ubc_handle_urma_rx_write(BusControllerDev *ubc_dev, uint32_t dst_jetty,
                              uint64_t remote_addr, const uint8_t *data,
                              uint32_t data_len);

/* SIM_DEC remote memory access (used by coherence module) */
MemTxResult ubc_sim_dec_remote_write(BusControllerDev *ubc_dev,
                                     uint64_t remote_uba,
                                     uint32_t token_id,
                                     uint32_t dcna,
                                     const uint8_t *data,
                                     uint32_t len);
MemTxResult ubc_sim_dec_remote_read(BusControllerDev *ubc_dev,
                                    uint64_t remote_uba,
                                    uint32_t token_id,
                                    uint32_t dcna,
                                    uint8_t *data,
                                    uint32_t len);
bool ubc_obmm_resolve_async_map(BusControllerDev *ubc_dev,
                                uint64_t local_pa, uint64_t length,
                                UbcObmmResolvedMap *resolved);
bool ubc_sim_dec_remote_read_async_submit(
    BusControllerDev *ubc_dev, const UbcObmmResolvedMap *map,
    uint64_t remote_offset, uint32_t length,
    const UbObmmRemoteOperation *operation, ObmmRemoteToken token,
    uint16_t child_index, UbcObmmAsyncReadCompleteFn complete,
    void *opaque);
void ubc_sim_dec_remote_read_async_cancel(BusControllerDev *ubc_dev,
                                          ObmmRemoteToken token);
void ubc_obmm_async_irq_notify(BusControllerDev *ubc_dev);

/* Coherence message send helper (obmm_coherence.c uses these) */
int obmm_coh_send_ub_link_msg(BusControllerDev *ubc_dev, uint32_t dcna,
                               uint8_t sub_msg_code,
                               const void *payload, uint32_t payload_len);
void obmm_coh_poll_rx_links(BusControllerDev *ubc_dev);

/* Entity table management */
void ub_entity_table_init(BusControllerDev *ubc_dev);
UBEntityDesc *ub_entity_desc_for_idx(BusControllerDev *ubc_dev, uint32_t entity_idx);
void ub_entity_cfg_spaces_init(BusControllerDev *ubc_dev);
void ub_entity_cfg_spaces_cleanup(BusControllerDev *ubc_dev);

/* Entity injection (UB_DEV_REG / UB_DEV_RLS) */
int ub_inject_entity_reg(BusControllerState *s, const UBEntityDesc *e, Error **errp);
int ub_inject_entity_rls(BusControllerState *s, uint32_t eid, uint8_t reason, Error **errp);

/* RDMA READ request/response handling (cross-node) */

/* READ request payload (sent from requester to remote) */
typedef struct QEMU_PACKED UBCReadReqPld {
    uint32_t req_id;
    uint32_t src_jetty_id;
    uint64_t remote_addr;
    uint32_t read_len;
    uint32_t rmt_obj_id;
} UBCReadReqPld;

/* READ response payload (sent from remote back to requester) */
typedef struct QEMU_PACKED UBCReadRespPld {
    uint32_t req_id;
    uint32_t src_jetty_id;
    uint32_t data_len;
    uint32_t status;
    /* data follows immediately after this header */
} UBCReadRespPld;

void ubc_handle_read_request(BusControllerDev *ubc_dev, const UBCReadReqPld *req,
                              uint32_t dcna);
void ubc_handle_read_response(BusControllerDev *ubc_dev, const UBCReadRespPld *resp,
                                const uint8_t *data, uint32_t data_len);

/* Cross-node SIM_DEC data-plane message (msg_code=7, distinguished by sub_msg_code). */
#define UBC_MSG_CODE_URMA_DATA        7
#define UBC_MSG_SUB_URMA_DATA         0
#define UBC_MSG_SUB_SIM_DEC_WRITE     1
#define UBC_MSG_SUB_SIM_DEC_READ_REQ  2
#define UBC_MSG_SUB_SIM_DEC_READ_RESP 3
#define UBC_MSG_SUB_SIM_DEC_BATCH     4
#define UBC_MSG_SUB_UB_SSD_READ_REQ   UBC_MSG_SUB_SIM_DEC_BATCH
#define UBC_MSG_SUB_UB_SSD_READ_RESP  UBC_MSG_SUB_SIM_DEC_BATCH
#define UBC_UB_SSD_READ_REQ_MAGIC     0x53534452U
#define UBC_UB_SSD_READ_RESP_MAGIC    0x53534472U

/* OBMM coherence protocol messages (msg_code=7, sub_msg_code 5+). */
#define UBC_MSG_SUB_COH_GETS          5
#define UBC_MSG_SUB_COH_GETM          6
#define UBC_MSG_SUB_COH_INV           7
#define UBC_MSG_SUB_COH_INV_ACK       8
#define UBC_MSG_SUB_COH_WB            9
#define UBC_MSG_SUB_COH_WB_ACK       10
#define UBC_MSG_SUB_COH_DATA         11
#define UBC_MSG_SUB_COH_FENCE        12
#define UBC_MSG_SUB_COH_FENCE_ACK    13
#define UBC_MSG_SUB_COH_DOWNGRADE    14
#define UBC_MSG_SUB_COH_DOWNGRADE_ACK 15

/*
 * GSVA coherence uses one 4-bit carrier subcode and carries the concrete
 * operation in GsvaCohMsgV1.op.  The UB message header stores sub_msg_code in
 * 4 bits, so values above 15 are not wire-safe.
 */
#define UBC_MSG_SUB_GSVA_COH             15

/* Legacy symbolic GSVA op subcodes. Do not put these values on the wire. */
#define UBC_MSG_SUB_GSVA_COH_READ_ACQ    16
#define UBC_MSG_SUB_GSVA_COH_READ_ACK    17
#define UBC_MSG_SUB_GSVA_COH_WRITE_ACQ   18
#define UBC_MSG_SUB_GSVA_COH_WRITE_ACK   19
#define UBC_MSG_SUB_GSVA_COH_INV         20
#define UBC_MSG_SUB_GSVA_COH_INV_ACK     21
#define UBC_MSG_SUB_GSVA_COH_RETIRE_REQ  22
#define UBC_MSG_SUB_GSVA_COH_RETIRE_ACK  23
#define UBC_MSG_SUB_GSVA_COH_DOWNGRADE   24
#define UBC_MSG_SUB_GSVA_COH_DOWNGRADE_ACK 25
#define UBC_MSG_SUB_GSVA_COH_FENCE       26
#define UBC_MSG_SUB_GSVA_COH_FENCE_ACK   27
#define UBC_MSG_SUB_GSVA_COH_TOKEN_REVOKE 30
#define UBC_MSG_SUB_GSVA_COH_TOKEN_ACK   31

/* SIM_DEC batched write payload */
typedef struct QEMU_PACKED SimDecBatchHdr {
    uint8_t version;       /* 1 */
    uint8_t op_count;
    uint16_t flags;
    uint32_t barrier_epoch;
} SimDecBatchHdr;

typedef struct QEMU_PACKED SimDecBatchWriteOp {
    uint64_t remote_uba;
    uint32_t token_id;
    uint32_t data_len;
    /* data bytes follow after all ops in the payload */
} SimDecBatchWriteOp;

#define SIM_DEC_BATCH_MAX_OPS     16
#define SIM_DEC_BATCH_MAX_DATA    (64 * 1024)  /* 64 KiB max per batch frame */

/* SIM_DEC instrumentation counters */
typedef struct SimDecStats {
    uint64_t gva_cpu_reads;
    uint64_t gva_cpu_writes;
    uint64_t gva_cpu_read_bytes;
    uint64_t gva_cpu_write_bytes;
    uint64_t gva_dma_reads;
    uint64_t gva_dma_writes;
    uint64_t gva_dma_read_bytes;
    uint64_t gva_dma_write_bytes;
    uint64_t cpu_window_reads;
    uint64_t cpu_window_writes;
    uint64_t cpu_window_read_bytes[4]; /* index 0=1B,1=2B,2=4B,3=8B */
    uint64_t cpu_window_write_bytes[4];
    uint64_t shadow_hits;
    uint64_t shadow_misses;
    uint64_t page_cache_hits;
    uint64_t page_cache_misses;
    uint64_t page_cache_prefetches;
    uint64_t page_cache_prefetch_skips;
    uint64_t remote_reads;
    uint64_t remote_writes;
    uint64_t remote_read_bytes;
    uint64_t remote_write_bytes;
    uint64_t dma_path_reads;
    uint64_t dma_path_writes;
    uint64_t dma_path_read_bytes;
    uint64_t dma_path_write_bytes;
    uint64_t read_timeouts;
    uint64_t read_errors;
    uint64_t write_errors;
    uint64_t batch_frames;
    uint64_t batch_ops;
    uint64_t batch_bytes;
} SimDecStats;

void sim_dec_stats_accumulate(SimDecStats *dst, const SimDecStats *src);
void sim_dec_print_stats(const SimDecStats *stats, const char *prefix);
MemTxResult obmm_coh_local_read(BusControllerDev *ubc_dev, uint64_t uba,
                                uint32_t token_id, uint8_t *buf,
                                uint32_t len);
MemTxResult obmm_coh_local_write(BusControllerDev *ubc_dev, uint64_t uba,
                                 uint32_t token_id, const uint8_t *buf,
                                 uint32_t len);

typedef struct QEMU_PACKED UBCSimDecWritePldHdr {
    uint64_t remote_uba;
    uint32_t token_id;
    uint32_t data_len;
    /* data follows */
} UBCSimDecWritePldHdr;

typedef struct QEMU_PACKED UBCSimDecReadReqPld {
    uint32_t req_id;
    uint32_t token_id;
    uint64_t remote_uba;
    uint32_t read_len;
    uint32_t rsvd;
} UBCSimDecReadReqPld;

typedef struct QEMU_PACKED UBCSimDecReadRespPldHdr {
    uint32_t req_id;
    uint32_t status;
    uint32_t data_len;
    uint32_t rsvd;
    /* data follows */
} UBCSimDecReadRespPldHdr;

typedef struct QEMU_PACKED UBCUbSsdBlockRefPld {
    uint64_t block_hi;
    uint64_t block_lo;
    uint64_t version;
    uint64_t offset;
    uint64_t bytes;
    uint64_t checksum64;
} UBCUbSsdBlockRefPld;

typedef struct QEMU_PACKED UBCUbSsdReadReqPld {
    uint32_t magic;
    uint32_t req_id;
    uint32_t target_ssd_cna;
    uint32_t source_cna;
    uint32_t read_len;
    uint64_t read_offset;
    UBCUbSsdBlockRefPld block_ref;
} UBCUbSsdReadReqPld;

typedef struct QEMU_PACKED UBCUbSsdReadRespPldHdr {
    uint32_t magic;
    uint32_t req_id;
    int32_t status;
    uint32_t data_len;
    uint32_t rsvd;
    uint64_t block_version;
    uint64_t block_bytes;
    uint64_t checksum64;
    /* data follows */
} UBCUbSsdReadRespPldHdr;

void ubc_handle_sim_dec_rx_write(BusControllerDev *ubc_dev,
                                 const UBCSimDecWritePldHdr *hdr,
                                 const uint8_t *data, uint32_t data_len);
void ubc_handle_sim_dec_rx_read_req(BusControllerDev *ubc_dev,
                                    const UBCSimDecReadReqPld *req,
                                    uint32_t dcna);
void ubc_handle_sim_dec_rx_read_resp(BusControllerDev *ubc_dev,
                                     const UBCSimDecReadRespPldHdr *hdr,
                                     const uint8_t *data, uint32_t data_len,
                                     uint32_t peer_cna);
int ubc_ub_ssd_remote_block_read(BusControllerDev *ubc_dev,
                                 uint32_t target_ssd_cna,
                                 uint32_t source_cna,
                                 const UBCUbSsdBlockRefPld *block_ref,
                                 uint8_t *data,
                                 uint32_t len,
                                 uint64_t *block_version,
                                 uint64_t *block_bytes,
                                 uint64_t *checksum64);
int ubc_ub_ssd_send_read_resp(BusControllerDev *ubc_dev, uint32_t dcna,
                              const UBCUbSsdReadRespPldHdr *hdr,
                              const uint8_t *data, uint32_t data_len);
void ubc_handle_ub_ssd_rx_read_resp(BusControllerDev *ubc_dev,
                                    const UBCUbSsdReadRespPldHdr *hdr,
                                    const uint8_t *data,
                                    uint32_t data_len,
                                    uint32_t peer_cna);
void ub_ssd_handle_remote_read_request(BusControllerDev *ubc_dev,
                                       const UBCUbSsdReadReqPld *req,
                                       uint32_t requester_cna);

/* SIM Decoder (SIM_DEC) protocol for cross-node memory access */
int ubc_handle_sim_dec_message(const uint8_t *data, uint32_t len,
                                uint8_t *resp, uint32_t *resp_len);
int sim_dec_lookup_by_pa(uint64_t pa, uint64_t *remote_uba,
                         uint32_t *token_id, uint32_t *src_eid,
                         uint32_t *address_profile,
                         uint32_t *dcna);
bool sim_dec_gva_tcg_translate(uint64_t va, bool is_write,
                               uint64_t *local_pa, uint64_t *page_size);

/* GSVA ARM MMU mode helpers */
bool gsva_arm_mmu_enabled(void);

/*
 * GSVA ARM MMU hook: lookup GSVA route, return backend PA and acquire
 * coherence permission for TLB fill.
 *
 * Return values:
 *   1  = GSVA route hit and permission acquired
 *   0  = no GSVA route, keep normal ARM translation
 *  <0  = GSVA route hit but semantic validation failed
 */
int gsva_arm_mmu_translate_full(uint64_t va, bool is_write,
                                uint32_t cpu_index,
                                uint64_t *local_pa,
                                uint64_t *page_size);

/* Compatibility helper kept for existing callers. */
int gsva_arm_mmu_translate(uint64_t va, bool is_write, uint32_t cpu_index);

/* Device-side GSVA access wrappers (NPU, SSD, etc.) */
#define UB_GSVA_DEVICE_ACCESS_READ       (1u << 0)
#define UB_GSVA_DEVICE_ACCESS_WRITE      (1u << 1)
#define UB_GSVA_DEVICE_ACCESS_READ_WRITE \
    (UB_GSVA_DEVICE_ACCESS_READ | UB_GSVA_DEVICE_ACCESS_WRITE)

int ubc_gsva_device_read_acquire(BusControllerDev *ubc,
                                 const GsvaKeyV1 *key,
                                 uint32_t requester_cna,
                                 uint64_t access_va, uint64_t access_len,
                                 uint32_t access_flags,
                                 uint32_t cmd_token_id,
                                 uint32_t cmd_token_value,
                                 uint64_t *pending_seq);
int ubc_gsva_device_write_acquire(BusControllerDev *ubc,
                                  const GsvaKeyV1 *key,
                                  uint32_t requester_cna,
                                  uint64_t access_va, uint64_t access_len,
                                  uint32_t access_flags,
                                  uint32_t cmd_token_id,
                                  uint32_t cmd_token_value,
                                  uint64_t *pending_seq);
int ubc_gsva_device_read(BusControllerDev *ubc,
                         const GsvaKeyV1 *key,
                         uint32_t requester_cna,
                         uint64_t gsva, void *dst, uint64_t len);
int ubc_gsva_device_write(BusControllerDev *ubc,
                          const GsvaKeyV1 *key,
                          uint32_t requester_cna,
                          uint64_t gsva, const void *src, uint64_t len);
int ubc_gsva_device_fence(BusControllerDev *ubc,
                          const GsvaKeyV1 *key,
                          uint32_t requester_cna,
                          uint64_t gsva, uint64_t len);

#endif

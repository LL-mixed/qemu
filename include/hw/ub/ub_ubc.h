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

#define UBC_MAX_JETTIES  64
#define UBC_MAX_JFCS     128
#define UBC_MAX_JFRS     128

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
} UBCJettyState;

typedef struct UBCJfcState {
    bool     active;
    uint32_t jfc_id;
    uint64_t cq_buf_addr;    /* CQ buffer guest physical address */
    uint32_t cq_depth;
    uint32_t cq_pi;
    uint32_t cq_ci;
} UBCJfcState;

typedef struct UBCJfrState {
    bool     active;
    uint32_t jfr_id;
    uint64_t rq_buf_addr;    /* RQ buffer guest physical address */
    uint32_t rq_depth;
    uint32_t rq_pi;
    uint32_t rq_ci;
} UBCJfrState;

typedef struct BusControllerDev {
    UBDevice parent;
    UbGuid bus_instance_guid;
    int bus_instance_lock_fd;

    /* Multi-entity support */
    uint32_t entity_count;  /* Number of entities (FEs), default=1 */
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
    bool csq_bias_valid;
    bool csq_bias_scanned;
    uint64_t csq_bias_iova_base;
    int64_t csq_iova_to_gpa_bias;
    bool crq_bias_valid;
    uint64_t crq_bias_iova_base;
    int64_t crq_iova_to_gpa_bias;

    /*
     * CSQ base address recorded after the last successful CMDQ processing.
     * When the guest re-programs CSQ for ctrlq, the base changes.
     * If cmd_csq.base differs from this value (and this value is non-zero),
     * the queue has been repurposed and ubc_process_cmdq must be skipped.
     */
    uint64_t cmdq_last_csq_base;

    /* URMA jetty / JFC / JFR state tracking (populated by mailbox cmds) */
    UBCJettyState jetties[UBC_MAX_JETTIES];
    UBCJfcState   jfcs[UBC_MAX_JFCS];
    UBCJfrState   jfrs[UBC_MAX_JFRS];

    /* URMA RX buffering for packets arriving before dst jetty is active */
    struct {
        uint32_t count;
        struct {
            uint32_t dst_jetty;
            uint32_t data_len;
            uint8_t  data[4096];
        } entries[64];
    } urma_rx_buf;
} BusControllerDev;

struct BusControllerDevClass {
    UBDeviceClass parent_class;
};

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
#define UBC_CLASS_CODE 0x0

void ub_save_ubc_list(BusControllerState *s);
BusControllerState *container_of_ubbus(UBBus *bus);
int ub_inject_remote_cfg_cpl_notify(BusControllerState *s,
                                    const UbGuid *remote_bi_guid,
                                    Error **errp);
void ub_try_inject_remote_cfg_notifies(BusControllerState *s);
void ub_notify_retry_timer_cb(void *opaque);
void ub_link_process_incoming_message(BusControllerState *s, UBLinkState *link);
void ubc_handle_urma_rx_data(BusControllerDev *ubc_dev, uint32_t dst_jetty,
                              const uint8_t *data, uint32_t data_len);

/* Entity table management */
void ub_entity_table_init(BusControllerDev *ubc_dev);
UBEntityDesc *ub_entity_desc_for_idx(BusControllerDev *ubc_dev, uint32_t entity_idx);
void ub_entity_cfg_spaces_init(BusControllerDev *ubc_dev);
void ub_entity_cfg_spaces_cleanup(BusControllerDev *ubc_dev);

/* Entity injection (UB_DEV_REG / UB_DEV_RLS) */
int ub_inject_entity_reg(BusControllerState *s, const UBEntityDesc *e, Error **errp);
int ub_inject_entity_rls(BusControllerState *s, uint32_t eid, uint8_t reason, Error **errp);
#endif

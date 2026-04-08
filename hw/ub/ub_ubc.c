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
#include <sys/file.h>
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "exec/address-spaces.h"
#include "hw/irq.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "hw/arm/virt.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "hw/ub/ub.h"
#include "hw/ub/ub_bus.h"
#include "hw/ub/ub_ubc.h"
#include "hw/ub/ub_ummu.h"
#include "hw/ub/ub_config.h"
#include "hw/ub/ub_usi.h"
#include "hw/pci/pci.h"
#include "hw/pci/msi.h"
#include "hw/ub/hisi/ubc.h"
#include "hw/ub/hisi/ub_mem.h"
#include "hw/ub/hisi/ub_fm.h"
#include "hw/ub/ub_link.h"
#include "sysemu/dma.h"
#include "migration/vmstate.h"
#include "hw/core/cpu.h"
#include "hw/ub/ub_common.h"
#include "hw/ub/ubus_instance.h"

#define UBC_ERS_PAGE_SIZE (4 * KiB)
#define UBC_ERS2_MMIO_SIZE (UBC_ERS2_SPACE_SIZE * UBC_ERS_PAGE_SIZE)

#define UBC_GUEST_RAM_BASE 0x40000000ULL
#define UBC_GUEST_RAM_SIZE 0x200000000ULL   /* 8 GB guest RAM */

/* ubase cmdq registers in ERS2 */
#define UBASE_CSQ_BASEADDR_L_REG 0x18400
#define UBASE_CSQ_BASEADDR_H_REG 0x18404
#define UBASE_CSQ_DEPTH_REG 0x18408
#define UBASE_CSQ_TAIL_REG 0x18410
#define UBASE_CSQ_HEAD_REG 0x18414
#define UBASE_CRQ_BASEADDR_L_REG 0x18418
#define UBASE_CRQ_BASEADDR_H_REG 0x1841c
#define UBASE_CRQ_DEPTH_REG 0x18420
#define UBASE_CRQ_TAIL_REG 0x18424
#define UBASE_CRQ_HEAD_REG 0x18428

/* ubase ctrlq registers in ERS2 (separate from cmdq, offset by 0x400) */
#define UBASE_CTRLQ_CSQ_BASEADDR_L_REG 0x18800
#define UBASE_CTRLQ_CSQ_BASEADDR_H_REG 0x18804
#define UBASE_CTRLQ_CSQ_DEPTH_REG 0x18808
#define UBASE_CTRLQ_CSQ_TAIL_REG 0x18810
#define UBASE_CTRLQ_CSQ_HEAD_REG 0x18814
#define UBASE_CTRLQ_CRQ_BASEADDR_L_REG 0x18818
#define UBASE_CTRLQ_CRQ_BASEADDR_H_REG 0x1881c
#define UBASE_CTRLQ_CRQ_DEPTH_REG 0x18820
#define UBASE_CTRLQ_CRQ_TAIL_REG 0x18824
#define UBASE_CTRLQ_CRQ_HEAD_REG 0x18828

#define UBASE_CTRLQ_BB_LEN 32
#define UBASE_CTRLQ_HDR_LEN 12
#define UBASE_CTRLQ_DATA_NUM 5

typedef struct QEMU_PACKED UBCCtrlqBaseBlock {
    uint8_t  service_ver;
    uint8_t  service_type;
    uint8_t  bb_num;
    uint8_t  opcode;
    uint8_t  ret;
    uint16_t seq;
    uint8_t  mbx_ue_id;
    uint16_t bus_ue_id;
    uint16_t rsv;
    uint32_t data[UBASE_CTRLQ_DATA_NUM];
} UBCCtrlqBaseBlock;

/* ctrlq service types */
#define UBASE_CTRLQ_SER_TYPE_QOS 0x04
#define UBASE_CTRLQ_SER_TYPE_DEV_REGISTER 0x02
#define UBASE_CTRLQ_SER_TYPE_TP_ACL 0x01

/* ctrlq DEV_REGISTER opcodes */
#define UBASE_CTRLQ_OPC_GET_SEID_INFO 0x01

/* ctrlq TP_ACL opcodes */
#define UBASE_CTRLQ_OPC_GET_TP_LIST 0x21

/* ctrlq QOS opcodes */
#define UBASE_CTRLQ_OPC_QUERY_VL 0x01
#define UBASE_CTRLQ_OPC_QUERY_SL 0x02

#define UBASE_VECTOR0_CMDQ_SRC_REG 0x18004
#define UBASE_VECTOR0_RX_CMDQ_INT_B 1

#define UBASE_OPC_QUERY_FW_VER 0x0001
#define UBASE_OPC_QUERY_UE_RES 0x0002
#define UBASE_OPC_QUERY_CTL_INFO 0x0003
#define UBASE_OPC_QUERY_COMM_RSRC_PARAM 0x0030
#define UBASE_OPC_QUERY_CHIP_INFO 0x6201
#define UBASE_OPC_QUERY_OOR_CAPS 0x4200
#define UBASE_OPC_QUERY_UB_PORT_BITMAP 0x5105
#define UBASE_OPC_QUERY_PORT_BITMAP 0xA017
#define UBASE_OPC_QUERY_TA_SL_VL_MAP 0x4201
#define UBASE_OPC_QUERY_TM_Q_INFO 0x4205
#define UBASE_OPC_QUERY_TM_QS_INFO 0x4206
#define UBASE_OPC_POST_MB 0x7000
#define UBASE_OPC_QUERY_MB_ST 0x7001
#define UBASE_OPC_UE2UE_UBASE 0xF00E

#define UBASE_CMD_FLAG_IN BIT(0)
#define UBASE_CMD_FLAG_OUT BIT(1)
#define UBASE_CMD_FLAG_NEXT BIT(2)
#define UBASE_CMD_FLAG_WR BIT(3)
#define UBASE_CMD_FLAG_NO_INTR BIT(4)
#define UBASE_CMD_FLAG_GET_BD_NUM BIT(6)

#define UBASE_SUPPORT_TA_EXTDB_BUF_B 2
#define UBASE_SUPPORT_TA_TIMER_BUF_B 3
#define UBASE_SUPPORT_IP_OVER_URMA_B 17
#define UBASE_SUPPORT_UBL_B 0
#define UBASE_MAX_VL_NUM 16
#define UBASE_MAX_SL_NUM 16

/* Mailbox sub-opcodes (cmd field in UBCMbox.cmd_tag) */
#define UBC_MB_CREATE_JFS_CONTEXT   0x04
#define UBC_MB_MODIFY_JFS_CONTEXT   0x05
#define UBC_MB_QUERY_JFS_CONTEXT    0x06
#define UBC_MB_DESTROY_JFS_CONTEXT  0x07
#define UBC_MB_CREATE_JFC_CONTEXT   0x24
#define UBC_MB_CREATE_JFR_CONTEXT   0x54

/* udma_jetty_ctx size and field extraction constants */
#define UDMA_JETTY_CTX_SIZE  256
#define UDMA_JFC_CTX_SIZE    256
#define UDMA_JFR_CTX_SIZE    256

/* WQE / SQE constants for DMA transport */
#define UDMA_SQE_SIZE          64   /* one WQEBB = 64 bytes */
#define UDMA_SQE_CTL_LEN_SEND  48   /* sizeof(udma_sqe_ctl) for SEND opcode */
#define UDMA_SQE_OPCODE_SEND  0   /* matches kernel enum udma_sq_opcode UDMA_OPC_SEND */
#define UDMA_SQE_INLINE_EN_BIT 6  /* bit position in DW1 */
#define UDMA_JFS_SGE_SIZE      16   /* sizeof(udma_normal_sge) */

/* URMA inter-node data transfer msg_code (fits in 3-bit msg_code field) */
#define UB_MSG_CODE_URMA_DATA  7
#define UBASE_CTRLQ_BB_LEN 32
#define UBASE_CTRLQ_HDR_LEN 12
#define UBASE_CTRLQ_DATA_LEN (UBASE_CTRLQ_BB_LEN - UBASE_CTRLQ_HDR_LEN)
#define UBASE_CTRLQ_SER_TYPE_QOS 0x04
#define UBASE_CTRLQ_OPC_QUERY_VL 0x01
#define UBASE_CTRLQ_OPC_QUERY_SL 0x02

/* --- URMA RX buffering for early-arriving packets --- */
#define UBC_URMA_RX_BUF_MAX 64

typedef struct UBCUrxBufEntry {
    uint32_t dst_jetty;
    uint32_t data_len;
    uint8_t  data[4096];
} UBCUrxBufEntry;

typedef struct QEMU_PACKED UBCCmdqDesc {
    uint16_t opcode;
    uint8_t flag;
    uint8_t bd_num;
    uint16_t ret;
    uint16_t rsv;
    uint32_t data[6];
} UBCCmdqDesc;

typedef struct QEMU_PACKED UBCQueryVersionResp {
    uint32_t fw_version;
    uint8_t rsv[20];
} UBCQueryVersionResp;

typedef struct QEMU_PACKED UBCResCmdResp {
    uint32_t cap_bits[3];
    uint32_t rsvd0[3];

    uint8_t node_type;
    uint8_t rsvd1;
    uint16_t ceq_vector_num;
    uint16_t aeq_vector_num;
    uint16_t misc_vector_num;
    uint16_t aeqe_size;
    uint16_t ceqe_size;
    uint16_t udma_cqe_size;
    uint16_t nic_cqe_size;
    uint32_t aeqe_depth;
    uint32_t ceqe_depth;
    uint32_t udma_jfs_max_cnt;
    uint8_t rsvd2[4];

    uint32_t udma_jfs_depth;
    uint32_t udma_jfr_max_cnt;
    uint8_t rsvd3[4];
    uint32_t udma_jfr_depth;
    uint8_t rsvd4[12];
    uint32_t udma_jfc_max_cnt;

    uint8_t rsvd5[4];
    uint32_t udma_jfc_depth;
    uint8_t rsvd6[24];

    uint32_t nic_jfs_max_cnt;
    uint8_t rsvd7[4];
    uint32_t nic_jfs_depth;
    uint32_t nic_jfr_max_cnt;
    uint8_t rsvd8[4];
    uint32_t nic_jfr_depth;
    uint32_t rsvd9[2];

    uint32_t rsvd10;
    uint32_t nic_jfc_max_cnt;
    uint8_t rsvd11[4];
    uint32_t nic_jfc_depth;
    uint8_t rsvd12[16];

    uint8_t rsvd13[8];
    uint32_t total_ue_num;
    uint8_t rsvd14[16];
    uint16_t rsvd_jetty_cnt;
    uint16_t mac_stats_num;

    uint32_t ta_extdb_buf_size;
    uint32_t ta_timer_buf_size;
    uint32_t public_jetty_cnt;
    uint8_t rsvd15[10];
    uint8_t udma_tp_resp_vl_offset;
    uint8_t ue_num;
    uint8_t rsvd16[8];

    uint8_t rsvd17[8];
    uint32_t udma_rc_depth;
    uint8_t rsvd18[4];
    uint32_t jtg_max_cnt;
    uint32_t rc_max_cnt_per_vl;
    uint8_t rsvd19[8];

    uint8_t rsvd20[32];
} UBCResCmdResp;

typedef struct QEMU_PACKED UBCCtrlInfoResp {
    uint32_t rsvd0[2];
    uint8_t flags;
    uint8_t rsvd1[15];
} UBCCtrlInfoResp;

typedef struct QEMU_PACKED UBCPortBitmapResp {
    uint32_t logic_port_bitmap;
    uint32_t chip_id;
    uint32_t die_id;
    uint32_t resv[3];
} UBCPortBitmapResp;

typedef struct QEMU_PACKED UBCChipInfoResp {
    uint16_t nl_port_id;
    uint16_t chip_id;
    uint16_t die_id;
    uint16_t io_port_id;
    uint16_t ue_id;
    uint16_t ub_port_logic_id;
    uint16_t nl_id;
    uint16_t io_port_logic_id;
} UBCChipInfoResp;

typedef struct QEMU_PACKED UBCOorResp {
    uint8_t oor_en;
    uint8_t reorder_cq_buffer_en;
    uint8_t reorder_cap;
    uint8_t reorder_cq_shift;
    uint32_t on_flight_size;
    uint8_t dynamic_ack_timeout;
    uint8_t rsvd0[15];
} UBCOorResp;

/*
 * Response for UDMA_CMD_QUERY_UE_RES (0x0002).
 * Must match guest struct udma_cmd_ue_resource in udma_cmd.h exactly.
 * The struct is split into BD0..BD3 blocks; total 128 bytes.
 */
typedef struct QEMU_PACKED UBCUeResourceResp {
    /* BD0 */
    uint16_t jfs_num_shift : 4;
    uint16_t jfr_num_shift : 4;
    uint16_t jfc_num_shift : 4;
    uint16_t jetty_num_shift : 4;

    uint16_t jetty_grp_num;

    uint16_t jfs_depth_shift : 4;
    uint16_t jfr_depth_shift : 4;
    uint16_t jfc_depth_shift : 4;
    uint16_t cqe_size_shift : 4;

    uint16_t jfs_sge : 5;
    uint16_t jfr_sge : 5;
    uint16_t jfs_rsge : 6;

    uint16_t max_jfs_inline_sz;
    uint16_t max_jfc_inline_sz;
    uint32_t cap_info;

    uint16_t trans_mode : 5;
    uint16_t ue_num : 8;
    uint16_t virtualization : 1;
    uint16_t dcqcn_sw_en : 1;
    uint16_t rsvd0 : 1;

    uint16_t ue_cnt;
    uint8_t  ue_id;
    uint8_t  default_cong_alg;
    uint8_t  cons_ctrl_alg;
    uint8_t  cc_priority_cnt;

    /* BD1 */
    uint16_t src_addr_tbl_sz;
    uint16_t src_addr_tbl_num;
    uint16_t dest_addr_tbl_sz;
    uint16_t dest_addr_tbl_num;
    uint16_t seid_upi_tbl_sz;
    uint16_t seid_upi_tbl_num;
    uint16_t tpm_tbl_sz;
    uint16_t tpm_tbl_num;
    uint32_t tp_range;
    uint8_t  port_num;
    uint8_t  port_id;
    uint8_t  rsvd1[2];
    uint16_t rc_queue_num;
    uint16_t rc_depth;
    uint8_t  rc_entry;
    uint8_t  rsvd2[3];

    /* BD2 */
    uint16_t well_known_jetty_start;
    uint16_t well_known_jetty_num;
    uint16_t ccu_jetty_start;
    uint16_t ccu_jetty_num;
    uint16_t drv_jetty_start;
    uint16_t drv_jetty_num;
    uint16_t cache_lock_jetty_start;
    uint16_t cache_lock_jetty_num;
    uint16_t normal_jetty_start;
    uint16_t normal_jetty_num;
    uint16_t standard_jetty_start;
    uint16_t standard_jetty_num;
    uint32_t rsvd3[2];

    /* BD3 */
    uint32_t max_write_size;
    uint32_t max_read_size;
    uint32_t max_cas_size;
    uint32_t max_fetch_and_add_size;
    uint32_t atomic_feat;
    uint32_t rsvd4[3];
} UBCUeResourceResp;

typedef struct QEMU_PACKED UBCSlVlResp {
    uint8_t sl_num;
    uint8_t sl_vl[23];
} UBCSlVlResp;

typedef struct QEMU_PACKED UBCTmQueueResp {
    uint16_t bus_ue_id;
    uint8_t queue_num;
    uint8_t resv0;
    uint8_t queue_vl[UBASE_MAX_VL_NUM];
    uint8_t queue_id[UBASE_MAX_VL_NUM];
    uint8_t qset_id[UBASE_MAX_VL_NUM];
    uint16_t link_vld_bitmap;
    uint8_t resv1[2];
} UBCTmQueueResp;

typedef struct QEMU_PACKED UBCTmQsetResp {
    uint16_t bus_ue_id;
    uint8_t qset_num;
    uint8_t rate_limit_bypass;
    uint8_t ir_b[UBASE_MAX_VL_NUM];
    uint8_t ir_u[UBASE_MAX_VL_NUM];
    uint8_t ir_s[UBASE_MAX_VL_NUM];
    uint8_t bs_b[UBASE_MAX_VL_NUM];
    uint8_t bs_s[UBASE_MAX_VL_NUM];
    uint32_t rate[UBASE_MAX_VL_NUM];
    uint8_t qset_id[UBASE_MAX_VL_NUM];
    uint8_t pri_id[UBASE_MAX_VL_NUM];
    uint16_t qset_pri_link_vld;
    uint16_t qset_sch_mode;
    uint8_t qset_weight[UBASE_MAX_VL_NUM];
    uint8_t resv1[16];
} UBCTmQsetResp;

typedef struct QEMU_PACKED UBCMbox {
    uint32_t in_param_l;
    uint32_t in_param_h;
    uint32_t cmd_tag;
    uint32_t ctrl;
    uint32_t status;
} UBCMbox;

typedef struct QEMU_PACKED UBCUe2UeCommonHead {
    uint16_t bus_ue_id;
    uint16_t mbx_ue_id;
    uint16_t sub_cmd;
    uint16_t status;
} UBCUe2UeCommonHead;

typedef struct QEMU_PACKED UBCUe2UeCtrlqHead {
    UBCUe2UeCommonHead head;
    uint16_t seq;
    uint16_t in_size;
    uint16_t out_size;
    uint8_t flags;
    uint8_t rsv;
} UBCUe2UeCtrlqHead;

typedef struct QEMU_PACKED UBCCtrlqMsgHdr {
    uint8_t service_ver;
    uint8_t service_type;
    uint8_t bb_num;
    uint8_t opcode;
    uint8_t ret;
    uint16_t seq;
    uint8_t mbx_ue_id;
    uint16_t bus_ue_id;
    uint16_t rsv;
} UBCCtrlqMsgHdr;

typedef struct QEMU_PACKED UBCCtrlqQuerySlResp {
    uint16_t unic_sl_bitmap;
    uint16_t rc_max_cnt;
    uint16_t udma_tp_sl_bitmap;
    uint16_t udma_ctp_sl_bitmap;
    uint8_t rsv[12];
} UBCCtrlqQuerySlResp;

typedef struct QEMU_PACKED UBCCtrlqQueryVlResp {
    uint16_t vl_bitmap;
    uint8_t rsv[18];
} UBCCtrlqQueryVlResp;

static void ubc_raise_cmdq_event(BusControllerDev *ubc_dev);

static uint32_t ub_msgq_cq_int_ro(BusControllerState *s)
{
    uint32_t status = ub_get_long(s->msgq_reg + CQ_INT_STATUS) & 0x1;
    uint32_t mask = ub_get_long(s->msgq_reg + CQ_INT_MASK) & 0x1;

    return status & ~mask;
}

static void ub_msgq_update_irq(BusControllerState *s)
{
    uint32_t ro = ub_msgq_cq_int_ro(s);
    qemu_log("ub_msgq_update_irq %s ro=%u mask=%u status=%u\n",
             s->ubc_dev ? s->ubc_dev->parent.qdev.id : "<unknown>",
             ro,
             ub_get_long(s->msgq_reg + CQ_INT_MASK) & 0x1,
             ub_get_long(s->msgq_reg + CQ_INT_STATUS) & 0x1);
    qemu_set_irq(s->irq, ro ? 1 : 0);
}

static uint64_t ub_msgq_reg_read(void *opaque, hwaddr addr, unsigned len)
{
    BusControllerState *s = opaque;
    uint64_t val;

    if (addr == CQ_INT_RO) {
        return ub_msgq_cq_int_ro(s);
    }

    switch (len) {
    case BYTE_SIZE:
        val = ub_get_byte(s->msgq_reg + addr);
        break;
    case WORD_SIZE:
        val = ub_get_word(s->msgq_reg + addr);
        break;
    case DWORD_SIZE:
        val = ub_get_long(s->msgq_reg + addr);
        break;
    default:
        qemu_log("invalid argument len 0x%x\n", len);
        val = ~0x0;
        break;
    }

    return val;
}

static void ub_msgq_reg_write(void *opaque, hwaddr addr, uint64_t val, unsigned len)
{
    BusControllerState *s = opaque;

    if (len != DWORD_SIZE) {
        switch (len) {
        case BYTE_SIZE:
            ub_set_byte(s->msgq_reg + addr, val);
            break;
        case WORD_SIZE:
            ub_set_word(s->msgq_reg + addr, val);
            break;
        default:
            qemu_log("invalid argument len 0x%x val 0x%" PRIx64 "\n", len, val);
            return;
        }
        return;
    }

    switch (addr) {
    case CQ_INT_MASK:
        ub_set_long(s->msgq_reg + addr, val & 0x1);
        ub_msgq_update_irq(s);
        return;
    case CQ_INT_STATUS:
        if (val & 0x1) {
            ub_set_long(s->msgq_reg + addr, 0);
        }
        ub_msgq_update_irq(s);
        return;
    case CQ_INT_SET:
        if (val & 0x1) {
            ub_set_long(s->msgq_reg + CQ_INT_STATUS, 0x1);
        }
        ub_msgq_update_irq(s);
        return;
    default:
        break;
    }

    switch (len) {
    case DWORD_SIZE:
        ub_set_long(s->msgq_reg + addr, val);
        break;
    default:
        /* As length is under guest control, handle illegal values. */
        qemu_log("invalid argument len 0x%x val 0x%" PRIx64 "\n", len, val);
        return;
    }

    /* only support 1 queue */
    switch (addr) {
    case SQ_PI:
        if (!s->msgq.sq_inited || !s->msgq.rq_inited || !s->msgq.cq_inited) {
            qemu_log("skip SQ_PI processing before msgq fully initialized: sq=%d rq=%d cq=%d pi=%" PRIu64 "\n",
                     s->msgq.sq_inited, s->msgq.rq_inited, s->msgq.cq_inited, val);
            break;
        }
        if (msgq_process_task(s, val)) {
            ub_try_inject_remote_cfg_notifies(s);
        }
        break;
    case SQ_ADDR_H:
        msgq_sq_init(s);
        break;
    case CQ_ADDR_H:
        msgq_cq_init(s);
        break;
    case RQ_ADDR_H:
        msgq_rq_init(s);
        break;
    case MSGQ_RST:
        msgq_handle_rst(s);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps ub_msgq_reg_ops = {
    .read = ub_msgq_reg_read,
    .write = ub_msgq_reg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static const MemoryRegionOps ub_fm_msgq_reg_ops = {
    .read = ub_fm_msgq_reg_read,
    .write = ub_fm_msgq_reg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static inline uint32_t ubc_ers2_read32(BusControllerDev *ubc_dev, hwaddr off)
{
    return ldl_le_p(ubc_dev->ers[2].storage + off);
}

static inline void ubc_ers2_write32(BusControllerDev *ubc_dev, hwaddr off,
                                    uint32_t val)
{
    stl_le_p(ubc_dev->ers[2].storage + off, val);
}

static inline uint64_t ubc_read_q_base(BusControllerDev *ubc_dev, hwaddr lo_reg)
{
    uint64_t lo = ubc_ers2_read32(ubc_dev, lo_reg);
    uint64_t hi = ubc_ers2_read32(ubc_dev, lo_reg + 4);

    return (hi << 32) | lo;
}

static inline uint16_t ubc_read_q_depth(BusControllerDev *ubc_dev, hwaddr depth_reg)
{
    uint32_t raw = ubc_ers2_read32(ubc_dev, depth_reg);

    return raw ? (uint16_t)(raw << 3) : 0;
}

static inline AddressSpace *ubc_dma_as(BusControllerDev *ubc_dev)
{
    UBDevice *ub_dev = &ubc_dev->parent;
    AddressSpace *as = ub_device_iommu_address_space(ub_dev);

    return as ? as : &address_space_memory;
}

static bool ubc_try_iova_linear_fallback(dma_addr_t iova, dma_addr_t *gpa)
{
    const dma_addr_t iova_base = 0xFFFE0000000ULL;
    const dma_addr_t ram_base = UBC_GUEST_RAM_BASE;
    const dma_addr_t ram_size = UBC_GUEST_RAM_SIZE;
    const dma_addr_t iova_limit = iova_base + ram_size;
    dma_addr_t off;

    if (iova < iova_base || iova >= iova_limit) {
        return false;
    }

    off = iova - iova_base;
    if (off >= ram_size) {
        return false;
    }

    *gpa = ram_base + off;
    return true;
}

static bool ubc_try_iova_lowbits_fallback(dma_addr_t iova, dma_addr_t *gpa)
{
    dma_addr_t off = iova & (UBC_GUEST_RAM_SIZE - 1);

    *gpa = UBC_GUEST_RAM_BASE + off;
    return true;
}

/*
 * Translate a guest kernel virtual address to guest physical address
 * using QEMU's CPU page table walker. Handles both direct-mapped
 * (kmalloc) and vmalloc addresses — unlike simple PAGE_OFFSET subtraction
 * which only works for the direct linear map region.
 */
static bool ubc_kva_to_gpa(dma_addr_t kva, dma_addr_t *gpa)
{
    CPUState *cpu = first_cpu;
    hwaddr phys;

    if (!cpu) {
        return false;
    }

    /* Only handle kernel virtual addresses (bit 63 set on ARM64) */
    if (!(kva >> 63)) {
        return false;
    }

    phys = cpu_get_phys_page_debug(cpu, kva);
    if (phys == (hwaddr)-1) {
        return false;
    }

    *gpa = (dma_addr_t)phys;
    return true;
}

static bool ubc_try_learned_bias_fallback(BusControllerDev *ubc_dev,
                                          const UBCmdQueueState *q,
                                          dma_addr_t iova,
                                          dma_addr_t *gpa)
{
    /* CRQ queues use their own bias (learned by scanning guest RAM) */
    if (q == &ubc_dev->cmd_crq || q == &ubc_dev->ctrl_crq) {
        if (!ubc_dev->crq_bias_valid ||
            ubc_dev->crq_bias_iova_base != ubc_dev->cmd_crq.base ||
            !ubc_dev->cmd_crq.base) {
            return false;
        }
        *gpa = (dma_addr_t)((int64_t)iova + ubc_dev->crq_iova_to_gpa_bias);
        return true;
    }

    if (!ubc_dev->csq_bias_valid ||
        ubc_dev->csq_bias_iova_base != ubc_dev->cmd_csq.base ||
        !ubc_dev->cmd_csq.base) {
        return false;
    }

    if (q != &ubc_dev->cmd_csq && q != &ubc_dev->ctrl_csq) {
        return false;
    }

    *gpa = (dma_addr_t)((int64_t)iova + ubc_dev->csq_iova_to_gpa_bias);
    return true;
}

/*
 * General-purpose DMA read from an IOVA address.
 * Tries IOMMU address space first, then CSQ learned bias,
 * then linear IOVA mapping and low-bits mapping.
 */
static MemTxResult ubc_dma_read(BusControllerDev *ubc_dev, dma_addr_t iova,
                                 void *buf, size_t len)
{
    AddressSpace *as = ubc_dma_as(ubc_dev);
    MemTxResult ret;
    dma_addr_t gpa;
    const uint32_t *first_dw = NULL;

    /* Try ARM64 KVA-to-GPA first — KVA addresses are never valid IOVAs,
     * and the IOMMU path would succeed with zeros for unmapped addresses. */
    if (ubc_kva_to_gpa(iova, &gpa)) {
        ret = address_space_read(&address_space_memory, gpa,
                                MEMTXATTRS_UNSPECIFIED, buf, len);
        if (ret == MEMTX_OK) {
            first_dw = (const uint32_t *)buf;
            fprintf(stderr, "ubc_dma_read: KVA iova=%#lx gpa=%#lx dw0=%#x\n",
                    (unsigned long)iova, (unsigned long)gpa, first_dw[0]);
            return ret;
        }
    }

    /* Try IOMMU address space */
    ret = address_space_read(as, iova, MEMTXATTRS_UNSPECIFIED, buf, len);
    if (ret == MEMTX_OK) {
        return ret;
    }

    /* Try CSQ learned bias — same IOMMU domain as CMDQ descriptors */
    if (ubc_dev->csq_bias_valid) {
        gpa = (dma_addr_t)((int64_t)iova + ubc_dev->csq_iova_to_gpa_bias);
        ret = address_space_read(&address_space_memory, gpa,
                                MEMTXATTRS_UNSPECIFIED, buf, len);
        if (ret == MEMTX_OK) {
            return ret;
        }
    }

    /* Try linear IOVA fallback */
    if (ubc_try_iova_linear_fallback(iova, &gpa)) {
        ret = address_space_read(&address_space_memory, gpa,
                                MEMTXATTRS_UNSPECIFIED, buf, len);
        if (ret == MEMTX_OK) {
            return ret;
        }
    }

    /* Try low-bits fallback */
    if (ubc_try_iova_lowbits_fallback(iova, &gpa)) {
        ret = address_space_read(&address_space_memory, gpa,
                                MEMTXATTRS_UNSPECIFIED, buf, len);
        if (ret == MEMTX_OK) {
            return ret;
        }
    }

    /* Last resort: treat iova as GPA */
    return address_space_read(&address_space_memory, iova,
                             MEMTXATTRS_UNSPECIFIED, buf, len);
}

static MemTxResult ubc_dma_write(BusControllerDev *ubc_dev, dma_addr_t iova,
                                  const void *buf, size_t len)
{
    AddressSpace *as = ubc_dma_as(ubc_dev);
    MemTxResult ret;
    dma_addr_t gpa;
    static int log_count = 0;

    /* Try ARM64 KVA-to-GPA first — KVA addresses are never valid IOVAs */
    if (ubc_kva_to_gpa(iova, &gpa)) {
        ret = address_space_write(&address_space_memory, gpa,
                                 MEMTXATTRS_UNSPECIFIED, buf, len);
        if (ret == MEMTX_OK) {
            return ret;
        }
    }

    /* Try IOMMU address space */
    ret = address_space_write(as, iova, MEMTXATTRS_UNSPECIFIED, buf, len);
    if (ret == MEMTX_OK) {
        return ret;
    }

    /* Try CSQ learned bias — same IOMMU domain as CMDQ descriptors */
    if (ubc_dev->csq_bias_valid) {
        gpa = (dma_addr_t)((int64_t)iova + ubc_dev->csq_iova_to_gpa_bias);
        ret = address_space_write(&address_space_memory, gpa,
                                 MEMTXATTRS_UNSPECIFIED, buf, len);
        if (ret == MEMTX_OK) {
            return ret;
        }
    }

    /* Try linear IOVA fallback */
    if (ubc_try_iova_linear_fallback(iova, &gpa)) {
        ret = address_space_write(&address_space_memory, gpa,
                                 MEMTXATTRS_UNSPECIFIED, buf, len);
        if (ret == MEMTX_OK) {
            return ret;
        }
    }

    /* Try low-bits fallback */
    if (ubc_try_iova_lowbits_fallback(iova, &gpa)) {
        ret = address_space_write(&address_space_memory, gpa,
                                 MEMTXATTRS_UNSPECIFIED, buf, len);
        if (ret == MEMTX_OK) {
            return ret;
        }
        if (log_count < 20) {
            fprintf(stderr, "ubc_dma_write: ALL FALLBACKS FAILED iova=%#lx lowbits_gpa=%#lx len=%zu\n",
                    (uint64_t)iova, (uint64_t)gpa, len);
            log_count++;
        }
    }

    /* Last resort: treat iova as GPA */
    return address_space_write(&address_space_memory, iova,
                              MEMTXATTRS_UNSPECIFIED, buf, len);
}

static inline bool ubc_is_query_fw_desc(const UBCCmdqDesc *desc)
{
    size_t i;
    const uint8_t must = UBASE_CMD_FLAG_IN | UBASE_CMD_FLAG_WR |
                         UBASE_CMD_FLAG_NO_INTR;
    const uint8_t forbid = UBASE_CMD_FLAG_OUT | UBASE_CMD_FLAG_NEXT |
                           UBASE_CMD_FLAG_GET_BD_NUM;

    if (le16_to_cpu(desc->opcode) != UBASE_OPC_QUERY_FW_VER ||
        desc->bd_num != 1 ||
        (desc->flag & must) != must ||
        (desc->flag & forbid) != 0 ||
        desc->ret != 0 || desc->rsv != 0) {
        return false;
    }

    for (i = 0; i < ARRAY_SIZE(desc->data); i++) {
        if (desc->data[i] != 0) {
            return false;
        }
    }
    return true;
}

static inline bool ubc_head_desc_suspicious(const UBCCmdqDesc *desc)
{
    return le16_to_cpu(desc->opcode) == 0 ||
           !(desc->flag & UBASE_CMD_FLAG_IN) ||
           (desc->flag & UBASE_CMD_FLAG_OUT) ||
           desc->bd_num == 0;
}

static bool ubc_get_ram_range(dma_addr_t *base, dma_addr_t *size)
{
    /* 
     * In a typical virt machine, RAM starts at 0x40000000.
     * We try to find the actual RAM range from the system memory.
     */
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *mr;

    /* Common case for 'virt' machine */
    mr = memory_region_find(sysmem, 0x40000000, 1).mr;
    if (mr && memory_region_is_ram(mr)) {
        *base = 0x40000000;
        *size = memory_region_size(mr);
        memory_region_unref(mr);
        return true;
    }

    /* Fallback or other machines: scan first 32GB for any RAM */
    for (uint64_t addr = 0; addr < 32 * GiB; addr += 128 * MiB) {
        mr = memory_region_find(sysmem, addr, 1).mr;
        if (mr && memory_region_is_ram(mr)) {
            *base = addr;
            *size = memory_region_size(mr);
            memory_region_unref(mr);
            return true;
        }
        if (mr) {
            memory_region_unref(mr);
        }
    }

    return false;
}

static bool ubc_learn_csq_bias(BusControllerDev *ubc_dev)
{
    UBCmdQueueState *csq = &ubc_dev->cmd_csq;
    dma_addr_t ram_base, ram_size, ram_end;
    uint8_t buf[0x10000];
    dma_addr_t cur;

    if (!csq->base || !csq->depth) {
        return false;
    }

    /* 
     * Optimization: try direct translation first (works if IOVA == KVA and
     * we can peek into guest page tables).
     */
    {
        CPUState *cpu = first_cpu;
        hwaddr gpa_try = cpu_get_phys_page_debug(cpu, csq->base);
        if (gpa_try != -1) {
            ubc_dev->csq_iova_to_gpa_bias = (int64_t)gpa_try - (int64_t)csq->base;
            ubc_dev->csq_bias_iova_base = csq->base;
            ubc_dev->csq_bias_valid = true;
            qemu_log("ubc cmdq learned csq bias via cpu_get_phys_page_debug: %" PRId64 "\n",
                     (int64_t)ubc_dev->csq_iova_to_gpa_bias);
            return true;
        }
    }

    if (!ubc_get_ram_range(&ram_base, &ram_size)) {
        qemu_log("ubc cmdq failed to detect guest RAM range\n");
        return false;
    }
    ram_end = ram_base + ram_size;

    for (cur = ram_base; cur < ram_end; cur += sizeof(buf)) {
        size_t chunk = MIN((dma_addr_t)sizeof(buf), ram_end - cur);
        size_t off;

        if (dma_memory_read(&address_space_memory, cur, buf, chunk,
                            MEMTXATTRS_MEMORY) != MEMTX_OK) {
            continue;
        }

        for (off = 0; off + sizeof(UBCCmdqDesc) <= chunk; off += sizeof(UBCCmdqDesc)) {
            UBCCmdqDesc *cand = (UBCCmdqDesc *)(buf + off);
            UBCCmdqDesc *next = NULL;
            dma_addr_t gpa = cur + off;

            if ((gpa & (sizeof(UBCCmdqDesc) - 1)) != 0 ||
                !ubc_is_query_fw_desc(cand)) {
                continue;
            }
            if (off + 2 * sizeof(UBCCmdqDesc) <= chunk) {
                next = (UBCCmdqDesc *)(buf + off + sizeof(*cand));
            }
            if (next && (next->opcode != 0 || next->flag != 0 ||
                         next->bd_num != 0 || next->ret != 0 ||
                         next->rsv != 0)) {
                continue;
            }

            ubc_dev->csq_iova_to_gpa_bias = (int64_t)gpa - (int64_t)csq->base;
            ubc_dev->csq_bias_iova_base = csq->base;
            ubc_dev->csq_bias_valid = true;
            qemu_log("ubc cmdq learned csq bias iova_base=%#" PRIx64
                     " gpa_base=%#" PRIx64 " bias=%" PRId64 "\n",
                     (uint64_t)csq->base, (uint64_t)gpa,
                     (int64_t)ubc_dev->csq_iova_to_gpa_bias);
            return true;
        }
    }

    qemu_log("ubc cmdq failed to learn csq bias for iova_base=%#" PRIx64 "\n",
             (uint64_t)csq->base);
    return false;
}

/*
 * Check if a GPA range is entirely zero-filled.
 */
static bool ubc_is_zero_range(dma_addr_t gpa, size_t len)
{
    uint8_t buf[4096];

    while (len > 0) {
        size_t chunk = MIN(len, sizeof(buf));

        if (dma_memory_read(&address_space_memory, gpa, buf, chunk,
                            MEMTXATTRS_MEMORY) != MEMTX_OK) {
            return false;
        }
        for (size_t i = 0; i < chunk; i++) {
            if (buf[i] != 0) {
                return false;
            }
        }
        gpa += chunk;
        len -= chunk;
    }
    return true;
}

/*
 * Learn the CRQ IOVA-to-GPA bias by scanning guest RAM near the CSQ buffer.
 *
 * The CSQ and CRQ are allocated sequentially by dma_alloc_coherent() in the
 * guest, so the CRQ GPA is typically close to the CSQ GPA.  Since the UMMU
 * IOMMU translation is not functional in pure-emulation mode, we find the
 * CRQ buffer by looking for a page-aligned, zero-filled block of the correct
 * size near the known CSQ location.
 */
static bool ubc_learn_crq_bias(BusControllerDev *ubc_dev)
{
    UBCmdQueueState *crq = &ubc_dev->cmd_crq;
    UBCmdQueueState *csq = &ubc_dev->cmd_csq;
    dma_addr_t csq_gpa, csq_size, crq_size;
    dma_addr_t scan_start, scan_end, gpa;
    dma_addr_t ram_base, ram_size, ram_end;
    const dma_addr_t scan_window = 4 * 1024 * 1024;

    if (!ubc_dev->csq_bias_valid || !crq->base || !crq->depth || !csq->base) {
        return false;
    }

    if (ubc_dev->crq_bias_valid &&
        ubc_dev->crq_bias_iova_base == crq->base) {
        return true; /* already learned for this IOVA */
    }

    csq_gpa = (dma_addr_t)((int64_t)csq->base + ubc_dev->csq_iova_to_gpa_bias);
    csq_size = (dma_addr_t)csq->depth * sizeof(UBCCmdqDesc);
    crq_size = (dma_addr_t)crq->depth * sizeof(UBCCmdqDesc);

    /* Scan forward from csq_gpa + csq_size (CRQ likely right after CSQ) */
    scan_start = csq_gpa + csq_size;
    if (scan_start & 0xfffu) {
        scan_start = (scan_start + 0x1000u) & ~(dma_addr_t)0xfffu;
    }
    scan_end = scan_start + scan_window;
    if (!ubc_get_ram_range(&ram_base, &ram_size)) {
        return false;
    }
    ram_end = ram_base + ram_size;

    if (scan_end > ram_end) {
        scan_end = ram_end;
    }

    for (gpa = scan_start; gpa + crq_size <= scan_end; gpa += 0x1000) {
        if (ubc_is_zero_range(gpa, crq_size)) {
            ubc_dev->crq_iova_to_gpa_bias = (int64_t)gpa - (int64_t)crq->base;
            ubc_dev->crq_bias_iova_base = crq->base;
            ubc_dev->crq_bias_valid = true;
            qemu_log("ubc learned crq bias crq_iova=%#" PRIx64
                     " crq_gpa=%#" PRIx64 " bias=%" PRId64 "\n",
                     (uint64_t)crq->base, (uint64_t)gpa,
                     ubc_dev->crq_iova_to_gpa_bias);
            return true;
        }
    }

    qemu_log("ubc failed to learn crq bias for iova=%#" PRIx64
             " (scanned %#" PRIx64 "..%#" PRIx64 ")\n",
             (uint64_t)crq->base, (uint64_t)scan_start,
             (uint64_t)scan_end);
    return false;
}

static MemTxResult ubc_read_desc(const UBCmdQueueState *q,
                                 BusControllerDev *ubc_dev,
                                 uint16_t idx,
                                 UBCCmdqDesc *desc)
{
    AddressSpace *as;
    dma_addr_t addr;
    dma_addr_t fallback_addr;
    MemTxResult ret;

    as = ubc_dma_as(ubc_dev);
    addr = q->base + (dma_addr_t)idx * sizeof(*desc);
    ret = dma_memory_read(as, addr, desc, sizeof(*desc), MEMTXATTRS_MEMORY);
    if (ret == MEMTX_OK) {
        return ret;
    }

    ret = dma_memory_read(&address_space_memory, addr, desc, sizeof(*desc),
                          MEMTXATTRS_MEMORY);
    if (ret == MEMTX_OK) {
        return ret;
    }

    if (ubc_try_learned_bias_fallback(ubc_dev, q, addr, &fallback_addr)) {
        ret = dma_memory_read(&address_space_memory, fallback_addr, desc,
                              sizeof(*desc), MEMTXATTRS_MEMORY);
        if (ret == MEMTX_OK) {
            return ret;
        }
    }

    if (!ubc_try_iova_linear_fallback(addr, &fallback_addr)) {
        if (!ubc_try_iova_lowbits_fallback(addr, &fallback_addr)) {
            qemu_log("ubc cmdq read no fallback iova=%#" PRIx64 " ret=%d\n",
                     (uint64_t)addr, ret);
            return ret;
        }
    }

    ret = dma_memory_read(&address_space_memory, fallback_addr, desc,
                          sizeof(*desc), MEMTXATTRS_MEMORY);
    if (ret != MEMTX_OK) {
        qemu_log("ubc cmdq read fallback failed iova=%#" PRIx64 " gpa=%#" PRIx64
                 " ret=%d\n", (uint64_t)addr, (uint64_t)fallback_addr, ret);
    } else {
        qemu_log("ubc cmdq read fallback iova=%#" PRIx64 " gpa=%#" PRIx64 "\n",
                 (uint64_t)addr, (uint64_t)fallback_addr);
    }
    return ret;
}

static MemTxResult ubc_write_desc(const UBCmdQueueState *q,
                                  BusControllerDev *ubc_dev,
                                  uint16_t idx,
                                  const UBCCmdqDesc *desc)
{
    AddressSpace *as;
    dma_addr_t addr;
    dma_addr_t fallback_addr;
    MemTxResult ret;

    addr = q->base + (dma_addr_t)idx * sizeof(*desc);
    as = ubc_dma_as(ubc_dev);

    /* Try ARM64 KVA-to-GPA first — handles kernel DMA addresses (bit 63 set)
     * that the IOMMU passthrough silently discards.  This is the primary
     * translation for both CSQ and CRQ descriptor writes. */
    if (ubc_kva_to_gpa(addr, &fallback_addr)) {
        ret = address_space_write(&address_space_memory, fallback_addr,
                                 MEMTXATTRS_UNSPECIFIED, desc, sizeof(*desc));
        if (ret == MEMTX_OK) {
            return ret;
        }
    }

    /*
     * CRQ (completion response queue) base addresses are IOVAs from the
     * guest DMA allocator.  The IOMMU passthrough "succeeds" (MEMTX_OK)
     * even when the GPA is beyond RAM, silently discarding the write.
     * For CRQ descriptors, skip the IOMMU path and go straight to the
     * fallback chain so the data actually lands in guest RAM.
     */
    if (q != &ubc_dev->cmd_crq && q != &ubc_dev->ctrl_crq) {
        ret = dma_memory_write(as, addr, desc, sizeof(*desc), MEMTXATTRS_MEMORY);
        if (ret == MEMTX_OK) {
            return ret;
        }
        ret = dma_memory_write(&address_space_memory, addr, desc, sizeof(*desc),
                               MEMTXATTRS_MEMORY);
        if (ret == MEMTX_OK) {
            return ret;
        }
    }

    if (ubc_try_learned_bias_fallback(ubc_dev, q, addr, &fallback_addr)) {
        ret = dma_memory_write(&address_space_memory, fallback_addr, desc,
                               sizeof(*desc), MEMTXATTRS_MEMORY);
        if (ret == MEMTX_OK) {
            if (q == &ubc_dev->cmd_crq || q == &ubc_dev->ctrl_crq) {
                qemu_log("ubc cmdq crq write fallback bias iova=%#" PRIx64
                         " gpa=%#" PRIx64 "\n",
                         (uint64_t)addr, (uint64_t)fallback_addr);
            }
            return ret;
        }
    }

    if (!ubc_try_iova_linear_fallback(addr, &fallback_addr)) {
        if (!ubc_try_iova_lowbits_fallback(addr, &fallback_addr)) {
            qemu_log("ubc cmdq write no fallback iova=%#" PRIx64 " ret=%d\n",
                     (uint64_t)addr, ret);
            return ret;
        }
    }

    ret = dma_memory_write(&address_space_memory, fallback_addr, desc,
                           sizeof(*desc), MEMTXATTRS_MEMORY);
    if (ret != MEMTX_OK) {
        qemu_log("ubc cmdq write fallback failed iova=%#" PRIx64 " gpa=%#" PRIx64
                 " ret=%d\n", (uint64_t)addr, (uint64_t)fallback_addr, ret);
    } else if (q == &ubc_dev->cmd_crq || q == &ubc_dev->ctrl_crq) {
        qemu_log("ubc cmdq crq write fallback map iova=%#" PRIx64
                 " gpa=%#" PRIx64 "\n",
                 (uint64_t)addr, (uint64_t)fallback_addr);
    }
    return ret;
}

static size_t ubc_cmdq_capacity(uint8_t bd_num)
{
    if (!bd_num) {
        return 0;
    }
    return (size_t)bd_num * sizeof(UBCCmdqDesc) - 8;
}

static inline uint16_t ubc_ring_free(uint16_t head, uint16_t tail,
                                     uint16_t depth)
{
    uint16_t used;

    if (!depth) {
        return 0;
    }
    used = (tail + depth - head) % depth;
    return depth - used - 1;
}

static uint8_t ubc_cmdq_bd_num_from_len(size_t len)
{
    if (len <= sizeof(((UBCCmdqDesc *)0)->data)) {
        return 1;
    }

    return (uint8_t)(1 + DIV_ROUND_UP(len - sizeof(((UBCCmdqDesc *)0)->data),
                                       sizeof(UBCCmdqDesc)));
}

static uint8_t ubc_ctrlq_bb_num_from_data_len(size_t data_len)
{
    if (data_len <= UBASE_CTRLQ_DATA_LEN) {
        return 1;
    }

    return (uint8_t)(1 + DIV_ROUND_UP(data_len - UBASE_CTRLQ_DATA_LEN,
                                       UBASE_CTRLQ_BB_LEN));
}

static int ubc_cmdq_push_crq_msg(BusControllerDev *ubc_dev, uint16_t opcode,
                                 const uint8_t *payload, size_t payload_len)
{
    UBCmdQueueState *crq = &ubc_dev->cmd_crq;
    UBCCmdqDesc *descs;
    uint8_t bd_num;
    uint16_t free_num;
    uint16_t idx;
    size_t off = 0;
    int i;
    int ret = 0;

    if (!crq->base || !crq->depth) {
        return -ENXIO;
    }

    bd_num = ubc_cmdq_bd_num_from_len(payload_len);
    free_num = ubc_ring_free(crq->head, crq->tail, crq->depth);
    if (bd_num > free_num) {
        return -ENOSPC;
    }

    descs = g_new0(UBCCmdqDesc, bd_num);
    descs[0].opcode = cpu_to_le16(opcode);
    descs[0].flag = UBASE_CMD_FLAG_OUT | UBASE_CMD_FLAG_NO_INTR;
    descs[0].bd_num = bd_num;

    if (payload_len) {
        size_t copy_len = MIN(sizeof(descs[0].data), payload_len);

        memcpy(descs[0].data, payload, copy_len);
        off = copy_len;
    }
    for (i = 1; i < bd_num && off < payload_len; i++) {
        size_t copy_len = MIN(sizeof(UBCCmdqDesc), payload_len - off);

        memcpy(&descs[i], payload + off, copy_len);
        off += copy_len;
    }

    idx = crq->tail;
    for (i = 0; i < bd_num; i++) {
        MemTxResult wret;
        qemu_log("ubc push_crq idx=%u/%u crq_base=%#" PRIx64 " depth=%u"
                 " head=%u tail=%u bd_num=%u opcode=0x%x\n",
                 idx, crq->depth, (uint64_t)crq->base, crq->depth,
                 crq->head, crq->tail, bd_num, opcode);
        wret = ubc_write_desc(crq, ubc_dev, idx, &descs[i]);
        if (wret != MEMTX_OK) {
            qemu_log("ubc push_crq write failed idx=%u wret=%d\n", idx, wret);
            ret = -EIO;
            break;
        }
        idx = (idx + 1) % crq->depth;
    }

    if (!ret) {
        UBCCmdqDesc verify;
        uint16_t first = (crq->tail - bd_num + crq->depth) % crq->depth;
        crq->tail = idx;
        ubc_ers2_write32(ubc_dev, UBASE_CRQ_TAIL_REG, crq->tail);
        ubc_raise_cmdq_event(ubc_dev);
        /* verify first desc was written to guest RAM */
        if (ubc_read_desc(crq, ubc_dev, first, &verify) == MEMTX_OK) {
            qemu_log("ubc push_crq verify[%u] opcode=0x%x flag=0x%x bd=%u"
                     " data[0]=0x%08x\n",
                     first, le16_to_cpu(verify.opcode), verify.flag,
                     verify.bd_num, le32_to_cpu(verify.data[0]));
        } else {
            qemu_log("ubc push_crq verify readback failed\n");
        }
    }
    g_free(descs);

    return ret;
}

static int ubc_handle_ue2ue_ctrlq(BusControllerDev *ubc_dev,
                                  const uint8_t *req, size_t req_len,
                                  uint8_t *resp_flat, size_t resp_cap)
{
    const UBCUe2UeCtrlqHead *req_head;
    const UBCCtrlqMsgHdr *req_hdr;
    UBCUe2UeCtrlqHead *resp_head;
    UBCCtrlqMsgHdr *resp_hdr;
    uint8_t *resp_data;
    size_t resp_data_len;
    size_t total_len;

    if (req_len < sizeof(*req_head) + UBASE_CTRLQ_HDR_LEN) {
        return -EINVAL;
    }

    req_head = (const UBCUe2UeCtrlqHead *)req;
    req_hdr = (const UBCCtrlqMsgHdr *)(req + sizeof(*req_head));
    qemu_log("ubc cmdq ue2ue req seq=%u service_type=0x%x opcode=0x%x "
             "in_size=%u out_size=%u flags=0x%x\n",
             le16_to_cpu(req_head->seq), req_hdr->service_type, req_hdr->opcode,
             le16_to_cpu(req_head->in_size), le16_to_cpu(req_head->out_size),
             req_head->flags);
    resp_data_len = le16_to_cpu(req_head->out_size);
    if (req_hdr->service_type == UBASE_CTRLQ_SER_TYPE_QOS &&
        req_hdr->opcode == UBASE_CTRLQ_OPC_QUERY_SL) {
        resp_data_len = MAX(resp_data_len, sizeof(UBCCtrlqQuerySlResp));
    } else if (req_hdr->service_type == UBASE_CTRLQ_SER_TYPE_QOS &&
               req_hdr->opcode == UBASE_CTRLQ_OPC_QUERY_VL) {
        resp_data_len = MAX(resp_data_len, sizeof(UBCCtrlqQueryVlResp));
    }

    total_len = sizeof(*resp_head) + UBASE_CTRLQ_HDR_LEN + resp_data_len;
    if (total_len > resp_cap) {
        total_len = resp_cap;
        resp_data_len = total_len - sizeof(*resp_head) - UBASE_CTRLQ_HDR_LEN;
    }

    /* Build response in resp_flat (scattered back to CSQ descriptors) */
    memset(resp_flat, 0, total_len);
    memcpy(resp_flat, req, MIN(sizeof(*resp_head) + UBASE_CTRLQ_HDR_LEN, total_len));

    resp_head = (UBCUe2UeCtrlqHead *)resp_flat;
    resp_hdr = (UBCCtrlqMsgHdr *)(resp_flat + sizeof(*resp_head));
    resp_data = resp_flat + sizeof(*resp_head) + UBASE_CTRLQ_HDR_LEN;
    resp_hdr->ret = 0;
    resp_hdr->bb_num = ubc_ctrlq_bb_num_from_data_len(resp_data_len);
    resp_head->flags |= BIT(1); /* is_resp */

    if (req_hdr->service_type == UBASE_CTRLQ_SER_TYPE_QOS &&
        req_hdr->opcode == UBASE_CTRLQ_OPC_QUERY_SL &&
        resp_data_len >= sizeof(UBCCtrlqQuerySlResp)) {
        UBCCtrlqQuerySlResp *sl = (UBCCtrlqQuerySlResp *)resp_data;

        sl->unic_sl_bitmap = cpu_to_le16(0x1);
        sl->rc_max_cnt = cpu_to_le16(64);
        sl->udma_tp_sl_bitmap = cpu_to_le16(0x1);
        sl->udma_ctp_sl_bitmap = cpu_to_le16(0x1);
    } else if (req_hdr->service_type == UBASE_CTRLQ_SER_TYPE_QOS &&
               req_hdr->opcode == UBASE_CTRLQ_OPC_QUERY_VL &&
               resp_data_len >= sizeof(UBCCtrlqQueryVlResp)) {
        UBCCtrlqQueryVlResp *vl = (UBCCtrlqQueryVlResp *)resp_data;

        vl->vl_bitmap = cpu_to_le16(0x1);
    } else if (req_hdr->service_type == UBASE_CTRLQ_SER_TYPE_TP_ACL &&
               req_hdr->opcode == UBASE_CTRLQ_OPC_GET_TP_LIST &&
               resp_data_len >= 8) {
        qemu_log("ubc ue2ue ctrlq: GET_TP_LIST returning 1 TP entry\n");
        *(uint32_t *)resp_data = cpu_to_le32(1);
        *(uint32_t *)(resp_data + 4) = cpu_to_le32((1 << 0) | (1 << 24));
        *(uint32_t *)(resp_data + 8) = 0;
    } else if (req_hdr->service_type == UBASE_CTRLQ_SER_TYPE_DEV_REGISTER &&
               req_hdr->opcode == UBASE_CTRLQ_OPC_GET_SEID_INFO &&
               resp_data_len >= 28) {
        static const uint8_t eid_hw[16] = {
            0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0xfe
        };
        *(uint32_t *)resp_data = cpu_to_le32(1);
        *(uint32_t *)(resp_data + 4) = cpu_to_le32(0);
        memcpy(resp_data + 8, eid_hw, 16);
        qemu_log("ubc ue2ue seid resp: 1 eid fe80::1\n");
    }

    return 0;
}

static void ubc_fill_res_caps(UBCResCmdResp *resp, uint32_t entity_count)
{
    memset(resp, 0, sizeof(*resp));

    resp->cap_bits[0] = cpu_to_le32(BIT(UBASE_SUPPORT_UBL_B) |
                                    BIT(UBASE_SUPPORT_TA_EXTDB_BUF_B) |
                                    BIT(UBASE_SUPPORT_TA_TIMER_BUF_B) |
                                    BIT(UBASE_SUPPORT_IP_OVER_URMA_B));
    resp->node_type = 1;
    resp->ceq_vector_num = cpu_to_le16(16);
    resp->aeq_vector_num = cpu_to_le16(16);
    resp->misc_vector_num = cpu_to_le16(16);
    resp->aeqe_size = cpu_to_le16(64);
    resp->ceqe_size = cpu_to_le16(64);
    resp->udma_cqe_size = cpu_to_le16(64);
    resp->nic_cqe_size = cpu_to_le16(64);
    resp->aeqe_depth = cpu_to_le32(512);
    resp->ceqe_depth = cpu_to_le32(1024);

    resp->udma_jfs_max_cnt = cpu_to_le32(64);
    resp->udma_jfs_depth = cpu_to_le32(4096);
    resp->udma_jfr_max_cnt = cpu_to_le32(64);
    resp->udma_jfr_depth = cpu_to_le32(4096);
    resp->udma_jfc_max_cnt = cpu_to_le32(64);
    resp->udma_jfc_depth = cpu_to_le32(4096);

    resp->nic_jfs_max_cnt = cpu_to_le32(64);
    resp->nic_jfs_depth = cpu_to_le32(128);
    resp->nic_jfr_max_cnt = cpu_to_le32(64);
    resp->nic_jfr_depth = cpu_to_le32(128);
    resp->nic_jfc_max_cnt = cpu_to_le32(64);
    resp->nic_jfc_depth = cpu_to_le32(128);

    resp->total_ue_num = cpu_to_le32(entity_count);
    resp->rsvd_jetty_cnt = cpu_to_le16(0);
    resp->mac_stats_num = cpu_to_le16(0);
    resp->ta_extdb_buf_size = cpu_to_le32(0x1000);
    resp->ta_timer_buf_size = cpu_to_le32(0x1000);
    resp->public_jetty_cnt = cpu_to_le32(64);
    resp->udma_tp_resp_vl_offset = 0;
    resp->ue_num = entity_count;

    resp->udma_rc_depth = cpu_to_le32(1024);
    resp->jtg_max_cnt = cpu_to_le32(64);
    resp->rc_max_cnt_per_vl = cpu_to_le32(64);
}

/*
 * Parse udma_jetty_ctx raw DW words to extract SQ buffer address.
 * Layout (little-endian):
 *   DW0[11:8]  = sqe_bb_shift
 *   DW0[18:16] = state
 *   DW0[19]    = jfs_mode
 *   DW1[31:12] = sqe_base_addr_l (20 bits)
 *   DW2         = sqe_base_addr_h (32 bits)
 *   DW4[19:0]  = tx_jfcn
 *   DW4[31:20] = jfrn_l (12 bits)
 *   DW5[7:0]   = jfrn_h (8 bits)
 *   DW5[31:12] = rx_jfcn (20 bits)
 *   DW6[9:0]   = seid_idx
 */
static void ubc_parse_jetty_ctx(const uint32_t *dw, UBCJettyState *js)
{
    uint32_t sqe_base_addr_l = (dw[1] >> 12) & 0xFFFFF;
    uint32_t sqe_base_addr_h = dw[2];

    js->sq_buf_addr = ((uint64_t)sqe_base_addr_h << 32) |
                      ((uint64_t)sqe_base_addr_l << 12);
    js->sqe_bb_shift = (dw[0] >> 8) & 0xF;
    js->sq_depth = 1u << js->sqe_bb_shift;
    js->jfs_mode = (dw[0] >> 19) & 0x1;
    js->tx_jfcn = dw[4] & 0xFFFFF;
    js->jfrn = ((dw[4] >> 20) & 0xFFF) | ((dw[5] & 0xFF) << 12);
    js->rx_jfcn = (dw[5] >> 12) & 0xFFFFF;
    js->seid_idx = dw[6] & 0x3FF;
}

/* Forward declarations for URMA RX buffering helpers (defined later) */
static void ubc_flush_urma_rx_buffer(BusControllerDev *ubc_dev, uint32_t jetty_id);

static int ubc_handle_post_mb(BusControllerDev *ubc_dev,
                               const uint8_t *req, size_t req_len,
                               uint8_t *resp, size_t resp_cap)
{
    UBCMbox mb = { 0 };
    uint32_t cmd_tag, sub_op, tag;
    uint64_t dma_addr;
    uint8_t ctx_buf[UDMA_JETTY_CTX_SIZE];
    MemTxResult dma_ret;
    size_t resp_len;

    if (req_len < sizeof(mb) || resp_cap < sizeof(mb)) {
        return -1;
    }

    memcpy(&mb, req, sizeof(mb));

    /* Extract sub-opcode and tag from cmd_tag word */
    cmd_tag = le32_to_cpu(mb.cmd_tag);
    sub_op = cmd_tag & 0xFF;
    tag = (cmd_tag >> 8) & 0xFFFFFF;

    /* Extract DMA address of mailbox input buffer */
    dma_addr = ((uint64_t)le32_to_cpu(mb.in_param_h) << 32) |
               le32_to_cpu(mb.in_param_l);

    qemu_log("ubc POST_MB raw cmd_tag=0x%08x sub_op=0x%02x tag=%u dma_addr=%#" PRIx64 "\n",
             cmd_tag, sub_op, tag, (uint64_t)dma_addr);
    fprintf(stderr, "ubc POST_MB raw cmd_tag=0x%08x sub_op=0x%02x tag=%u\n",
            cmd_tag, sub_op, tag);

    switch (sub_op) {
    case UBC_MB_CREATE_JFS_CONTEXT: {
        UBCJettyState *js;

        fprintf(stderr, "ubc POST_MB CREATE_JFS: tag=%u\n", tag);

        if (tag >= UBC_MAX_JETTIES) {
            qemu_log("ubc POST_MB CREATE_JFS: tag %u out of range\n", tag);
            mb.status = cpu_to_le32(1);
            break;
        }

        /* DMA-read the udma_jetty_ctx from guest memory */
        dma_ret = ubc_dma_read(ubc_dev, dma_addr, ctx_buf, sizeof(ctx_buf));
        if (dma_ret != MEMTX_OK) {
            qemu_log("ubc POST_MB CREATE_JFS: DMA read failed addr=%#" PRIx64 "\n",
                     (uint64_t)dma_addr);
            mb.status = cpu_to_le32(1);
            break;
        }

        js = &ubc_dev->jetties[tag];
        memset(js, 0, sizeof(*js));
        js->active = true;
        js->jetty_id = tag;
        ubc_parse_jetty_ctx((const uint32_t *)ctx_buf, js);

        qemu_log("ubc POST_MB CREATE_JFS: jetty_id=%u sq_buf=%#" PRIx64
                 " sq_depth=%u tx_jfcn=%u jfs_mode=%d seid_idx=%u\n",
                 js->jetty_id, js->sq_buf_addr, js->sq_depth,
                 js->tx_jfcn, js->jfs_mode, js->seid_idx);

        /* Flush any buffered URMA RX packets for this jetty now active */
        ubc_flush_urma_rx_buffer(ubc_dev, tag);

        mb.status = cpu_to_le32(0); /* success */
        break;
    }
    case UBC_MB_DESTROY_JFS_CONTEXT: {
        UBCJettyState *js;

        if (tag >= UBC_MAX_JETTIES) {
            mb.status = cpu_to_le32(0);
            break;
        }
        js = &ubc_dev->jetties[tag];
        qemu_log("ubc POST_MB DESTROY_JFS: jetty_id=%u was_active=%d\n",
                 tag, js->active);
        memset(js, 0, sizeof(*js));
        mb.status = cpu_to_le32(0);
        break;
    }
    case UBC_MB_MODIFY_JFS_CONTEXT: {
        qemu_log("ubc POST_MB MODIFY_JFS: tag=%u (accepted)\n", tag);
        mb.status = cpu_to_le32(0);
        break;
    }
    case UBC_MB_QUERY_JFS_CONTEXT: {
        /* Driver queries jetty state to check PI/CI and state.
         * udma_jetty_ctx layout (little-endian bitfields in DW0):
         *   ta_timeout[1:0] | rnr_retry_num[4:2] | type[7:5] | sqe_bb_shift[11:8]
         *   | sl[15:12] | state[18:16] | jfs_mode[19] | sqe_token_id_l[31:20]
         * enum jetty_state: RESET=0, READY=1, ERROR=2, SUSPEND=3
         * DW18[31:16] = CI, DW20[15:0] = PI
         */
        fprintf(stderr, "ubc QUERY_JFS: tag=%u dma_addr=%#lx active=%d\n",
                tag, (unsigned long)dma_addr,
                tag < UBC_MAX_JETTIES ? (int)ubc_dev->jetties[tag].active : -1);
        if (tag < UBC_MAX_JETTIES && ubc_dev->jetties[tag].active) {
            UBCJettyState *js = &ubc_dev->jetties[tag];
            uint8_t ctx_buf[UDMA_JETTY_CTX_SIZE];
            uint32_t *dw = (uint32_t *)ctx_buf;
            MemTxResult dma_ret;

            /* First DMA-read the existing context */
            dma_ret = ubc_dma_read(ubc_dev, dma_addr, ctx_buf, sizeof(ctx_buf));
            fprintf(stderr, "ubc QUERY_JFS: DMA-read ret=%d dw0=%#x dw18=%#x dw20=%#x sq_ci=%u sq_pi=%u\n",
                    dma_ret, dw[0], dw[18], dw[20], js->sq_ci, js->sq_pi);
            if (dma_ret == MEMTX_OK) {
                /* Update state = JETTY_READY (1) in DW0 bits [18:16] */
                dw[0] = (dw[0] & ~(0x7u << 16)) | (1u << 16);
                /* Update CI = sq_ci in DW18 bits [31:16] */
                dw[18] = (dw[18] & 0xFFFFu) | ((uint32_t)js->sq_ci << 16);
                /* Update PI = sq_pi in DW20 bits [15:0] */
                dw[20] = (dw[20] & ~0xFFFFu) | (js->sq_pi & 0xFFFFu);

                /* DMA-write back the modified context */
                dma_ret = ubc_dma_write(ubc_dev, dma_addr, ctx_buf, sizeof(ctx_buf));
                fprintf(stderr, "ubc QUERY_JFS: DMA-write ret=%d dw0=%#x dw18=%#x dw20=%#x\n",
                        dma_ret, dw[0], dw[18], dw[20]);

                /* Verify: read back */
                if (dma_ret == MEMTX_OK) {
                    uint8_t verify_buf[UDMA_JETTY_CTX_SIZE];
                    memset(verify_buf, 0xAA, sizeof(verify_buf));
                    dma_ret = ubc_dma_read(ubc_dev, dma_addr, verify_buf, sizeof(verify_buf));
                    if (dma_ret == MEMTX_OK) {
                        uint32_t *vdw = (uint32_t *)verify_buf;
                        fprintf(stderr, "ubc QUERY_JFS: verify read-back dw0=%#x dw18=%#x dw20=%#x\n",
                                vdw[0], vdw[18], vdw[20]);
                    }
                }
            }
        }
        mb.status = cpu_to_le32(0);
        break;
    }
    case UBC_MB_CREATE_JFC_CONTEXT: {
        if (tag < UBC_MAX_JFCS) {
            UBCJfcState *jfc = &ubc_dev->jfcs[tag];
            memset(jfc, 0, sizeof(*jfc));
            jfc->active = true;
            jfc->jfc_id = tag;

            dma_ret = ubc_dma_read(ubc_dev, dma_addr, ctx_buf,
                                   UDMA_JFC_CTX_SIZE);
            if (dma_ret == MEMTX_OK) {
                /* Extract CQ buffer address from JFC context:
                 * DW1[31:12] = cqe_base_addr_l, DW2 = cqe_base_addr_h */
                const uint32_t *dw = (const uint32_t *)ctx_buf;
                uint32_t cqe_base_l = (dw[1] >> 12) & 0xFFFFF;
                uint32_t cqe_base_h = dw[2];
                uint32_t cqe_bb_shift = (dw[0] >> 8) & 0xF;
                jfc->cq_buf_addr = ((uint64_t)cqe_base_h << 32) |
                                   ((uint64_t)cqe_base_l << 12);
                jfc->cq_depth = 1u << cqe_bb_shift;
            }

            qemu_log("ubc POST_MB CREATE_JFC: jfc_id=%u cq_buf=%#" PRIx64
                     " cq_depth=%u\n", jfc->jfc_id, jfc->cq_buf_addr,
                     jfc->cq_depth);
        }
        mb.status = cpu_to_le32(0);
        break;
    }
    case UBC_MB_CREATE_JFR_CONTEXT: {
        if (tag < UBC_MAX_JFRS) {
            UBCJfrState *jfr = &ubc_dev->jfrs[tag];
            memset(jfr, 0, sizeof(*jfr));
            jfr->active = true;
            jfr->jfr_id = tag;

            dma_ret = ubc_dma_read(ubc_dev, dma_addr, ctx_buf,
                                   UDMA_JFR_CTX_SIZE);
            if (dma_ret == MEMTX_OK) {
                const uint32_t *dw = (const uint32_t *)ctx_buf;
                uint32_t rqe_base_l = (dw[1] >> 12) & 0xFFFFF;
                uint32_t rqe_base_h = dw[2];
                uint32_t rqe_bb_shift = (dw[0] >> 8) & 0xF;
                jfr->rq_buf_addr = ((uint64_t)rqe_base_h << 32) |
                                   ((uint64_t)rqe_base_l << 12);
                jfr->rq_depth = 1u << rqe_bb_shift;
            }

            qemu_log("ubc POST_MB CREATE_JFR: jfr_id=%u rq_buf=%#" PRIx64
                     " rq_depth=%u\n", jfr->jfr_id, jfr->rq_buf_addr,
                     jfr->rq_depth);

            /* Flush any URMA RX data buffered for jetties using this JFR */
            for (uint32_t ji = 0; ji < UBC_MAX_JETTIES; ji++) {
                UBCJettyState *js = &ubc_dev->jetties[ji];
                if (js->active && js->jfrn == tag) {
                    ubc_flush_urma_rx_buffer(ubc_dev, ji);
                }
            }
        }
        mb.status = cpu_to_le32(0);
        break;
    }
    case 0x14: { /* sub_op 0x14 - likely page table / DMA region config */
        dma_ret = ubc_dma_read(ubc_dev, dma_addr, ctx_buf, sizeof(ctx_buf));
        if (dma_ret == MEMTX_OK) {
            const uint32_t *dw = (const uint32_t *)ctx_buf;
            qemu_log("ubc POST_MB sub_op=0x14: tag=%u dma_addr=%#" PRIx64
                     " dw[0..7]=%08x %08x %08x %08x %08x %08x %08x %08x\n",
                     tag, (uint64_t)dma_addr,
                     dw[0], dw[1], dw[2], dw[3], dw[4], dw[5], dw[6], dw[7]);
            fprintf(stderr, "ubc POST_MB sub_op=0x14: tag=%u dw0-7=%08x %08x %08x %08x %08x %08x %08x %08x\n",
                     tag, dw[0], dw[1], dw[2], dw[3], dw[4], dw[5], dw[6], dw[7]);
        }
        mb.status = cpu_to_le32(0);
        break;
    }
    case 0x44:  /* MODIFY_JFR_CONTEXT - stub: accept, return success */
        qemu_log("ubc POST_MB MODIFY_JFR: tag=%u (stub success)\n", tag);
        mb.status = cpu_to_le32(0);
        break;
    case 0x34:  /* DESTROY_JFS_CONTEXT - stub */
        qemu_log("ubc POST_MB DESTROY_JFS: tag=%u (stub success)\n", tag);
        mb.status = cpu_to_le32(0);
        break;
    case 0x55:  /* MODIFY_JFC_CONTEXT - stub */
        qemu_log("ubc POST_MB MODIFY_JFC: tag=%u (stub success)\n", tag);
        mb.status = cpu_to_le32(0);
        break;
    case 0x00:  /* generic / nop */
        qemu_log("ubc POST_MB NOP: tag=%u (stub success)\n", tag);
        mb.status = cpu_to_le32(0);
        break;
    case 0x10:  /* QUERY_JFS_CONTEXT - stub */
        qemu_log("ubc POST_MB QUERY_JFS: tag=%u (stub success)\n", tag);
        mb.status = cpu_to_le32(0);
        break;
    case 0x20:  /* QUERY_JFC_CONTEXT - stub */
        qemu_log("ubc POST_MB QUERY_JFC: tag=%u (stub success)\n", tag);
        mb.status = cpu_to_le32(0);
        break;
    case 0x50:  /* QUERY_JFR_CONTEXT - stub */
        qemu_log("ubc POST_MB QUERY_JFR: tag=%u (stub success)\n", tag);
        mb.status = cpu_to_le32(0);
        break;
    case 0x60:  /* DESTROY_JFR_CONTEXT - stub */
        qemu_log("ubc POST_MB DESTROY_JFR: tag=%u (stub success)\n", tag);
        mb.status = cpu_to_le32(0);
        break;
    default:
        qemu_log("ubc POST_MB: unhandled sub_op=0x%02x tag=%u\n", sub_op, tag);
        mb.status = cpu_to_le32(0);
        break;
    }

    /* Echo back the mailbox with updated status */
    resp_len = sizeof(mb);
    memcpy(resp, &mb, resp_len);
    (void)resp_len;
    return 0;
}

static size_t ubc_cmd_fill_resp(uint16_t opcode, const uint8_t *req, size_t req_len,
                                uint8_t *resp, size_t resp_cap)
{
    size_t resp_len = 0;

    switch (opcode) {
    case UBASE_OPC_QUERY_FW_VER: {
        UBCQueryVersionResp fw = { 0 };
        fw.fw_version = cpu_to_le32(0x01000001);
        resp_len = MIN(sizeof(fw), resp_cap);
        memcpy(resp, &fw, resp_len);
        break;
    }
    case UBASE_OPC_QUERY_UE_RES: {
        UBCUeResourceResp ue = { 0 };

        /* BD0: basic capability fields */
        ue.jetty_grp_num = cpu_to_le16(4);
        ue.jfs_sge = 13;
        ue.jfr_sge = 4;
        ue.jfs_rsge = 4;
        ue.max_jfs_inline_sz = cpu_to_le16(64);
        ue.max_jfc_inline_sz = cpu_to_le16(32);
        ue.trans_mode = 1;  /* UD mode */
        ue.ue_cnt = cpu_to_le16(ubc_dev->entity_count);
        ue.ue_id = 1;

        /* BD1: address tables and SEID — critical for ubcore registration */
        ue.seid_upi_tbl_sz = cpu_to_le16(64);
        ue.seid_upi_tbl_num = cpu_to_le16(64);
        ue.src_addr_tbl_sz = cpu_to_le16(64);
        ue.src_addr_tbl_num = cpu_to_le16(64);
        ue.dest_addr_tbl_sz = cpu_to_le16(64);
        ue.dest_addr_tbl_num = cpu_to_le16(64);
        ue.port_num = 1;
        ue.port_id = 0;
        ue.rc_queue_num = cpu_to_le16(64);
        ue.rc_depth = cpu_to_le16(1024);

        /* BD2: jetty ranges — well-known jetties 0-1023 to match ubcore UBCORE_RESERVED_JETTY_ID_MAX */
        ue.well_known_jetty_start = cpu_to_le16(0);
        ue.well_known_jetty_num = cpu_to_le16(1024);
        ue.ccu_jetty_start = cpu_to_le16(0);
        ue.ccu_jetty_num = cpu_to_le16(0);
        ue.drv_jetty_start = cpu_to_le16(0);
        ue.drv_jetty_num = cpu_to_le16(0);
        ue.cache_lock_jetty_start = cpu_to_le16(0);
        ue.cache_lock_jetty_num = cpu_to_le16(0);
        ue.normal_jetty_start = cpu_to_le16(0);
        ue.normal_jetty_num = cpu_to_le16(0);
        ue.standard_jetty_start = cpu_to_le16(1024);
        ue.standard_jetty_num = cpu_to_le16(0);

        /* BD3: RDMA sizes */
        ue.max_write_size = cpu_to_le32(0x100000);
        ue.max_read_size = cpu_to_le32(0x100000);
        ue.max_cas_size = cpu_to_le32(8);
        ue.max_fetch_and_add_size = cpu_to_le32(8);

        resp_len = MIN(sizeof(ue), resp_cap);
        memcpy(resp, &ue, resp_len);
        qemu_log("ubc UE_RES resp: sizeof=%zu resp_len=%zu "
                 "standard_start=%u standard_num=%u "
                 "bytes@76=[%02x %02x] bytes@78=[%02x %02x]\n",
                 sizeof(ue), resp_len,
                 le16_to_cpu(ue.standard_jetty_start),
                 le16_to_cpu(ue.standard_jetty_num),
                 resp[76], resp[77], resp[78], resp[79]);
        break;
    }
    case UBASE_OPC_QUERY_COMM_RSRC_PARAM: {
        UBCResCmdResp res;
        ubc_fill_res_caps(&res);
        resp_len = MIN(sizeof(res), resp_cap);
        memcpy(resp, &res, resp_len);
        break;
    }
    case UBASE_OPC_QUERY_CTL_INFO: {
        UBCCtrlInfoResp ctl = { 0 };
        resp_len = MIN(sizeof(ctl), resp_cap);
        memcpy(resp, &ctl, resp_len);
        break;
    }
    case UBASE_OPC_QUERY_CHIP_INFO: {
        UBCChipInfoResp chip = { 0 };
        chip.chip_id = cpu_to_le16(1);
        resp_len = MIN(sizeof(chip), resp_cap);
        memcpy(resp, &chip, resp_len);
        break;
    }
    case UBASE_OPC_QUERY_OOR_CAPS: {
        UBCOorResp oor = { 0 };
        resp_len = MIN(sizeof(oor), resp_cap);
        memcpy(resp, &oor, resp_len);
        break;
    }
    case UBASE_OPC_QUERY_UB_PORT_BITMAP:
    case UBASE_OPC_QUERY_PORT_BITMAP: {
        UBCPortBitmapResp bm = { 0 };
        bm.logic_port_bitmap = cpu_to_le32(0x1);
        bm.chip_id = cpu_to_le32(1);
        resp_len = MIN(sizeof(bm), resp_cap);
        memcpy(resp, &bm, resp_len);
        break;
    }
    case UBASE_OPC_QUERY_TA_SL_VL_MAP: {
        UBCSlVlResp sl = { 0 };
        int i;

        sl.sl_num = UBASE_MAX_SL_NUM;
        for (i = 0; i < UBASE_MAX_SL_NUM && i < ARRAY_SIZE(sl.sl_vl); i++) {
            sl.sl_vl[i] = i;
        }
        resp_len = MIN(sizeof(sl), resp_cap);
        memcpy(resp, &sl, resp_len);
        break;
    }
    case UBASE_OPC_QUERY_TM_Q_INFO: {
        UBCTmQueueResp q = { 0 };
        q.queue_num = 1;
        q.queue_vl[0] = 0;
        q.queue_id[0] = 0;
        q.qset_id[0] = 0;
        q.link_vld_bitmap = cpu_to_le16(1);
        resp_len = MIN(sizeof(q), resp_cap);
        memcpy(resp, &q, resp_len);
        break;
    }
    case UBASE_OPC_QUERY_TM_QS_INFO: {
        UBCTmQsetResp q = { 0 };
        q.qset_num = 1;
        q.qset_id[0] = 0;
        q.pri_id[0] = 0;
        q.qset_weight[0] = 100;
        q.rate[0] = cpu_to_le32(100000);
        q.qset_pri_link_vld = cpu_to_le16(1);
        resp_len = MIN(sizeof(q), resp_cap);
        memcpy(resp, &q, resp_len);
        break;
    }
    case UBASE_OPC_QUERY_MB_ST: {
        UBCMbox mb = { 0 };
        mb.in_param_l = cpu_to_le32(1);
        mb.status = cpu_to_le32(1); /* query_status bit 0 = 1 → mailbox done */
        resp_len = MIN(sizeof(mb), resp_cap);
        memcpy(resp, &mb, resp_len);
        break;
    }
    default:
        resp_len = 0;
        break;
    }

    return resp_len;
}

static void ubc_raise_cmdq_event(BusControllerDev *ubc_dev)
{
    UBDevice *ub_dev = &ubc_dev->parent;
    uint32_t src;

    src = ubc_ers2_read32(ubc_dev, UBASE_VECTOR0_CMDQ_SRC_REG);
    src |= BIT(UBASE_VECTOR0_RX_CMDQ_INT_B);
    ubc_ers2_write32(ubc_dev, UBASE_VECTOR0_CMDQ_SRC_REG, src);

    if (ub_dev->usi_entries_nr > 0) {
        usi_notify(ub_dev, 0);
    } else {
        /*
         * Some current ubc paths run without USI vector-table setup.
         * Use PCI MSI if available, otherwise fallback to minimal poll-mode
         * notification.
         */
        PCIDevice *pci_dev = PCI_DEVICE(ubc_dev);
        if (msi_enabled(pci_dev)) {
            MSIMessage msg = msi_get_message(pci_dev, 0);
            USIMessage usi_msg = {
                .address = msg.address,
                .data = msg.data,
            };
            usi_send_message(&usi_msg, UBC_INTERRUPT_ID_START, ub_dev);
            qemu_log("ubc cmdq pci msi notify addr=%#" PRIx64 " data=%#x\n",
                     usi_msg.address, usi_msg.data);
        } else {
            /* Fallback for pure simulation without full PCI MSI setup */
            USIMessage msg = {
                .address = 0x8090040ULL, /* Legacy fallback, but logged */
                .data = 0,
            };
            usi_send_message(&msg, UBC_INTERRUPT_ID_START, ub_dev);
            qemu_log("ubc cmdq legacy fallback msi notify rid=0x%x\n",
                     UBC_INTERRUPT_ID_START);
        }
    }
}

static void ubc_sync_cmdq_regs(BusControllerDev *ubc_dev)
{
    dma_addr_t old_csq_base = ubc_dev->cmd_csq.base;

    ubc_dev->cmd_csq.base = ubc_read_q_base(ubc_dev, UBASE_CSQ_BASEADDR_L_REG);
    ubc_dev->cmd_csq.depth = ubc_read_q_depth(ubc_dev, UBASE_CSQ_DEPTH_REG);
    ubc_dev->cmd_csq.head = (uint16_t)ubc_ers2_read32(ubc_dev, UBASE_CSQ_HEAD_REG);
    ubc_dev->cmd_csq.tail = (uint16_t)ubc_ers2_read32(ubc_dev, UBASE_CSQ_TAIL_REG);
    if (old_csq_base != ubc_dev->cmd_csq.base) {
        ubc_dev->csq_bias_valid = false;
        ubc_dev->csq_bias_scanned = false;
        ubc_dev->csq_bias_iova_base = ubc_dev->cmd_csq.base;
        ubc_dev->csq_iova_to_gpa_bias = 0;
        /* Reset CMDQ base tracking when CSQ is cleared (guest resets queue) */
        if (ubc_dev->cmd_csq.base == 0) {
            ubc_dev->cmdq_last_csq_base = 0;
        }
    }

    ubc_dev->cmd_crq.base = ubc_read_q_base(ubc_dev, UBASE_CRQ_BASEADDR_L_REG);
    ubc_dev->cmd_crq.depth = ubc_read_q_depth(ubc_dev, UBASE_CRQ_DEPTH_REG);
    ubc_dev->cmd_crq.head = (uint16_t)ubc_ers2_read32(ubc_dev, UBASE_CRQ_HEAD_REG);
    ubc_dev->cmd_crq.tail = (uint16_t)ubc_ers2_read32(ubc_dev, UBASE_CRQ_TAIL_REG);
    qemu_log("ubc cmdq sync base=%#" PRIx64 " depth=%u head=%u tail=%u crq_base=%#" PRIx64
             " crq_depth=%u crq_head=%u crq_tail=%u\n",
             (uint64_t)ubc_dev->cmd_csq.base, ubc_dev->cmd_csq.depth,
             ubc_dev->cmd_csq.head, ubc_dev->cmd_csq.tail,
             (uint64_t)ubc_dev->cmd_crq.base, ubc_dev->cmd_crq.depth,
             ubc_dev->cmd_crq.head, ubc_dev->cmd_crq.tail);

    /* Once CSQ bias is known and CRQ registers are written, learn CRQ GPA */
    if (ubc_dev->csq_bias_valid && ubc_dev->cmd_crq.base) {
        ubc_learn_crq_bias(ubc_dev);
    }
}

static void ubc_process_cmdq(BusControllerDev *ubc_dev)
{
    UBCmdQueueState *csq = &ubc_dev->cmd_csq;
    uint16_t guard = 0;

    if (!csq->base || !csq->depth) {
        return;
    }

    while (csq->head != csq->tail && guard++ < csq->depth) {
        UBCCmdqDesc *descs;
        UBCCmdqDesc head_desc;
        uint8_t *resp_flat;
        uint8_t *req_flat;
        uint16_t idx;
        uint16_t pending;
        uint8_t bd_num;
        uint16_t opcode;
        size_t cap;
        size_t req_len;
        size_t copy_len;
        int ret = 0;
        int i;
        bool need_bias_relearn;

        if (ubc_read_desc(csq, ubc_dev, csq->head, &head_desc) != MEMTX_OK) {
            break;
        }
        opcode = le16_to_cpu(head_desc.opcode);
        need_bias_relearn = csq->head == 0 && csq->tail != 0 &&
                            !ubc_dev->csq_bias_valid && !ubc_dev->csq_bias_scanned &&
                            ubc_head_desc_suspicious(&head_desc);
        if (need_bias_relearn) {
            ubc_dev->csq_bias_scanned = true;
            if (ubc_learn_csq_bias(ubc_dev) &&
                ubc_read_desc(csq, ubc_dev, csq->head, &head_desc) == MEMTX_OK) {
                opcode = le16_to_cpu(head_desc.opcode);
            }
        }
        qemu_log("ubc cmdq process opcode=0x%x bd=%u head=%u tail=%u depth=%u\n",
                 opcode, head_desc.bd_num, csq->head, csq->tail, csq->depth);
        bd_num = head_desc.bd_num ? head_desc.bd_num : 1;
        if (bd_num > csq->depth) {
            bd_num = 1;
        }
        pending = (csq->tail + csq->depth - csq->head) % csq->depth;
        if (!pending) {
            break;
        }
        if (!(head_desc.flag & UBASE_CMD_FLAG_IN) ||
            (head_desc.flag & UBASE_CMD_FLAG_OUT) ||
            head_desc.bd_num == 0) {
            break;
        }
        if (bd_num > pending) {
            bd_num = pending;
        }

        cap = ubc_cmdq_capacity(bd_num);
        req_len = cap;
        req_flat = g_malloc0(MAX(cap, (size_t)1));
        resp_flat = g_malloc0(MAX(cap, (size_t)1));
        descs = g_new0(UBCCmdqDesc, bd_num);

        idx = csq->head;
        for (i = 0; i < bd_num; i++) {
            if (ubc_read_desc(csq, ubc_dev, idx, &descs[i]) != MEMTX_OK) {
                break;
            }
            idx = (idx + 1) % csq->depth;
        }
        if (i != bd_num) {
            g_free(req_flat);
            g_free(resp_flat);
            g_free(descs);
            break;
        }

        if (cap) {
            copy_len = MIN(sizeof(descs[0].data), cap);
            memcpy(req_flat, descs[0].data, copy_len);
            for (i = 1; i < bd_num; i++) {
                size_t off = sizeof(descs[0].data) +
                             (size_t)(i - 1) * sizeof(UBCCmdqDesc);
                if (off >= cap) {
                    break;
                }
                copy_len = MIN(sizeof(UBCCmdqDesc), cap - off);
                memcpy(req_flat + off, &descs[i], copy_len);
            }
        }

        if (descs[0].flag & UBASE_CMD_FLAG_GET_BD_NUM) {
            resp_flat[0] = 1;
        } else if (opcode == UBASE_OPC_UE2UE_UBASE) {
            ret = ubc_handle_ue2ue_ctrlq(ubc_dev, req_flat, req_len,
                                                   resp_flat, cap);
        } else if (opcode == UBASE_OPC_POST_MB) {
            ret = ubc_handle_post_mb(ubc_dev, req_flat, req_len, resp_flat, cap);
        } else {
            (void)ubc_cmd_fill_resp(opcode, req_flat, req_len, resp_flat, cap);
        }

        if (cap) {
            copy_len = MIN(sizeof(descs[0].data), cap);
            memcpy(descs[0].data, resp_flat, copy_len);
            for (i = 1; i < bd_num; i++) {
                size_t off = sizeof(descs[0].data) +
                             (size_t)(i - 1) * sizeof(UBCCmdqDesc);
                if (off >= cap) {
                    break;
                }
                copy_len = MIN(sizeof(UBCCmdqDesc), cap - off);
                memcpy(&descs[i], resp_flat + off, copy_len);
            }
            if (opcode == UBASE_OPC_QUERY_UE_RES) {
                qemu_log("ubc UE_RES scatter: bd_num=%u cap=%zu "
                         "desc0.data[12..15]=[%02x %02x %02x %02x] "
                         "desc1.data[8..15]=[%02x %02x %02x %02x %02x %02x %02x %02x] "
                         "desc2.bytes[16..23]=[%02x %02x %02x %02x %02x %02x %02x %02x]\n",
                         bd_num, cap,
                         ((uint8_t *)descs[0].data)[12], ((uint8_t *)descs[0].data)[13],
                         ((uint8_t *)descs[0].data)[14], ((uint8_t *)descs[0].data)[15],
                         ((uint8_t *)&descs[1])[8], ((uint8_t *)&descs[1])[9],
                         ((uint8_t *)&descs[1])[10], ((uint8_t *)&descs[1])[11],
                         ((uint8_t *)&descs[1])[12], ((uint8_t *)&descs[1])[13],
                         ((uint8_t *)&descs[1])[14], ((uint8_t *)&descs[1])[15],
                         ((uint8_t *)&descs[2])[16], ((uint8_t *)&descs[2])[17],
                         ((uint8_t *)&descs[2])[18], ((uint8_t *)&descs[2])[19],
                         ((uint8_t *)&descs[2])[20], ((uint8_t *)&descs[2])[21],
                         ((uint8_t *)&descs[2])[22], ((uint8_t *)&descs[2])[23]);
            }
        }

        descs[0].flag |= UBASE_CMD_FLAG_OUT;
        descs[0].ret = cpu_to_le16((ret < 0) ? (uint16_t)(-ret) : 0);

        idx = csq->head;
        for (i = 0; i < bd_num; i++) {
            if (opcode == UBASE_OPC_QUERY_UE_RES) {
                qemu_log("ubc UE_RES write_desc[%d] addr=%#" PRIx64
                         " bytes[0..7]=[%02x %02x %02x %02x %02x %02x %02x %02x]"
                         " data[0..7]=[%02x %02x %02x %02x %02x %02x %02x %02x]\n",
                         i, (uint64_t)(csq->base + (dma_addr_t)idx * sizeof(descs[0])),
                         ((uint8_t *)&descs[i])[0], ((uint8_t *)&descs[i])[1],
                         ((uint8_t *)&descs[i])[2], ((uint8_t *)&descs[i])[3],
                         ((uint8_t *)&descs[i])[4], ((uint8_t *)&descs[i])[5],
                         ((uint8_t *)&descs[i])[6], ((uint8_t *)&descs[i])[7],
                         ((uint8_t *)descs[i].data)[0], ((uint8_t *)descs[i].data)[1],
                         ((uint8_t *)descs[i].data)[2], ((uint8_t *)descs[i].data)[3],
                         ((uint8_t *)descs[i].data)[4], ((uint8_t *)descs[i].data)[5],
                         ((uint8_t *)descs[i].data)[6], ((uint8_t *)descs[i].data)[7]);
            }
            (void)ubc_write_desc(csq, ubc_dev, idx, &descs[i]);
            idx = (idx + 1) % csq->depth;
        }

        csq->head = idx;
        ubc_ers2_write32(ubc_dev, UBASE_CSQ_HEAD_REG, csq->head);
        ubc_raise_cmdq_event(ubc_dev);

        /* Record the CSQ base used for successful CMDQ processing */
        ubc_dev->cmdq_last_csq_base = csq->base;

        /* Diagnostic: verify ue2ue ctrlq response reached guest RAM */
        if (opcode == UBASE_OPC_UE2UE_UBASE && bd_num > 0) {
            UBCCmdqDesc verify;
            uint16_t first = (idx + csq->depth - bd_num) % csq->depth;
            fprintf(stderr, "ubc ue2ue resp write: bd0 op=0x%x flag=0x%x ret=%u"
                    " data[0..3]=[%08x %08x %08x %08x] data[4..7]=[%08x %08x %08x %08x]\n",
                    le16_to_cpu(descs[0].opcode), descs[0].flag,
                    le16_to_cpu(descs[0].ret),
                    le32_to_cpu(descs[0].data[0]), le32_to_cpu(descs[0].data[1]),
                    le32_to_cpu(descs[0].data[2]), le32_to_cpu(descs[0].data[3]),
                    le32_to_cpu(descs[0].data[4]), le32_to_cpu(descs[0].data[5]),
                    le32_to_cpu(descs[0].data[6]), le32_to_cpu(descs[0].data[7]));
            if (ubc_read_desc(csq, ubc_dev, first, &verify) == MEMTX_OK) {
                fprintf(stderr, "ubc ue2ue resp readback[%u]: op=0x%x flag=0x%x ret=%u"
                        " data[0..3]=[%08x %08x %08x %08x] data[4..7]=[%08x %08x %08x %08x]\n",
                        first, le16_to_cpu(verify.opcode), verify.flag,
                        le16_to_cpu(verify.ret),
                        le32_to_cpu(verify.data[0]), le32_to_cpu(verify.data[1]),
                        le32_to_cpu(verify.data[2]), le32_to_cpu(verify.data[3]),
                        le32_to_cpu(verify.data[4]), le32_to_cpu(verify.data[5]),
                        le32_to_cpu(verify.data[6]), le32_to_cpu(verify.data[7]));
            } else {
                fprintf(stderr, "ubc ue2ue resp readback FAILED for idx=%u\n", first);
            }
        }

        /* Push ue2ue ctrlq response to CMDQ CRQ for event dispatch.
         * The guest's ctrlq CMDQ-fallback path expects the ue2ue response
         * on CMDQ CRQ so ubase_cmd_crq_handler can dispatch it to the
         * registered ue2ue event handler, which then calls
         * ubase_ctrlq_notify_completed → complete(&ctx->done) to wake
         * the ctrlq waiter. */
        if (opcode == UBASE_OPC_UE2UE_UBASE && ret == 0) {
            UBCmdQueueState *cmd_crq = &ubc_dev->cmd_crq;
            const UBCCtrlqMsgHdr *crq_msg_hdr;
            UBCCmdqDesc crq_bd;
            size_t actual_len;
            uint8_t crq_bd_num, j;
            uint16_t crq_next;

            /* Use cached CRQ state (already synced by ubc_sync_cmdq_regs
             * at start of ubc_process_cmdq).  Do NOT re-read from storage
             * here — the values are stable for the duration of this call. */
            fprintf(stderr, "ubc cmdq crq push ue2ue: cached base=%#lx"
                    " depth=%u head=%u tail=%u\n",
                    (unsigned long)cmd_crq->base, cmd_crq->depth,
                    cmd_crq->head, cmd_crq->tail);

            if (cmd_crq->base && cmd_crq->depth) {
                /* Determine actual response length from ctrlq bb_num field */
                if (cap >= 28) {
                    crq_msg_hdr = (const UBCCtrlqMsgHdr *)(resp_flat + 16);
                    actual_len = 28 + (size_t)crq_msg_hdr->bb_num * 20;
                    if (actual_len > cap) {
                        actual_len = cap;
                    }
                } else {
                    actual_len = cap;
                }

                /* Calculate CRQ BDs: msg_data_len = bd_num * 32 - 8 */
                crq_bd_num = (uint8_t)((actual_len + 8 + 31) / 32);
                if (crq_bd_num < 1) {
                    crq_bd_num = 1;
                }

                fprintf(stderr, "ubc cmdq crq push ue2ue: actual_len=%zu"
                        " crq_bd_num=%u head=%u tail=%u depth=%u\n",
                        actual_len, crq_bd_num, cmd_crq->head,
                        cmd_crq->tail, cmd_crq->depth);

                /* Write BD0: header + first 24 bytes of resp_flat */
                memset(&crq_bd, 0, sizeof(crq_bd));
                crq_bd.opcode = cpu_to_le16(UBASE_OPC_UE2UE_UBASE);
                crq_bd.flag = UBASE_CMD_FLAG_IN | UBASE_CMD_FLAG_OUT;
                crq_bd.bd_num = crq_bd_num;
                crq_bd.ret = 0;
                memcpy(crq_bd.data, resp_flat, MIN(24, actual_len));

                crq_next = (cmd_crq->tail + 1) % cmd_crq->depth;
                if (crq_next == cmd_crq->head) {
                    qemu_log("ubc cmdq crq full, cannot push ue2ue\n");
                } else if (ubc_write_desc(cmd_crq, ubc_dev,
                                           cmd_crq->tail,
                                           &crq_bd) != MEMTX_OK) {
                    qemu_log("ubc cmdq crq write bd0 failed\n");
                } else {
                    cmd_crq->tail = crq_next;

                    /* Write continuation BDs */
                    for (j = 1; j < crq_bd_num; j++) {
                        size_t off = 24 + (size_t)(j - 1) * 32;

                        crq_next = (cmd_crq->tail + 1) % cmd_crq->depth;
                        if (crq_next == cmd_crq->head) {
                            qemu_log("ubc cmdq crq full at cont bd%u\n", j);
                            break;
                        }
                        memset(&crq_bd, 0, sizeof(crq_bd));
                        if (off < actual_len) {
                            memcpy(&crq_bd, resp_flat + off,
                                   MIN(32, actual_len - off));
                        }
                        if (ubc_write_desc(cmd_crq, ubc_dev,
                                           cmd_crq->tail,
                                           &crq_bd) != MEMTX_OK) {
                            qemu_log("ubc cmdq crq write cont bd%u"
                                     " failed\n", j);
                            break;
                        }
                        cmd_crq->tail = crq_next;
                    }

                    /* Update CRQ tail register and raise interrupt */
                    ubc_ers2_write32(ubc_dev, UBASE_CRQ_TAIL_REG,
                                     cmd_crq->tail);
                    fprintf(stderr, "ubc cmdq crq pushed: new_tail=%u\n",
                            cmd_crq->tail);
                    ubc_raise_cmdq_event(ubc_dev);
                }
            } else {
                fprintf(stderr, "ubc cmdq crq not initialized"
                        " (base=%#lx depth=%u)\n",
                        (unsigned long)cmd_crq->base, cmd_crq->depth);
            }
        }

        g_free(req_flat);
        g_free(resp_flat);
        g_free(descs);
    }
}

static void ubc_sync_ctrlq_regs(BusControllerDev *ubc_dev)
{
    ubc_dev->ctrl_csq.base = ubc_read_q_base(ubc_dev, UBASE_CTRLQ_CSQ_BASEADDR_L_REG);
    ubc_dev->ctrl_csq.depth = ubc_read_q_depth(ubc_dev, UBASE_CTRLQ_CSQ_DEPTH_REG);
    ubc_dev->ctrl_csq.head = (uint16_t)ubc_ers2_read32(ubc_dev, UBASE_CTRLQ_CSQ_HEAD_REG);
    ubc_dev->ctrl_csq.tail = (uint16_t)ubc_ers2_read32(ubc_dev, UBASE_CTRLQ_CSQ_TAIL_REG);

    ubc_dev->ctrl_crq.base = ubc_read_q_base(ubc_dev, UBASE_CTRLQ_CRQ_BASEADDR_L_REG);
    ubc_dev->ctrl_crq.depth = ubc_read_q_depth(ubc_dev, UBASE_CTRLQ_CRQ_DEPTH_REG);
    ubc_dev->ctrl_crq.head = (uint16_t)ubc_ers2_read32(ubc_dev, UBASE_CTRLQ_CRQ_HEAD_REG);
    ubc_dev->ctrl_crq.tail = (uint16_t)ubc_ers2_read32(ubc_dev, UBASE_CTRLQ_CRQ_TAIL_REG);

    qemu_log("ubc ctrlq sync csq_base=%#" PRIx64 " csq_depth=%u csq_head=%u csq_tail=%u"
             " crq_base=%#" PRIx64 " crq_depth=%u crq_head=%u crq_tail=%u\n",
             (uint64_t)ubc_dev->ctrl_csq.base, ubc_dev->ctrl_csq.depth,
             ubc_dev->ctrl_csq.head, ubc_dev->ctrl_csq.tail,
             (uint64_t)ubc_dev->ctrl_crq.base, ubc_dev->ctrl_crq.depth,
             ubc_dev->ctrl_crq.head, ubc_dev->ctrl_crq.tail);
}

static void ubc_process_ctrlq(BusControllerDev *ubc_dev)
{
    UBCmdQueueState *csq = &ubc_dev->ctrl_csq;
    UBCmdQueueState *crq = &ubc_dev->ctrl_crq;
    uint16_t guard = 0;

    if (!csq->base || !csq->depth) {
        fprintf(stderr, "ubc ctrlq SKIP: csq_base=%#lx csq_depth=%u\n",
                (unsigned long)csq->base, csq->depth);
        return;
    }

    fprintf(stderr, "ubc ctrlq ENTER: csq_head=%u csq_tail=%u csq_base=%#lx"
            " crq_head=%u crq_tail=%u crq_base=%#lx crq_depth=%u\n",
            csq->head, csq->tail, (unsigned long)csq->base,
            crq->head, crq->tail, (unsigned long)crq->base, crq->depth);

    while (csq->head != csq->tail && guard++ < csq->depth) {
        UBCCtrlqBaseBlock req_bb;
        UBCCtrlqBaseBlock resp_bb;
        uint16_t crq_next;

        if (ubc_read_desc(csq, ubc_dev, csq->head,
                          (UBCCmdqDesc *)&req_bb) != MEMTX_OK) {
            qemu_log("ubc ctrlq read csq[%u] failed\n", csq->head);
            break;
        }

        qemu_log("ubc ctrlq process service_type=0x%x opcode=0x%x bb_num=%u"
                 " seq=%u head=%u tail=%u\n",
                 req_bb.service_type, req_bb.opcode, req_bb.bb_num,
                 le16_to_cpu(req_bb.seq), csq->head, csq->tail);

        csq->head = (csq->head + 1) % csq->depth;
        ubc_ers2_write32(ubc_dev, UBASE_CTRLQ_CSQ_HEAD_REG, csq->head);
        ubc_raise_cmdq_event(ubc_dev);

        memset(&resp_bb, 0, sizeof(resp_bb));
        resp_bb.service_ver = req_bb.service_ver;
        resp_bb.service_type = req_bb.service_type;
        resp_bb.opcode = req_bb.opcode;
        resp_bb.seq = req_bb.seq;
        resp_bb.mbx_ue_id = req_bb.mbx_ue_id;
        resp_bb.bus_ue_id = req_bb.bus_ue_id;
        resp_bb.bb_num = 1;

        if (req_bb.service_type == UBASE_CTRLQ_SER_TYPE_QOS) {
            switch (req_bb.opcode) {
            case UBASE_CTRLQ_OPC_QUERY_VL:
                resp_bb.data[0] = cpu_to_le32(0x0001);
                resp_bb.ret = 0;
                break;
            case UBASE_CTRLQ_OPC_QUERY_SL: {
                uint16_t sl_bitmap = 0xFFFF;
                uint16_t rc_max = 64;
                uint8_t *dp = (uint8_t *)resp_bb.data;
                memcpy(dp, &sl_bitmap, sizeof(sl_bitmap));
                memcpy(dp + 2, &rc_max, sizeof(rc_max));
                memcpy(dp + 4, &sl_bitmap, sizeof(sl_bitmap));
                resp_bb.ret = 0;
                break;
            }
            default:
                resp_bb.ret = 0;
                break;
            }
        } else if (req_bb.service_type == UBASE_CTRLQ_SER_TYPE_DEV_REGISTER &&
                   req_bb.opcode == UBASE_CTRLQ_OPC_GET_SEID_INFO) {
            /*
             * Return 1 SEID entry with a link-local EID (fe80::1).
             * The ctrlq EID bytes are in hw order; udma_swap_endian()
             * in the guest driver will reverse them to produce the
             * kernel-native EID, which ipourma then uses as IPv6 addr.
             *
             * EID response layout (after 12-byte ctrlq header):
             *   [0:3]  seid_num=1 | rsv=0       (4 bytes)
             *   [4:7]  eid_idx=0                 (4 bytes)
             *   [8:23] eid[16] byte-reversed     (16 bytes)
             *   [24:27] upi=0                    (4 bytes)
             *
             * Total data = 28 bytes → needs 2 BDs (20 + 8).
             * fe80::1 in big-endian = fe80...01, reversed = 01...80fe
             * fe80::2 in big-endian = fe80...02, reversed = 02...80fe
             * Use UB_FM_NODE_ID to differentiate: nodeA → ::1, nodeB → ::2
             */
            const char *fm_node_id = g_getenv("UB_FM_NODE_ID");
            uint8_t eid_suffix = (fm_node_id && strcmp(fm_node_id, "nodeB") == 0) ? 0x02 : 0x01;
            uint8_t eid_hw[16] = {
                eid_suffix, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0xfe
            };
            UBCCmdqDesc cont;
            uint16_t crq_next2;

            resp_bb.bb_num = 2;
            resp_bb.ret = 0;
            /* data[0] = seid_num(1) | rsv(0) */
            resp_bb.data[0] = cpu_to_le32(1);
            /* data[1] = eid_idx(0) */
            resp_bb.data[1] = cpu_to_le32(0);
            /* data[2..4] = eid_hw[0..11] */
            memcpy(&resp_bb.data[2], eid_hw, 12);

            /* BD1: continuation — remaining eid bytes + upi */
            memset(&cont, 0, sizeof(cont));
            memcpy(cont.data, eid_hw + 12, 4);
            /* cont.data[1] = upi = 0 (already zeroed) */

            /* write BD0 (header) */
            crq_next = (crq->tail + 1) % crq->depth;
            if (crq_next == crq->head) {
                qemu_log("ubc ctrlq crq full (seid resp)\n");
                break;
            }
            if (ubc_write_desc(crq, ubc_dev, crq->tail,
                               (const UBCCmdqDesc *)&resp_bb) != MEMTX_OK) {
                qemu_log("ubc ctrlq write crq[seid bd0] failed\n");
                break;
            }
            crq->tail = crq_next;

            /* write BD1 (continuation) */
            crq_next2 = (crq->tail + 1) % crq->depth;
            if (crq_next2 == crq->head) {
                qemu_log("ubc ctrlq crq full (seid cont)\n");
                break;
            }
            if (ubc_write_desc(crq, ubc_dev, crq->tail,
                               &cont) != MEMTX_OK) {
                qemu_log("ubc ctrlq write crq[seid bd1] failed\n");
                break;
            }
            crq->tail = crq_next2;

            ubc_ers2_write32(ubc_dev, UBASE_CTRLQ_CRQ_TAIL_REG, crq->tail);
            ubc_raise_cmdq_event(ubc_dev);
            qemu_log("ubc ctrlq seid resp: 1 eid fe80::1 (2 BDs)\n");
            continue;
        } else if (req_bb.service_type == UBASE_CTRLQ_SER_TYPE_TP_ACL &&
                   req_bb.opcode == UBASE_CTRLQ_OPC_GET_TP_LIST) {
            /*
             * TP ACL GET_TP_LIST response (single BD, 12 bytes data):
             *   [0:3]  tp_list_cnt(1) | rsv(0)
             *   [4:7]  tpid=1 | tpn_cnt=1
             *   [8:11] tpn_start=0 | migr=0 | rsv=0
             */
            resp_bb.ret = 0;
            resp_bb.data[0] = cpu_to_le32(1);                    /* tp_list_cnt=1 */
            resp_bb.data[1] = cpu_to_le32((1 << 0) | (1 << 24)); /* tpid=1, tpn_cnt=1 */
            resp_bb.data[2] = 0;                                  /* tpn_start=0 */
            qemu_log("ubc ctrlq tp_acl get_tp_list resp: 1 TP entry\n");
        } else {
            resp_bb.ret = 0;
        }

        if (!crq->base || !crq->depth) {
            fprintf(stderr, "ubc ctrlq crq not initialized: crq_base=%#lx"
                    " crq_depth=%u, skipping response\n",
                    (unsigned long)crq->base, crq->depth);
            continue;
        }

        crq_next = (crq->tail + 1) % crq->depth;
        if (crq_next == crq->head) {
            qemu_log("ubc ctrlq crq full, cannot push response\n");
            break;
        }

        fprintf(stderr, "ubc ctrlq write resp: crq_base=%#lx crq_tail=%u"
                " crq_head=%u crq_depth=%u\n",
                (unsigned long)crq->base, crq->tail, crq->head, crq->depth);

        if (ubc_write_desc(crq, ubc_dev, crq->tail,
                           (const UBCCmdqDesc *)&resp_bb) != MEMTX_OK) {
            qemu_log("ubc ctrlq write crq[%u] failed\n", crq->tail);
            break;
        }
        crq->tail = crq_next;
        ubc_ers2_write32(ubc_dev, UBASE_CTRLQ_CRQ_TAIL_REG, crq->tail);
        fprintf(stderr, "ubc ctrlq resp done: crq_tail=%u\n", crq->tail);
        ubc_raise_cmdq_event(ubc_dev);
    }
}

static void ubc_ers2_mmio_write(BusControllerDev *ubc_dev, hwaddr addr,
                                uint64_t val, unsigned len)
{
    uint8_t *storage = ubc_dev->ers[2].storage;
    uint32_t cur;

    if (addr + len > ubc_dev->ers[2].storage_size) {
        fprintf(stderr, "ubc ers2 mmio write OUT OF BOUNDS addr=%#lx val=%#lx"
               " storage_size=%#lx\n", (unsigned long)addr,
               (unsigned long)val,
               (unsigned long)ubc_dev->ers[2].storage_size);
        return;
    }

    /* Diagnostic: log all ctrlq-range writes */
    if (addr >= 0x18800 && addr < 0x18830) {
        fprintf(stderr, "ubc ers2 CTRLQ WRITE addr=%#lx val=%#lx len=%u\n",
                (unsigned long)addr, (unsigned long)val, len);
    }

    if (len == DWORD_SIZE) {
        switch (addr) {
        case UBASE_VECTOR0_CMDQ_SRC_REG:
            cur = ubc_ers2_read32(ubc_dev, addr);
            cur &= ~(uint32_t)val;
            ubc_ers2_write32(ubc_dev, addr, cur);
            return;
        /* CMDQ CSQ registers */
        case UBASE_CSQ_BASEADDR_L_REG:
        case UBASE_CSQ_BASEADDR_H_REG:
        case UBASE_CSQ_DEPTH_REG:
        case UBASE_CSQ_HEAD_REG:
            ubc_ers2_write32(ubc_dev, addr, (uint32_t)val);
            ubc_sync_cmdq_regs(ubc_dev);
            return;
        case UBASE_CSQ_TAIL_REG:
            ubc_ers2_write32(ubc_dev, addr, (uint32_t)val);
            ubc_sync_cmdq_regs(ubc_dev);
            ubc_process_cmdq(ubc_dev);
            return;
        /* CMDQ CRQ registers */
        case UBASE_CRQ_BASEADDR_L_REG:
        case UBASE_CRQ_BASEADDR_H_REG:
        case UBASE_CRQ_DEPTH_REG:
        case UBASE_CRQ_HEAD_REG:
            ubc_ers2_write32(ubc_dev, addr, (uint32_t)val);
            ubc_sync_cmdq_regs(ubc_dev);
            return;
        case UBASE_CRQ_TAIL_REG:
            return;
        /* CtrlQ CSQ registers (separate register set at 0x18800) */
        case UBASE_CTRLQ_CSQ_BASEADDR_L_REG:
        case UBASE_CTRLQ_CSQ_BASEADDR_H_REG:
        case UBASE_CTRLQ_CSQ_DEPTH_REG:
        case UBASE_CTRLQ_CSQ_HEAD_REG:
            ubc_ers2_write32(ubc_dev, addr, (uint32_t)val);
            ubc_sync_ctrlq_regs(ubc_dev);
            return;
        case UBASE_CTRLQ_CSQ_TAIL_REG:
            ubc_ers2_write32(ubc_dev, addr, (uint32_t)val);
            ubc_sync_ctrlq_regs(ubc_dev);
            ubc_process_ctrlq(ubc_dev);
            return;
        /* CtrlQ CRQ registers (separate register set at 0x18800) */
        case UBASE_CTRLQ_CRQ_BASEADDR_L_REG:
        case UBASE_CTRLQ_CRQ_BASEADDR_H_REG:
        case UBASE_CTRLQ_CRQ_DEPTH_REG:
        case UBASE_CTRLQ_CRQ_HEAD_REG:
            ubc_ers2_write32(ubc_dev, addr, (uint32_t)val);
            ubc_sync_ctrlq_regs(ubc_dev);
            return;
        case UBASE_CTRLQ_CRQ_TAIL_REG:
            return;
        default:
            break;
        }
    }

    switch (len) {
    case BYTE_SIZE:
        storage[addr] = (uint8_t)val;
        break;
    case WORD_SIZE:
        stw_le_p(storage + addr, (uint16_t)val);
        break;
    case DWORD_SIZE:
        stl_le_p(storage + addr, (uint32_t)val);
        break;
    case sizeof(uint64_t):
        stq_le_p(storage + addr, val);
        break;
    default:
        break;
    }
}

static uint64_t ub_ers_region_read(void *opaque, hwaddr addr, unsigned len)
{
    typeof(((BusControllerDev *)0)->ers[0]) *ers = opaque;

    if (!ers->storage || addr > ers->storage_size || len > ers->storage_size - addr) {
        return 0;
    }

    switch (len) {
    case BYTE_SIZE:
        return ub_get_byte(ers->storage + addr);
    case WORD_SIZE:
        return ub_get_word(ers->storage + addr);
    case DWORD_SIZE:
        return ub_get_long(ers->storage + addr);
    case sizeof(uint64_t):
        return ub_get_quad(ers->storage + addr);
    default:
        return 0;
    }
}

/*
 * Send URMA data payload over ub_link to the remote node.
 * Wraps the data in a MsgPktHeader with msg_code=UB_MSG_CODE_URMA_DATA
 * so the receive side can distinguish it from control messages.
 *
 * Packet layout: [MsgPktHeader (32 bytes)] [URMA data (payload)]
 * The URMA data payload starts at offset 32 and contains the raw data.
 * src_jetty is in MsgPktHeader.sjetty, dst_jetty is in MsgPktHeader.djetty
 * (using the sjetty/rjetty fields at DW6).
 */
static void ubc_send_data_to_remote(BusControllerDev *ubc_dev, UBCJettyState *js,
                                       const uint8_t *payload, uint32_t payload_len,
                                       uint32_t rmt_obj_id, const uint8_t *rmt_eid)
{
    UBDevice *ub_dev = &ubc_dev->parent;
    UBFMManagedLink *fm_link = NULL;
    UBLinkState *link;
    Error *local_err = NULL;
    uint32_t dcna = 0;
    uint32_t i;

    qemu_log("ubc SEND: jetty=%u rmt_obj_id=%u payload_len=%u\n",
             js->jetty_id, rmt_obj_id, payload_len);

    /* Find the FM link to the remote node */
    for (i = 0; i < ub_dev->port.port_num && !fm_link; i++) {
        NeighborInfo *ni = &ub_dev->port.neighbors[i];
        if (ni->is_remote_neighbor && ni->remote_primary_cna_valid) {
            fm_link = ub_fm_find_link_by_cna(ni->remote_primary_cna);
            if (fm_link) {
                dcna = ni->remote_primary_cna;
            }
        }
    }

    if (!fm_link) {
        qemu_log("ubc SEND: no FM link found, skipping send\n");
        return;
    }

    link = fm_link->runtime;
    if (!link || !link->link_up || !link->ioc) {
        qemu_log("ubc SEND: link not up or no ioc, skipping send\n");
        return;
    }

    /*
     * Build a MsgPktHeader-wrapped URMA data packet.
     * The URMA data payload goes in the payload[] section of MsgPktHeader.
     * We use msg_code=7 for the receive side to distinguish from control messages.
     */
    {
        size_t total_len = sizeof(MsgPktHeader) + payload_len;
        uint8_t *pkt = g_malloc0(total_len);
        MsgPktHeader *hdr = (MsgPktHeader *)pkt;

        /* Fill UbLinkHeader (DW0) */
        hdr->ulh.cfg = UB_CLAN_LINK_CFG;
        hdr->ulh.plen = payload_len;

        /* Fill ClanNetworkHeader (DW1-DW2) */
        hdr->nth.scna = ub_dev->cna;
        hdr->nth.dcna = dcna;

        /* DW6: sjetty/rjetty */
        hdr->sjetty = js->jetty_id;
        hdr->jetty_en = 1;
        /* Store dst_jetty in the djetty field (bits [20:0] of DW6)
         * Actually sjetty uses [20:0], but we also need rjetty.
         * Use the rsv1/odr bits or a separate field. For simplicity,
         * encode dst_jetty as (rmt_obj_id << 20) in sjetty field area.
         * Better: store in the deid field (DW4) which is normally the
         * destination EID but can carry our dst_jetty.
         */
        hdr->deid = rmt_obj_id & 0xFFFFF;

        /* MsgExtendedHeader (DW7) */
        hdr->msgetah.msg_code = UB_MSG_CODE_URMA_DATA;
        hdr->msgetah.type = 1;  /* data (not request) */
        hdr->msgetah.plen = payload_len & 0xFFF;

        /* Copy payload after the MsgPktHeader */
        memcpy(pkt + sizeof(MsgPktHeader), payload, payload_len);

        int rc = ub_link_write_message(link, pkt, total_len, &local_err);
        if (rc < 0) {
            qemu_log("ubc SEND: ub_link write failed: %s\n",
                     local_err ? error_get_pretty(local_err) : "unknown");
            if (local_err) {
                error_free(local_err);
            }
        } else {
            qemu_log("ubc SEND: sent %zu bytes (hdr=%zu data=%u) via ub_link\n",
                     total_len, sizeof(MsgPktHeader), payload_len);
            ub_link_kick_remote(link, NULL);
        }

        g_free(pkt);
    }
}

/*
 * Process a single WQE (64 bytes) from the SQ buffer.
 * udma_sqe_ctl layout (little-endian, 16 uint32_t words = 64 bytes):
 *   DW0:  sqe_bb_idx[15:0] | place_odr[17:16] | comp_order[18] | fence[19]
 *          | se[20] | cqe[21] | inline_en[22] | rsv[27:23] | token_en[28]
 *          | rmt_jetty_type[30:29] | owner[31] | target_hint[39:32]
 *   DW1:  opcode[39:32] | rsv1[45:40] | inline_msg_len[55:46]
 *   DW2:  tpn[23:0] | sge_num[31:24]
 *   DW3:  rmt_obj_id[19:0] | rsv2[31:20]
 *   DW4-7: rmt_eid[0:15]
 *   DW8:  rmt_token_value
 *   DW9:  rsv3
 *   DW10: rmt_addr_l_or_token_id
 *   DW11: rmt_addr_h_or_token_value
 *
 * Followed by SGE entries (16 bytes each):
 *   udma_normal_sge: length(4) | token_id(4) | va(8)
 *
 * For inline SEND: data starts at byte 64 (after the 64-byte header).
 */
static void ubc_process_sq_wqe(BusControllerDev *ubc_dev, UBCJettyState *js,
                                uint32_t wqe_idx)
{
    uint8_t wqe_buf[UDMA_SQE_SIZE];
    dma_addr_t wqe_addr = js->sq_buf_addr + (dma_addr_t)wqe_idx * UDMA_SQE_SIZE;
    MemTxResult ret;
    uint32_t opcode, inline_en, sge_num, inline_msg_len;
    uint32_t rmt_obj_id;
    uint8_t rmt_eid[16];
    uint32_t i;
    dma_addr_t gpa_scan;

    /* DMA-read the 64-byte WQE header */
    ret = ubc_dma_read(ubc_dev, wqe_addr, wqe_buf, UDMA_SQE_SIZE);
    if (ret != MEMTX_OK) {
        qemu_log("ubc WQE: DMA read failed addr=%#" PRIx64 "\n", (uint64_t)wqe_addr);
        return;
    }

    /* Debug: if WQE is all zeros, scan nearby GPAs to find the actual data */
    {
        const uint32_t *dw = (const uint32_t *)wqe_buf;
        bool all_zero = true;
        for (int j = 0; j < 16; j++) {
            if (dw[j] != 0) { all_zero = false; break; }
        }
        if (all_zero && ubc_kva_to_gpa(wqe_addr, &gpa_scan)) {
            uint32_t test_val;
            /* Sanity check: read from RAM base to verify address_space works */
            address_space_read(&address_space_memory, UBC_GUEST_RAM_BASE,
                              MEMTXATTRS_UNSPECIFIED, &test_val, 4);
            fprintf(stderr, "ubc WQE: all zeros at iova=%#lx gpa=%#lx, RAM_BASE[0]=%#x\n",
                    (unsigned long)wqe_addr, (unsigned long)gpa_scan, test_val);
            /* Try reading from address as-is (raw GPA without KVA translation) */
            address_space_read(&address_space_memory, wqe_addr & 0xFFFFFFFFFFFF,
                              MEMTXATTRS_UNSPECIFIED, &test_val, 4);
            fprintf(stderr, "ubc WQE: raw iova&mask=%#lx val=%#x\n",
                    (unsigned long)(wqe_addr & 0xFFFFFFFFFFFF), test_val);
        }
    }

    /* Parse DW0-DW1 fields (little-endian bitfield layout):
     *   DW0: sqe_bb_idx[15:0]|place_odr[17:16]|comp_order[18]|fence[19]|se[20]|cqe[21]
     *        |inline_en[22]|rsv[27:23]|token_en[28]|rmt_jetty_type[30:29]|owner[31]
     *   DW1: target_hint[7:0]|opcode[15:8]|rsv1[21:16]|inline_msg_len[31:22]
     *   DW2: tpn[23:0]|sge_num[31:24]
     *   DW3: rmt_obj_id[19:0]|rsv2[31:20]
     *   DW4-7: rmt_eid[0:15]
     */
    {
        const uint32_t *dw = (const uint32_t *)wqe_buf;
        fprintf(stderr, "ubc WQE raw: dw0=%#x dw1=%#x dw2=%#x dw3=%#x dw4=%#x dw5=%#x dw6=%#x dw7=%#x\n",
                dw[0], dw[1], dw[2], dw[3], dw[4], dw[5], dw[6], dw[7]);
        inline_en = (dw[0] >> 22) & 0x1;
        opcode = (dw[1] >> 8) & 0xFF;
        inline_msg_len = (dw[1] >> 22) & 0x3FF;
        sge_num = (dw[2] >> 24) & 0xFF;
        rmt_obj_id = dw[3] & 0xFFFFF;
        /* rmt_eid at DW4-7 */
        memcpy(rmt_eid, &dw[4], 16);
    }

    qemu_log("ubc WQE: jetty=%u idx=%u op=0x%02x inline=%u sge_num=%u "
             "inline_len=%u rmt_obj_id=%u\n",
             js->jetty_id, wqe_idx, opcode, inline_en, sge_num,
             inline_msg_len, rmt_obj_id);

    /* Only handle SEND opcode for now */
    if (opcode != UDMA_SQE_OPCODE_SEND) {
        qemu_log("ubc WQE: unhandled opcode 0x%02x\n", opcode);
        return;
    }

    /*
     * Extract payload from inline data or SGEs.
     * For inline: data starts at offset 64 in the SQ buffer (next WQEBB).
     * For SGE: each SGE has {length, token_id, va} and we DMA-read from va.
     */
    if (inline_en && inline_msg_len > 0) {
        /* Inline data: starts right after the 48-byte sqe_ctl header */
        uint8_t *payload = g_malloc(inline_msg_len);
        dma_addr_t payload_addr = js->sq_buf_addr +
            (dma_addr_t)wqe_idx * UDMA_SQE_SIZE + UDMA_SQE_CTL_LEN_SEND;

        ret = ubc_dma_read(ubc_dev, payload_addr, payload, inline_msg_len);
        if (ret != MEMTX_OK) {
            qemu_log("ubc WQE inline: DMA read payload failed len=%u\n",
                     inline_msg_len);
            g_free(payload);
            return;
        }

        qemu_log("ubc WQE inline: jetty=%u len=%u first_bytes=%02x %02x %02x %02x\n",
                 js->jetty_id, inline_msg_len,
                 inline_msg_len > 0 ? payload[0] : 0,
                 inline_msg_len > 1 ? payload[1] : 0,
                 inline_msg_len > 2 ? payload[2] : 0,
                 inline_msg_len > 3 ? payload[3] : 0);

        /* Send payload over ub_link */
        ubc_send_data_to_remote(ubc_dev, js, payload, inline_msg_len,
                                rmt_obj_id, rmt_eid);
        g_free(payload);
    } else if (sge_num > 0) {
        /* SGE mode: SGEs start at offset 48 (after the 48-byte sqe_ctl header for SEND) */
        for (i = 0; i < sge_num && i < 8; i++) {
            dma_addr_t sge_addr = js->sq_buf_addr +
                (dma_addr_t)wqe_idx * UDMA_SQE_SIZE + UDMA_SQE_CTL_LEN_SEND +
                (dma_addr_t)i * UDMA_JFS_SGE_SIZE;
            uint8_t sge_buf[UDMA_JFS_SGE_SIZE];
            uint32_t sge_len;
            uint64_t sge_va;

            ret = ubc_dma_read(ubc_dev, sge_addr, sge_buf, UDMA_JFS_SGE_SIZE);
            if (ret != MEMTX_OK) {
                qemu_log("ubc WQE SGE[%u]: DMA read failed\n", i);
                continue;
            }

            /* Parse udma_normal_sge: length(4) token_id(4) va(8) */
            memcpy(&sge_len, sge_buf, 4);
            memcpy(&sge_va, sge_buf + 8, 8);

            if (sge_len == 0) {
                continue;
            }

            qemu_log("ubc WQE SGE[%u]: va=%#" PRIx64 " len=%u\n",
                     i, sge_va, sge_len);

            /* DMA-read the actual data from SGE VA */
            uint8_t *payload = g_malloc(sge_len);
            ret = ubc_dma_read(ubc_dev, sge_va, payload, sge_len);
            if (ret != MEMTX_OK) {
                qemu_log("ubc WQE SGE[%u]: data DMA read failed\n", i);
                g_free(payload);
                continue;
            }

            ubc_send_data_to_remote(ubc_dev, js, payload, sge_len,
                                    rmt_obj_id, rmt_eid);
            g_free(payload);
            /* For simplicity, send only the first SGE's data */
            break;
        }
    }
}

/*
 * Generate a send CQE in the CQ buffer associated with the jetty's TX JFC.
 * CQE format (64 bytes, 16 DWORDs):
 *   DW0: s_r=0 | is_jetty=1 | owner | ... | status=0 | entry_idx
 *   DW1: entry_idx[15:0] | local_num_l
 *   DW4: byte_cnt
 *   DW5-DW6: user_data (from WQE)
 * After writing, update cq_pi and raise a CEQ interrupt.
 */
static void ubc_generate_send_cqe(BusControllerDev *ubc_dev, UBCJettyState *js,
                                   uint32_t wqe_idx, uint32_t byte_cnt)
{
    UBCJfcState *jfc;
    uint8_t cqe[64];
    uint32_t *dw = (uint32_t *)cqe;
    dma_addr_t cqe_addr;
    MemTxResult ret;

    if (js->tx_jfcn >= UBC_MAX_JFCS) {
        qemu_log("ubc CQE: tx_jfcn=%u out of range\n", js->tx_jfcn);
        return;
    }

    jfc = &ubc_dev->jfcs[js->tx_jfcn];
    if (!jfc->active) {
        qemu_log("ubc CQE: tx JFC %u not active\n", js->tx_jfcn);
        return;
    }

    memset(cqe, 0, sizeof(cqe));

    /* DW0: s_r=0(send), is_jetty=1, owner=1, status=0(success) */
    dw[0] = (0 << 0)    /* s_r: send */
          | (1 << 1)    /* is_jetty */
          | (1 << 2)    /* owner */
          | (0 << 24);  /* status = 0 (success) */

    /* DW1: entry_idx = wqe_idx */
    dw[1] = wqe_idx & 0xFFFF;

    /* DW4: byte_cnt */
    dw[4] = byte_cnt;

    /* Write CQE to CQ buffer at cq_pi */
    cqe_addr = jfc->cq_buf_addr + (dma_addr_t)jfc->cq_pi * 64;
    ret = ubc_dma_write(ubc_dev, cqe_addr, cqe, 64);
    if (ret != MEMTX_OK) {
        qemu_log("ubc CQE: DMA write failed addr=%#" PRIx64 "\n", (uint64_t)cqe_addr);
        return;
    }

    jfc->cq_pi = (jfc->cq_pi + 1) % jfc->cq_depth;
    qemu_log("ubc CQE: jetty=%u wqe_idx=%u byte_cnt=%u cq_pi=%u jfc=%u\n",
             js->jetty_id, wqe_idx, byte_cnt, jfc->cq_pi, js->tx_jfcn);

    /* Raise CEQ interrupt via USI */
    ubc_raise_cmdq_event(ubc_dev);
}

/*
 * Process all pending WQEs on a jetty's SQ from sq_ci up to sq_pi.
 */
static void ubc_process_sq(BusControllerDev *ubc_dev, UBCJettyState *js)
{
    uint32_t pi = js->sq_pi;
    uint32_t ci = js->sq_ci;

    while (ci != pi) {
        ubc_process_sq_wqe(ubc_dev, js, ci);
        /* Generate send CQE for this WQE */
        ubc_generate_send_cqe(ubc_dev, js, ci, 0);
        ci = (ci + 1) % js->sq_depth;
    }
    js->sq_ci = ci;
}

/*
 * Handle incoming URMA data from a remote node.
 * This is called from ubc_msgq.c when a URMA data packet arrives via ub_link.
 *
 * For now, we log the data and raise an interrupt to notify the guest.
 * The guest kernel's ipourma driver will process the received data through
 * the normal URMA completion path.
 */
/* --- URMA RX buffering for early-arriving packets --- */
#define UBC_URMA_RX_BUF_MAX 64
#define UBC_URMA_RX_BUF_DATA_MAX 4096

static void ubc_buffer_urma_rx(BusControllerDev *ubc_dev, uint32_t dst_jetty,
                                   const uint8_t *data, uint32_t data_len)
{
    if (ubc_dev->urma_rx_buf.count >= UBC_URMA_RX_BUF_MAX) {
        qemu_log("ubc URMA RX: buffer full (%u), dropping packet for jetty %u\n",
                      ubc_dev->urma_rx_buf.count, dst_jetty);
        return;
    }
    if (data_len > UBC_URMA_RX_BUF_DATA_MAX) {
        qemu_log("ubc URMA RX: packet too large (%u) for buffer, jetty %u\n",
                      data_len, dst_jetty);
        return;
    }
    ubc_dev->urma_rx_buf.entries[ubc_dev->urma_rx_buf.count].dst_jetty = dst_jetty;
    ubc_dev->urma_rx_buf.entries[ubc_dev->urma_rx_buf.count].data_len = data_len;
    memcpy(ubc_dev->urma_rx_buf.entries[ubc_dev->urma_rx_buf.count].data, data, data_len);
    ubc_dev->urma_rx_buf.count++;
    qemu_log("ubc URMA RX: buffered packet for jetty %u len=%u (total buffered=%u)\n",
             dst_jetty, data_len, ubc_dev->urma_rx_buf.count);
}

static void ubc_flush_urma_rx_buffer(BusControllerDev *ubc_dev, uint32_t jetty_id)
{
    uint32_t i, delivered = 0;
    for (i = 0; i < ubc_dev->urma_rx_buf.count; i++) {
        if (ubc_dev->urma_rx_buf.entries[i].dst_jetty == jetty_id) {
            qemu_log("ubc URMA RX flush: delivering buffered packet %u/%u for jetty %u\n",
                     i, ubc_dev->urma_rx_buf.count, jetty_id);
            ubc_handle_urma_rx_data(ubc_dev, jetty_id,
                                      ubc_dev->urma_rx_buf.entries[i].data,
                                      ubc_dev->urma_rx_buf.entries[i].data_len);
            delivered++;
        }
    }
    if (delivered > 0) {
        /* Compact: remove delivered entries */
        uint32_t new_count = 0;
        for (i = 0; i < ubc_dev->urma_rx_buf.count; i++) {
            if (ubc_dev->urma_rx_buf.entries[i].dst_jetty != jetty_id) {
                if (new_count != i) {
                    ubc_dev->urma_rx_buf.entries[new_count] =
                    ubc_dev->urma_rx_buf.entries[i];
                }
                new_count++;
            }
        }
        ubc_dev->urma_rx_buf.count = new_count;
    }
}

void ubc_handle_urma_rx_data(BusControllerDev *ubc_dev, uint32_t dst_jetty,
                              const uint8_t *data, uint32_t data_len)
{
    UBCJettyState *js;
    UBCJfrState  *jfr;
    UBCJfcState  *jfc;
    uint32_t rx_jfcn;
    uint8_t sge_buf[16];
    dma_addr_t rqe_addr;
    uint64_t sge_va;
    uint32_t sge_len;
    uint8_t cqe_buf[64];
    dma_addr_t cqe_addr;
    uint32_t *dw;

    fprintf(stderr, "ubc URMA RX: dst_jetty=%u len=%u\n", dst_jetty, data_len);

    if (!ubc_dev || dst_jetty >= UBC_MAX_JETTIES) {
        qemu_log("ubc URMA RX: dst_jetty %u out of range\n", dst_jetty);
        ubc_buffer_urma_rx(ubc_dev, dst_jetty, data, data_len);
        return;
    }

    if (!ubc_dev->jetties[dst_jetty].active) {
        qemu_log("ubc URMA RX: dst jetty %u not active, buffering\n", dst_jetty);
        ubc_buffer_urma_rx(ubc_dev, dst_jetty, data, data_len);
        return;
    }

    js = &ubc_dev->jetties[dst_jetty];

    if (js->jfrn >= UBC_MAX_JFRS || !ubc_dev->jfrs[js->jfrn].active) {
        qemu_log("ubc URMA RX: JFR %u not active for jetty %u\n", js->jfrn, dst_jetty);
        return;
    }

    jfr = &ubc_dev->jfrs[js->jfrn];
    if (!jfr->rq_buf_addr) {
        qemu_log("ubc URMA RX: no RQ buffer for JFR %u\n", js->jfrn);
        return;
    }

    /* DMA-read the next RECV WQE (16-byte SGE) from the RQ */
    rqe_addr = jfr->rq_buf_addr + (dma_addr_t)jfr->rq_ci * 16;
    if (ubc_dma_read(ubc_dev, rqe_addr, sge_buf, 16) != MEMTX_OK) {
        qemu_log("ubc URMA RX: RQE DMA read failed at ci=%u addr=%#" PRIx64 "\n",
                 jfr->rq_ci, rqe_addr);
        return;
    }

    /* Parse SGE: length(4) + token_id(4) + va(8) */
    memcpy(&sge_len, sge_buf, 4);
    memcpy(&sge_va, sge_buf + 8, 8);

    qemu_log("ubc URMA RX: RQE ci=%u sge_va=%#" PRIx64 " sge_len=%u data_len=%u\n",
             jfr->rq_ci, sge_va, sge_len, data_len);

    if (sge_len < data_len || !sge_va) {
        qemu_log("ubc URMA RX: RQE buffer too small (%u < %u) or null va\n",
                 sge_len, data_len);
        return;
    }

    /* DMA-write the received data into the guest's RECV buffer */
    if (ubc_dma_write(ubc_dev, sge_va, data, data_len) != MEMTX_OK) {
        qemu_log("ubc URMA RX: data DMA write failed\n");
        return;
    }

    /* Generate a receive CQE in the RX JFC's CQ */
    rx_jfcn = js->rx_jfcn;
    if (rx_jfcn >= UBC_MAX_JFCS || !ubc_dev->jfcs[rx_jfcn].active) {
        qemu_log("ubc URMA RX: RX JFC %u not active\n", rx_jfcn);
        return;
    }

    jfc = &ubc_dev->jfcs[rx_jfcn];
    memset(cqe_buf, 0, 64);
    dw = (uint32_t *)cqe_buf;
    /* CQE: wqe_idx at [15:0], owner bit at [31] */
    dw[0] = (jfr->rq_ci & 0xFFFF) | (1u << 31);
    /* byte_cnt at DW4 */
    dw[4] = data_len;

    cqe_addr = jfc->cq_buf_addr + (dma_addr_t)jfc->cq_pi * 64;
    if (ubc_dma_write(ubc_dev, cqe_addr, cqe_buf, 64) != MEMTX_OK) {
        qemu_log("ubc URMA RX: CQE DMA write failed\n");
        return;
    }

    jfc->cq_pi = (jfc->cq_pi + 1) % jfc->cq_depth;
    jfr->rq_ci = (jfr->rq_ci + 1) % jfr->rq_depth;

    qemu_log("ubc URMA RX: CQE written jetty=%u rq_ci=%u cq_pi=%u byte_cnt=%u\n",
             dst_jetty, jfr->rq_ci, jfc->cq_pi, data_len);
    fprintf(stderr, "ubc URMA RX: CQE done jetty=%u rq_ci=%u cq_pi=%u len=%u\n",
            dst_jetty, jfr->rq_ci, jfc->cq_pi, data_len);

    /* Raise interrupt to notify guest */
    ubc_raise_cmdq_event(ubc_dev);
}

static void ub_ers_region_write(void *opaque, hwaddr addr, uint64_t val, unsigned len)
{
    typeof(((BusControllerDev *)0)->ers[0]) *ers = opaque;
    BusControllerDev *ubc_dev = ers->owner;

    /* Diagnostic: log ALL ERS2 writes */
    if (ubc_dev && ers->idx == 2) {
        static int ers2_log_count = 0;
        if (ers2_log_count < 200) {
            fprintf(stderr, "ubc ERS2 WRITE addr=0x%lx val=0x%lx len=%u"
                    " storage=%p storage_size=0x%lx\n",
                    (unsigned long)addr, (unsigned long)val, len,
                    ers->storage, (unsigned long)ers->storage_size);
            ers2_log_count++;
        }
    }

    /*
     * ERS1 doorbell intercept (must be BEFORE storage bounds check):
     * The doorbell/MMIO region is ERS1 (resource index 1 = MEM_RESOURCE).
     * ERS1 storage is small (1 page), but doorbells write at high offsets.
     * Doorbell offset = 0x1000 + jetty_id * 0x1000 + 0x80
     */
    if (ubc_dev && ers->idx == 1 && len == DWORD_SIZE &&
        addr >= 0x1080 && (addr & 0xFFF) == 0x80) {
        uint32_t jetty_id = ((addr & ~0xFFFULL) - 0x1000) >> 12;

        fprintf(stderr, "ubc doorbell: jetty_id=%u addr=0x%lx val=%lu active=%d\n",
                jetty_id, (unsigned long)addr, (unsigned long)val,
                jetty_id < UBC_MAX_JETTIES ? (int)ubc_dev->jetties[jetty_id].active : -1);
        if (jetty_id < UBC_MAX_JETTIES && ubc_dev->jetties[jetty_id].active) {
            UBCJettyState *js = &ubc_dev->jetties[jetty_id];
            js->sq_pi = (uint32_t)val;
            fprintf(stderr, "ubc doorbell: jetty_id=%u new_pi=%u sq_ci=%u\n",
                     jetty_id, js->sq_pi, js->sq_ci);
            ubc_process_sq(ubc_dev, js);
            return; /* doorbell is write-only, don't store */
        }
    }

    if (!ers->storage || addr > ers->storage_size || len > ers->storage_size - addr) {
        return;
    }

    if (ubc_dev && ers->idx == 2) {
        ubc_ers2_mmio_write(ubc_dev, addr, val, len);
        return;
    }

    switch (len) {
    case BYTE_SIZE:
        ub_set_byte(ers->storage + addr, val);
        break;
    case WORD_SIZE:
        ub_set_word(ers->storage + addr, val);
        break;
    case DWORD_SIZE:
        ub_set_long(ers->storage + addr, val);
        break;
    case sizeof(uint64_t):
        ub_set_quad(ers->storage + addr, val);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps ub_ers_region_ops = {
    .read = ub_ers_region_read,
    .write = ub_ers_region_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void ub_bus_controller_init_ers_regions(UBDevice *dev)
{
    BusControllerDev *ubc_dev = BUS_CONTROLLER_DEV(dev);
    static const uint64_t ers_size_pages[UB_NUM_REGIONS] = {
        UBC_ERS0_SPACE_SIZE,
        UBC_ERS1_SPACE_SIZE,
        UBC_ERS2_SPACE_SIZE,
    };
    static const char *ers_names[UB_NUM_REGIONS] = {
        "ubc-ers0",
        "ubc-ers1",
        "ubc-ers2",
    };
    static const uint64_t ers_start_addr[UB_NUM_REGIONS] = {
        UBC_ERS0_SPACE_ADDR,
        UBC_ERS1_SPACE_ADDR,
        UBC_ERS2_SPACE_ADDR,
    };
    uint8_t i;

    for (i = 0; i < UB_NUM_REGIONS; i++) {
        typeof(ubc_dev->ers[0]) *ers = &ubc_dev->ers[i];
        uint64_t region_size = ers_size_pages[i] * UBC_ERS_PAGE_SIZE;

        ers->owner = ubc_dev;
        ers->idx = i;
        ers->storage_size = (i == 1) ? UBC_ERS_PAGE_SIZE : region_size;
        ers->storage = g_malloc0(ers->storage_size);
        memory_region_init_io(&ers->region, OBJECT(dev), &ub_ers_region_ops,
                              ers, ers_names[i], region_size);
        ub_register_ers(dev, i, &ers->region);

        qemu_log("ubc ers%u initialized size=%#" PRIx64 "\n",
                 i, region_size);
    }
}

static void ub_bus_controller_activate_ers_mappings(UBDevice *dev)
{
    UbCfg1Basic *cfg1_basic;
    uint64_t emulated_offset;
    static const uint64_t ers_ubba[UB_NUM_REGIONS] = {
        UBC_ERS0_SPACE_ADDR,
        UBC_ERS1_SPACE_ADDR,
        UBC_ERS2_SPACE_ADDR,
    };
    uint8_t i;

    emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG1_BASIC_START, true);
    cfg1_basic = (UbCfg1Basic *)(dev->config + emulated_offset);
    cfg1_basic->dev_rs_access_en = 1;

    for (i = 0; i < UB_NUM_REGIONS; i++) {
        uint64_t cfg_off = UB_CFG1_BASIC_START + offsetof(UbCfg1Basic, ers_ubba) +
                           i * sizeof(uint64_t);
        uint32_t data_l = (uint32_t)(ers_ubba[i] & UINT32_MAX);
        uint32_t data_h = (uint32_t)(ers_ubba[i] >> 32);

        ub_default_write_config(dev, cfg_off, &data_l, ~0U);
        ub_default_write_config(dev, cfg_off + sizeof(uint32_t), &data_h, ~0U);
        qemu_log("ubc ers%u activate: ubba=0x%" PRIx64 " mapped_addr=0x%" PRIx64
                 " size=0x%" PRIx64 "\n",
                 i, ers_ubba[i], dev->io_regions[i].addr,
                 dev->io_regions[i].size);
    }
}

static void ub_reg_alloc(DeviceState *dev)
{
    BusControllerState *s = BUS_CONTROLLER(dev);

    s->msgq_reg = g_malloc0(s->msgq_reg_size);
    s->fm_msgq_reg = g_malloc0(s->fm_msgq_reg_size);
    qemu_log("alloc ub reg mem size: msgq_reg %u, "
             "fm_msgq_reg %u\n",
             s->msgq_reg_size, s->fm_msgq_reg_size);
}

static void ub_reg_free(DeviceState *dev)
{
    BusControllerState *s = BUS_CONTROLLER(dev);

    g_free(s->msgq_reg);
    g_free(s->fm_msgq_reg);
    qemu_log("free ub reg mem\n");
}

void ub_notify_retry_timer_cb(void *opaque)
{
    BusControllerState *s = BUS_CONTROLLER(opaque);
    ub_try_inject_remote_cfg_notifies(s);
}

static void ub_bus_controller_realize(DeviceState *dev, Error **errp)
{
    BusControllerState *s = BUS_CONTROLLER(dev);
    SysBusDevice *sysdev = SYS_BUS_DEVICE(dev);
    static uint8_t NO = 0;
    char *name = g_strdup_printf("ubus.%u", NO);

    sysdev->parent_obj.id = g_strdup_printf("ubc.%u", NO++);
    /* for msgq reg */
    memory_region_init_io(&s->msgq_reg_mem, OBJECT(s), &ub_msgq_reg_ops,
                          s, TYPE_BUS_CONTROLLER, s->msgq_reg_size);
    sysbus_init_mmio(sysdev, &s->msgq_reg_mem);
    sysbus_init_irq(sysdev, &s->irq);
    /* for fm msgq reg */
    memory_region_init_io(&s->fm_msgq_reg_mem, OBJECT(s), &ub_fm_msgq_reg_ops,
                          s, TYPE_BUS_CONTROLLER, s->fm_msgq_reg_size);
    sysbus_init_mmio(sysdev, &s->fm_msgq_reg_mem);
    ub_reg_alloc(dev);
    /* for ub controller mmio */
    memory_region_init(&s->io_mmio, OBJECT(s), "UB_MMIO", UINT64_MAX);
    sysbus_init_mmio(sysdev, &s->io_mmio);

    s->bus = ub_register_root_bus(dev, name, &s->io_mmio);
    ub_save_ubc_list(s);
    s->notify_retry_timer = timer_new_ms(QEMU_CLOCK_REALTIME,
                                         ub_notify_retry_timer_cb, s);
    g_free(name);
}

static void ub_bus_instance_guid_unlock(BusControllerDev *ubc_dev);
static void ub_bus_controller_unrealize(DeviceState *dev)
{
    BusControllerState *s = BUS_CONTROLLER(dev);
    SysBusDevice *sysdev = SYS_BUS_DEVICE(dev);
    g_free(sysdev->parent_obj.id);
    ub_fm_controller_unregister(s);
    QLIST_REMOVE(s, node);
    ub_unregister_root_bus(s->bus);
    ub_reg_free(dev);
    ub_bus_instance_guid_unlock(s->ubc_dev);
}

static bool ub_bus_controller_needed(void *opaque)
{
    BusControllerState *s = opaque;
    return s->mig_enabled;
}

static Property ub_bus_controller_properties[] = {
    DEFINE_PROP_UINT32("ub-msgq-reg-size", BusControllerState,
                       msgq_reg_size, 0),
    DEFINE_PROP_UINT32("ub-fm-msgq-reg-size", BusControllerState,
                       fm_msgq_reg_size, 0),
    DEFINE_PROP_BOOL("ub-migration-enabled", BusControllerState,
                     mig_enabled, true),
    DEFINE_PROP_END_OF_LIST(),
};

const VMStateDescription vmstate_ub_bus_controller = {
    .name = TYPE_BUS_CONTROLLER,
    .needed = ub_bus_controller_needed,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        /* support migration later */
        VMSTATE_END_OF_LIST()
    }
};

const VMStateDescription vmstate_ub_bus_controller_dev = {
    .name = TYPE_BUS_CONTROLLER_DEV,
    .needed = ub_bus_controller_needed,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        /* support migration later */
        VMSTATE_END_OF_LIST()
    }
};

static void ub_bus_controller_class_init(ObjectClass *class, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(class);

    device_class_set_props(dc, ub_bus_controller_properties);
    dc->realize = ub_bus_controller_realize;
    dc->unrealize = ub_bus_controller_unrealize;
    dc->vmsd = &vmstate_ub_bus_controller;
}

static void ub_bus_controller_instance_init(Object *obj)
{
    /* do nothing now */
}

static void ub_bus_controller_instance_finalize(Object *obj)
{
    /* do nothing now */
}
static const TypeInfo ub_bus_controller_type_info = {
    .name = TYPE_BUS_CONTROLLER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BusControllerState),
    .instance_init = ub_bus_controller_instance_init,
    .instance_finalize = ub_bus_controller_instance_finalize,
    .class_size = sizeof(BusControllerClass),
    .class_init = ub_bus_controller_class_init,
};

static void ub_bus_controller_cfg0_route_table_init(UBDevice *ub_dev)
{
    uint64_t emulated_offset = ub_cfg_offset_to_emulated_offset(UB_ROUTE_TABLE_START, true);
    UbRouteTable *route_table = (UbRouteTable *)(ub_dev->config + emulated_offset);
    uint32_t port_num = MAX(ub_dev->port.port_num, 1);

    /*
     * The route-table shape needs to follow the guest-visible port topology of the
     * controller. Keep the controller entry plus one local and one remote-facing
     * entry per visible port so multi-port UBC models can scale without reworking
     * the cfg0 contract again when inter-node links are added later.
     */
    route_table->entry_num = port_num * 2 + 1;
    route_table->ers = 1;  /* support exact route */
}

static void ub_bus_controller_space_cfg0_init(UBDevice *ub_dev)
{
    UbCfg0Basic *cfg0_basic;
    Cfg0SupportFeature *support_feature;
    UbCfg0ShpCap *shp_cap;
    UbSlotInfo *slot_info;
    BusControllerDev *ubc_dev = BUS_CONTROLLER_DEV(ub_dev);
    uint64_t emulated_offset;

    emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG0_BASIC_START, true);
    cfg0_basic = (UbCfg0Basic *)(ub_dev->config + emulated_offset);
    cfg0_basic->header.slice_version = UB_SLICE_VERSION;
    cfg0_basic->header.slice_used_size = UB_CFG0_BASIC_SLICE_USED_SIZE;
    cfg0_basic->total_num_of_port = ub_dev->port.port_num & UINT16_MASK;
    cfg0_basic->total_num_of_ue = ubc_dev->entity_count;
    cfg0_basic->cap_bitmap[CFG0_CAP2_SHP_INDEX / BITS_PER_BYTE] =
        1 << (CFG0_CAP2_SHP_INDEX % BITS_PER_BYTE);
    support_feature = &cfg0_basic->support_feature;
    support_feature->bits.entity_available = 1;
    support_feature->bits.mtu_supported = 1;
    support_feature->bits.route_table_supported = SUPPORTED;
    support_feature->bits.upi_supported = SUPPORTED;
    support_feature->bits.switch_supported = SUPPORTED;
    support_feature->bits.cc_supported = NOT_SUPPORTED;
    /* SHP CAP */
    emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG0_CAP2_SHP_START, true);
    shp_cap = (UbCfg0ShpCap *)(ub_dev->config + emulated_offset);
    shp_cap->slot_num = 1;
    shp_cap->header.slice_version = UB_SLICE_VERSION;
    shp_cap->header.slice_used_size = (shp_cap->slot_num * sizeof(UbSlotInfo) + sizeof(UbCfg0ShpCap)) / DWORD_SIZE;
    for (int i = 0; i < shp_cap->slot_num; ++i) {
        slot_info = (UbSlotInfo *)((uint8_t *)shp_cap->slot_info + i * sizeof(UbSlotInfo));
        slot_info->pps = 1;
        slot_info->wlps = 1;
        slot_info->plps = 1;
        slot_info->pdss = 1;
        slot_info->pwcs = 1;
        slot_info->start_port_idx = 0;
        slot_info->end_port_idx = 0;
        slot_info->pp_ctrl = 1;
        slot_info->ms_ctrl = 1;
        slot_info->pd_ctrl = 1;
        slot_info->pds_ctrl = 1;
    }
    ub_bus_controller_cfg0_route_table_init(ub_dev);
}

static void ub_bus_controller_space_cfg1_init(UBDevice *ub_dev)
{
    UbCfg1Basic *cfg1_basic;
    Cfg1SupportFeature *support_feature;
    UbCfg1DecoderCap *dec_cap;
    uint8_t *int_type1_raw;
    uint64_t emulated_offset;
    uint8_t *cfg1_raw;
    uint32_t support_feature_l = 0;

    /* basic */
    emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG1_BASIC_START, true);
    cfg1_basic = (UbCfg1Basic *)(ub_dev->config + emulated_offset);
    cfg1_raw = ub_dev->config + emulated_offset;
    cfg1_basic->header.slice_version = UB_SLICE_VERSION;
    cfg1_basic->header.slice_used_size = UB_CFG1_BASIC_SLICE_USED_SIZE;

    cfg1_basic->cap_bitmap[CFG1_DECODER_CAP_INDEX / BITS_PER_BYTE] |=
        1 << (CFG1_DECODER_CAP_INDEX % BITS_PER_BYTE);
    cfg1_basic->cap_bitmap[CFG1_INT_CAP_INDEX / BITS_PER_BYTE] |=
        1 << (CFG1_INT_CAP_INDEX % BITS_PER_BYTE);

    support_feature = &cfg1_basic->support_feature;
    support_feature->bits.mgs = SUPPORTED;
    support_feature->bits.ubbas = SUPPORTED;
    support_feature->bits.ers0s = SUPPORTED;
    support_feature->bits.ers1s = SUPPORTED;
    support_feature->bits.ers2s = SUPPORTED;
    support_feature->bits.matt_juris = UB_DRIVE;
    support_feature_l |= BIT(2);  /* MGS */
    support_feature_l |= BIT(5);  /* UBBAS */
    support_feature_l |= BIT(6);  /* ERS0S */
    support_feature_l |= BIT(7);  /* ERS1S */
    support_feature_l |= BIT(8);  /* ERS2S */
    support_feature_l |= BIT(10); /* DECODER_JURIS */
    cfg1_basic->ers_space_size[0] = UBC_ERS0_SPACE_SIZE;
    cfg1_basic->ers_space_size[1] = UBC_ERS1_SPACE_SIZE;
    cfg1_basic->ers_space_size[2] = UBC_ERS2_SPACE_SIZE;
    cfg1_basic->ers_start_addr[0] = UBC_ERS0_SPACE_ADDR;
    cfg1_basic->ers_start_addr[1] = UBC_ERS1_SPACE_ADDR;
    cfg1_basic->ers_start_addr[2] = UBC_ERS2_SPACE_ADDR;
    cfg1_basic->ers_ubba[0] = UBC_ERS0_SPACE_ADDR;
    cfg1_basic->ers_ubba[1] = UBC_ERS1_SPACE_ADDR;
    cfg1_basic->ers_ubba[2] = UBC_ERS2_SPACE_ADDR;
    cfg1_basic->eid_upi_ten = UBC_EID_UPI_TEN_DEFAULT_VAL;
    cfg1_basic->class_code = UBC_CLASS_CODE;
    /*
     * Keep the raw config image aligned with the guest-visible register layout.
     * Some packed struct fields in the current tree do not land on the exact
     * offsets the Linux UB driver reads.
     */
    *(uint32_t *)(cfg1_raw + 0x24) = support_feature_l;
    *(uint32_t *)(cfg1_raw + 0x34) = UBC_ERS0_SPACE_SIZE;
    *(uint32_t *)(cfg1_raw + 0x38) = UBC_ERS1_SPACE_SIZE;
    *(uint32_t *)(cfg1_raw + 0x3c) = UBC_ERS2_SPACE_SIZE;
    *(uint32_t *)(cfg1_raw + 0x40) = (uint32_t)(UBC_ERS0_SPACE_ADDR & UINT32_MAX);
    *(uint32_t *)(cfg1_raw + 0x44) = (uint32_t)(UBC_ERS0_SPACE_ADDR >> 32);
    *(uint32_t *)(cfg1_raw + 0x48) = (uint32_t)(UBC_ERS1_SPACE_ADDR & UINT32_MAX);
    *(uint32_t *)(cfg1_raw + 0x4c) = (uint32_t)(UBC_ERS1_SPACE_ADDR >> 32);
    *(uint32_t *)(cfg1_raw + 0x50) = (uint32_t)(UBC_ERS2_SPACE_ADDR & UINT32_MAX);
    *(uint32_t *)(cfg1_raw + 0x54) = (uint32_t)(UBC_ERS2_SPACE_ADDR >> 32);
    *(uint32_t *)(cfg1_raw + 0x58) = (uint32_t)(UBC_ERS0_SPACE_ADDR & UINT32_MAX);
    *(uint32_t *)(cfg1_raw + 0x5c) = (uint32_t)(UBC_ERS0_SPACE_ADDR >> 32);
    *(uint32_t *)(cfg1_raw + 0x60) = (uint32_t)(UBC_ERS1_SPACE_ADDR & UINT32_MAX);
    *(uint32_t *)(cfg1_raw + 0x64) = (uint32_t)(UBC_ERS1_SPACE_ADDR >> 32);
    *(uint32_t *)(cfg1_raw + 0x68) = (uint32_t)(UBC_ERS2_SPACE_ADDR & UINT32_MAX);
    *(uint32_t *)(cfg1_raw + 0x6c) = (uint32_t)(UBC_ERS2_SPACE_ADDR >> 32);
    cfg1_basic->dev_rs_access_en = 1;
    *(uint32_t *)(cfg1_raw + 0xbc) = 1;
    *(uint32_t *)(cfg1_raw + 0x90) = UBC_EID_UPI_TEN_DEFAULT_VAL;
    *(uint32_t *)(cfg1_raw + 0xa4) = UBC_CLASS_CODE;
    cfg1_raw[0x04 + (CFG1_INT_CAP_INDEX / BITS_PER_BYTE)] |=
        1 << (CFG1_INT_CAP_INDEX % BITS_PER_BYTE);
    /* decoder cap */
    emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG1_CAP1_DECODER, true);
    dec_cap = (UbCfg1DecoderCap *)(ub_dev->config + emulated_offset);
    dec_cap->header.slice_version = UB_SLICE_VERSION;
    dec_cap->header.slice_used_size = sizeof(UbCfg1DecoderCap) / DWORD_SIZE;
    dec_cap->decoder.event_size_sup = DECODER_CAP_EVENT_SIZE;
    dec_cap->decoder.cmd_size_sup = DECODER_CAP_CMD_SIZE;
    dec_cap->decoder.mmio_size_sup = DECODER_CAP_MMIO_SIZE;

    emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG1_CAP3_INT_TYPE1, true);
    int_type1_raw = ub_dev->config + emulated_offset;
    *(uint32_t *)(int_type1_raw + 0x0) = UB_SLICE_VERSION | (sizeof(UbCfg1IntType1Cap) / DWORD_SIZE) << 16;
    *(uint16_t *)(int_type1_raw + 0x8) = 6;
}

static void ub_bus_controller_wmask_init(UBDevice *ub_dev)
{
    UbCfg1DecoderCap *dec_cap_mask;
    UbCfg0ShpCap *cfg0_shp_wmask, *cfg0_shp;
    uint64_t emulated_offset;
    uint8_t *cfg1_wmask_raw;
    uint8_t *int_type1_wmask_raw;

    /* cfg0 cap */
    emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG0_CAP2_SHP_START, true);
    cfg0_shp_wmask = (UbCfg0ShpCap *)(ub_dev->wmask + emulated_offset);
    cfg0_shp = (UbCfg0ShpCap *)(ub_dev->config + emulated_offset);
    memset(cfg0_shp_wmask, 0, UB_SLICE_SZ);
    for (int i = 0; i < cfg0_shp->slot_num; ++i) {
        cfg0_shp_wmask->slot_info[i].pp_ctrl = ~0;
        cfg0_shp_wmask->slot_info[i].wl_ctrl = ~0;
        cfg0_shp_wmask->slot_info[i].pl_ctrl = ~0;
        cfg0_shp_wmask->slot_info[i].ms_ctrl = ~0;
        cfg0_shp_wmask->slot_info[i].pd_ctrl = ~0;
        cfg0_shp_wmask->slot_info[i].pds_ctrl = ~0;
        cfg0_shp_wmask->slot_info[i].pw_ctrl = ~0;
    }

    /* cfg1 cap */
    emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG1_CAP1_DECODER, true);
    dec_cap_mask = (UbCfg1DecoderCap *)(ub_dev->wmask + emulated_offset);
    memset(dec_cap_mask, 0, sizeof(UbCfg1DecoderCap));
    dec_cap_mask->decoder_ctrl.decoder_en = ~0;
    memset(&dec_cap_mask->dec_matt_ba, 0xff, sizeof(dec_cap_mask->dec_matt_ba));
    memset(&dec_cap_mask->dec_mmio_ba, 0xff, sizeof(dec_cap_mask->dec_mmio_ba));
    dec_cap_mask->decoder_cmdq_cfg.cmdq_en = ~0;
    dec_cap_mask->decoder_cmdq_cfg.cmdq_size_use = ~0;
    dec_cap_mask->decoder_cmdq_prod.cmdq_wr_idx = ~0;
    dec_cap_mask->decoder_cmdq_prod.cmdq_err_resp = ~0;
    dec_cap_mask->decoder_cmdq_cons.cmdq_rd_idx = ~0;
    dec_cap_mask->decoder_cmdq_cons.cmdq_err = ~0;
    dec_cap_mask->decoder_cmdq_cons.cmdq_err_res = ~0;
    dec_cap_mask->decoder_cmdq_ba.cmdq_ba = ~0;
    dec_cap_mask->decoder_evtq_cfg.evtq_en = ~0;
    dec_cap_mask->decoder_evtq_cfg.evtq_size_use = ~0;
    dec_cap_mask->decoder_evtq_prod.evtq_wr_idx = ~0;
    dec_cap_mask->decoder_evtq_cons.evtq_rd_idx = ~0;
    dec_cap_mask->decoder_evtq_ba.evtq_ba = ~0;

    emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG1_BASIC_START, true);
    cfg1_wmask_raw = ub_dev->wmask + emulated_offset;
    *(uint32_t *)(cfg1_wmask_raw + 0x88) = ~0U;
    *(uint32_t *)(cfg1_wmask_raw + 0x8c) = ~0U;
    *(uint32_t *)(cfg1_wmask_raw + 0xb4) = ~0U;

    emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG1_CAP3_INT_TYPE1, true);
    int_type1_wmask_raw = ub_dev->wmask + emulated_offset;
    *(uint8_t *)(int_type1_wmask_raw + 0x4) = 0x1;
    *(uint32_t *)(int_type1_wmask_raw + 0xc) = ~0U;
    *(uint32_t *)(int_type1_wmask_raw + 0x10) = ~0U;
    *(uint32_t *)(int_type1_wmask_raw + 0x14) = ~0U;
    *(uint32_t *)(int_type1_wmask_raw + 0x18) = ~0U;
    *(uint32_t *)(int_type1_wmask_raw + 0x1c) = ~0U;
    *(uint32_t *)(int_type1_wmask_raw + 0x20) = ~0U;
}

static void ub_bus_controller_w1cmask_init(UBDevice *ub_dev)
{
    UbCfg0ShpCap *cfg0_shp_w1cmask, *cfg0_shp;
    uint64_t emulated_offset;

    /* cfg0 cap */
    emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG0_CAP2_SHP_START, true);
    cfg0_shp_w1cmask = (UbCfg0ShpCap *)(ub_dev->w1cmask + emulated_offset);
    cfg0_shp = (UbCfg0ShpCap *)(ub_dev->config + emulated_offset);
    memset(cfg0_shp_w1cmask, 0, UB_SLICE_SZ);
    for (int i = 0; i < cfg0_shp->slot_num; ++i) {
        cfg0_shp_w1cmask->slot_info[i].pp_st = ~0;
        cfg0_shp_w1cmask->slot_info[i].pdsc_st = ~0;
    }
}

static void ub_bus_controller_dev_config_space_init(UBDevice *dev)
{
    ub_bus_controller_space_cfg0_init(dev);
    ub_bus_controller_space_cfg1_init(dev);
    ub_bus_controller_wmask_init(dev);
    ub_bus_controller_w1cmask_init(dev);
}

static bool ub_ubc_is_empty(UBBus *bus)
{
    UBDevice *dev;
    QLIST_FOREACH(dev, &bus->devices, node) {
        if (dev->dev_type == UB_TYPE_IBUS_CONTROLLER) {
            return false;
        }
    }
    return true;
}

#ifdef __APPLE__
#define UB_BUSINSTANCE_GUID_LOCK_DIR "/tmp/ub-qemu"
#else
#define UB_BUSINSTANCE_GUID_LOCK_DIR "/run/libvirt/qemu"
#endif

static int ub_bus_instance_guid_lock(UbGuid *guid)
{
    char path[256] = {0};
    char guid_str[UB_DEV_GUID_STRING_LENGTH + 1] = {0};
    int lock_fd;

    if (g_mkdir_with_parents(UB_BUSINSTANCE_GUID_LOCK_DIR, 0755) < 0) {
        qemu_log("failed to create bus instance lock dir %s: %s\n",
                 UB_BUSINSTANCE_GUID_LOCK_DIR, strerror(errno));
        return -1;
    }

    ub_device_get_str_from_guid(guid, guid_str, UB_DEV_GUID_STRING_LENGTH + 1);
    snprintf(path, sizeof(path), "%s/ub-bus-instance-%s.lock",  UB_BUSINSTANCE_GUID_LOCK_DIR, guid_str);
    lock_fd = open(path, O_RDONLY | O_CREAT, 0600);
    if (lock_fd < 0) {
        qemu_log("failed to open lock file %s: %s\n", path, strerror(errno));
        return -1;
    }

    if (flock(lock_fd, LOCK_EX | LOCK_NB)) {
        qemu_log("lock %s failed: %s\n", path, strerror(errno));
        close(lock_fd);
        return -1;
    }

    return lock_fd;
}

static void ub_bus_instance_guid_unlock(BusControllerDev *ubc_dev)
{
    char guid_str[UB_DEV_GUID_STRING_LENGTH + 1] = {0};

    if (ubc_dev->bus_instance_lock_fd <= 0) {
        return;
    }

    ub_device_get_str_from_guid(&ubc_dev->bus_instance_guid, guid_str,
                                UB_DEV_GUID_STRING_LENGTH + 1);
    qemu_log("unlock ub bus instance lock for guid: %s\n", guid_str);
    if (flock(ubc_dev->bus_instance_lock_fd, LOCK_UN)) {
        qemu_log("failed to unlock for bus instance guid %s: %s\n",
                 guid_str, strerror(errno));
    }
    close(ubc_dev->bus_instance_lock_fd);
}

static int ub_bus_instance_process(BusControllerDev *ubc_dev, Error **errp)
{
    int lock_fd;

    if (ub_guid_is_none(&ubc_dev->bus_instance_guid)) {
        error_setg(errp, "ubc bus instance guid is required");
        return -1;
    }

    lock_fd = ub_bus_instance_guid_lock(&ubc_dev->bus_instance_guid);
    if (lock_fd < 0) {
        error_setg(errp, "ubc bus instance guid lock failed, it may used by other vm");
        return -1;
    }

    ubc_dev->bus_instance_lock_fd = lock_fd;
    return 0;
}

void ub_entity_table_init(BusControllerDev *ubc_dev)
{
    uint32_t i;

    memset(ubc_dev->entities, 0, sizeof(ubc_dev->entities));
    for (i = 0; i < ubc_dev->entity_count && i < UB_MAX_ENTITIES; i++) {
        UBEntityDesc *e = &ubc_dev->entities[i];
        e->entity_idx = i;
        e->device_id = (i == 0) ? 0x0541 : 0x0542;
        e->state = UB_ENTITY_STATE_PRESENT;
        e->upi = 1;
        e->cna = ubc_dev->parent.cna ? ubc_dev->parent.cna : (0x200 + i);
        e->eid[0] = (i == 0) ? ubc_dev->parent.eid : (0x10001 + i - 1);
        e->ueid[0] = (i == 0) ? ubc_dev->parent.eid : (0x10001 + i - 1);
        e->guid[0] = (uint32_t)ubc_dev->parent.guid.seq_num;
        e->guid[1] = 0;
        e->guid[2] = (ubc_dev->parent.guid.rsv << 8) |
                     (ubc_dev->parent.guid.type) |
                     (ubc_dev->parent.guid.version << 4) |
                     (e->device_id << 16);
        e->guid[3] = ubc_dev->parent.guid.vendor;
        e->ers[0].ss = UBC_ERS0_SPACE_SIZE;
        e->ers[0].sa_l = (uint32_t)(UBC_ERS0_SPACE_ADDR + i * 0x200000);
        e->ers[0].sa_h = 0;
        e->ers[1].ss = UBC_ERS1_SPACE_SIZE;
        e->ers[1].sa_l = (uint32_t)(UBC_ERS1_SPACE_ADDR + i * 0x200000);
        e->ers[1].sa_h = 0;
        e->ers[2].ss = UBC_ERS2_SPACE_SIZE;
        e->ers[2].sa_l = (uint32_t)(UBC_ERS2_SPACE_ADDR + i * 0x200000);
        e->ers[2].sa_h = 0;
        qemu_log("entity_table_init: [%u] device_id=0x%04x eid=0x%x ueid=0x%x "
                 "cna=0x%x upi=%u state=%s\n",
                 i, e->device_id, e->eid[0], e->ueid[0], e->cna, e->upi,
                 e->state == UB_ENTITY_STATE_PRESENT ? "present" : "absent");
    }
    qemu_log("entity_table_init: %u entities initialized\n", ubc_dev->entity_count);
}

UBEntityDesc *ub_entity_desc_for_idx(BusControllerDev *ubc_dev, uint32_t entity_idx)
{
    if (!ubc_dev || entity_idx >= ubc_dev->entity_count || entity_idx >= UB_MAX_ENTITIES) {
        return NULL;
    }
    return &ubc_dev->entities[entity_idx];
}

void ub_entity_cfg_spaces_init(BusControllerDev *ubc_dev)
{
    uint32_t i;
    UBDevice *ub_dev = &ubc_dev->parent;
    uint32_t base_cfg_size = ub_emulated_config_size();

    for (i = 0; i < ubc_dev->entity_count && i < UB_MAX_ENTITIES; i++) {
        UBEntityCfgSpace *space = &ubc_dev->entity_cfg_spaces[i];
        UBEntityDesc *e = &ubc_dev->entities[i];

        /* 分配独立配置空间 */
        space->cfg_base = g_malloc0(base_cfg_size);
        if (!space->cfg_base) {
            qemu_log("entity_cfg_spaces_init: failed to alloc for entity %u\n", i);
            continue;
        }

        /* 复制基础配置 */
        memcpy(space->cfg_base, ub_dev->config, base_cfg_size);

        /* 设置实体特定字段 */
        space->cfg_size = base_cfg_size;
        space->eid = e->eid[0];
        space->cna = e->cna;
        space->upi = e->upi;
        space->initialized = true;

        /* 修改该实体配置空间中的 EID/CNA/UPICNA */
        uint64_t emulated_offset;

        /* 修改 EID */
        emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG0_EID_0_OFFSET, true);
        uint32_t *eid_ptr = (uint32_t *)(space->cfg_base + emulated_offset);
        *eid_ptr = cpu_to_le32(e->eid[0]);

        /* 修改 UPICNA */
        emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG0_UPI_OFFSET, true);
        uint32_t *upi_cna_ptr = (uint32_t *)(space->cfg_base + emulated_offset);
        *upi_cna_ptr = cpu_to_le32((e->upi & 0x7FFF) | ((e->cna & 0xFF) << 16));

        /* 修改 FM CNA */
        emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG0_FM_CNA_OFFSET, true);
        uint32_t *cna_ptr = (uint32_t *)(space->cfg_base + emulated_offset);
        *cna_ptr = cpu_to_le32(e->cna);

        qemu_log("entity_cfg_spaces_init: [%u] eid=%#x cna=%#x upi=%u cfg_size=%u\n",
                 i, space->eid, space->cna, space->upi, space->cfg_size);
    }

    qemu_log("entity_cfg_spaces_init: %u entity cfg spaces initialized\n",
             ubc_dev->entity_count);
}

void ub_entity_cfg_spaces_cleanup(BusControllerDev *ubc_dev)
{
    uint32_t i;

    for (i = 0; i < UB_MAX_ENTITIES; i++) {
        UBEntityCfgSpace *space = &ubc_dev->entity_cfg_spaces[i];
        if (space->initialized && space->cfg_base) {
            g_free(space->cfg_base);
            space->cfg_base = NULL;
            space->initialized = false;
        }
    }
}

static void ub_bus_controller_dev_realize(UBDevice *dev, Error **errp)
{
    UBBus *bus = UB_BUS(qdev_get_parent_bus(DEVICE(dev)));
    BusControllerState *ubc = container_of_ubbus(bus);
    VirtMachineState *vms = VIRT_MACHINE(qdev_get_machine());

    vms->ub_bus = bus;

    if (!ub_ubc_is_empty(bus)) {
        qemu_log("ubc realize repetitively\n");
        error_setg(errp, "ubc realize repetitively");
        return;
    }

    ubc->ubc_dev = BUS_CONTROLLER_DEV(dev);
    ub_entity_table_init(ubc->ubc_dev);
    ub_entity_cfg_spaces_init(ubc->ubc_dev);
    if (dev->guid.type != UB_GUID_TYPE_IBUS_CONTROLLER &&
        dev->guid.type != UB_GUID_TYPE_BUS_CONTROLLER) {
        qemu_log("%s device type set error, expect: %u or %u, actual: %u\n",
                 dev->qdev.id, UB_GUID_TYPE_IBUS_CONTROLLER,
                 UB_GUID_TYPE_BUS_CONTROLLER, dev->guid.type);
        error_setg(errp, "%s device type set error, expect: %u or %u, actual: %u\n",
                   dev->qdev.id, UB_GUID_TYPE_IBUS_CONTROLLER,
                   UB_GUID_TYPE_BUS_CONTROLLER, dev->guid.type);
        return;
    }

    dev->dev_type = UB_TYPE_IBUS_CONTROLLER;
    ub_bus_controller_dev_config_space_init(dev);
    ub_bus_controller_init_ers_regions(dev);
    ub_bus_controller_activate_ers_mappings(dev);
    if (0 > ummu_associating_with_ubc(ubc)) {
        qemu_log("failed to associating ubc with ummu. %s\n", dev->name);
    }

    qemu_log("set type UB_TYPE_CONTROLLER, ubc %p, "
             "ubc->ubc_dev %p, bus %p\n", ubc, ubc->ubc_dev, bus);
    if (ub_bus_instance_process(ubc->ubc_dev, errp)) {
        qemu_log("ub bus instance process failed\n");
        return;
    }
    ub_fm_controller_register(ubc);

    /* Publish endpoint info for discovery scripts */
    {
        const char *node_id = g_getenv("UB_FM_NODE_ID");
        const char *shared_dir = g_getenv("UB_FM_SHARED_DIR");
        if (node_id && shared_dir) {
            g_autofree char *token = g_strdup_printf("%s_%s", node_id, DEVICE(dev)->id);
            g_autofree char *path = g_strdup_printf("%s/%s__1.ini", shared_dir, token);
            GKeyFile *keyfile = g_key_file_new();
            char guid_str[UB_DEV_GUID_STRING_LENGTH + 1];

            ub_device_get_str_from_guid(&dev->guid, guid_str, sizeof(guid_str));
            g_key_file_set_string(keyfile, "endpoint", "device_id", DEVICE(dev)->id);
            g_key_file_set_uint64(keyfile, "endpoint", "port_idx", 1);
            g_key_file_set_string(keyfile, "endpoint", "guid", guid_str);
            g_key_file_set_uint64(keyfile, "endpoint", "primary_cna", dev->cna);

            g_autofree char *data = g_key_file_to_data(keyfile, NULL, NULL);
            g_mkdir_with_parents(shared_dir, 0755);
            g_file_set_contents(path, data, -1, NULL);
            g_key_file_free(keyfile);
            qemu_log("ubc: published endpoint info to %s\n", path);
        }
    }

    /* Finally, publish the full device snapshot */
    ub_publish_device_snapshot(dev, errp);
}

static Property ub_bus_controller_dev_properties[] = {
    DEFINE_PROP_UB_DEV_GUID("bus_instance_guid", BusControllerDev, bus_instance_guid),
    DEFINE_PROP_UINT32("entity_count", BusControllerDev, entity_count, 1),
    DEFINE_PROP_END_OF_LIST(),
};

static void ub_bus_controller_dev_class_init(ObjectClass *class, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(class);
    UBDeviceClass *uc = UB_DEVICE_CLASS(class);

    device_class_set_props(dc, ub_bus_controller_dev_properties);
    uc->realize = ub_bus_controller_dev_realize;
    dc->vmsd = &vmstate_ub_bus_controller_dev;
}

static const TypeInfo ub_bus_controller_dev_type_info = {
    .name = TYPE_BUS_CONTROLLER_DEV,
    .parent = TYPE_UB_DEVICE,
    .instance_size = sizeof(BusControllerDev),
    .class_size = sizeof(BusControllerDevClass),
    .class_init = ub_bus_controller_dev_class_init,
};

static void ub_bus_controller_register_types(void)
{
    type_register_static(&ub_bus_controller_type_info);
    type_register_static(&ub_bus_controller_dev_type_info);
}
type_init(ub_bus_controller_register_types)

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
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "hw/qdev-properties.h"
#include "hw/ub/ub.h"
#include "hw/ub/ub_bus.h"
#include "hw/ub/ub_ubc.h"
#include "hw/ub/ub_config.h"
#include "hw/ub/ub_msg.h"
#include "hw/ub/ub_sec.h"
#include "hw/ub/ub_enum.h"
#include "hw/ub/hisi/ubc.h"
#include "trace.h"
#include "sysemu/dma.h"
#include "hw/ub/ub_cna_mgmt.h"
#include "hw/ub/ub_common.h"
#include "hw/ub/ub_link.h"
#include "hw/ub/hisi/ub_fm.h"
#include "qemu/timer.h"

#define UB_MSG_CODE_URMA_DATA  7  /* URMA data transfer between nodes */

#define UB_CFG_CPL_NOTIFY_MAX_ATTEMPTS 64
#define UB_CFG_CPL_NOTIFY_RETRY_MS 100

static void (*msgq_pool_handlers[])(BusControllerState *s, HiMsgSqe *sqe,
                                    MsgPktHeader *header) = {
    [UB_DEV_REG]         = NULL, /* only send from CFM */
    [UB_DEV_RLS]         = NULL, /* only send from CFM */
    [UB_BI_CREATE]       = NULL,
    [UB_BI_DESTROY]      = NULL,
    [UB_CFG_CPL_NOTIFY]  = NULL, /* only send from CFM */
};

typedef struct UBCfgCplNotifyPld {
    uint32_t flag : 1;
    uint32_t rsvd : 31;
    uint32_t guid[4];
    uint32_t eid[4];
} UBCfgCplNotifyPld;

typedef struct UBPoolNotifyPkt {
    MsgPktHeader header;
    UBCfgCplNotifyPld notify;
} UBPoolNotifyPkt;

static uint32_t ub_remote_cluster_eid(const UbGuid *guid)
{
    uint32_t span = UB_SUPPORT_MAX_EID - 0x10000;
    uint32_t offset = span ? (uint32_t)(guid->seq_num % span) : 0;

    return 0x10000 + offset;
}

void ub_try_inject_remote_cfg_notifies(BusControllerState *s)
{
    UBDevice *udev;
    uint32_t i;
    uint64_t now_ms;

    if (!s || !s->ubc_dev || !s->msgq.rq_inited || !s->msgq.cq_inited) {
        return;
    }

    now_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    udev = UB_DEVICE(s->ubc_dev);
    for (i = 0; i < udev->port.port_num; i++) {
        NeighborInfo *neighbor = &udev->port.neighbors[i];

        if (!neighbor->neighbor_id[0] ||
            !neighbor->remote_bus_instance_guid_valid ||
            neighbor->remote_cfg_notify_sent) {
            continue;
        }
        if (neighbor->remote_cfg_notify_attempts >=
            UB_CFG_CPL_NOTIFY_MAX_ATTEMPTS) {
            qemu_log("ub_cfg_cpl_notify: stop retry after %u attempts for %s port%u -> %s:%u\n",
                     neighbor->remote_cfg_notify_attempts,
                     s->ubc_dev ? s->ubc_dev->parent.qdev.id : "<unknown>",
                     i, neighbor->neighbor_id, neighbor->neighbor_port_idx);
            neighbor->remote_cfg_notify_sent = true;
            continue;
        }
        if (neighbor->remote_cfg_notify_next_retry_ms &&
            now_ms < neighbor->remote_cfg_notify_next_retry_ms) {
            continue;
        }
        qemu_log("ub_cfg_cpl_notify: retry inject for %s port%u -> %s:%u\n",
                 s->ubc_dev ? s->ubc_dev->parent.qdev.id : "<unknown>",
                 i, neighbor->neighbor_id, neighbor->neighbor_port_idx);
        if (ub_inject_remote_cfg_cpl_notify(s, &neighbor->remote_bus_instance_guid,
                                            NULL) == 0) {
            neighbor->remote_cfg_notify_attempts++;
            neighbor->remote_cfg_notify_next_retry_ms =
                now_ms + UB_CFG_CPL_NOTIFY_RETRY_MS;
            qemu_log("ub_cfg_cpl_notify: queued notify attempt %u for %s port%u\n",
                     neighbor->remote_cfg_notify_attempts,
                     s->ubc_dev ? s->ubc_dev->parent.qdev.id : "<unknown>",
                     i);
        }
    }
}

int ub_inject_remote_cfg_cpl_notify(BusControllerState *s,
                                    const UbGuid *remote_bi_guid,
                                    Error **errp)
{
    static uint16_t msn_seed = 1;
    UBPoolNotifyPkt pkt = { 0 };
    HiMsgCqe cqe = { 0 };
    uint32_t pi;

    if (!s || !remote_bi_guid) {
        error_setg(errp, "ub_cfg_cpl_notify: invalid arguments");
        return -1;
    }
    if (!s->msgq.rq_inited || !s->msgq.cq_inited) {
        qemu_log("ub_cfg_cpl_notify: skip inject before rq/cq init for %s\n",
                 s->ubc_dev ? s->ubc_dev->parent.qdev.id : "<unknown>");
        return 1;
    }

    pkt.header.ta_opcode = TAH_OPCODE_MSG;
    pkt.header.msgetah.plen = sizeof(pkt.notify);
    pkt.header.msgetah.type = MSG_REQ;
    pkt.header.msgetah.msg_code = UB_MSG_CODE_POOL;
    pkt.header.msgetah.sub_msg_code = UB_CFG_CPL_NOTIFY;
    pkt.notify.flag = 1;
    memcpy(pkt.notify.guid, remote_bi_guid, sizeof(pkt.notify.guid));
    pkt.notify.eid[0] = ub_remote_cluster_eid(remote_bi_guid);

    pi = fill_rq(s, &pkt, sizeof(pkt));
    if (pi == UINT32_MAX) {
        error_setg(errp, "ub_cfg_cpl_notify: fill_rq failed");
        return -1;
    }

    cqe.task_type = PROTOCOL_MSG;
    cqe.type = MSG_REQ;
    cqe.msg_code = UB_MSG_CODE_POOL;
    cqe.sub_msg_code = UB_CFG_CPL_NOTIFY;
    cqe.p_len = sizeof(pkt);
    cqe.msn = msn_seed++;
    cqe.rq_pi = pi;
    cqe.status = CQE_SUCCESS;
    if (fill_cq(s, &cqe) == UINT32_MAX) {
        error_setg(errp, "ub_cfg_cpl_notify: fill_cq failed");
        return -1;
    }

    qemu_log("ub_cfg_cpl_notify: injected remote bus instance guid for %s eid=%#x\n",
             s->ubc_dev ? s->ubc_dev->parent.qdev.id : "<unknown>",
             pkt.notify.eid[0]);
    return 0;
}

static void handle_msg_pool(void *opaque, HiMsgSqe *sqe, void *payload)
{
    BusControllerState *s = opaque;
    MsgPktHeader *header = (MsgPktHeader *)payload;
    MsgExtendedHeader *msgetah = &header->msgetah;

    if (msgetah->msg_code != UB_MSG_CODE_POOL ||
        msgetah->sub_msg_code >= ARRAY_SIZE(msgq_pool_handlers)) {
        qemu_log("invalid msg code %u or sub msg code %u, array size %lu\n",
                 msgetah->msg_code, msgetah->sub_msg_code, ARRAY_SIZE(msgq_pool_handlers));
        return;
    }

    if (msgq_pool_handlers[msgetah->sub_msg_code]) {
        msgq_pool_handlers[msgetah->sub_msg_code](s, sqe, header);
    } else {
        qemu_log("dont support sub msg code %d.\n", msgetah->sub_msg_code);
    }
}

static void ub_obtain_entity_info_ms_fill_cq_rq(BusControllerState *s, HiMsgSqe *sqe,
                                                MsgPktHeader *header, EntityInfoMsgPkt *rsp_pkt)
{
    HiMsgCqe cqe;
    uint32_t pi;

    memset(&cqe, 0, sizeof(cqe));
    cqe.type = MSG_RSP;
    cqe.msg_code = UB_MSG_CODE_EXCH;
    cqe.sub_msg_code = header->msgetah.sub_msg_code;

    rsp_pkt->header.nth.scna = header->nth.dcna;
    rsp_pkt->header.nth.dcna = header->nth.scna;
    rsp_pkt->header.deid = EID_GEN(header->seid_h, header->seid_l);
    rsp_pkt->header.seid_h = EID_HIGH(header->deid);
    rsp_pkt->header.seid_l = EID_LOW(header->deid);

    cqe.msn = sqe->msn;
    cqe.p_len = sizeof(EntityInfoMsgPkt) + sizeof(struct UeMap);
    pi = fill_rq(s, rsp_pkt, sizeof(*rsp_pkt));
    if (pi == UINT32_MAX) {
        qemu_log("fill rq failed!\n");
        return;
    }

    cqe.status = CQE_SUCCESS;
    cqe.rq_pi = pi;
    (void)fill_cq(s, &cqe);
}

static void ub_obtain_entity_info(BusControllerState *s, HiMsgSqe *sqe, MsgPktHeader *header)
{
    EntityInfoMsgPkt *rsp_pkt = NULL;
    uint32_t rsp_pkt_size;

    rsp_pkt_size = sizeof(EntityInfoMsgPkt) + sizeof(struct UeMap);
    rsp_pkt = g_malloc0(rsp_pkt_size);
    memcpy(&rsp_pkt->header, header, sizeof(rsp_pkt->header));
    rsp_pkt->pld.rsp.entity_nums = 1;
    rsp_pkt->pld.rsp.mue_nums = 1;
    rsp_pkt->pld.rsp.map[0].start_entity_idx = 0;
    rsp_pkt->pld.rsp.map[0].end_entity_idx = 0;
    rsp_pkt->header.msgetah.rsp_status = UB_MSG_RSP_SUCCESS;
    rsp_pkt->header.msgetah.plen = ENTITY_INFO_BASE_PLD_SIZE + sizeof(struct UeMap);
    ub_obtain_entity_info_ms_fill_cq_rq(s, sqe, header, rsp_pkt);
    g_free(rsp_pkt);
}

static void (*msgq_exch_handlers[])(BusControllerState *s, HiMsgSqe *sqe,
                                   MsgPktHeader *header) = {
    [UB_OBTAIN_ENTITY_INFO]     = ub_obtain_entity_info,
    [UB_LINK_NEIGHBOR_QUERY]    = NULL,
};

static void handle_msg_exch(void *opaque, HiMsgSqe *sqe, void *payload)
{
    BusControllerState *s =  opaque;
    MsgPktHeader *header = (MsgPktHeader *)payload;
    MsgExtendedHeader *msgetah = &header->msgetah;

    if (msgetah->msg_code != UB_MSG_CODE_EXCH ||
        msgetah->sub_msg_code >= UB_EXCH_MAX_SUB_MSG_CODE) {
        qemu_log("invalid msg code %u or sub msg code %u, "
                 "please check the driver inside guestos\n",
                 msgetah->msg_code, msgetah->sub_msg_code);
        return;
    }

    if (msgq_exch_handlers[msgetah->sub_msg_code]) {
        msgq_exch_handlers[msgetah->sub_msg_code](s, sqe, header);
    } else {
        qemu_log("exch don't support sub msg code %d.\n", msgetah->sub_msg_code);
    }
}

/*
 * Handle VDM (Vendor Defined Message) — msg_code 3.
 * The guest ubase driver sends entity enable/disable (sub_code 0xf6)
 * as a sync request via this message type.  Acknowledge with success
 * so that the driver does not time out (ETIMEDOUT / -110).
 */
static void handle_msg_vdm(void *opaque, HiMsgSqe *sqe, void *payload)
{
    BusControllerState *s = opaque;
    MsgPktHeader *header = (MsgPktHeader *)payload;
    MsgPktHeader rsp;
    HiMsgCqe cqe;
    uint32_t pi;

    memcpy(&rsp, header, sizeof(rsp));

    /* swap source / destination */
    rsp.nth.scna = header->nth.dcna;
    rsp.nth.dcna = header->nth.scna;
    rsp.deid = EID_GEN(header->seid_h, header->seid_l);
    rsp.seid_h = EID_HIGH(header->deid);
    rsp.seid_l = EID_LOW(header->deid);
    rsp.msgetah.type = MSG_RSP;
    rsp.msgetah.rsp_status = 0; /* success */

    pi = fill_rq(s, &rsp, sizeof(rsp));
    if (pi == UINT32_MAX) {
        qemu_log("vdm: fill_rq failed for sub_code=0x%02x msn=%u\n",
                 header->msgetah.sub_msg_code, sqe->msn);
        return;
    }

    memset(&cqe, 0, sizeof(cqe));
    cqe.type = MSG_RSP;
    cqe.msg_code = UB_MSG_CODE_VDM;
    cqe.sub_msg_code = header->msgetah.sub_msg_code;
    cqe.msn = sqe->msn;
    cqe.p_len = sizeof(rsp) - MSG_PKT_HEADER_SIZE;
    cqe.rq_pi = pi;
    cqe.status = CQE_SUCCESS;
    (void)fill_cq(s, &cqe);

    qemu_log("vdm: ack sub_code=0x%02x code=0x%02x msn=%u\n",
             header->msgetah.sub_msg_code, header->msgetah.code, sqe->msn);
}

static void (*msgq_handlers[])(void *opaque, HiMsgSqe *sqe, void *payload) = {
    [UB_MSG_CODE_RAS]  = NULL,
    [UB_MSG_CODE_LINK] = NULL,
    [UB_MSG_CODE_CFG]  = handle_msg_cfg,
    [UB_MSG_CODE_VDM]  = handle_msg_vdm,
    [UB_MSG_CODE_EXCH] = handle_msg_exch,
    [UB_MSG_CODE_SEC]  = handle_msg_sec,
    [UB_MSG_CODE_POOL]  = handle_msg_pool,
};

static bool paddr_is_validate(BusControllerState *s, uint32_t paddr)
{
    uint32_t payload_start_offset = s->msgq.sq_depth * HI_MSG_SQE_SIZE;;
    uint32_t payload_end_offset = s->msgq.sq_sz;

    if (paddr < payload_start_offset || paddr > payload_end_offset - HI_MSG_SQE_PLD_SIZE) {
        qemu_log("invalid paddr %u, expect paddr in [0x%x, 0x%x]\n",
                 paddr, payload_start_offset, payload_end_offset);
        return false;
    }
    trace_paddr_is_validate(payload_start_offset, payload_end_offset, paddr);

    return true;
}

static void handle_task_type_msg(BusControllerState *s, HiMsgSqe *sqe)
{
    MsgPktHeader *payload = NULL;
    uint8_t msg_code = sqe->msg_code;
    uint32_t p_addr = sqe->p_addr;
    uint32_t plen;

    if (msg_code >= (ARRAY_SIZE(msgq_handlers))) {
        qemu_log("invalid msg code %u, array size %lu\n",
                 msg_code, ARRAY_SIZE(msgq_handlers));
        return;
    }

    if (!paddr_is_validate(s, p_addr)) {
        qemu_log("invalid p_addr 0x%x\n", p_addr);
        return;
    }

    assert(HI_MSG_SQE_PLD_SIZE > sizeof(MsgPktHeader));
    payload = g_malloc0(sizeof(MsgPktHeader));
    if (dma_memory_read(&address_space_memory, s->msgq.sq_base_addr_gpa + p_addr,
                        payload, sizeof(MsgPktHeader), MEMTXATTRS_MEMORY)) {
        qemu_log("Failed to read sq_base_addr_gpa entry\n");
        g_free(payload);
        return;
    }
    plen = payload->msgetah.plen;
    g_free(payload);

    if (HI_MSG_SQE_PLD_SIZE - sizeof(MsgPktHeader) < plen) {
        qemu_log("unexpect msgq seq ply size(0x%x) - MsgPktHeader(0x%lx) < plen(0x%x)\n",
                 HI_MSG_SQE_PLD_SIZE, sizeof(MsgPktHeader), plen);
        return;
    }

    payload = g_malloc0(sizeof(MsgPktHeader) + plen);
    if (dma_memory_read(&address_space_memory, s->msgq.sq_base_addr_gpa + p_addr,
                        payload, sizeof(MsgPktHeader) + plen, MEMTXATTRS_MEMORY)) {
        qemu_log("Failed to read sq_base_addr_gpa entry\n");
        g_free(payload);
        return;
    }

    if (msgq_handlers[msg_code]) {
        msgq_handlers[msg_code](s, sqe, payload);
    } else {
        qemu_log("current cannot support process msg code: %u.\n", msg_code);
    }
    g_free(payload);
}

static void handle_task_type_enum(BusControllerState *s, HiMsgSqe *sqe)
{
    void *payload = NULL;
    uint32_t p_addr = sqe->p_addr;

    if (!paddr_is_validate(s, p_addr)) {
        qemu_log("invalid p_addr 0x%x\n", p_addr);
        return;
    }

    payload = (void *)s->msgq.sq_base_addr_gpa + p_addr;
    handle_msg_enum(s, sqe, payload);
}

static void handle_eu_table_cfg_cmd(BusControllerState *s, HiMsgSqe *sqe, void *payload)
{
    HiEuCfgReq *req = (HiEuCfgReq *)payload;
    HiEuCfgRsp rsp;
    HiMsgCqe cqe;

    /* qemu do nothing for hisi_private msg, just mask the msg return success */
    trace_handle_eu_table_cfg_cmd(req->eu_msg_code, req->cfg_entry_num,
                                  req->tbl_cfg_mode, req->tbl_cfg_status,
                                  req->entry_start_id, req->eid, req->upi);

    memset(&rsp, 0, sizeof(rsp));
    rsp.tbl_cfg_status = EU_CFG_SUCCESS;

    memset(&cqe, 0, sizeof(cqe));
    cqe.opcode = EU_TABLE_CFG_CMD;
    cqe.task_type = HISI_PRIVATE;
    cqe.msn = sqe->msn;
    cqe.p_len = sizeof(rsp);
    cqe.status = CQE_SUCCESS;
    cqe.rq_pi = fill_rq(s, &rsp, sizeof(rsp));
    (void)fill_cq(s, &cqe);
}

static void (*hisi_private_handlers[])(BusControllerState *s, HiMsgSqe *sqe, void *payload) = {
    [CC_CTX_CFG_CMD] = NULL,
    [QUERY_UB_MEM_ROUTE_CMD] = NULL,
    [EU_TABLE_CFG_CMD] = handle_eu_table_cfg_cmd,
    [CC_CTX_QUERY_CMD] = NULL,
};

static void handle_task_type_hisi_private(BusControllerState *s, HiMsgSqe *sqe)
{
    HiEuCfgReq *payload = NULL;
    uint8_t opcode = sqe->opcode;
    uint32_t p_addr = sqe->p_addr;

    if (opcode >= ARRAY_SIZE(hisi_private_handlers)) {
        qemu_log("invalid msg code %u, array size %lu\n",
                 opcode, ARRAY_SIZE(hisi_private_handlers));
        return;
    }

    if (!paddr_is_validate(s, p_addr)) {
        qemu_log("invalid p_addr 0x%x\n", p_addr);
        return;
    }

    assert(HI_MSG_SQE_PLD_SIZE > sizeof(HiEuCfgReq));
    payload = g_malloc0(sizeof(HiEuCfgReq));
    if (dma_memory_read(&address_space_memory, s->msgq.sq_base_addr_gpa + p_addr,
                        payload, sizeof(HiEuCfgReq), MEMTXATTRS_MEMORY)) {
        qemu_log("Failed to read sq_base_addr_gpa entry\n");
        g_free(payload);
        return;
    }

    if (hisi_private_handlers[opcode]) {
        hisi_private_handlers[opcode](s, sqe, payload);
    } else {
        qemu_log("current cannot support process hisi private opcode: %u.\n", opcode);
    }
    g_free(payload);
}

bool msgq_process_task(void *opaque, uint64_t val)
{
    BusControllerState *s = opaque;
    HiMsgSqe *sqe = NULL;
    uint16_t i;
    uint16_t cnt;
    uint32_t ci = ub_get_long(s->msgq_reg + SQ_CI);
    uint32_t pi = ub_get_long(s->msgq_reg + SQ_PI);
    uint32_t depth = s->msgq.sq_depth;

    if (!s->msgq.sq_base_addr_gpa) {
        /* not ready */
        return false;
    }

    if (ci >= depth || pi >= depth) {
        qemu_log("Invalid arguments: ci=%u pi=%u depth=%u\n", ci, pi, depth);
        return false;
    }

    sqe = g_malloc0(sizeof(HiMsgSqe));
    cnt = (pi + depth - ci) % depth;
    for (i = 0; i < cnt; i++) {
        if (dma_memory_read(&address_space_memory, (unsigned long)((HiMsgSqe *)s->msgq.sq_base_addr_gpa + ci),
                            sqe, sizeof(HiMsgSqe), MEMTXATTRS_MEMORY)) {
            qemu_log("Failed to read sq_base_addr_gpa entry\n");
            g_free(sqe);
            return false;
        }
        if (sqe->msg_code >= (ARRAY_SIZE(msgq_handlers))) {
            qemu_log("invalid msg code %u, array size %lu\n",
                     sqe->msg_code, ARRAY_SIZE(msgq_handlers));
            g_free(sqe);
            return false;
        }

        switch (sqe->task_type) {
            case PROTOCOL_MSG:
                handle_task_type_msg(s, sqe);
                break;
            case PROTOCOL_ENUM:
                handle_task_type_enum(s, sqe);
                break;
            case HISI_PRIVATE:
                handle_task_type_hisi_private(s, sqe);
                break;
            default:
                qemu_log("current can not process task type: %u\n", sqe->task_type);
                break;
        }
        ci = (ci + 1) % depth;
    }
    ub_set_long(s->msgq_reg + SQ_CI, ci);
    g_free(sqe);
    return cnt > 0;
}

void msgq_sq_init(void *opaque)
{
    BusControllerState *s = opaque;
    uint32_t addr_l = ub_get_long(s->msgq_reg + SQ_ADDR_L);
    uint32_t addr_h = ub_get_long(s->msgq_reg + SQ_ADDR_H);
    uint32_t depth = ub_get_long(s->msgq_reg + SQ_DEPTH);
    uint64_t size = 0;
    uint64_t sq_base_addr_gpa = addr_l | ((uint64_t)addr_h << 32);

    if (s->msgq.sq_inited) {
        qemu_log("cannot init msgq repeat: gpa(0x%" PRIu64 "), depth(%u)\n",
                 sq_base_addr_gpa, depth);
        return;
    }

    if (depth > HI_MSGQ_MAX_DEPTH || depth < HI_MSGQ_MIN_DEPTH) {
        qemu_log("invalid depth value %u, expect depth [%u, %u]\n",
                 depth, HI_MSGQ_MIN_DEPTH, HI_MSGQ_MAX_DEPTH);
        return;
    }

    if (sq_base_addr_gpa == 0) {
        qemu_log("invalid sq_base_addr_gpa is 0\n");
        return;
    }

    size = (uint64_t)depth * (HI_MSG_SQE_SIZE + HI_MSG_SQE_PLD_SIZE);
    s->msgq.sq_sz = size;
    s->msgq.sq_depth = depth;
    s->msgq.sq_base_addr_gpa = sq_base_addr_gpa;
    s->msgq.sq_inited = true;
    trace_msgq_sq_init(sq_base_addr_gpa, depth, size);
}

void msgq_cq_init(void *opaque)
{
    BusControllerState *s = opaque;
    uint32_t addr_l = ub_get_long(s->msgq_reg + CQ_ADDR_L);
    uint32_t addr_h = ub_get_long(s->msgq_reg + CQ_ADDR_H);
    uint32_t depth = ub_get_long(s->msgq_reg + CQ_DEPTH);
    uint64_t size = 0;
    uint64_t cq_base_addr_gpa = addr_l | ((uint64_t)addr_h << 32);

    if (s->msgq.cq_inited) {
        qemu_log("cannot init msgq repeat: gpa(0x%" PRIu64 "), depth(%u)\n",
                 cq_base_addr_gpa, depth);
        return;
    }

    if (depth > HI_MSGQ_MAX_DEPTH || depth < HI_MSGQ_MIN_DEPTH) {
        qemu_log("invalid depth value %u, expect depth [%u, %u]\n",
                 depth, HI_MSGQ_MIN_DEPTH, HI_MSGQ_MAX_DEPTH);
        return;
    }

    if (cq_base_addr_gpa == 0) {
        qemu_log("invalid cq_base_addr_gpa is 0\n");
        return;
    }

    size = (uint64_t)depth * (HI_MSG_SQE_SIZE + HI_MSG_SQE_PLD_SIZE);
    s->msgq.cq_sz = size;
    s->msgq.cq_depth = depth;
    s->msgq.cq_base_addr_gpa = cq_base_addr_gpa;
    s->msgq.cq_inited = true;
    trace_msgq_cq_init(cq_base_addr_gpa, depth, size);
}

void msgq_rq_init(void *opaque)
{
    BusControllerState *s = opaque;
    uint32_t addr_l = ub_get_long(s->msgq_reg + RQ_ADDR_L);
    uint32_t addr_h = ub_get_long(s->msgq_reg + RQ_ADDR_H);
    uint32_t depth = ub_get_long(s->msgq_reg + RQ_DEPTH);
    uint64_t size = 0;
    uint64_t rq_base_addr_gpa = addr_l | ((uint64_t)addr_h << 32);

    if (s->msgq.rq_inited) {
        qemu_log("cannot init msgq repeat: gpa(0x%" PRIu64 "), depth(%u)\n",
                 rq_base_addr_gpa, depth);
        return;
    }

    if (depth > HI_MSGQ_MAX_DEPTH || depth < HI_MSGQ_MIN_DEPTH) {
        qemu_log("invalid depth value %u, expect depth [%u, %u]\n",
                 depth, HI_MSGQ_MIN_DEPTH, HI_MSGQ_MAX_DEPTH);
        return;
    }

    if (rq_base_addr_gpa == 0) {
        qemu_log("invalid cq_base_addr_gpa is 0\n");
        return;
    }

    size = (uint64_t)depth * (HI_MSG_SQE_SIZE + HI_MSG_SQE_PLD_SIZE);
    s->msgq.rq_sz = size;
    s->msgq.rq_depth = depth;
    s->msgq.rq_base_addr_gpa = rq_base_addr_gpa;
    s->msgq.rq_inited = true;
    trace_msgq_rq_init(rq_base_addr_gpa, depth, size);
}

void msgq_handle_rst(void *opaque)
{
    BusControllerState *s = opaque;

    qemu_log("BusControllerState receive reset event, clear msgq reg info to 0.\n");
    ub_set_long(s->msgq_reg + SQ_CI, 0);
    ub_set_long(s->msgq_reg + SQ_ADDR_L, 0);
    ub_set_long(s->msgq_reg + SQ_ADDR_H, 0);
    ub_set_long(s->msgq_reg + SQ_DEPTH, 0);
    ub_set_long(s->msgq_reg + CQ_PI, 0);
    ub_set_long(s->msgq_reg + CQ_ADDR_L, 0);
    ub_set_long(s->msgq_reg + CQ_ADDR_H, 0);
    ub_set_long(s->msgq_reg + CQ_DEPTH, 0);
    ub_set_long(s->msgq_reg + RQ_PI, 0);
    ub_set_long(s->msgq_reg + RQ_ADDR_L, 0);
    ub_set_long(s->msgq_reg + RQ_ADDR_H, 0);
    ub_set_long(s->msgq_reg + RQ_DEPTH, 0);

    memset(&s->msgq, 0, sizeof(s->msgq));
}

/*
 * Process an incoming message from a remote node via ub_link.
 * For control messages (msg_code 0-6): inject into the guest msgq RQ + CQ.
 * For URMA data (msg_code 7): forward to the URMA receive handler.
 */
void ub_link_process_incoming_message(BusControllerState *s, UBLinkState *link)
{
    void *buf = NULL;
    size_t len = 0;
    Error *local_err = NULL;
    int ret;

    if (!s || !link) {
        return;
    }

    ret = ub_link_read_message(link, &buf, &len, &local_err);
    if (ret < 0) {
        qemu_log("ubc_msgq: failed to read remote message: %s\n",
                 local_err ? error_get_pretty(local_err) : "unknown");
        if (local_err) {
            error_free(local_err);
        }
        return;
    }

    if (ret > 0 && buf) {
        MsgPktHeader *header = (MsgPktHeader *)buf;
        HiMsgCqe cqe = { 0 };
        uint32_t pi;

        qemu_log("ubc_msgq: received remote msg code=%u len=%zu\n",
                 header->msgetah.msg_code, len);

        /* URMA data packets (msg_code=7) are forwarded to ub_ubc.c handler */
        if (header->msgetah.msg_code == UB_MSG_CODE_URMA_DATA &&
            s->ubc_dev && len > sizeof(MsgPktHeader)) {
            uint32_t dst_jetty = header->deid & 0xFFFFF;
            uint8_t *data = (uint8_t *)buf + sizeof(MsgPktHeader);
            uint32_t data_len = len - sizeof(MsgPktHeader);

            ubc_handle_urma_rx_data(s->ubc_dev, dst_jetty, data, data_len);
            g_free(buf);
            return;
        }

        /* Control message: inject into Guest RQ */
        pi = fill_rq(s, buf, len);
        if (pi == UINT32_MAX) {
            qemu_log("ubc_msgq: failed to fill rq for remote message\n");
            g_free(buf);
            return;
        }

        /* Notify Guest via CQ */
        cqe.task_type = PROTOCOL_MSG;
        cqe.type = header->msgetah.type;
        cqe.msg_code = header->msgetah.msg_code;
        cqe.sub_msg_code = header->msgetah.sub_msg_code;
        cqe.p_len = len - sizeof(MsgPktHeader);
        cqe.msn = 0x8000 | (uint16_t)(qemu_clock_get_ms(QEMU_CLOCK_REALTIME) & 0x7FFF);
        cqe.rq_pi = pi;
        cqe.status = CQE_SUCCESS;

        if (fill_cq(s, &cqe) == UINT32_MAX) {
            qemu_log("ubc_msgq: failed to fill cq for remote message\n");
        }

        g_free(buf);
    }
}

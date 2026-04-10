/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2023-2024. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef UB_POOL_MSG_H
#define UB_POOL_MSG_H

#include "hw/ub/ub_msg.h"
#include "hw/ub/ub_ubc.h"

/* Pool 消息码 */
#define UB_MSG_CODE_POOL  6

/* Pool 子消息码 */
#define UB_DEV_REG         0
#define UB_DEV_RLS         1
#define UB_BI_CREATE       2
#define UB_BI_DESTROY      3
#define UB_CFG_CPL_NOTIFY  4

/* entity_base_info 结构 (匹配 guest pool.h) */
typedef struct QEMU_PACKED UBPoolEntityBaseInfo {
    /* DW0 */
    uint32_t entity_idx : 16;
    uint32_t upi        : 15;
    uint32_t rsvd0      : 1;
    /* DW1~DW4 */
    uint32_t eid[4];
    /* DW5~DW8 */
    uint32_t guid[UB_ENTITY_GUID_DW_NUM];
    /* DW9 */
    uint32_t cna        : 24;
    uint32_t rsvd2      : 8;
    /* DW10~DW13 */
    uint32_t ueid[4];
} UBPoolEntityBaseInfo;

/* entity_rs_info 结构 */
typedef struct QEMU_PACKED UBPoolEntityRsInfo {
    uint32_t ss;    /* segment size */
    uint32_t sa_l;  /* start address low */
    uint32_t sa_h;  /* start address high */
} UBPoolEntityRsInfo;

/* entity_reg_msg_pld 结构 */
typedef struct QEMU_PACKED UBPoolEntityRegMsg {
    UBPoolEntityBaseInfo base;
    UBPoolEntityRsInfo ers[UB_ENTITY_MAX_RES_NUM];
} UBPoolEntityRegMsg;

/* entity_rls_msg_pld 结构 */
typedef struct QEMU_PACKED UBPoolEntityRlsMsg {
    uint32_t eid[4];
    uint32_t reason : 8;
    uint32_t rsvd1  : 24;
} UBPoolEntityRlsMsg;

#define UB_POOL_ENTITY_BASE_SIZE  56
#define UB_POOL_ENTITY_RS_SIZE    36  /* total size of all ers[3] entries (matches guest ENTITY_RS_PLD_SIZE) */
#define UB_POOL_ENTITY_REG_SIZE   (UB_POOL_ENTITY_BASE_SIZE + UB_POOL_ENTITY_RS_SIZE)
#define UB_POOL_ENTITY_RLS_SIZE   20

#endif /* UB_POOL_MSG_H */

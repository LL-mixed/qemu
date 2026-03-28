/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
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

#ifndef UB_HISI_FM_H
#define UB_HISI_FM_H
#include "hw/ub/hisi/ubc.h"
#include "hw/ub/ub_link.h"
#include "hw/qdev-core.h"
#include "hw/ub/ub_common.h"

#define FM_MSGQ_REG_OFFSET (UBC_MSGQ_REG_OFFSET + UBC_MSGQ_REG_SIZE)
#define FM_MSGQ_REG_SIZE    0x100000   /* 1MiB */

typedef struct UBFMEndpointDesc {
    char *device_id;
    uint32_t port_idx;
} UBFMEndpointDesc;

typedef struct UBFMTopologyLinkDesc {
    UBFMEndpointDesc a;
    UBFMEndpointDesc b;
    bool link_up;
} UBFMTopologyLinkDesc;

typedef struct UBFMManagedLink {
    UBFMTopologyLinkDesc desc;
    UBLinkState *runtime;
} UBFMManagedLink;

typedef int (*UBFMTopologyPopulateFn)(void *opaque, Error **errp);

uint64_t ub_fm_msgq_reg_read(void *opaque, hwaddr addr, unsigned len);
void ub_fm_msgq_reg_write(void *opaque, hwaddr addr, uint64_t val, unsigned len);
void ub_fm_controller_register(BusControllerState *s);
void ub_fm_controller_unregister(BusControllerState *s);
void ub_fm_set_topology_source(const char *name,
                               UBFMTopologyPopulateFn populate,
                               void *opaque);
void ub_fm_clear_topology_source(void);
const char *ub_fm_get_topology_source_name(void);
int ub_fm_refresh_topology(Error **errp);
int ub_fm_validate_topology_links(const UBFMTopologyLinkDesc *links,
                                  size_t nr_links,
                                  Error **errp);
int ub_fm_set_snapshot_topology_source(const char *name,
                                       const UBFMTopologyLinkDesc *links,
                                       size_t nr_links,
                                       Error **errp);
int ub_fm_load_topology_snapshot_from_file(const char *path, Error **errp);
void ub_fm_set_topology_link(const char *a_device_id, uint32_t a_port_idx,
                             const char *b_device_id, uint32_t b_port_idx,
                             bool link_up);
int ub_fm_install_topology_links(const UBFMTopologyLinkDesc *links,
                                 size_t nr_links, Error **errp);
void ub_fm_remove_topology_link(const char *a_device_id, uint32_t a_port_idx,
                                const char *b_device_id, uint32_t b_port_idx);
void ub_fm_clear_declared_topology(void);
int ub_fm_apply_declared_topology(Error **errp);
#endif

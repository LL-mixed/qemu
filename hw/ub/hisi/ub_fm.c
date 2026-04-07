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

#include "qemu/osdep.h"
#include "hw/ub/hisi/ub_fm.h"
#include "hw/ub/ub.h"
#include "hw/ub/ub_link.h"
#include "hw/ub/ub_ubc.h"
#include "qemu/log.h"
#include "qemu/timer.h"

static GPtrArray *ub_fm_declared_links;
static GPtrArray *ub_fm_active_links;
static char *ub_fm_topology_source_name;
static UBFMTopologyPopulateFn ub_fm_topology_populate;
static void *ub_fm_topology_populate_opaque;
static GPtrArray *ub_fm_snapshot_source_links;
static QEMUTimer *ub_fm_pending_refresh_timer;

static const char *ub_fm_get_local_node_id(void)
{
    const char *local_node_id = g_getenv("UB_FM_NODE_ID");

    return (local_node_id && local_node_id[0]) ? local_node_id : NULL;
}

static bool ub_fm_device_id_is_local_node_scoped(const char *device_id)
{
    const char *local_node_id = ub_fm_get_local_node_id();
    const char *dot;

    if (!local_node_id || !device_id) {
        return false;
    }

    dot = strchr(device_id, '.');
    if (!dot) {
        return false;
    }

    return (size_t)(dot - device_id) == strlen(local_node_id) &&
           !strncmp(device_id, local_node_id, dot - device_id);
}

static bool ub_fm_link_desc_is_relevant_to_local_node(const UBFMTopologyLinkDesc *desc)
{
    const char *local_node_id = ub_fm_get_local_node_id();
    bool a_is_scoped;
    bool b_is_scoped;

    if (!desc || !local_node_id) {
        return true;
    }

    a_is_scoped = desc->a.device_id && strchr(desc->a.device_id, '.');
    b_is_scoped = desc->b.device_id && strchr(desc->b.device_id, '.');
    if (!a_is_scoped && !b_is_scoped) {
        return true;
    }

    return ub_fm_device_id_is_local_node_scoped(desc->a.device_id) ||
           ub_fm_device_id_is_local_node_scoped(desc->b.device_id);
}

static bool ub_fm_raw_device_id_matches_local_node(const char *device_id)
{
    const char *local_node_id = ub_fm_get_local_node_id();
    const char *dot;

    if (!local_node_id || !device_id) {
        return false;
    }

    dot = strchr(device_id, '.');
    if (!dot) {
        return false;
    }

    return (size_t)(dot - device_id) == strlen(local_node_id) &&
           !strncmp(device_id, local_node_id, dot - device_id);
}

static bool ub_fm_raw_link_is_relevant_to_local_node(const char *a_device_id,
                                                     const char *b_device_id)
{
    const char *local_node_id = ub_fm_get_local_node_id();
    bool a_is_scoped;
    bool b_is_scoped;

    if (!local_node_id) {
        return true;
    }

    a_is_scoped = a_device_id && strchr(a_device_id, '.');
    b_is_scoped = b_device_id && strchr(b_device_id, '.');
    if (!a_is_scoped && !b_is_scoped) {
        return true;
    }

    return ub_fm_raw_device_id_matches_local_node(a_device_id) ||
           ub_fm_raw_device_id_matches_local_node(b_device_id);
}

static char *ub_fm_resolve_device_id_for_local_node(const char *device_id)
{
    const char *dot;
    const char *local_node_id = ub_fm_get_local_node_id();

    if (!device_id || !device_id[0]) {
        return NULL;
    }

    if (!local_node_id || !local_node_id[0]) {
        return g_strdup(device_id);
    }

    dot = strchr(device_id, '.');
    if (!dot) {
        return g_strdup(device_id);
    }

    if ((size_t)(dot - device_id) == strlen(local_node_id) &&
        !strncmp(device_id, local_node_id, dot - device_id) &&
        dot[1] != '\0') {
        return g_strdup(dot + 1);
    }

    return g_strdup(device_id);
}

static bool ub_fm_desc_matches_device(UBFMTopologyLinkDesc *desc, UBDevice *dev)
{
    if (!desc || !dev || !dev->qdev.id) {
        return false;
    }

    return (desc->a.device_id &&
            !strcmp(desc->a.device_id, dev->qdev.id)) ||
           (desc->b.device_id &&
            !strcmp(desc->b.device_id, dev->qdev.id));
}

static void ub_fm_configure_remote_links(void);

void ub_fm_controller_register(BusControllerState *s)
{
    Error *local_err = NULL;

    if (!s || !s->ubc_dev) {
        return;
    }

    qemu_log("ub_fm register controller eid=%u guid=%04x-%04x port_num=%u\n",
             s->ubc_dev->parent.eid,
             s->ubc_dev->parent.guid.vendor,
             s->ubc_dev->parent.guid.device_id,
             s->ubc_dev->parent.port.port_num);

    if (ub_fm_refresh_topology(&local_err) < 0) {
        error_report_err(local_err);
    }
}

/* Configure remote links for all active links */
static void ub_fm_configure_remote_links(void)
{
    guint i;

    qemu_log("ub_fm: configure_remote_links: ub_fm_active_links=%p len=%u\n",
             ub_fm_active_links, ub_fm_active_links ? ub_fm_active_links->len : 0);

    if (!ub_fm_active_links) {
        return;
    }

    for (i = 0; i < ub_fm_active_links->len; i++) {
        UBFMManagedLink *link = g_ptr_array_index(ub_fm_active_links, i);
        UBFMEndpointDesc *local = NULL;
        UBFMEndpointDesc *remote = NULL;
        UBDevice *local_dev = NULL;
        g_autofree char *remote_path = NULL;
        g_autoptr(GKeyFile) keyfile = NULL;
        g_autoptr(GError) gerr = NULL;
        g_autofree char *remote_guid_str = NULL;
        UbGuid remote_guid;

        qemu_log("ub_fm: checking link %u: %s:%u <-> %s:%u\n", i,
                 link->desc.a.device_id ? link->desc.a.device_id : "(null)",
                 link->desc.a.port_idx,
                 link->desc.b.device_id ? link->desc.b.device_id : "(null)",
                 link->desc.b.port_idx);

        /* Determine which endpoint is local and which is remote */
        if (link->desc.a.device_id && strchr(link->desc.a.device_id, '.') == NULL) {
            /* a is local (no dot), b is remote */
            local = &link->desc.a;
            remote = &link->desc.b;
        } else if (link->desc.b.device_id && strchr(link->desc.b.device_id, '.') == NULL) {
            /* b is local (no dot), a is remote */
            local = &link->desc.b;
            remote = &link->desc.a;
        } else {
            /* Both endpoints have dots or neither does - skip */
            qemu_log("ub_fm: skipping link %u - cannot determine local endpoint\n", i);
            continue;
        }

        if (!local->device_id) {
            qemu_log("ub_fm: skipping link %u - local device_id is NULL\n", i);
            continue;
        }

        if (!remote->device_id) {
            qemu_log("ub_fm: skipping link %u - remote device_id is NULL\n", i);
            continue;
        }

        qemu_log("ub_fm: local=%s:%u remote=%s:%u\n",
                 local->device_id, local->port_idx,
                 remote->device_id, remote->port_idx);

        /* Find the local device by ID */
        qemu_log("ub_fm: about to call ub_find_device_by_id for %s\n", local->device_id);
        local_dev = ub_find_device_by_id(local->device_id);
        qemu_log("ub_fm: ub_find_device_by_id returned %p\n", local_dev);
        if (!local_dev) {
            qemu_log("ub_fm: cannot find local device for %s\n", local->device_id);
            continue;
        }

        /* Read remote endpoint info from shared directory */
        qemu_log("ub_fm: about to call ub_link_endpoint_path for %s:%u\n", remote->device_id, remote->port_idx);
        remote_path = ub_link_endpoint_path(remote->device_id, remote->port_idx, false);
        qemu_log("ub_fm: remote_path=%s\n", remote_path);
        keyfile = g_key_file_new();
        qemu_log("ub_fm: about to call g_key_file_load_from_file\n");
        if (!g_key_file_load_from_file(keyfile, remote_path, G_KEY_FILE_NONE, &gerr)) {
            qemu_log("ub_fm: failed to load remote endpoint info from %s: %s\n",
                     remote_path, gerr ? gerr->message : "unknown");
            continue;
        }
        qemu_log("ub_fm: g_key_file_load_from_file succeeded\n");

        /* Get remote GUID */
        fprintf(stderr, "ub_fm: about to call g_key_file_get_string\n"); fflush(stderr);
        remote_guid_str = g_key_file_get_string(keyfile, "endpoint", "guid", &gerr);
        fprintf(stderr, "ub_fm: g_key_file_get_string returned %p, gerr=%p\n", remote_guid_str, gerr); fflush(stderr);
        if (!remote_guid_str || gerr) {
            qemu_log("ub_fm: remote endpoint %s missing guid\n", remote_path);
            continue;
        }
        fprintf(stderr, "ub_fm: remote_guid_str=%s\n", remote_guid_str); fflush(stderr);

        /* Parse GUID string */
        fprintf(stderr, "ub_fm: about to call ub_device_get_guid_from_str\n"); fflush(stderr);
        if (!ub_device_get_guid_from_str(&remote_guid, remote_guid_str)) {
            qemu_log("ub_fm: failed to parse remote guid %s\n", remote_guid_str);
            continue;
        }
        fprintf(stderr, "ub_fm: ub_device_get_guid_from_str succeeded\n"); fflush(stderr);

        /* Configure device with remote endpoint info */
        fprintf(stderr, "ub_fm: about to call ub_connect_device_port_remote\n"); fflush(stderr);
        if (ub_connect_device_port_remote(local_dev, local->port_idx,
                                           remote->device_id, &remote_guid,
                                           remote->port_idx, NULL) == 0) {
            char guid_str[UB_DEV_GUID_STRING_LENGTH + 1];
            ub_device_get_str_from_guid(&remote_guid, guid_str, sizeof(guid_str));
            fprintf(stderr, "ub_fm: configured remote link %s:%u -> %s:%u guid=%s\n",
                     local->device_id, local->port_idx,
                     remote->device_id, remote->port_idx,
                     guid_str); fflush(stderr);
        } else {
            fprintf(stderr, "ub_fm: ub_connect_device_port_remote failed\n"); fflush(stderr);
        }
    }
}

void ub_fm_controller_unregister(BusControllerState *s)
{
    guint i;

    if (!s || !s->ubc_dev) {
        return;
    }

    qemu_log("ub_fm unregister controller eid=%u guid=%04x-%04x\n",
             s->ubc_dev->parent.eid,
             s->ubc_dev->parent.guid.vendor,
             s->ubc_dev->parent.guid.device_id);

    if (!ub_fm_active_links) {
        return;
    }

    for (i = 0; i < ub_fm_active_links->len; i++) {
        UBFMManagedLink *link = g_ptr_array_index(ub_fm_active_links, i);

        if (!link->runtime) {
            continue;
        }
        if (ub_fm_desc_matches_device(&link->desc, &s->ubc_dev->parent)) {
            Error *local_err = NULL;

            if (ub_link_deactivate(link->runtime, &local_err) < 0) {
                error_report_err(local_err);
            }
            ub_link_detach_endpoints(link->runtime);
        }
    }
}

void ub_fm_set_topology_source(const char *name,
                               UBFMTopologyPopulateFn populate,
                               void *opaque)
{
    g_free(ub_fm_topology_source_name);
    ub_fm_topology_source_name = g_strdup(name);
    ub_fm_topology_populate = populate;
    ub_fm_topology_populate_opaque = opaque;
}

void ub_fm_clear_topology_source(void)
{
    g_clear_pointer(&ub_fm_topology_source_name, g_free);
    ub_fm_topology_populate = NULL;
    ub_fm_topology_populate_opaque = NULL;
    g_clear_pointer(&ub_fm_snapshot_source_links, g_ptr_array_unref);
}

const char *ub_fm_get_topology_source_name(void)
{
    return ub_fm_topology_source_name;
}

static void ub_fm_free_link_desc_fields(UBFMTopologyLinkDesc *desc)
{
    if (!desc) {
        return;
    }
    g_free(desc->a.device_id);
    g_free(desc->b.device_id);
}

static void ub_fm_free_link_desc(gpointer data)
{
    UBFMTopologyLinkDesc *desc = data;

    if (!desc) {
        return;
    }
    ub_fm_free_link_desc_fields(desc);
    g_free(desc);
}

static void ub_fm_free_managed_link(gpointer data)
{
    UBFMManagedLink *link = data;

    if (!link) {
        return;
    }
    if (link->runtime) {
        object_unref(OBJECT(link->runtime));
    }
    ub_fm_free_link_desc_fields(&link->desc);
    g_free(link);
}

static UBFMTopologyLinkDesc *ub_fm_link_desc_dup(const UBFMTopologyLinkDesc *src)
{
    UBFMTopologyLinkDesc *dst;

    if (!src) {
        return NULL;
    }

    dst = g_new0(UBFMTopologyLinkDesc, 1);
    dst->a.device_id = g_strdup(src->a.device_id);
    dst->a.port_idx = src->a.port_idx;
    dst->b.device_id = g_strdup(src->b.device_id);
    dst->b.port_idx = src->b.port_idx;
    dst->link_up = src->link_up;
    return dst;
}

static bool ub_fm_same_endpoint(const UBFMEndpointDesc *a,
                                const UBFMEndpointDesc *b)
{
    return a && b &&
           a->device_id && b->device_id &&
           a->port_idx == b->port_idx &&
           !strcmp(a->device_id, b->device_id);
}

static bool ub_fm_endpoint_is_valid(const UBFMEndpointDesc *ep)
{
    return ep && ep->device_id && ep->device_id[0];
}

static bool ub_fm_same_topology_pair(const UBFMTopologyLinkDesc *a,
                                     const UBFMTopologyLinkDesc *b)
{
    if (!a || !b) {
        return false;
    }

    return (ub_fm_same_endpoint(&a->a, &b->a) &&
            ub_fm_same_endpoint(&a->b, &b->b)) ||
           (ub_fm_same_endpoint(&a->a, &b->b) &&
            ub_fm_same_endpoint(&a->b, &b->a));
}

static bool ub_fm_link_desc_identical(const UBFMTopologyLinkDesc *a,
                                      const UBFMTopologyLinkDesc *b)
{
    return ub_fm_same_topology_pair(a, b) && a->link_up == b->link_up;
}

static int ub_fm_validate_local_endpoint(const UBFMEndpointDesc *ep, Error **errp)
{
    UBDevice *dev;

    if (!ub_fm_endpoint_is_valid(ep)) {
        error_setg(errp, "ub_fm: endpoint is missing device_id");
        return -1;
    }

    dev = ub_find_device_by_id(ep->device_id);
    if (!dev) {
        return 0;
    }

    if (ep->port_idx >= dev->port.port_num) {
        error_setg(errp,
                   "ub_fm: endpoint %s:%u exceeds local port count %u",
                   ep->device_id, ep->port_idx, dev->port.port_num);
        return -1;
    }

    return 0;
}

int ub_fm_validate_topology_links(const UBFMTopologyLinkDesc *links,
                                  size_t nr_links,
                                  Error **errp)
{
    size_t i, j;

    for (i = 0; i < nr_links; i++) {
        const UBFMTopologyLinkDesc *desc = &links[i];

        if (!ub_fm_link_desc_is_relevant_to_local_node(desc)) {
            continue;
        }

        if (!ub_fm_endpoint_is_valid(&desc->a) ||
            !ub_fm_endpoint_is_valid(&desc->b)) {
            error_setg(errp, "ub_fm: topology link %zu is missing endpoint ids", i);
            return -1;
        }

        if (ub_fm_same_endpoint(&desc->a, &desc->b)) {
            error_setg(errp,
                       "ub_fm: topology link %zu connects endpoint %s:%u to itself",
                       i, desc->a.device_id, desc->a.port_idx);
            return -1;
        }

        if (ub_fm_validate_local_endpoint(&desc->a, errp) < 0 ||
            ub_fm_validate_local_endpoint(&desc->b, errp) < 0) {
            return -1;
        }

        for (j = i + 1; j < nr_links; j++) {
            const UBFMTopologyLinkDesc *other = &links[j];

            if (!ub_fm_link_desc_is_relevant_to_local_node(other)) {
                continue;
            }

            if (ub_fm_same_topology_pair(desc, other) &&
                !ub_fm_link_desc_identical(desc, other)) {
                error_setg(errp,
                           "ub_fm: conflicting duplicate links for %s:%u <-> %s:%u",
                           desc->a.device_id, desc->a.port_idx,
                           desc->b.device_id, desc->b.port_idx);
                return -1;
            }

            if ((ub_fm_same_endpoint(&desc->a, &other->a) &&
                 !ub_fm_same_endpoint(&desc->b, &other->b)) ||
                (ub_fm_same_endpoint(&desc->a, &other->b) &&
                 !ub_fm_same_endpoint(&desc->b, &other->a)) ||
                (ub_fm_same_endpoint(&desc->b, &other->a) &&
                 !ub_fm_same_endpoint(&desc->a, &other->b)) ||
                (ub_fm_same_endpoint(&desc->b, &other->b) &&
                 !ub_fm_same_endpoint(&desc->a, &other->a))) {
                error_setg(errp,
                           "ub_fm: endpoint reuse conflict for %s:%u",
                           ub_fm_same_endpoint(&desc->a, &other->a) ||
                           ub_fm_same_endpoint(&desc->a, &other->b) ?
                           desc->a.device_id : desc->b.device_id,
                           ub_fm_same_endpoint(&desc->a, &other->a) ||
                           ub_fm_same_endpoint(&desc->a, &other->b) ?
                           desc->a.port_idx : desc->b.port_idx);
                return -1;
            }
        }
    }

    return 0;
}

static int ub_fm_validate_declared_topology(Error **errp)
{
    guint i;
    g_autofree UBFMTopologyLinkDesc *links = NULL;

    if (!ub_fm_declared_links || ub_fm_declared_links->len == 0) {
        return 0;
    }

    links = g_new0(UBFMTopologyLinkDesc, ub_fm_declared_links->len);
    for (i = 0; i < ub_fm_declared_links->len; i++) {
        UBFMTopologyLinkDesc *src = g_ptr_array_index(ub_fm_declared_links, i);

        links[i] = *src;
    }

    return ub_fm_validate_topology_links(links, ub_fm_declared_links->len, errp);
}

static int ub_fm_validate_link_ptr_array(GPtrArray *links_array, Error **errp)
{
    guint i;
    g_autofree UBFMTopologyLinkDesc *links = NULL;

    if (!links_array || links_array->len == 0) {
        return 0;
    }

    links = g_new0(UBFMTopologyLinkDesc, links_array->len);
    for (i = 0; i < links_array->len; i++) {
        UBFMTopologyLinkDesc *src = g_ptr_array_index(links_array, i);

        links[i] = *src;
    }

    return ub_fm_validate_topology_links(links, links_array->len, errp);
}

static bool ub_fm_has_declared_active_link(const UBFMTopologyLinkDesc *desc)
{
    guint i;

    if (!ub_fm_declared_links || !desc) {
        return false;
    }

    for (i = 0; i < ub_fm_declared_links->len; i++) {
        UBFMTopologyLinkDesc *declared = g_ptr_array_index(ub_fm_declared_links, i);

        if (declared->link_up && ub_fm_same_topology_pair(desc, declared)) {
            return true;
        }
    }

    return false;
}

static UBFMManagedLink *ub_fm_find_active_link(const UBFMTopologyLinkDesc *desc)
{
    guint i;

    if (!ub_fm_active_links) {
        return NULL;
    }

    for (i = 0; i < ub_fm_active_links->len; i++) {
        UBFMManagedLink *link = g_ptr_array_index(ub_fm_active_links, i);

        if (ub_fm_same_topology_pair(&link->desc, desc)) {
            return link;
        }
    }
    return NULL;
}

static UBFMTopologyLinkDesc *ub_fm_find_declared_link(const UBFMTopologyLinkDesc *desc)
{
    guint i;

    if (!ub_fm_declared_links) {
        return NULL;
    }

    for (i = 0; i < ub_fm_declared_links->len; i++) {
        UBFMTopologyLinkDesc *declared = g_ptr_array_index(ub_fm_declared_links, i);

        if (ub_fm_same_topology_pair(declared, desc)) {
            return declared;
        }
    }

    return NULL;
}

static int ub_fm_prune_inactive_links(Error **errp)
{
    guint i = 0;

    if (!ub_fm_active_links) {
        return 0;
    }

    while (i < ub_fm_active_links->len) {
        UBFMManagedLink *link = g_ptr_array_index(ub_fm_active_links, i);

        if (ub_fm_has_declared_active_link(&link->desc)) {
            i++;
            continue;
        }

        if (link->runtime && ub_link_deactivate(link->runtime, errp) < 0) {
            return -1;
        }
        g_ptr_array_remove_index(ub_fm_active_links, i);
    }

    return 0;
}

static bool ub_fm_has_pending_links(void)
{
    guint i;

    if (!ub_fm_active_links) {
        return false;
    }

    for (i = 0; i < ub_fm_active_links->len; i++) {
        UBFMManagedLink *link = g_ptr_array_index(ub_fm_active_links, i);

        if (link->runtime && ub_link_is_pending(link->runtime)) {
            return true;
        }
    }

    return false;
}

static void ub_fm_schedule_pending_refresh(bool needed);

static void ub_fm_collect_local_device(GPtrArray *devices, UBDevice *dev)
{
    guint i;

    if (!devices || !dev) {
        return;
    }

    for (i = 0; i < devices->len; i++) {
        if (g_ptr_array_index(devices, i) == dev) {
            return;
        }
    }

    g_ptr_array_add(devices, dev);
}

static void ub_fm_reconcile_local_fabric_config(void)
{
    g_autoptr(GPtrArray) devices = g_ptr_array_new();
    guint i;

    if (!ub_fm_active_links) {
        return;
    }

    for (i = 0; i < ub_fm_active_links->len; i++) {
        UBFMManagedLink *link = g_ptr_array_index(ub_fm_active_links, i);

        if (!link->runtime) {
            continue;
        }
        ub_fm_collect_local_device(devices, link->runtime->a.device);
        ub_fm_collect_local_device(devices, link->runtime->b.device);
    }

    for (i = 0; i < devices->len; i++) {
        UBDevice *dev = g_ptr_array_index(devices, i);
        uint32_t desired_cna = dev->cna;
        bool snapshot_changed = false;

        if (object_dynamic_cast(OBJECT(dev), TYPE_BUS_CONTROLLER_DEV)) {
            (void)ub_sync_local_device_cfg_from_snapshot(dev, &snapshot_changed, NULL);
            if (snapshot_changed) {
                qemu_log("ub_fm: reconcile applied snapshot update for %s\n",
                         dev->qdev.id);
            }
        }

        if (!desired_cna) {
            desired_cna = ub_default_cna_for_device(dev);
        }
        ub_set_device_cna(dev, desired_cna);
        ub_program_route_table(dev);
        (void)ub_publish_device_snapshot(dev, NULL);
        qemu_log("ub_fm: reconciled local fabric config for %s cna=%#x ports=%u\n",
                 dev->qdev.id, dev->cna, dev->port.port_num);
    }
}

static void ub_fm_pending_refresh_cb(void *opaque)
{
    Error *local_err = NULL;

    if (ub_fm_refresh_topology(&local_err) < 0) {
        error_report_err(local_err);
    }
    ub_fm_schedule_pending_refresh(ub_fm_has_pending_links());
}

static void ub_fm_schedule_pending_refresh(bool needed)
{
    if (!ub_fm_pending_refresh_timer) {
        ub_fm_pending_refresh_timer = timer_new_ms(QEMU_CLOCK_REALTIME,
                                                   ub_fm_pending_refresh_cb,
                                                   NULL);
    }

    if (needed) {
        timer_mod(ub_fm_pending_refresh_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 2000);
    } else {
        timer_del(ub_fm_pending_refresh_timer);
    }
}

void ub_fm_set_topology_link(const char *a_device_id, uint32_t a_port_idx,
                             const char *b_device_id, uint32_t b_port_idx,
                             bool link_up)
{
    UBFMTopologyLinkDesc *desc = NULL;
    UBFMTopologyLinkDesc key = { 0 };

    if (!a_device_id || !b_device_id) {
        return;
    }
    if (!ub_fm_declared_links) {
        ub_fm_declared_links = g_ptr_array_new_with_free_func(ub_fm_free_link_desc);
    }

    key.a.device_id = (char *)a_device_id;
    key.a.port_idx = a_port_idx;
    key.b.device_id = (char *)b_device_id;
    key.b.port_idx = b_port_idx;

    desc = ub_fm_find_declared_link(&key);
    if (desc) {
        desc->link_up = link_up;
        return;
    }

    desc = g_new0(UBFMTopologyLinkDesc, 1);
    desc->a.device_id = g_strdup(a_device_id);
    desc->a.port_idx = a_port_idx;
    desc->b.device_id = g_strdup(b_device_id);
    desc->b.port_idx = b_port_idx;
    desc->link_up = link_up;
    g_ptr_array_add(ub_fm_declared_links, desc);
}

int ub_fm_install_topology_links(const UBFMTopologyLinkDesc *links,
                                 size_t nr_links, Error **errp)
{
    size_t i;

    if (ub_fm_validate_topology_links(links, nr_links, errp) < 0) {
        return -1;
    }

    ub_fm_clear_declared_topology();
    for (i = 0; i < nr_links; i++) {
        const UBFMTopologyLinkDesc *desc = &links[i];

        if (!desc->a.device_id || !desc->b.device_id) {
            error_setg(errp, "ub_fm: topology link %zu is missing endpoint ids", i);
            return -1;
        }
        ub_fm_set_topology_link(desc->a.device_id, desc->a.port_idx,
                                desc->b.device_id, desc->b.port_idx,
                                desc->link_up);
    }

    return ub_fm_apply_declared_topology(errp);
}

static int ub_fm_populate_snapshot_source(void *opaque, Error **errp)
{
    GPtrArray *links = opaque;
    guint i;

    if (!links) {
        return 0;
    }

    for (i = 0; i < links->len; i++) {
        const UBFMTopologyLinkDesc *desc = g_ptr_array_index(links, i);

        if (!desc->a.device_id || !desc->b.device_id) {
            error_setg(errp, "ub_fm: snapshot topology link %u is missing endpoint ids", i);
            return -1;
        }
        ub_fm_set_topology_link(desc->a.device_id, desc->a.port_idx,
                                desc->b.device_id, desc->b.port_idx,
                                desc->link_up);
    }

    return 0;
}

int ub_fm_refresh_topology(Error **errp)
{
    if (!ub_fm_topology_populate) {
        return ub_fm_apply_declared_topology(errp);
    }

    ub_fm_clear_declared_topology();
    if (ub_fm_topology_populate(ub_fm_topology_populate_opaque, errp) < 0) {
        return -1;
    }

    return ub_fm_apply_declared_topology(errp);
}

int ub_fm_set_snapshot_topology_source(const char *name,
                                       const UBFMTopologyLinkDesc *links,
                                       size_t nr_links,
                                       Error **errp)
{
    GPtrArray *snapshot;
    size_t i;

    if (ub_fm_validate_topology_links(links, nr_links, errp) < 0) {
        return -1;
    }

    snapshot = g_ptr_array_new_with_free_func(ub_fm_free_link_desc);
    for (i = 0; i < nr_links; i++) {
        const UBFMTopologyLinkDesc *desc = &links[i];

        if (!desc->a.device_id || !desc->b.device_id) {
            g_ptr_array_unref(snapshot);
            error_setg(errp, "ub_fm: snapshot topology link %zu is missing endpoint ids", i);
            return -1;
        }
        g_ptr_array_add(snapshot, ub_fm_link_desc_dup(desc));
    }

    g_clear_pointer(&ub_fm_snapshot_source_links, g_ptr_array_unref);
    ub_fm_snapshot_source_links = snapshot;
    ub_fm_set_topology_source(name, ub_fm_populate_snapshot_source,
                              ub_fm_snapshot_source_links);
    return ub_fm_refresh_topology(errp);
}

int ub_fm_load_topology_snapshot_from_file(const char *path, Error **errp)
{
    g_autoptr(GKeyFile) keyfile = NULL;
    g_auto(GStrv) groups = NULL;
    GPtrArray *snapshot;
    GError *gerr = NULL;
    gsize i;

    if (!path || !path[0]) {
        error_setg(errp, "ub_fm: topology file path is empty");
        return -1;
    }

    keyfile = g_key_file_new();
    if (!g_key_file_load_from_file(keyfile, path, G_KEY_FILE_NONE, &gerr)) {
        error_setg(errp, "ub_fm: failed to load topology file %s: %s",
                   path, gerr->message);
        g_error_free(gerr);
        return -1;
    }

    snapshot = g_ptr_array_new_with_free_func(ub_fm_free_link_desc);
    groups = g_key_file_get_groups(keyfile, NULL);
    for (i = 0; groups && groups[i]; i++) {
        const char *group = groups[i];
        UBFMTopologyLinkDesc *desc;
        g_autofree char *raw_a_device_id = NULL;
        g_autofree char *raw_b_device_id = NULL;

        if (!g_str_has_prefix(group, "link ")) {
            continue;
        }

        desc = g_new0(UBFMTopologyLinkDesc, 1);
        raw_a_device_id = g_key_file_get_string(keyfile, group,
                                                "a_device_id", &gerr);
        if (!gerr) {
            desc->a.device_id = ub_fm_resolve_device_id_for_local_node(raw_a_device_id);
        }
        if (gerr) {
            error_setg(errp, "ub_fm: %s missing a_device_id: %s",
                       group, gerr->message);
            g_error_free(gerr);
            g_free(desc);
            g_ptr_array_unref(snapshot);
            return -1;
        }
        desc->a.port_idx = g_key_file_get_uint64(keyfile, group,
                                                 "a_port_idx", &gerr);
        if (gerr) {
            error_setg(errp, "ub_fm: %s missing a_port_idx: %s",
                       group, gerr->message);
            g_error_free(gerr);
            ub_fm_free_link_desc(desc);
            g_ptr_array_unref(snapshot);
            return -1;
        }
        raw_b_device_id = g_key_file_get_string(keyfile, group,
                                                "b_device_id", &gerr);
        if (!gerr) {
            desc->b.device_id = ub_fm_resolve_device_id_for_local_node(raw_b_device_id);
        }
        if (gerr) {
            error_setg(errp, "ub_fm: %s missing b_device_id: %s",
                       group, gerr->message);
            g_error_free(gerr);
            ub_fm_free_link_desc(desc);
            g_ptr_array_unref(snapshot);
            return -1;
        }
        desc->b.port_idx = g_key_file_get_uint64(keyfile, group,
                                                 "b_port_idx", &gerr);
        if (gerr) {
            error_setg(errp, "ub_fm: %s missing b_port_idx: %s",
                       group, gerr->message);
            g_error_free(gerr);
            ub_fm_free_link_desc(desc);
            g_ptr_array_unref(snapshot);
            return -1;
        }
        desc->link_up = g_key_file_get_boolean(keyfile, group,
                                               "link_up", &gerr);
        if (gerr) {
            error_setg(errp, "ub_fm: %s missing link_up: %s",
                       group, gerr->message);
            g_error_free(gerr);
            ub_fm_free_link_desc(desc);
            g_ptr_array_unref(snapshot);
            return -1;
        }

        if (!ub_fm_raw_link_is_relevant_to_local_node(raw_a_device_id,
                                                      raw_b_device_id)) {
            qemu_log("ub_fm: skip non-local link %s:%u <-> %s:%u\n",
                     raw_a_device_id, desc->a.port_idx,
                     raw_b_device_id, desc->b.port_idx);
            ub_fm_free_link_desc(desc);
            continue;
        }

        qemu_log("ub_fm: accept topology file link %s:%u -> %s:%u (resolved %s:%u -> %s:%u up=%d)\n",
                 raw_a_device_id, desc->a.port_idx,
                 raw_b_device_id, desc->b.port_idx,
                 desc->a.device_id, desc->a.port_idx,
                 desc->b.device_id, desc->b.port_idx,
                 desc->link_up);
        g_ptr_array_add(snapshot, desc);
    }

    if (snapshot->len == 0) {
        g_ptr_array_unref(snapshot);
        error_setg(errp, "ub_fm: topology file %s defines no [link ...] groups", path);
        return -1;
    }

    if (ub_fm_validate_link_ptr_array(snapshot, errp) < 0) {
        g_ptr_array_unref(snapshot);
        return -1;
    }

    g_clear_pointer(&ub_fm_snapshot_source_links, g_ptr_array_unref);
    ub_fm_snapshot_source_links = snapshot;
    ub_fm_set_topology_source(path, ub_fm_populate_snapshot_source,
                              ub_fm_snapshot_source_links);
    return ub_fm_refresh_topology(errp);
}

void ub_fm_remove_topology_link(const char *a_device_id, uint32_t a_port_idx,
                                const char *b_device_id, uint32_t b_port_idx)
{
    UBFMTopologyLinkDesc key = { 0 };
    UBFMTopologyLinkDesc *desc = NULL;
    guint i;

    if (!ub_fm_declared_links || !a_device_id || !b_device_id) {
        return;
    }

    key.a.device_id = (char *)a_device_id;
    key.a.port_idx = a_port_idx;
    key.b.device_id = (char *)b_device_id;
    key.b.port_idx = b_port_idx;
    desc = ub_fm_find_declared_link(&key);
    if (!desc) {
        return;
    }

    for (i = 0; i < ub_fm_declared_links->len; i++) {
        UBFMTopologyLinkDesc *declared = g_ptr_array_index(ub_fm_declared_links, i);

        if (declared == desc) {
            g_ptr_array_remove_index(ub_fm_declared_links, i);
            return;
        }
    }
}

void ub_fm_clear_declared_topology(void)
{
    if (!ub_fm_declared_links) {
        return;
    }

    g_ptr_array_set_size(ub_fm_declared_links, 0);
}

int ub_fm_apply_declared_topology(Error **errp)
{
    guint i;
    bool has_pending = false;

    if (!ub_fm_declared_links) {
        return 0;
    }
    if (ub_fm_validate_declared_topology(errp) < 0) {
        return -1;
    }
    if (!ub_fm_active_links) {
        ub_fm_active_links = g_ptr_array_new_with_free_func(ub_fm_free_managed_link);
    }
    if (ub_fm_prune_inactive_links(errp) < 0) {
        return -1;
    }

    for (i = 0; i < ub_fm_declared_links->len; i++) {
        UBFMTopologyLinkDesc *desc = g_ptr_array_index(ub_fm_declared_links, i);
        UBFMManagedLink *link = NULL;

        if (!desc->link_up) {
            qemu_log("ub_fm: declared link down %s:%u <-> %s:%u\n",
                     desc->a.device_id, desc->a.port_idx,
                     desc->b.device_id, desc->b.port_idx);
            continue;
        }
        qemu_log("ub_fm: apply declared link %s:%u <-> %s:%u\n",
                 desc->a.device_id, desc->a.port_idx,
                 desc->b.device_id, desc->b.port_idx);
        link = ub_fm_find_active_link(desc);
        if (!link) {
            link = g_new0(UBFMManagedLink, 1);
            link->desc.a.device_id = g_strdup(desc->a.device_id);
            link->desc.a.port_idx = desc->a.port_idx;
            link->desc.b.device_id = g_strdup(desc->b.device_id);
            link->desc.b.port_idx = desc->b.port_idx;
            link->desc.link_up = desc->link_up;
            link->runtime = UB_LINK(object_new(TYPE_UB_LINK));
            ub_link_configure(link->runtime,
                              desc->a.device_id, desc->a.port_idx,
                              desc->b.device_id, desc->b.port_idx,
                              desc->link_up);
            g_ptr_array_add(ub_fm_active_links, link);
        }
        {
            int ret = ub_link_apply(link->runtime, errp);

            if (ret < 0) {
                /* Link failed - mark as failed but continue with other links */
                has_pending = true;
                continue;
            }
            if (ret > 0) {
                /* Link pending - not ready yet */
                has_pending = true;
                continue;
            }
            /* ret == 0: link fully applied and READY — register AIO receive callback */
            if (link->runtime->remote_applied && !link->runtime->rx_cb) {
                UBDevice *dev = NULL;
                if (link->runtime->a.device) {
                    dev = link->runtime->a.device;
                } else if (link->runtime->b.device) {
                    dev = link->runtime->b.device;
                }
                if (dev && object_dynamic_cast(OBJECT(dev), TYPE_BUS_CONTROLLER_DEV)) {
                    BusControllerState *ubc = container_of_ubbus(
                        UB_BUS(qdev_get_parent_bus(DEVICE(dev))));
                    link->runtime->rx_cb =
                        (void (*)(void *, UBLinkState *))ub_link_process_incoming_message;
                    link->runtime->rx_cb_opaque = ubc;
                }
            }
        }
    }

    if (ub_fm_active_links) {
        for (i = 0; i < ub_fm_active_links->len; i++) {
            UBFMManagedLink *link = g_ptr_array_index(ub_fm_active_links, i);

            if (link->runtime && ub_link_poll_kick(link->runtime)) {
                UBDevice *dev = NULL;
                uint32_t port_idx = 0;

                if (link->runtime->a.device) {
                    dev = link->runtime->a.device;
                    port_idx = link->runtime->a.port_idx;
                } else if (link->runtime->b.device) {
                    dev = link->runtime->b.device;
                    port_idx = link->runtime->b.port_idx;
                }

                if (dev && object_dynamic_cast(OBJECT(dev), TYPE_BUS_CONTROLLER_DEV)) {
                    BusControllerState *ubc = container_of_ubbus(
                        UB_BUS(qdev_get_parent_bus(DEVICE(dev))));
                    bool snapshot_changed = false;

                    /* Register AIO receive callback for direct URMA data delivery */
                    if (!link->runtime->rx_cb) {
                        link->runtime->rx_cb = (void (*)(void *, UBLinkState *))ub_link_process_incoming_message;
                        link->runtime->rx_cb_opaque = ubc;
                    }

                    qemu_log("ub_fm: remote kick received on %s:%u, processing msgq\n",
                             dev->qdev.id, port_idx);
                    ub_link_process_incoming_message(ubc, link->runtime);
                    msgq_process_task(ubc, 0);
                    if (ub_sync_local_device_cfg_from_snapshot(dev, &snapshot_changed, NULL) &&
                        snapshot_changed) {
                        qemu_log("ub_fm: remote kick applied snapshot update on %s:%u\n",
                                 dev->qdev.id, port_idx);
                    }
                }
            }
            /*
             * Keep polling for remote links so runtime kicks/snapshot changes
             * can be observed after the initial topology apply.
             */
            if (link->runtime &&
                (link->runtime->remote_applied ||
                 link->runtime->a.device == NULL ||
                 link->runtime->b.device == NULL)) {
                has_pending = true;
            }
        }
    }

    /* Configure remote links after topology is applied */
    ub_fm_configure_remote_links();

    ub_fm_reconcile_local_fabric_config();
    ub_fm_schedule_pending_refresh(has_pending || ub_fm_has_pending_links());
    return 0;
}

int ub_fm_kick_by_cna(uint32_t dcna, Error **errp)
{
    guint i;

    if (!ub_fm_active_links) {
        return 0;
    }

    for (i = 0; i < ub_fm_active_links->len; i++) {
        UBFMManagedLink *link = g_ptr_array_index(ub_fm_active_links, i);
        UBDevice *local = NULL;
        uint32_t port_idx = 0;

        if (!link->runtime || !link->runtime->remote_applied) {
            continue;
        }

        if (link->runtime->a.device) {
            local = link->runtime->a.device;
            port_idx = link->runtime->a.port_idx;
        } else if (link->runtime->b.device) {
            local = link->runtime->b.device;
            port_idx = link->runtime->b.port_idx;
        }

        if (local && local->port.neighbors[port_idx].is_remote_neighbor &&
            local->port.neighbors[port_idx].remote_primary_cna == (dcna & 0x00ffffffU)) {
            return ub_link_kick_remote(link->runtime, errp);
        }
    }

    return 0;
}

UBFMManagedLink *ub_fm_find_link_by_cna(uint32_t dcna)
{
    guint i;

    if (!ub_fm_active_links) {
        return NULL;
    }

    for (i = 0; i < ub_fm_active_links->len; i++) {
        UBFMManagedLink *link = g_ptr_array_index(ub_fm_active_links, i);
        UBDevice *local = NULL;
        uint32_t port_idx = 0;

        if (!link->runtime || !link->runtime->remote_applied) {
            continue;
        }

        if (link->runtime->a.device) {
            local = link->runtime->a.device;
            port_idx = link->runtime->a.port_idx;
        } else if (link->runtime->b.device) {
            local = link->runtime->b.device;
            port_idx = link->runtime->b.port_idx;
        }

        if (local && local->port.neighbors[port_idx].is_remote_neighbor &&
            local->port.neighbors[port_idx].remote_primary_cna == (dcna & 0x00ffffffU)) {
            return link;
        }
    }

    return NULL;
}

uint64_t ub_fm_msgq_reg_read(void *opaque, hwaddr addr, unsigned len)
{
    BusControllerState *s = opaque;
    uint64_t val;

    switch (len) {
    case BYTE_SIZE:
        val = ub_get_byte(s->fm_msgq_reg + addr);
        break;
    case WORD_SIZE:
        val = ub_get_word(s->fm_msgq_reg + addr);
        break;
    case DWORD_SIZE:
        val = ub_get_long(s->fm_msgq_reg + addr);
        break;
    default:
        qemu_log("invalid argument len 0x%x\n", len);
        val = ~0x0;
        break;
    }

    qemu_log("ub_fm_msgq_reg_read addr 0x%lx len 0x%x val 0x%lx\n",
             addr, len, val);
    return val;
}

void ub_fm_msgq_reg_write(void *opaque, hwaddr addr, uint64_t val, unsigned len)
{
    BusControllerState *s = opaque;

    switch (len) {
    case BYTE_SIZE:
        ub_set_byte(s->fm_msgq_reg + addr, val);
        break;
    case WORD_SIZE:
        ub_set_word(s->fm_msgq_reg + addr, val);
        break;
    case DWORD_SIZE:
        ub_set_long(s->fm_msgq_reg + addr, val);
        break;
    default:
        /* As length is under guest control, handle illegal values. */
        qemu_log("invalid argument len 0x%x addr 0x%lx val 0x%lx\n",
                 len, addr, val);
        return;
    }
    qemu_log("ub_fm_msgq_reg_write addr 0x%lx len 0x%x val 0x%lx\n",
             addr, len, val);
}

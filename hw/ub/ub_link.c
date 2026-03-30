/*
 * Minimal standalone UB point-to-point link object.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "glib/gstdio.h"
#include "hw/qdev-core.h"
#include "hw/ub/ub_link.h"
#include "hw/ub/ub.h"
#include "hw/ub/ub_ubc.h"

typedef struct UBLinkPublishedState {
    char *device_id;
    uint32_t port_idx;
    UbGuid guid;
    bool has_bus_instance_guid;
    UbGuid bus_instance_guid;
} UBLinkPublishedState;

static const char *ub_link_shared_dir(void)
{
    const char *dir = g_getenv("UB_FM_SHARED_DIR");

    return (dir && dir[0]) ? dir : "/tmp/ub-qemu-links";
}

static char *ub_link_global_device_id(const char *device_id)
{
    const char *local_node_id = g_getenv("UB_FM_NODE_ID");

    if (!device_id || !device_id[0]) {
        return NULL;
    }
    if (strchr(device_id, '.')) {
        return g_strdup(device_id);
    }
    if (local_node_id && local_node_id[0]) {
        return g_strdup_printf("%s.%s", local_node_id, device_id);
    }
    return g_strdup(device_id);
}

static char *ub_link_sanitize_token(const char *token)
{
    char *out;
    size_t i;

    if (!token) {
        return g_strdup("unknown");
    }

    out = g_strdup(token);
    for (i = 0; out[i]; i++) {
        if (!g_ascii_isalnum(out[i]) && out[i] != '-' && out[i] != '_') {
            out[i] = '_';
        }
    }
    return out;
}

static char *ub_link_endpoint_state_path(const UBLinkEndpointDesc *ep)
{
    g_autofree char *global_id = ub_link_global_device_id(ep->device_id);
    g_autofree char *sanitized = ub_link_sanitize_token(global_id);

    g_mkdir_with_parents(ub_link_shared_dir(), 0755);
    return g_strdup_printf("%s/%s__%u.ini",
                           ub_link_shared_dir(), sanitized, ep->port_idx);
}

static int ub_link_publish_local_endpoint(const UBLinkEndpointDesc *ep, Error **errp)
{
    g_autoptr(GKeyFile) keyfile = NULL;
    g_autofree char *path = NULL;
    g_autofree char *tmp_path = NULL;
    g_autofree char *guid_str = NULL;
    g_autofree char *data = NULL;
    gsize data_len = 0;
    GError *gerr = NULL;

    if (!ep || !ep->device) {
        qemu_log("ub_link: skip publish for unresolved local endpoint %s:%u\n",
                 ep ? ep->device_id : "<null>", ep ? ep->port_idx : 0);
        return 0;
    }

    path = ub_link_endpoint_state_path(ep);
    tmp_path = g_strdup_printf("%s.tmp-%d", path, (int)getpid());
    guid_str = g_malloc0(UB_DEV_GUID_STRING_LENGTH + 1);
    ub_device_get_str_from_guid(&ep->device->guid, guid_str, UB_DEV_GUID_STRING_LENGTH + 1);

    keyfile = g_key_file_new();
    g_key_file_set_string(keyfile, "endpoint", "device_id",
                          ub_link_global_device_id(ep->device_id));
    g_key_file_set_uint64(keyfile, "endpoint", "port_idx", ep->port_idx);
    g_key_file_set_string(keyfile, "endpoint", "guid", guid_str);
    if (BUS_CONTROLLER_DEV(ep->device)) {
        g_autofree char *bi_guid_str = g_malloc0(UB_DEV_GUID_STRING_LENGTH + 1);
        BusControllerDev *ubc_dev = BUS_CONTROLLER_DEV(ep->device);

        ub_device_get_str_from_guid(&ubc_dev->bus_instance_guid, bi_guid_str,
                                    UB_DEV_GUID_STRING_LENGTH + 1);
        g_key_file_set_string(keyfile, "endpoint", "bus_instance_guid", bi_guid_str);
    }
    data = g_key_file_to_data(keyfile, &data_len, &gerr);
    if (gerr) {
        error_setg(errp, "ub_link: failed to serialize endpoint state: %s",
                   gerr->message);
        g_error_free(gerr);
        return -1;
    }
    if (!g_file_set_contents(tmp_path, data, data_len, &gerr)) {
        error_setg(errp, "ub_link: failed to write endpoint state %s: %s",
                   tmp_path, gerr->message);
        g_error_free(gerr);
        return -1;
    }
    if (g_rename(tmp_path, path) < 0) {
        error_setg(errp, "ub_link: failed to publish endpoint state %s: %s",
                   path, strerror(errno));
        return -1;
    }
    qemu_log("ub_link: published local endpoint %s:%u -> %s\n",
             ep->device_id, ep->port_idx, path);
    return 0;
}

static void ub_link_remove_local_endpoint_state(const UBLinkEndpointDesc *ep)
{
    g_autofree char *path = NULL;

    if (!ep || !ep->device_id) {
        return;
    }
    path = ub_link_endpoint_state_path(ep);
    g_remove(path);
}

static int ub_link_read_remote_endpoint(const UBLinkEndpointDesc *ep,
                                        UBLinkPublishedState *state,
                                        Error **errp)
{
    g_autoptr(GKeyFile) keyfile = NULL;
    g_autofree char *path = NULL;
    g_autofree char *guid_str = NULL;
    GError *gerr = NULL;
    unsigned int attempt;

    if (!ep || !state) {
        error_setg(errp, "ub_link: invalid remote endpoint read arguments");
        return -1;
    }

    path = ub_link_endpoint_state_path(ep);
    for (attempt = 0; attempt < 20; attempt++) {
        if (g_file_test(path, G_FILE_TEST_EXISTS)) {
            break;
        }
        if (attempt == 0) {
            qemu_log("ub_link: remote endpoint file not found for %s:%u (%s), retrying\n",
                     ep->device_id, ep->port_idx, path);
        }
        g_usleep(100 * 1000);
    }
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        qemu_log("ub_link: remote endpoint still missing for %s:%u (%s)\n",
                 ep->device_id, ep->port_idx, path);
        return 1;
    }

    keyfile = g_key_file_new();
    if (!g_key_file_load_from_file(keyfile, path, G_KEY_FILE_NONE, &gerr)) {
        error_setg(errp, "ub_link: failed to load remote endpoint state %s: %s",
                   path, gerr->message);
        g_error_free(gerr);
        return -1;
    }

    state->device_id = g_key_file_get_string(keyfile, "endpoint", "device_id", &gerr);
    if (gerr) {
        error_setg(errp, "ub_link: invalid remote endpoint file %s: %s",
                   path, gerr->message);
        g_error_free(gerr);
        return -1;
    }
    state->port_idx = g_key_file_get_uint64(keyfile, "endpoint", "port_idx", &gerr);
    if (gerr) {
        error_setg(errp, "ub_link: invalid remote endpoint file %s: %s",
                   path, gerr->message);
        g_error_free(gerr);
        return -1;
    }
    guid_str = g_key_file_get_string(keyfile, "endpoint", "guid", &gerr);
    if (gerr) {
        error_setg(errp, "ub_link: invalid remote endpoint file %s: %s",
                   path, gerr->message);
        g_error_free(gerr);
        return -1;
    }
    if (!ub_device_get_guid_from_str(&state->guid, guid_str)) {
        error_setg(errp, "ub_link: invalid remote guid string in %s", path);
        return -1;
    }
    guid_str = g_key_file_get_string(keyfile, "endpoint", "bus_instance_guid", NULL);
    if (guid_str && guid_str[0] &&
        ub_device_get_guid_from_str(&state->bus_instance_guid, guid_str)) {
        state->has_bus_instance_guid = true;
    }
    return 0;
}

static void ub_link_published_state_clear(UBLinkPublishedState *state)
{
    if (!state) {
        return;
    }
    g_free(state->device_id);
    memset(state, 0, sizeof(*state));
}

static int ub_link_apply_remote_bridge(UBLinkState *s, Error **errp)
{
    UBLinkEndpointDesc *local = NULL;
    UBLinkEndpointDesc *remote = NULL;
    UBLinkPublishedState remote_state = { 0 };
    int ret;

    if (s->a.device && !s->b.device) {
        local = &s->a;
        remote = &s->b;
    } else if (s->b.device && !s->a.device) {
        local = &s->b;
        remote = &s->a;
    } else {
        qemu_log("ub_link: bridge unresolved because local/remote split unavailable "
                 "(a=%s:%u resolved=%d, b=%s:%u resolved=%d)\n",
                 s->a.device_id, s->a.port_idx, !!s->a.device,
                 s->b.device_id, s->b.port_idx, !!s->b.device);
        s->pending = true;
        return 1;
    }

    if (ub_link_publish_local_endpoint(local, errp) < 0) {
        return -1;
    }

    ret = ub_link_read_remote_endpoint(remote, &remote_state, errp);
    if (ret < 0) {
        return -1;
    }
    if (ret > 0) {
        if (s->remote_applied) {
            if (ub_disconnect_device_port_remote(local->device, local->port_idx,
                                                 ub_link_global_device_id(remote->device_id),
                                                 remote->port_idx, errp) < 0) {
                ub_link_published_state_clear(&remote_state);
                return -1;
            }
            s->remote_applied = false;
        }
        s->pending = true;
        qemu_log("ub_link: pending remote bridge %s:%u <-> %s:%u\n",
                 s->a.device_id, s->a.port_idx,
                 s->b.device_id, s->b.port_idx);
        ub_link_published_state_clear(&remote_state);
        return 1;
    }

    if (ub_connect_device_port_remote(local->device, local->port_idx,
                                      remote_state.device_id,
                                      &remote_state.guid,
                                      remote_state.port_idx, errp) < 0) {
        ub_link_published_state_clear(&remote_state);
        return -1;
    }
    qemu_log("ub_link: remote cfg path after connect %s:%u <-> %s:%u\n",
             local->device_id, local->port_idx,
             remote->device_id, remote->port_idx);
    if (remote_state.has_bus_instance_guid) {
        NeighborInfo *neighbor = &local->device->port.neighbors[local->port_idx];

        neighbor->remote_bus_instance_guid = remote_state.bus_instance_guid;
        neighbor->remote_bus_instance_guid_valid = true;
        neighbor->remote_cfg_notify_sent = false;
        neighbor->remote_cfg_notify_attempts = 0;
        neighbor->remote_cfg_notify_next_retry_ms = 0;
        qemu_log("ub_link: remote bus instance guid captured for %s:%u\n",
                 local->device_id, local->port_idx);
    }
    if (object_dynamic_cast(OBJECT(local->device), TYPE_BUS_CONTROLLER_DEV)) {
        UBRemoteDeviceSnapshot remote_snapshot = { 0 };

        qemu_log("ub_link: remote snapshot load start for %s:%u\n",
                 local->device_id, local->port_idx);
        if (ub_load_remote_device_snapshot_by_guid(&remote_state.guid,
                                                   &remote_snapshot, NULL)) {
            ub_set_cluster_peer_cfg(local->device, remote_snapshot.eid,
                                    remote_snapshot.upi,
                                    remote_snapshot.fm_cna);
            qemu_log("ub_link: remote snapshot load done for %s:%u eid=%u upi=%u fm_cna=%#x\n",
                     local->device_id, local->port_idx,
                     remote_snapshot.eid, remote_snapshot.upi,
                     remote_snapshot.fm_cna);
        }
    }
    if (object_dynamic_cast(OBJECT(local->device), TYPE_BUS_CONTROLLER_DEV) &&
        remote_state.has_bus_instance_guid) {
        BusControllerState *ubc = container_of_ubbus(
            UB_BUS(qdev_get_parent_bus(DEVICE(local->device))));

        qemu_log("ub_link: remote cfg notify start for %s:%u\n",
                 local->device_id, local->port_idx);
        if (ub_inject_remote_cfg_cpl_notify(ubc, &remote_state.bus_instance_guid,
                                            errp) < 0) {
            ub_link_published_state_clear(&remote_state);
            return -1;
        }
        qemu_log("ub_link: remote cfg notify done for %s:%u\n",
                 local->device_id, local->port_idx);
    }

    s->remote_applied = true;
    s->pending = false;
    s->applied = true;
    qemu_log("ub_link: applied remote bridge %s:%u <-> %s:%u via remote guid\n",
             s->a.device_id, s->a.port_idx,
             s->b.device_id, s->b.port_idx);
    ub_link_published_state_clear(&remote_state);
    return 0;
}

static void ub_link_finalize(Object *obj)
{
    UBLinkState *s = UB_LINK(obj);

    g_free(s->a.device_id);
    g_free(s->b.device_id);
}

void ub_link_configure(UBLinkState *s, const char *a_device_id, uint32_t a_port_idx,
                       const char *b_device_id, uint32_t b_port_idx, bool link_up)
{
    if (!s) {
        return;
    }

    g_free(s->a.device_id);
    g_free(s->b.device_id);
    s->a.device_id = g_strdup(a_device_id);
    s->a.port_idx = a_port_idx;
    s->a.device = NULL;
    s->b.device_id = g_strdup(b_device_id);
    s->b.port_idx = b_port_idx;
    s->b.device = NULL;
    s->link_up = link_up;
    s->attached = false;
    s->applied = false;
    s->pending = false;
    s->remote_applied = false;
    qemu_log("ub_link: configure %s:%u <-> %s:%u up=%d\n",
             s->a.device_id, s->a.port_idx,
             s->b.device_id, s->b.port_idx, s->link_up);
}

int ub_link_attach_endpoints(UBLinkState *s, Error **errp)
{
    if (!s) {
        error_setg(errp, "ub_link: state is null");
        return -1;
    }
    if (!s->a.device_id || !s->b.device_id) {
        error_setg(errp, "ub_link: endpoints are not configured");
        return -1;
    }

    s->a.device = ub_find_device_by_id(s->a.device_id);
    s->b.device = ub_find_device_by_id(s->b.device_id);
    if (!s->a.device || !s->b.device) {
        qemu_log("ub_link: endpoints pending resolution %s:%u <-> %s:%u\n",
                 s->a.device_id, s->a.port_idx,
                 s->b.device_id, s->b.port_idx);
        s->attached = false;
        return 1;
    }

    qemu_log("ub_link: endpoints attached %s:%u <-> %s:%u\n",
             s->a.device_id, s->a.port_idx,
             s->b.device_id, s->b.port_idx);
    s->attached = true;
    return 0;
}

int ub_link_apply(UBLinkState *s, Error **errp)
{
    if (!s) {
        error_setg(errp, "ub_link: state is null");
        return -1;
    }
    s->pending = false;
    if (!s->link_up) {
        return 0;
    }
    if (!s->attached) {
        int ret = ub_link_attach_endpoints(s, errp);

        if (ret < 0) {
            return -1;
        }
        if (ret > 0) {
            return ub_link_apply_remote_bridge(s, errp);
        }
    }

    if (ub_connect_device_ports(s->a.device, s->a.port_idx,
                                s->b.device, s->b.port_idx, errp) < 0) {
        return -1;
    }

    s->applied = true;
    s->remote_applied = false;
    return 0;
}

int ub_link_deactivate(UBLinkState *s, Error **errp)
{
    if (!s) {
        error_setg(errp, "ub_link: state is null");
        return -1;
    }
    if (s->remote_applied) {
        UBLinkEndpointDesc *local = NULL;
        UBLinkEndpointDesc *remote = NULL;
        g_autofree char *remote_global_id = NULL;

        if (s->a.device && !s->b.device) {
            local = &s->a;
            remote = &s->b;
        } else if (s->b.device && !s->a.device) {
            local = &s->b;
            remote = &s->a;
        }
        if (local && remote) {
            remote_global_id = ub_link_global_device_id(remote->device_id);
            if (ub_disconnect_device_port_remote(local->device, local->port_idx,
                                                 remote_global_id,
                                                 remote->port_idx, errp) < 0) {
                return -1;
            }
        }
    }
    ub_link_remove_local_endpoint_state(&s->a);
    ub_link_remove_local_endpoint_state(&s->b);

    if (!s->attached && !s->remote_applied && !s->applied) {
        s->applied = false;
        s->pending = false;
        s->remote_applied = false;
        return 0;
    }

    if (ub_disconnect_device_ports(s->a.device, s->a.port_idx,
                                   s->b.device, s->b.port_idx, errp) < 0) {
        return -1;
    }

    s->applied = false;
    s->pending = false;
    s->remote_applied = false;
    return 0;
}

void ub_link_detach_endpoints(UBLinkState *s)
{
    if (!s) {
        return;
    }

    s->a.device = NULL;
    s->b.device = NULL;
    s->attached = false;
    s->applied = false;
    s->pending = false;
    s->remote_applied = false;
}

bool ub_link_is_pending(UBLinkState *s)
{
    return s ? s->pending : false;
}

static void ub_link_class_init(ObjectClass *klass, void *data)
{
    object_class_property_add_bool(klass, "configured",
                                   NULL, NULL);
}

static const TypeInfo ub_link_type_info = {
    .name = TYPE_UB_LINK,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(UBLinkState),
    .instance_finalize = ub_link_finalize,
    .class_init = ub_link_class_init,
};

static void ub_link_register_types(void)
{
    type_register_static(&ub_link_type_info);
}

type_init(ub_link_register_types)

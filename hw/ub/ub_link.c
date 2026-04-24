/*
 * Minimal standalone UB point-to-point link object with Socket support and File discovery.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "glib/gstdio.h"
#include "glib/gprintf.h"
#include "hw/qdev-core.h"
#include "hw/ub/ub_common.h"
#include "hw/ub/ub_link.h"
#include "hw/ub/ub.h"
#include "hw/ub/ub_config.h"
#include "hw/ub/ub_ubc.h"
#include "io/channel-socket.h"
#include "qapi/error.h"
#include "qemu/main-loop.h"
#include "qom/object.h"
#include "sysemu/sysemu.h"

typedef struct UBLinkPublishedState {
    char *device_id;
    uint32_t port_idx;
    UbGuid guid;
    uint32_t primary_cna;
    bool bus_instance_guid_valid;
    UbGuid bus_instance_guid;
} UBLinkPublishedState;

static UBDevice *ub_link_try_resolve_device(const char *device_id)
{
    Object *obj;

    if (!device_id) {
        return NULL;
    }
    if (!qdev_get_machine()) {
        return NULL;
    }

    obj = object_resolve_path_component(qdev_get_machine(), device_id);
    if (!obj || !object_dynamic_cast(obj, TYPE_UB_DEVICE)) {
        return NULL;
    }
    return UB_DEVICE(obj);
}

static UBDevice *ub_link_resolve_device(const char *device_id)
{
    UBDevice *dev = ub_link_try_resolve_device(device_id);

    if (!dev) {
        dev = ub_find_device_by_id(device_id);
    }
    return dev;
}

static bool ub_link_device_matches_node(const char *device_id, const char *node_id)
{
    size_t node_len;

    if (!device_id || !node_id) {
        return false;
    }

    node_len = strlen(node_id);
    return strncmp(device_id, node_id, node_len) == 0 &&
           device_id[node_len] == '.';
}

static void ub_link_select_endpoints(UBLinkState *s,
                                     UBLinkEndpointDesc **local,
                                     UBLinkEndpointDesc **remote)
{
    UBDevice *a_dev = ub_link_resolve_device(s->a.device_id);
    UBDevice *b_dev = ub_link_resolve_device(s->b.device_id);
    const char *node_id = g_getenv("UB_FM_NODE_ID");

    s->a.device = a_dev;
    s->b.device = b_dev;

    if (a_dev && !b_dev) {
        *local = &s->a;
        *remote = &s->b;
        return;
    }
    if (b_dev && !a_dev) {
        *local = &s->b;
        *remote = &s->a;
        return;
    }

    /*
     * Fallback for early boot before objects are fully discoverable.
     * Prefer matching the endpoint device_id prefix with the local node id.
     */
    if (ub_link_device_matches_node(s->a.device_id, node_id) &&
        !ub_link_device_matches_node(s->b.device_id, node_id)) {
        *local = &s->a;
        *remote = &s->b;
        return;
    }
    if (ub_link_device_matches_node(s->b.device_id, node_id) &&
        !ub_link_device_matches_node(s->a.device_id, node_id)) {
        *local = &s->b;
        *remote = &s->a;
        return;
    }

    /*
     * Final deterministic fallback: keep endpoint ordering stable across all
     * nodes so both sides derive the same local/remote orientation.
     */
    if (g_strcmp0(s->a.device_id, s->b.device_id) < 0 ||
        (g_strcmp0(s->a.device_id, s->b.device_id) == 0 &&
         s->a.port_idx <= s->b.port_idx)) {
        *local = &s->a;
        *remote = &s->b;
        return;
    }

    *local = &s->b;
    *remote = &s->a;
}

static char *ub_link_shared_dir(void)
{
    const char *env = g_getenv("UB_FM_SHARED_DIR");
    if (env) {
        return g_strdup(env);
    }
    return g_build_filename(g_get_tmp_dir(), "ub-qemu-links", NULL);
}

static char *ub_link_sanitize_token(const char *token)
{
    g_autofree char *s = g_strdup(token);
    char *p = s;
    while (*p) {
        if (!g_ascii_isalnum(*p)) {
            *p = '_';
        }
        p++;
    }
    return g_steal_pointer(&s);
}

static char *ub_link_make_endpoint_token(const char *device_id, bool local_scope)
{
    const char *node_id = g_getenv("UB_FM_NODE_ID");
    bool has_node_scope = device_id && strchr(device_id, '.') != NULL;

    if (local_scope && node_id && device_id && !has_node_scope) {
        return g_strdup_printf("%s_%s", node_id, device_id);
    }
    if (device_id) {
        return g_strdup(device_id);
    }
    return g_strdup("unknown");
}

char *ub_link_endpoint_path(const char *device_id, uint32_t port_idx, bool local_scope)
{
    g_autofree char *token = ub_link_make_endpoint_token(device_id, local_scope);
    g_autofree char *sanitized = ub_link_sanitize_token(token);
    g_autofree char *dir = ub_link_shared_dir();
    g_mkdir_with_parents(dir, 0755);
    return g_strdup_printf("%s/%s__%u.ini", dir, sanitized, port_idx);
}

static void ub_link_published_state_save(UBLinkState *s)
{
    UBLinkEndpointDesc *local = NULL;
    UBLinkEndpointDesc *remote = NULL;
    UBDevice *local_dev;
    g_autofree char *path = NULL;
    GKeyFile *keyfile = NULL;
    char guid_str[UB_DEV_GUID_STRING_LENGTH + 1];

    ub_link_select_endpoints(s, &local, &remote);
    (void)remote;

    if (!local->device) {
        local->device = ub_link_resolve_device(local->device_id);
    }
    local_dev = local->device;
    if (!local_dev) {
        qemu_log("ub_link: local device %s not found yet, deferring publish\n",
                 local->device_id);
        return;
    }

    path = ub_link_endpoint_path(local->device_id, local->port_idx, true);
    keyfile = g_key_file_new();

    ub_device_get_str_from_guid(&local_dev->guid, guid_str, sizeof(guid_str));

    g_key_file_set_string(keyfile, "endpoint", "device_id", local->device_id);
    g_key_file_set_uint64(keyfile, "endpoint", "port_idx", local->port_idx);
    g_key_file_set_string(keyfile, "endpoint", "guid", guid_str);
    g_key_file_set_uint64(keyfile, "endpoint", "primary_cna", local_dev->cna);

    g_autofree char *data = g_key_file_to_data(keyfile, NULL, NULL);
    g_file_set_contents(path, data, -1, NULL);
    g_key_file_free(keyfile);
    qemu_log("ub_link: published endpoint info to %s\n", path);
}

static char *ub_link_socket_path(const char *device_id, uint32_t port_idx, bool local_scope)
{
    g_autofree char *token = ub_link_make_endpoint_token(device_id, local_scope);
    g_autofree char *sanitized = ub_link_sanitize_token(token);
    g_autofree char *dir = ub_link_shared_dir();

    g_mkdir_with_parents(dir, 0755);
    char *path = g_strdup_printf("%s/%s__%u.sock", dir, sanitized, port_idx);

    if (strlen(path) >= 100) {
        uint32_t hash = g_str_hash(path);
        g_free(path);
        path = g_strdup_printf("%s/ub_%x.sock", dir, hash);
    }
    return path;
}

/* Read remote endpoint info from shared directory and update device config space */
static void ub_link_load_remote_endpoint_and_connect(UBLinkState *s,
                                                      UBLinkEndpointDesc *local,
                                                      UBLinkEndpointDesc *remote)
{
    g_autofree char *path = NULL;
    g_autoptr(GKeyFile) keyfile = NULL;
    g_autoptr(GError) gerr = NULL;
    g_autofree char *remote_guid_str = NULL;
    UbGuid remote_guid;
    UBDevice *local_dev;
    uint64_t primary_cna;

    if (!remote->device_id || !remote->device_id[0]) {
        return;
    }

    /* Try to find local device */
    if (local->device) {
        local_dev = local->device;
    } else {
        local_dev = ub_find_device_by_id(local->device_id);
        if (!local_dev) {
            local_dev = ub_link_try_resolve_device(local->device_id);
        }
        local->device = local_dev;
    }

    if (!local_dev) {
        qemu_log("ub_link: local device %s not found, cannot configure remote link\n",
                 local->device_id);
        return;
    }

    /* Read remote endpoint info from shared directory */
    path = ub_link_endpoint_path(remote->device_id, remote->port_idx, false);
    keyfile = g_key_file_new();
    if (!g_key_file_load_from_file(keyfile, path, G_KEY_FILE_NONE, &gerr)) {
        qemu_log("ub_link: failed to load remote endpoint info from %s: %s\n",
                 path, gerr ? gerr->message : "unknown");
        return;
    }

    /* Get remote GUID */
    remote_guid_str = g_key_file_get_string(keyfile, "endpoint", "guid", &gerr);
    if (!remote_guid_str || gerr) {
        qemu_log("ub_link: remote endpoint %s missing guid\n", path);
        return;
    }

    /* Parse GUID string */
    if (!ub_device_get_guid_from_str(&remote_guid, remote_guid_str)) {
        qemu_log("ub_link: failed to parse remote guid %s\n", remote_guid_str);
        return;
    }

    /* Get primary CNA */
    primary_cna = g_key_file_get_uint64(keyfile, "endpoint", "primary_cna", NULL);

    /* Update device config space with remote endpoint info */
    if (ub_connect_device_port_remote(local_dev, local->port_idx,
                                       remote->device_id, &remote_guid,
                                       remote->port_idx, NULL) == 0) {
        char guid_str[UB_DEV_GUID_STRING_LENGTH + 1];
        ub_device_get_str_from_guid(&remote_guid, guid_str, sizeof(guid_str));
        qemu_log("ub_link: configured remote link %s:%u -> %s:%u guid=%s cna=0x%llx\n",
                 local->device_id, local->port_idx,
                 remote->device_id, remote->port_idx,
                 guid_str, (unsigned long long)primary_cna);

        /* Update state - remote GUID is now valid */
        s->remote_guid_valid = true;
        if (s->socket_connected && s->remote_guid_valid) {
            s->state = UB_LINK_STATE_READY;
        }
        /* Re-evaluate snapshot reconciliation now that remote_guid_valid is set */
        s->snapshot_reconciled = (s->socket_connected && s->remote_guid_valid);
        ub_link_update_status_file(s);
    }
}

static void ub_link_accept(QIONetListener *listener, QIOChannelSocket *cioc, gpointer opaque)
{
    UBLinkState *s = UB_LINK(opaque);
    UBLinkEndpointDesc *local = NULL;
    UBLinkEndpointDesc *remote = NULL;

    ub_link_select_endpoints(s, &local, &remote);
    if (s->ioc) {
        object_unref(OBJECT(s->ioc));
    }
    s->ioc = QIO_CHANNEL(cioc);
    object_ref(OBJECT(s->ioc));
    qio_channel_set_blocking(s->ioc, false, NULL);
    s->link_up = true;
    qemu_log("ub_link: accepted connection for %s:%u\n",
             local->device_id, local->port_idx);

    /* Mark as connected and update status */
    ub_link_mark_connected(s);

    /* Configure remote link now that connection is established */
    ub_link_load_remote_endpoint_and_connect(s, local, remote);
}

static void ub_link_setup_socket(UBLinkState *s, bool is_server)
{
    UBLinkEndpointDesc *local = NULL;
    UBLinkEndpointDesc *remote = NULL;
    g_autofree char *path = NULL;
    SocketAddress addr = {
        .type = SOCKET_ADDRESS_TYPE_UNIX,
        .u.q_unix.path = NULL,
    };
    Error *local_err = NULL;

    ub_link_select_endpoints(s, &local, &remote);
    path = ub_link_socket_path(local->device_id, local->port_idx, true);
    addr.u.q_unix.path = path;

    if (is_server) {
        if (s->lioc || s->ioc || s->socket_connected) {
            qemu_log("ub_link: server socket already active for %s:%u, skip setup\n",
                     local->device_id, local->port_idx);
            return;
        }
        unlink(path);
        QIONetListener *listener = qio_net_listener_new();
        if (qio_net_listener_open_sync(listener, &addr, 1, &local_err) < 0) {
            qemu_log("ub_link: server listen failed on %s: %s\n",
                     path, local_err ? error_get_pretty(local_err) : "unknown");
            error_free(local_err);
            object_unref(OBJECT(listener));
            return;
        }
        s->lioc = listener;
        qio_net_listener_set_client_func(listener, ub_link_accept, s, NULL);
        qemu_log("ub_link: server listening on %s for %s:%u\n",
                 path, local->device_id, local->port_idx);
    } else {
        if (s->ioc || s->socket_connected) {
            qemu_log("ub_link: client socket already active for %s:%u, skip setup\n",
                     local->device_id, local->port_idx);
            return;
        }
        /* Always publish our endpoint so the other node can eventually find us */
        ub_link_published_state_save(s);

        /* Connect to remote server endpoint. */
        g_free(path);
        path = ub_link_socket_path(remote->device_id, remote->port_idx, false);
        addr.u.q_unix.path = path;
        
        QIOChannelSocket *sioc = qio_channel_socket_new();
        for (int i = 0; i < 10; i++) {
            if (qio_channel_socket_connect_sync(sioc, &addr, &local_err) == 0) {
                s->ioc = QIO_CHANNEL(sioc);
                qio_channel_set_blocking(s->ioc, false, NULL);
                s->link_up = true;
                qemu_log("ub_link: connected to remote server %s\n", path);
                /* Mark as connected and update status */
                ub_link_mark_connected(s);
                /* Configure remote link now that connection is established */
                ub_link_load_remote_endpoint_and_connect(s, local, remote);
                return;
            }
            error_free(local_err);
            local_err = NULL;
            g_usleep(500000);
        }
        qemu_log("ub_link: failed to connect remote server %s after retries\n", path);
        object_unref(OBJECT(sioc));
    }
}

int ub_link_write_message(UBLinkState *s, const void *buf, size_t len, Error **errp)
{
    UBLinkEndpointDesc *local = NULL;
    UBLinkEndpointDesc *remote = NULL;
    bool is_server;
    uint32_t plen = cpu_to_le32((uint32_t)len);

    if (!s || !s->ioc) {
        error_setg(errp, "ub_link: socket is not connected");
        return -1;
    }
    if (qio_channel_write_all(s->ioc, (const char *)&plen, sizeof(plen), errp) < 0) {
        qio_channel_close(s->ioc, NULL);
        object_unref(OBJECT(s->ioc));
        s->ioc = NULL;
        s->link_up = false;
        s->socket_connected = false;
        s->remote_guid_valid = false;
        s->snapshot_reconciled = false;
        s->applied = false;
        s->remote_applied = false;
        s->state = UB_LINK_STATE_PENDING;
        ub_link_update_status_file(s);
        ub_link_select_endpoints(s, &local, &remote);
        is_server = (local == &s->a);
        ub_link_setup_socket(s, is_server);
        return -1;
    }
    if (qio_channel_write_all(s->ioc, (const char *)buf, len, errp) < 0) {
        qio_channel_close(s->ioc, NULL);
        object_unref(OBJECT(s->ioc));
        s->ioc = NULL;
        s->link_up = false;
        s->socket_connected = false;
        s->remote_guid_valid = false;
        s->snapshot_reconciled = false;
        s->applied = false;
        s->remote_applied = false;
        s->state = UB_LINK_STATE_PENDING;
        ub_link_update_status_file(s);
        ub_link_select_endpoints(s, &local, &remote);
        is_server = (local == &s->a);
        ub_link_setup_socket(s, is_server);
        return -1;
    }
    return 0;
}

static bool ub_link_ensure_rx_capacity(UBLinkState *s, size_t need, Error **errp)
{
    size_t cap = s->rx_buf_cap ? s->rx_buf_cap : 4096;

    if (need > UB_LINK_RX_BUF_MAX + sizeof(uint32_t)) {
        error_setg(errp, "ub_link: rx buffer need %zu exceeds max", need);
        return false;
    }

    while (cap < need) {
        cap <<= 1;
    }

    if (cap != s->rx_buf_cap) {
        uint8_t *new_buf = g_realloc(s->rx_buf, cap);
        if (!new_buf) {
            error_setg(errp, "ub_link: rx buffer realloc failed");
            return false;
        }
        s->rx_buf = new_buf;
        s->rx_buf_cap = cap;
    }

    return true;
}

int ub_link_read_message(UBLinkState *s, void **buf, size_t *len, Error **errp)
{
    uint8_t tmp[4096];
    ssize_t ret;

    if (!s || !s->ioc) {
        return 0;
    }

    if (!s->rx_buf && !ub_link_ensure_rx_capacity(s, sizeof(tmp), errp)) {
        return -1;
    }

    /* Non-blocking stream read: accumulate as much as available this turn. */
    for (;;) {
        ret = qio_channel_read(s->ioc, (char *)tmp, sizeof(tmp), NULL);
        if (ret <= 0) {
            break;
        }

        if (!ub_link_ensure_rx_capacity(s, s->rx_buf_used + (size_t)ret, errp)) {
            s->rx_buf_used = 0;
            return -1;
        }

        memcpy(s->rx_buf + s->rx_buf_used, tmp, (size_t)ret);
        s->rx_buf_used += (size_t)ret;
    }

    if (s->rx_buf_used < sizeof(uint32_t)) {
        return 0;
    }

    uint32_t plen_le;
    uint32_t plen;
    size_t frame_len;
    memcpy(&plen_le, s->rx_buf, sizeof(plen_le));
    plen = le32_to_cpu(plen_le);
    if (plen == 0 || plen > UB_LINK_RX_BUF_MAX) {
        error_setg(errp, "ub_link: invalid frame length %u", plen);
        s->rx_buf_used = 0;
        return -1;
    }

    frame_len = sizeof(uint32_t) + (size_t)plen;
    if (s->rx_buf_used < frame_len) {
        return 0;
    }

    *buf = g_malloc((size_t)plen);
    memcpy(*buf, s->rx_buf + sizeof(uint32_t), (size_t)plen);
    *len = (size_t)plen;
    s->rx_buf_used -= frame_len;
    if (s->rx_buf_used > 0) {
        memmove(s->rx_buf, s->rx_buf + frame_len, s->rx_buf_used);
    }

    return 1;
}

static char *ub_link_kick_path(const char *device_id, uint32_t port_idx, bool local_scope)
{
    g_autofree char *token = ub_link_make_endpoint_token(device_id, local_scope);
    g_autofree char *sanitized = ub_link_sanitize_token(token);
    g_autofree char *dir = ub_link_shared_dir();
    g_mkdir_with_parents(dir, 0755);
    return g_strdup_printf("%s/%s__%u.kick", dir, sanitized, port_idx);
}

int ub_link_poll_kick(UBLinkState *s)
{
    UBLinkEndpointDesc *local = NULL;
    UBLinkEndpointDesc *remote = NULL;

    if (!s) return 0;
    
    /* Connected socket itself is the kick source; do not consume stream bytes here. */
    if (s->ioc) {
        return 1;
    }

    /* 2. Check file-based kick (fallback for orchestration scripts) */
    ub_link_select_endpoints(s, &local, &remote);
    (void)remote;
    g_autofree char *path = ub_link_kick_path(local->device_id, local->port_idx, true);
    if (g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_remove(path);
        return 1;
    }
    
    return 0;
}

int ub_link_kick_remote(UBLinkState *s, Error **errp)
{
    UBLinkEndpointDesc *local = NULL;
    UBLinkEndpointDesc *remote = NULL;
    g_autofree char *path = NULL;
    int fd;

    if (!s || !s->ioc) {
        return 0;
    }

    /* Write a kick file on the remote endpoint's shared directory.
     * The peer QEMU will detect this file in ub_link_poll_kick
     * and process incoming data from the socket. */
    ub_link_select_endpoints(s, &local, &remote);
    if (!remote || !remote->device_id) {
        return 0;
    }

    path = ub_link_kick_path(remote->device_id, remote->port_idx, false);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        const char marker[] = "1";
        (void)write(fd, marker, sizeof(marker));
        close(fd);
    }

    return 0;
}

/* Generate status file path for a link */
char *ub_link_status_path(const char *device_id, uint32_t port_idx)
{
    g_autofree char *token = ub_link_make_endpoint_token(device_id, true);
    g_autofree char *sanitized = ub_link_sanitize_token(token);
    g_autofree char *dir = ub_link_shared_dir();
    g_mkdir_with_parents(dir, 0755);
    return g_strdup_printf("%s/%s__%u.status", dir, sanitized, port_idx);
}

/* Update status file with current link state */
int ub_link_update_status_file(UBLinkState *s)
{
    g_autofree char *path = NULL;
    g_autoptr(GKeyFile) keyfile = NULL;
    g_autofree char *data = NULL;
    UBLinkEndpointDesc *local = NULL;
    UBLinkEndpointDesc *remote = NULL;
    const char *device_id = NULL;
    uint32_t port_idx = 0;

    if (!s) {
        return -1;
    }

    ub_link_select_endpoints(s, &local, &remote);
    (void)remote;

    /* Try to get device_id from resolved endpoint, fallback to descriptor */
    if (local && local->device_id) {
        device_id = local->device_id;
        port_idx = local->port_idx;
    } else if (s->a.device_id) {
        device_id = s->a.device_id;
        port_idx = s->a.port_idx;
    } else if (s->b.device_id) {
        device_id = s->b.device_id;
        port_idx = s->b.port_idx;
    } else {
        qemu_log("ub_link: cannot determine device_id for status file\n");
        return -1;
    }

    path = ub_link_status_path(device_id, port_idx);
    keyfile = g_key_file_new();

    /* Write current state */
    g_key_file_set_string(keyfile, "status", "state",
                          s->state == UB_LINK_STATE_READY ? "READY" :
                          s->state == UB_LINK_STATE_FAILED ? "FAILED" : "PENDING");
    g_key_file_set_boolean(keyfile, "status", "socket_connected", s->socket_connected);
    g_key_file_set_boolean(keyfile, "status", "remote_guid_valid", s->remote_guid_valid);
    g_key_file_set_uint64(keyfile, "status", "reconcile_ts_ms", s->reconcile_ts_ms);
    if (s->last_error) {
        g_key_file_set_string(keyfile, "status", "last_error", s->last_error);
    }

    data = g_key_file_to_data(keyfile, NULL, NULL);
    if (!g_file_set_contents(path, data, -1, NULL)) {
        qemu_log("ub_link: failed to write status file %s\n", path);
        return -1;
    }

    if (!s->status_file_path) {
        s->status_file_path = g_strdup(path);
    }

    return 0;
}

/* Mark link as connected and update status */
void ub_link_mark_connected(UBLinkState *s)
{
    if (!s) {
        return;
    }

    qemu_log("ub_link: mark_connected %s:%u <-> %s:%u\n",
             s->a.device_id, s->a.port_idx, s->b.device_id, s->b.port_idx);

    s->socket_connected = true;
    s->reconcile_ts_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);

    /* Check if remote GUID is valid */
    UBLinkEndpointDesc *local = NULL;
    UBLinkEndpointDesc *remote = NULL;
    UBDevice *local_dev = NULL;

    ub_link_select_endpoints(s, &local, &remote);

    if (local->device) {
        local_dev = local->device;
    } else {
        local_dev = ub_link_try_resolve_device(local->device_id);
        if (local_dev) {
            local->device = local_dev;
        }
    }

    if (local_dev) {
        uint64_t offset = UB_PORT_SLICE_START + local->port_idx * UB_PORT_SZ;
        uint64_t emulated_offset = ub_cfg_offset_to_emulated_offset(offset, true);
        ConfigPortBasic *port_basic = (ConfigPortBasic *)(local_dev->config + emulated_offset);

        if (ub_guid_initialized(&port_basic->neighbor_port_info.neighbot_port_guid)) {
            s->remote_guid_valid = true;
            qemu_log("ub_link: remote guid valid for %s:%u\n", local->device_id, local->port_idx);
        } else {
            qemu_log("ub_link: remote guid NOT YET initialized for %s:%u\n", local->device_id, local->port_idx);
        }
    } else {
        qemu_log("ub_link: local device %s not yet resolved during mark_connected\n", local->device_id);
    }

    /* Update state based on Ready Contract */
    bool was_ready = (s->state == UB_LINK_STATE_READY);
    if (s->socket_connected && s->remote_guid_valid) {
        if (!was_ready) {
            s->state_set_ts_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);  /* M1: Track state change time */
        }
        s->state = UB_LINK_STATE_READY;
    } else {
        s->state = UB_LINK_STATE_PENDING;
    }

    /* M1: Snapshot reconciliation - mark as reconciled when both ends are connected */
    s->snapshot_reconciled = (s->socket_connected && s->remote_guid_valid);

    ub_link_update_status_file(s);
    qemu_log("ub_link: marked connected for %s:%u state=%d socket=%d guid_valid=%d snapshot_reconciled=%d\n",
             local->device_id, local->port_idx, s->state,
             s->socket_connected, s->remote_guid_valid, s->snapshot_reconciled);

    /* If we are connected but guid is not yet valid, we might be in a race.
     * Force a reload of the remote endpoint info. */
    if (s->socket_connected && !s->remote_guid_valid) {
        ub_link_load_remote_endpoint_and_connect(s, local, remote);
    }
}

/* Mark link as failed and update status */
void ub_link_mark_failed(UBLinkState *s, const char *reason)
{
    if (!s) {
        return;
    }

    s->state = UB_LINK_STATE_FAILED;
    if (s->last_error) {
        g_free(s->last_error);
    }
    s->last_error = g_strdup(reason);

    ub_link_update_status_file(s);

    UBLinkEndpointDesc *local = NULL;
    UBLinkEndpointDesc *remote = NULL;
    ub_link_select_endpoints(s, &local, &remote);
    (void)remote;

    qemu_log("ub_link: marked failed for %s:%u reason=%s\n",
             local ? local->device_id : "unknown", local ? local->port_idx : 0, reason);
}

/* Check if link is ready according to Ready Contract */
bool ub_link_is_ready(UBLinkState *s)
{
    if (!s) {
        return false;
    }

    return s->state == UB_LINK_STATE_READY &&
           s->socket_connected &&
           s->remote_guid_valid;
}

int ub_link_deactivate(UBLinkState *s, Error **errp)
{
    if (s->ioc) {
        qio_channel_close(s->ioc, NULL);
        object_unref(OBJECT(s->ioc));
        s->ioc = NULL;
    }
    if (s->lioc) {
        qio_net_listener_disconnect(s->lioc);
        object_unref(OBJECT(s->lioc));
        s->lioc = NULL;
    }
    s->link_up = false;
    s->applied = false;
    s->socket_connected = false;
    s->remote_guid_valid = false;
    s->state = UB_LINK_STATE_PENDING;

    /* Clean up status file */
    if (s->status_file_path) {
        g_unlink(s->status_file_path);
        g_free(s->status_file_path);
        s->status_file_path = NULL;
    }

    return 0;
}

void ub_link_detach_endpoints(UBLinkState *s)
{
    s->a.device = NULL;
    s->b.device = NULL;
    s->attached = false;
}

bool ub_link_is_pending(UBLinkState *s)
{
    return !s->applied;
}

int ub_link_attach_endpoints(UBLinkState *s, Error **errp)
{
    s->attached = true;
    ub_link_published_state_save(s);
    return 0;
}

static void ub_link_finalize(Object *obj)
{
    UBLinkState *s = UB_LINK(obj);
    ub_link_deactivate(s, NULL);
    g_free(s->a.device_id);
    g_free(s->b.device_id);
}

void ub_link_configure(UBLinkState *s, const char *a_device_id, uint32_t a_port_idx,
                       const char *b_device_id, uint32_t b_port_idx, bool link_up)
{
    s->a.device_id = g_strdup(a_device_id);
    s->a.port_idx = a_port_idx;
    s->b.device_id = g_strdup(b_device_id);
    s->b.port_idx = b_port_idx;
    s->link_up = link_up;

    /* M1: Initialize Ready Contract fields */
    s->socket_connected = false;
    s->remote_guid_valid = false;
    s->snapshot_reconciled = false;
    s->reconcile_ts_ms = 0;
    s->state_set_ts_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);

    /* Publish our endpoint immediately so others can find us */
    ub_link_published_state_save(s);
}

int ub_link_apply(UBLinkState *s, Error **errp)
{
    UBLinkEndpointDesc *local = NULL;
    UBLinkEndpointDesc *remote = NULL;
    bool is_server;

    if (s->applied) {
        /* Already applied, check if still ready */
        if (s->state == UB_LINK_STATE_READY) {
            return 0;
        } else if (s->state == UB_LINK_STATE_FAILED) {
            return -1;
        } else {
            return 1; /* Still pending */
        }
    }

    /* Ensure our info is out there */
    ub_link_published_state_save(s);

    ub_link_select_endpoints(s, &local, &remote);
    is_server = (local == &s->a);
    ub_link_setup_socket(s, is_server);

    /* Check Ready Contract conditions before marking applied */
    uint64_t now_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    bool state_age_ok = (now_ms - s->state_set_ts_ms) >= 200;  /* M1: 200ms stability check */

    qemu_log("ub_link: apply %s:%u <-> %s:%u (sock=%d guid=%d snap=%d age=%d ms_ago=%lu)\n",
             s->a.device_id, s->a.port_idx, s->b.device_id, s->b.port_idx,
             s->socket_connected, s->remote_guid_valid, s->snapshot_reconciled,
             state_age_ok, (unsigned long)(now_ms - s->state_set_ts_ms));

    if (s->socket_connected && s->remote_guid_valid &&
        s->snapshot_reconciled && state_age_ok) {
        /* All 4 Ready Contract conditions met - mark as READY and applied */
        s->state = UB_LINK_STATE_READY;
        s->applied = true;
        s->remote_applied = true;
        ub_link_update_status_file(s);
        return 0;
    } else if (s->socket_connected && s->remote_guid_valid && s->snapshot_reconciled) {
        /* Conditions met but stability window hasn't elapsed — stay PENDING
         * but do NOT reset state if already READY from configure_remote_links. */
        if (s->state != UB_LINK_STATE_READY) {
            s->state = UB_LINK_STATE_PENDING;
        }
        s->applied = false;
        s->remote_applied = false;
        ub_link_update_status_file(s);
        return 1; /* PENDING */
    } else {
        /* Not ready yet - mark as PENDING */
        s->state = UB_LINK_STATE_PENDING;
        s->applied = false;  /* Don't set applied=true until ready */
        s->remote_applied = false;
        ub_link_update_status_file(s);
        return 1; /* PENDING */
    }
}

static void ub_link_class_init(ObjectClass *klass, void *data) {}

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

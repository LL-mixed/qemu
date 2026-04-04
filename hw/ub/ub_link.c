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
#include "hw/ub/ub_common.h"
#include "io/channel-socket.h"
#include "io/net-listener.h"
#include "qapi/qapi-types-sockets.h"
#include "qapi/error.h"
#include "qemu/main-loop.h"

/* Forward declarations for static functions defined later */
static void ub_link_rx_reset(UBLinkState *s);
static void ub_link_init_rx(UBLinkState *s);
static void ub_link_cleanup_rx(UBLinkState *s);
static void ub_link_register_aio_watch(UBLinkState *s);
static gboolean ub_link_socket_readable(QIOChannel *ioc, GIOCondition cond, gpointer opaque);
static int ub_link_recv_frames(UBLinkState *s);

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

static char *ub_link_kick_path(const UBLinkEndpointDesc *ep)
{
    g_autofree char *global_id = ub_link_global_device_id(ep->device_id);
    g_autofree char *sanitized = ub_link_sanitize_token(global_id);

    g_mkdir_with_parents(ub_link_shared_dir(), 0755);
    return g_strdup_printf("%s/%s__%u.kick",
                           ub_link_shared_dir(), sanitized, ep->port_idx);
}

static char *ub_link_socket_path(const UBLinkEndpointDesc *ep)
{
    g_autofree char *global_id = ub_link_global_device_id(ep->device_id);
    g_autofree char *sanitized = ub_link_sanitize_token(global_id);

    g_mkdir_with_parents(ub_link_shared_dir(), 0755);
    return g_strdup_printf("%s/%s__%u.sock",
                           ub_link_shared_dir(), sanitized, ep->port_idx);
}

static void ub_link_accept(QIONetListener *listener, QIOChannelSocket *cioc, gpointer opaque)
{
    UBLinkState *s = UB_LINK(opaque);
    (void)listener;

    /* Clean up previous connection state */
    if (s->aio_watch_id) {
        g_source_remove(s->aio_watch_id);
        s->aio_watch_id = 0;
    }
    ub_link_cleanup_rx(s);

    if (s->ioc) {
        object_unref(OBJECT(s->ioc));
    }
    s->ioc = QIO_CHANNEL(cioc);
    object_ref(OBJECT(s->ioc));
    qio_channel_set_blocking(s->ioc, false, NULL);
    qemu_log("ub_link: accepted incoming ulink connection\n");

    ub_link_init_rx(s);
    ub_link_register_aio_watch(s);
}

/* Initial size for the receive buffer */
#define UB_LINK_RX_BUF_INITIAL  4096

/* ---------- spec-aligned protocol: send ---------- */

static int ub_link_send_packet(UBLinkState *s,
                                const void *pkt, size_t len,
                                Error **errp)
{
    if (!s || !s->ioc) {
        error_setg(errp, "ub_link_send_packet: no connection");
        return -1;
    }
    if (len < UB_LINK_PKT_HDR_SIZE) {
        error_setg(errp, "ub_link_send_packet: packet too short (%zu < %d)",
                   len, UB_LINK_PKT_HDR_SIZE);
        return -1;
    }
    if (qio_channel_write_all(s->ioc, pkt, len, errp) < 0) {
        return -1;
    }
    qemu_log("ub_link: packet tx len=%zu\n", len);
    return 0;
}

/* ---------- spec-aligned protocol: receive state machine ---------- */

/*
 * Drain bytes from the socket into the rx buffer, then walk through
 * complete UB packets and enqueue them in rx_msgq.
 *
 * State machine:
 *   Phase 1: Collect UB_LINK_PKT_HDR_SIZE bytes (fixed header for cfg==6).
 *            Validate ulh.cfg, extract msgetah.plen.
 *   Phase 2: Collect plen bytes of payload.
 *   Phase 3: Enqueue complete MsgPktHeader + payload packet.
 *
 * Returns: -1 on error, 0 on success (may have parsed zero or more packets).
 */
static int ub_link_recv_frames(UBLinkState *s)
{
    ssize_t nr;
    size_t want;
    Error *local_err = NULL;

    if (!s || !s->ioc) {
        return 0;
    }

    for (;;) {
        /* Phase 1: collect fixed header (UB_LINK_PKT_HDR_SIZE bytes) */
        if (!s->rx_hdr_done) {
            size_t have = s->rx_buf_used;
            if (have < UB_LINK_PKT_HDR_SIZE) {
                want = UB_LINK_PKT_HDR_SIZE - have;
                if (s->rx_buf_cap < UB_LINK_PKT_HDR_SIZE) {
                    s->rx_buf_cap = UB_LINK_PKT_HDR_SIZE;
                    s->rx_buf = g_realloc(s->rx_buf, s->rx_buf_cap);
                }
                nr = qio_channel_read(s->ioc,
                                      (char *)s->rx_buf + have,
                                      want, &local_err);
                if (nr <= 0) {
                    if (nr == 0 || nr == QIO_CHANNEL_ERR_BLOCK) {
                        return 0;
                    }
                    qemu_log("ub_link: header read error: %s\n",
                             error_get_pretty(local_err));
                    error_free(local_err);
                    return -1;
                }
                s->rx_buf_used += nr;
                if (s->rx_buf_used < UB_LINK_PKT_HDR_SIZE) {
                    return 0;
                }
            }

            /* Validate LPH from the fixed header */
            MsgPktHeader *hdr = (MsgPktHeader *)s->rx_buf;
            if (hdr->ulh.cfg != UB_CLAN_LINK_CFG) {
                qemu_log("ub_link: invalid LPH.cfg=%u, dropping\n",
                         hdr->ulh.cfg);
                ub_link_rx_reset(s);
                return -1;
            }
            uint32_t plen = hdr->msgetah.plen;
            if (plen > UB_LINK_RX_BUF_MAX - UB_LINK_PKT_HDR_SIZE) {
                qemu_log("ub_link: plen too large %u\n", plen);
                ub_link_rx_reset(s);
                return -1;
            }

            s->rx_plen = (uint16_t)plen;
            s->rx_payload_remaining = plen;
            s->rx_hdr_done = true;
        }

        /* Phase 2: collect payload (rx_plen bytes) */
        if (s->rx_hdr_done && s->rx_payload_remaining > 0) {
            size_t total_needed = UB_LINK_PKT_HDR_SIZE + s->rx_payload_remaining;
            if (s->rx_buf_cap < total_needed) {
                s->rx_buf_cap = MIN(total_needed * 2, UB_LINK_RX_BUF_MAX);
                s->rx_buf = g_realloc(s->rx_buf, s->rx_buf_cap);
            }

            size_t have_after_hdr = s->rx_buf_used - UB_LINK_PKT_HDR_SIZE;
            want = s->rx_payload_remaining -
                   (have_after_hdr > s->rx_payload_remaining
                    ? s->rx_payload_remaining : have_after_hdr);
            if (want > 0) {
                nr = qio_channel_read(s->ioc,
                                      (char *)s->rx_buf + s->rx_buf_used,
                                      want, &local_err);
                if (nr <= 0) {
                    if (nr == QIO_CHANNEL_ERR_BLOCK || nr == 0) {
                        return 0;
                    }
                    qemu_log("ub_link: payload read error: %s\n",
                             error_get_pretty(local_err));
                    error_free(local_err);
                    return -1;
                }
                s->rx_buf_used += nr;
            }

            if (s->rx_buf_used < UB_LINK_PKT_HDR_SIZE + s->rx_payload_remaining) {
                return 0;
            }
        }

        /* Phase 3: complete packet — enqueue */
        if (s->rx_hdr_done &&
            (s->rx_payload_remaining == 0 ||
             s->rx_buf_used >= UB_LINK_PKT_HDR_SIZE + s->rx_payload_remaining)) {
            size_t total_len = UB_LINK_PKT_HDR_SIZE + s->rx_plen;

            UBLinkRxMsg *msg = g_malloc(sizeof(*msg));
            msg->len = total_len;
            msg->data = g_malloc(msg->len);
            memcpy(msg->data, s->rx_buf, msg->len);
            g_queue_push_tail(s->rx_msgq, msg);

            qemu_log("ub_link: packet rx cfg=%u plen=%u total=%zu\n",
                     ((MsgPktHeader *)msg->data)->ulh.cfg,
                     s->rx_plen, msg->len);

            /* Shift past consumed bytes */
            size_t consumed = total_len;
            size_t leftover = s->rx_buf_used - consumed;
            if (leftover > 0) {
                memmove(s->rx_buf, s->rx_buf + consumed, leftover);
            }
            s->rx_buf_used = leftover;

            /* Reset for next packet */
            s->rx_hdr_done = false;
            s->rx_plen = 0;
            s->rx_payload_remaining = 0;

            /* Continue looping to parse more packets from remaining data */
            continue;
        }

        break;
    }
    return 0;
}

/* ---------- AIO integration ---------- */

static gboolean ub_link_socket_readable(QIOChannel *ioc,
                                        GIOCondition cond,
                                        gpointer opaque)
{
    UBLinkState *s = UB_LINK(opaque);

    if (cond & (G_IO_HUP | G_IO_ERR)) {
        qemu_log("ub_link: socket hangup/error\n");
        s->aio_watch_id = 0;
        return G_SOURCE_REMOVE;
    }

    if (ub_link_recv_frames(s) < 0) {
        qemu_log("ub_link: recv error, disabling watch\n");
        s->aio_watch_id = 0;
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

static void ub_link_register_aio_watch(UBLinkState *s)
{
    if (!s || !s->ioc || s->aio_watch_id) {
        return;
    }
    s->aio_watch_id = qio_channel_add_watch(
        s->ioc, G_IO_IN | G_IO_HUP | G_IO_ERR,
        ub_link_socket_readable, s, NULL);
    qemu_log("ub_link: AIO watch registered\n");
}

static void ub_link_setup_socket(UBLinkState *s, bool is_server)
{
    /*
     * Both sides must agree on the same socket path.
     * Since is_server is determined by a_global < b_global, the server
     * always listens on endpoint a's path. The client must connect to
     * the same path (endpoint a's path), not its own endpoint's path.
     */
    SocketAddress addr = {
        .type = SOCKET_ADDRESS_TYPE_UNIX,
        .u.q_unix.path = ub_link_socket_path(&s->a),
    };
    Error *local_err = NULL;

    if (is_server) {
        QIONetListener *listener = qio_net_listener_new();
        qio_net_listener_set_name(listener, "ub-link-listener");
        if (qio_net_listener_open_sync(listener, &addr, 1, &local_err) < 0) {
            qemu_log("ub_link: server listen failed: %s\n",
                     error_get_pretty(local_err));
            error_free(local_err);
            object_unref(OBJECT(listener));
            return;
        }
        s->lioc = listener;
        qio_net_listener_set_client_func(listener, ub_link_accept, s, NULL);
        qemu_log("ub_link: server listening on %s\n", addr.u.q_unix.path);
    } else {
        QIOChannelSocket *sioc = qio_channel_socket_new();
        qio_channel_set_name(QIO_CHANNEL(sioc), "ub-link-client");
        if (qio_channel_socket_connect_sync(sioc, &addr, &local_err) < 0) {
            qemu_log("ub_link: client connect to %s failed: %s\n",
                     addr.u.q_unix.path, error_get_pretty(local_err));
            object_unref(OBJECT(sioc));
            error_free(local_err);
            return;
        }
        s->ioc = QIO_CHANNEL(sioc);
        qio_channel_set_blocking(s->ioc, false, NULL);
        ub_link_init_rx(s);
        ub_link_register_aio_watch(s);
        qemu_log("ub_link: client connected to %s\n", addr.u.q_unix.path);
    }
}

int ub_link_kick_remote(UBLinkState *s, Error **errp)
{
    UBLinkEndpointDesc *remote = NULL;
    g_autofree char *path = NULL;

    if (!s || !s->remote_applied) {
        return 0;
    }

    if (s->a.device && !s->b.device) {
        remote = &s->b;
    } else if (s->b.device && !s->a.device) {
        remote = &s->a;
    }

    if (!remote) {
        return 0;
    }

    path = ub_link_kick_path(remote);
    if (!g_file_set_contents(path, "1", 1, NULL)) {
        error_setg(errp, "ub_link: failed to write kick file %s", path);
        return -1;
    }

    return 0;
}

int ub_link_poll_kick(UBLinkState *s)
{
    UBLinkEndpointDesc *local = NULL;
    g_autofree char *path = NULL;

    if (!s || !s->remote_applied) {
        return 0;
    }

    if (s->a.device && !s->b.device) {
        local = &s->a;
    } else if (s->b.device && !s->a.device) {
        local = &s->b;
    }

    if (!local) {
        return 0;
    }

    path = ub_link_kick_path(local);
    if (g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_remove(path);
        return 1;
    }

    return 0;
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
    if (object_dynamic_cast(OBJECT(ep->device), TYPE_BUS_CONTROLLER_DEV)) {
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

    if (!ep || !state) {
        error_setg(errp, "ub_link: invalid remote endpoint read arguments");
        return -1;
    }

    path = ub_link_endpoint_state_path(ep);
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        qemu_log("ub_link: remote endpoint file not found for %s:%u (%s), pending\n",
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
        neighbor->remote_linkup_notify_sent = false;
        neighbor->remote_linkup_notify_attempts = 0;
        neighbor->remote_linkup_notify_next_retry_ms = 0;
        qemu_log("ub_link: remote bus instance guid captured for %s:%u\n",
                 local->device_id, local->port_idx);
    }
    if (object_dynamic_cast(OBJECT(local->device), TYPE_BUS_CONTROLLER_DEV)) {
        UBRemoteDeviceSnapshot remote_snapshot = { 0 };
        NeighborInfo *neighbor = &local->device->port.neighbors[local->port_idx];

        qemu_log("ub_link: remote snapshot load start for %s:%u\n",
                 local->device_id, local->port_idx);
        if (ub_load_remote_device_snapshot_by_guid(&remote_state.guid,
                                                   &remote_snapshot, NULL)) {
            neighbor->remote_primary_cna = remote_snapshot.primary_cna & 0x00ffffffU;
            neighbor->remote_primary_cna_valid = true;
            ub_set_cluster_peer_cfg(local->device, remote_snapshot.eid,
                                    remote_snapshot.upi,
                                    remote_snapshot.fm_cna);
            qemu_log("ub_link: remote route program start for %s:%u remote_cna=%#x\n",
                     local->device_id, local->port_idx,
                     neighbor->remote_primary_cna);
            ub_program_route_table(local->device);
            (void)ub_publish_device_snapshot(local->device, NULL);
            qemu_log("ub_link: remote route program done for %s:%u\n",
                     local->device_id, local->port_idx);
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
    }

    /* Setup high-performance data plane if not already done */
    if (!s->ioc && !s->lioc) {
        /*
         * Deterministic server/client: use global IDs of both endpoints.
         * Both nodes see the same a/b pair, so the comparison is identical
         * on both sides. The node whose local endpoint matches endpoint 'a'
         * becomes server if a_global < b_global, otherwise client.
         */
        g_autofree char *a_global = ub_link_global_device_id(s->a.device_id);
        g_autofree char *b_global = ub_link_global_device_id(s->b.device_id);
        bool local_is_a = (local == &s->a);
        bool a_is_server = (strcmp(a_global, b_global) < 0);
        bool is_server = local_is_a ? a_is_server : !a_is_server;
        qemu_log("ub_link: socket setup local=%s a_global=%s b_global=%s is_server=%d\n",
                 local->device_id, a_global, b_global, is_server);
        ub_link_setup_socket(s, is_server);
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

static void ub_link_rx_reset(UBLinkState *s)
{
    s->rx_buf_used = 0;
    s->rx_hdr_done = false;
    s->rx_plen = 0;
    s->rx_payload_remaining = 0;
}

static void ub_link_init_rx(UBLinkState *s)
{
    s->rx_buf_cap = UB_LINK_RX_BUF_INITIAL;
    s->rx_buf = g_malloc(s->rx_buf_cap);
    ub_link_rx_reset(s);
    s->rx_msgq = g_queue_new();
}

static void ub_link_cleanup_rx(UBLinkState *s)
{
    if (s->rx_msgq) {
        while (!g_queue_is_empty(s->rx_msgq)) {
            UBLinkRxMsg *msg = g_queue_pop_head(s->rx_msgq);
            if (msg) {
                g_free(msg->data);
                g_free(msg);
            }
        }
        g_queue_free(s->rx_msgq);
        s->rx_msgq = NULL;
    }
    g_free(s->rx_buf);
    s->rx_buf = NULL;
    s->rx_buf_cap = 0;
    s->rx_buf_used = 0;
    s->rx_hdr_done = false;
    s->rx_plen = 0;
    s->rx_payload_remaining = 0;
}

static void ub_link_finalize(Object *obj)
{
    UBLinkState *s = UB_LINK(obj);

    if (s->aio_watch_id) {
        g_source_remove(s->aio_watch_id);
        s->aio_watch_id = 0;
    }
    ub_link_cleanup_rx(s);
    if (s->ioc) {
        object_unref(OBJECT(s->ioc));
    }
    if (s->lioc) {
        object_unref(OBJECT(s->lioc));
    }
    g_free(s->socket_path);
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
            /* One side is remote, try to apply the bridge */
            return ub_link_apply_remote_bridge(s, errp);
        }
    }

    /* If we get here, both endpoints are local */
    if (ub_link_publish_local_endpoint(&s->a, errp) < 0) {
        return -1;
    }
    if (ub_link_publish_local_endpoint(&s->b, errp) < 0) {
        return -1;
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

static char *ub_link_message_path(const UBLinkEndpointDesc *ep)
{
    g_autofree char *global_id = ub_link_global_device_id(ep->device_id);
    g_autofree char *sanitized = ub_link_sanitize_token(global_id);

    g_mkdir_with_parents(ub_link_shared_dir(), 0755);
    return g_strdup_printf("%s/%s__%u.msg",
                           ub_link_shared_dir(), sanitized, ep->port_idx);
}

int ub_link_write_message(UBLinkState *s, const void *buf, size_t len, Error **errp)
{
    UBLinkEndpointDesc *remote = NULL;
    g_autofree char *path = NULL;

    if (!s || !s->remote_applied) {
        return 0;
    }

    /* Try spec-aligned socket protocol first */
    if (s->ioc) {
        int ret = ub_link_send_packet(s, buf, len, errp);
        if (ret >= 0) {
            return 0;
        }
        /* Socket error, fallback to file */
        qemu_log("ub_link: packet send failed, falling back to file: %s\n",
                 *errp ? error_get_pretty(*errp) : "unknown");
        if (*errp) {
            error_free(*errp);
            *errp = NULL;
        }
    }

    if (s->a.device && !s->b.device) {
        remote = &s->b;
    } else if (s->b.device && !s->a.device) {
        remote = &s->a;
    }

    if (!remote) {
        return 0;
    }

    path = ub_link_message_path(remote);
    if (!g_file_set_contents(path, buf, len, NULL)) {
        error_setg(errp, "ub_link: failed to write message file %s", path);
        return -1;
    }

    return 0;
}

int ub_link_read_message(UBLinkState *s, void **buf, size_t *len, Error **errp)
{
    UBLinkEndpointDesc *local = NULL;
    g_autofree char *path = NULL;
    gsize length = 0;
    GError *gerr = NULL;

    if (!s || !s->remote_applied || !buf || !len) {
        return 0;
    }

    /* Dequeue from framed-protocol receive queue (filled by AIO) */
    if (s->rx_msgq && !g_queue_is_empty(s->rx_msgq)) {
        UBLinkRxMsg *msg = g_queue_pop_head(s->rx_msgq);
        if (msg) {
            *buf = msg->data;
            *len = msg->len;
            g_free(msg);
            return 1;
        }
    }

    if (s->a.device && !s->b.device) {
        local = &s->a;
    } else if (s->b.device && !s->a.device) {
        local = &s->b;
    }

    if (!local) {
        return 0;
    }

    path = ub_link_message_path(local);
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        return 0;
    }

    if (!g_file_get_contents(path, (char **)buf, &length, &gerr)) {
        error_setg(errp, "ub_link: failed to read message file %s: %s",
                   path, gerr->message);
        g_error_free(gerr);
        return -1;
    }

    *len = length;
    g_remove(path);
    return 1;
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

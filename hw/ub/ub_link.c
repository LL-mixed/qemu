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
#include "qemu/atomic.h"
#include "qemu/lockable.h"
#include "qemu/main-loop.h"
#include "qom/object.h"
#include "sysemu/sysemu.h"

#define UB_LINK_WRITE_WAIT_USEC 1000
#define UB_LINK_WRITE_TIMEOUT_USEC (30 * G_USEC_PER_SEC)
#define UB_LINK_SHM_WAIT_USEC 50
#define UB_LINK_SHM_TIMEOUT_USEC (30 * G_USEC_PER_SEC)

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

static void ub_link_aio_read(void *opaque)
{
    UBLinkState *s = opaque;

    if (!s || !s->rx_cb) {
        return;
    }
    s->rx_cb(s->rx_cb_opaque, s);
}

static void ub_link_arm_aio_rx(UBLinkState *s)
{
    if (!s || !s->ioc || !s->rx_cb) {
        return;
    }

    qio_channel_set_aio_fd_handler(s->ioc,
                                   qemu_get_aio_context(),
                                   ub_link_aio_read,
                                   NULL,
                                   NULL,
                                   s);
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

static void ub_link_load_remote_endpoint_and_connect(UBLinkState *s,
                                                     UBLinkEndpointDesc *local,
                                                     UBLinkEndpointDesc *remote);

static uint32_t ub_link_shm_ring_size(void)
{
    const char *env = g_getenv("UB_LINK_SHM_RING_SIZE");
    uint64_t size = UB_LINK_SHM_RING_DEFAULT_SIZE;

    if (env && env[0]) {
        size = g_ascii_strtoull(env, NULL, 0);
    }
    if (size < 65536) {
        size = 65536;
    }
    if (size > UB_LINK_RX_BUF_MAX * 8ULL) {
        size = UB_LINK_RX_BUF_MAX * 8ULL;
    }
    return (uint32_t)size;
}

static bool ub_link_transport_allows_shmem(void)
{
    const char *env = g_getenv("UB_LINK_TRANSPORT");
    return !env || !env[0] || g_strcmp0(env, "shmem") == 0 ||
           g_strcmp0(env, "auto") == 0;
}

static bool ub_link_transport_allows_socket_fallback(void)
{
    const char *env = g_getenv("UB_LINK_TRANSPORT");
    return g_strcmp0(env, "socket") == 0 || g_strcmp0(env, "auto") == 0;
}

static char *ub_link_shm_ring_path(const char *src_device_id,
                                   uint32_t src_port_idx,
                                   bool src_local_scope,
                                   const char *dst_device_id,
                                   uint32_t dst_port_idx,
                                   bool dst_local_scope,
                                   const char *suffix)
{
    g_autofree char *src_token = ub_link_make_endpoint_token(src_device_id,
                                                             src_local_scope);
    g_autofree char *dst_token = ub_link_make_endpoint_token(dst_device_id,
                                                             dst_local_scope);
    g_autofree char *src_sanitized = ub_link_sanitize_token(src_token);
    g_autofree char *dst_sanitized = ub_link_sanitize_token(dst_token);
    g_autofree char *dir = ub_link_shared_dir();

    g_mkdir_with_parents(dir, 0755);
    return g_strdup_printf("%s/%s__%u_to_%s__%u.%s",
                           dir,
                           src_sanitized, src_port_idx,
                           dst_sanitized, dst_port_idx, suffix);
}

static void ub_link_shm_notify_read(void *opaque)
{
    UBLinkState *s = opaque;
    uint8_t buf[256];

    if (!s || s->shmem_rx_notify_fd < 0) {
        return;
    }
    while (read(s->shmem_rx_notify_fd, buf, sizeof(buf)) > 0) {
    }
    if (s->rx_cb) {
        s->rx_cb(s->rx_cb_opaque, s);
    }
}

static int ub_link_shm_open_notify_rx(const char *path, Error **errp)
{
    int fd;

    if (mkfifo(path, 0644) != 0 && errno != EEXIST) {
        error_setg_errno(errp, errno, "ub_link: mkfifo notify %s failed", path);
        return -1;
    }
    fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        error_setg_errno(errp, errno, "ub_link: open rx notify %s failed", path);
        return -1;
    }
    return fd;
}

static int ub_link_shm_open_notify_tx(const char *path)
{
    int fd;

    if (mkfifo(path, 0644) != 0 && errno != EEXIST) {
        return -1;
    }
    fd = open(path, O_WRONLY | O_NONBLOCK);
    return fd;
}

static void ub_link_shm_close_notify(int *fd)
{
    if (*fd >= 0) {
        qemu_set_fd_handler(*fd, NULL, NULL, NULL);
        close(*fd);
        *fd = -1;
    }
}

static int ub_link_shm_map_ring(const char *path,
                                UBLinkShmRing **ring_out,
                                int *fd_out,
                                size_t *map_size_out,
                                Error **errp)
{
    uint32_t ring_size = ub_link_shm_ring_size();
    size_t map_size = sizeof(UBLinkShmRing) + ring_size;
    UBLinkShmRing *ring;
    int fd;

    fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        error_setg_errno(errp, errno, "ub_link: open shmem ring %s failed", path);
        return -1;
    }
    if (ftruncate(fd, (off_t)map_size) != 0) {
        error_setg_errno(errp, errno, "ub_link: resize shmem ring %s failed", path);
        close(fd);
        return -1;
    }

    ring = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ring == MAP_FAILED) {
        error_setg_errno(errp, errno, "ub_link: mmap shmem ring %s failed", path);
        close(fd);
        return -1;
    }

    if (qatomic_read(&ring->magic) != UB_LINK_SHM_RING_MAGIC ||
        qatomic_read(&ring->version) != UB_LINK_SHM_RING_VER ||
        qatomic_read(&ring->ring_size) != ring_size) {
        memset(ring, 0, map_size);
        ring->version = UB_LINK_SHM_RING_VER;
        ring->header_size = sizeof(UBLinkShmRing);
        ring->ring_size = ring_size;
        smp_wmb();
        qatomic_set(&ring->magic, UB_LINK_SHM_RING_MAGIC);
    }

    *ring_out = ring;
    *fd_out = fd;
    *map_size_out = map_size;
    return 0;
}

static void ub_link_shm_unmap_ring(UBLinkShmRing **ring,
                                   int *fd,
                                   size_t *map_size)
{
    if (*ring && *ring != MAP_FAILED) {
        munmap(*ring, *map_size);
        *ring = NULL;
    }
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
    *map_size = 0;
}

static int ub_link_setup_shmem(UBLinkState *s, Error **errp)
{
    UBLinkEndpointDesc *local = NULL;
    UBLinkEndpointDesc *remote = NULL;
    g_autofree char *tx_path = NULL;
    g_autofree char *rx_path = NULL;
    g_autofree char *tx_notify_path = NULL;
    g_autofree char *rx_notify_path = NULL;

    if (!s || !ub_link_transport_allows_shmem()) {
        return -1;
    }
    if (s->shmem_ready) {
        ub_link_select_endpoints(s, &local, &remote);
        ub_link_load_remote_endpoint_and_connect(s, local, remote);
        return 0;
    }

    ub_link_select_endpoints(s, &local, &remote);
    if (!local || !remote || !local->device_id || !remote->device_id) {
        error_setg(errp, "ub_link: missing endpoints for shmem transport");
        return -1;
    }

    tx_path = ub_link_shm_ring_path(local->device_id, local->port_idx, true,
                                    remote->device_id, remote->port_idx, false,
                                    "ring");
    rx_path = ub_link_shm_ring_path(remote->device_id, remote->port_idx, false,
                                    local->device_id, local->port_idx, true,
                                    "ring");
    tx_notify_path = ub_link_shm_ring_path(local->device_id, local->port_idx, true,
                                           remote->device_id, remote->port_idx, false,
                                           "notify");
    rx_notify_path = ub_link_shm_ring_path(remote->device_id, remote->port_idx, false,
                                           local->device_id, local->port_idx, true,
                                           "notify");

    if (ub_link_shm_map_ring(tx_path, &s->shmem_tx_ring, &s->shmem_tx_fd,
                             &s->shmem_tx_map_size, errp) != 0) {
        return -1;
    }
    if (ub_link_shm_map_ring(rx_path, &s->shmem_rx_ring, &s->shmem_rx_fd,
                             &s->shmem_rx_map_size, errp) != 0) {
        ub_link_shm_unmap_ring(&s->shmem_tx_ring, &s->shmem_tx_fd,
                               &s->shmem_tx_map_size);
        return -1;
    }
    s->shmem_tx_notify_fd = ub_link_shm_open_notify_tx(tx_notify_path);
    s->shmem_rx_notify_fd = ub_link_shm_open_notify_rx(rx_notify_path, errp);
    if (s->shmem_rx_notify_fd < 0) {
        ub_link_shm_close_notify(&s->shmem_tx_notify_fd);
        ub_link_shm_unmap_ring(&s->shmem_tx_ring, &s->shmem_tx_fd,
                               &s->shmem_tx_map_size);
        ub_link_shm_unmap_ring(&s->shmem_rx_ring, &s->shmem_rx_fd,
                               &s->shmem_rx_map_size);
        return -1;
    }
    qemu_set_fd_handler(s->shmem_rx_notify_fd, ub_link_shm_notify_read,
                        NULL, s);

    s->shmem_tx_path = g_strdup(tx_path);
    s->shmem_rx_path = g_strdup(rx_path);
    s->shmem_tx_notify_path = g_strdup(tx_notify_path);
    s->shmem_rx_notify_path = g_strdup(rx_notify_path);
    s->shmem_ready = true;
    s->link_up = true;
    qemu_log("ub_link: shmem rings ready tx=%s rx=%s notify_tx=%s notify_rx=%s size=%u\n",
             s->shmem_tx_path, s->shmem_rx_path,
             s->shmem_tx_notify_path, s->shmem_rx_notify_path,
             ub_link_shm_ring_size());
    ub_link_mark_connected(s);
    ub_link_load_remote_endpoint_and_connect(s, local, remote);
    return 0;
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
    ub_link_arm_aio_rx(s);
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
                ub_link_arm_aio_rx(s);
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

static int ub_link_write_all_bounded(UBLinkState *s, const char *buf, size_t len,
                                     Error **errp);
static void ub_link_reopen_after_write_error(UBLinkState *s);

static void ub_link_shm_ring_copy_in(UBLinkShmRing *ring,
                                     uint64_t offset,
                                     const void *src,
                                     size_t len)
{
    uint32_t cap = ring->ring_size;
    uint32_t pos = offset % cap;
    size_t first = MIN(len, (size_t)(cap - pos));

    memcpy(ring->data + pos, src, first);
    if (first < len) {
        memcpy(ring->data, (const uint8_t *)src + first, len - first);
    }
}

static void ub_link_shm_ring_copy_out(UBLinkShmRing *ring,
                                      uint64_t offset,
                                      void *dst,
                                      size_t len)
{
    uint32_t cap = ring->ring_size;
    uint32_t pos = offset % cap;
    size_t first = MIN(len, (size_t)(cap - pos));

    memcpy(dst, ring->data + pos, first);
    if (first < len) {
        memcpy((uint8_t *)dst + first, ring->data, len - first);
    }
}

static void ub_link_account_tx(UBLinkState *s, const void *buf, size_t len)
{
    if (len >= sizeof(MsgPktHeader)) {
        const MsgPktHeader *hdr = (const MsgPktHeader *)buf;
        uint8_t sub = hdr->msgetah.sub_msg_code;
        if (sub < 8) {
            s->tx_packets[sub]++;
            s->tx_bytes[sub] += len;
        }
    }
}

static void ub_link_account_rx(UBLinkState *s, const void *buf, size_t len)
{
    if (len >= sizeof(MsgPktHeader)) {
        const MsgPktHeader *hdr = (const MsgPktHeader *)buf;
        uint8_t sub = hdr->msgetah.sub_msg_code;
        if (sub < 8) {
            s->rx_packets[sub]++;
            s->rx_bytes[sub] += len;
        }
    }
}

static int ub_link_shm_write_message(UBLinkState *s,
                                     const void *buf,
                                     size_t len,
                                     Error **errp)
{
    UBLinkShmRing *ring = s->shmem_tx_ring;
    uint32_t cap;
    uint32_t len_le;
    size_t frame_len = sizeof(len_le) + len;
    gint64 deadline;

    if (!ring || qatomic_read(&ring->magic) != UB_LINK_SHM_RING_MAGIC) {
        error_setg(errp, "ub_link: shmem tx ring is not ready");
        return -1;
    }

    cap = qatomic_read(&ring->ring_size);
    if (frame_len >= cap) {
        error_setg(errp, "ub_link: shmem frame %zu exceeds ring size %u",
                   frame_len, cap);
        return -1;
    }

    len_le = cpu_to_le32((uint32_t)len);
    deadline = g_get_monotonic_time() + UB_LINK_SHM_TIMEOUT_USEC;
    for (;;) {
        uint64_t head = qatomic_read(&ring->head);
        uint64_t tail = qatomic_read(&ring->tail);
        uint64_t used = head - tail;
        uint64_t free_bytes = used <= cap ? cap - used : 0;

        if (free_bytes >= frame_len) {
            ub_link_shm_ring_copy_in(ring, head, &len_le, sizeof(len_le));
            ub_link_shm_ring_copy_in(ring, head + sizeof(len_le), buf, len);
            smp_wmb();
            qatomic_set(&ring->head, head + frame_len);
            ub_link_account_tx(s, buf, len);
            if (s->shmem_tx_notify_fd < 0 && s->shmem_tx_notify_path) {
                s->shmem_tx_notify_fd =
                    ub_link_shm_open_notify_tx(s->shmem_tx_notify_path);
            }
            if (s->shmem_tx_notify_fd >= 0) {
                uint8_t notify = 1;
                ssize_t n = write(s->shmem_tx_notify_fd, &notify, sizeof(notify));
                (void)n;
            }
            return 0;
        }

        if (g_get_monotonic_time() >= deadline) {
            s->write_timeouts++;
            error_setg(errp, "ub_link: shmem write timed out free=%" PRIu64
                       " need=%zu", free_bytes, frame_len);
            return -1;
        }
        s->write_retries++;
        /*
         * A remote process advances tail.  Do not poll local AIO while the
         * tx_lock is held: an RX callback can send a response on this link
         * and recursively enter the same message writer.
         */
        g_usleep(UB_LINK_SHM_WAIT_USEC);
    }
}

static int ub_link_shm_read_message(UBLinkState *s,
                                    void **buf,
                                    size_t *len,
                                    Error **errp)
{
    UBLinkShmRing *ring = s->shmem_rx_ring;
    uint32_t len_le;
    uint32_t frame_payload_len;
    uint64_t head;
    uint64_t tail;
    uint64_t used;

    if (!ring || qatomic_read(&ring->magic) != UB_LINK_SHM_RING_MAGIC) {
        return 0;
    }

    head = qatomic_read(&ring->head);
    tail = qatomic_read(&ring->tail);
    used = head - tail;
    if (used < sizeof(len_le)) {
        return 0;
    }

    ub_link_shm_ring_copy_out(ring, tail, &len_le, sizeof(len_le));
    frame_payload_len = le32_to_cpu(len_le);
    if (frame_payload_len == 0 || frame_payload_len > UB_LINK_RX_BUF_MAX) {
        error_setg(errp, "ub_link: invalid shmem frame length %u",
                   frame_payload_len);
        qatomic_set(&ring->tail, head);
        return -1;
    }
    if (used < sizeof(len_le) + frame_payload_len) {
        return 0;
    }

    *buf = g_malloc(frame_payload_len);
    ub_link_shm_ring_copy_out(ring, tail + sizeof(len_le), *buf,
                              frame_payload_len);
    *len = frame_payload_len;
    smp_wmb();
    qatomic_set(&ring->tail, tail + sizeof(len_le) + frame_payload_len);
    ub_link_account_rx(s, *buf, *len);
    return 1;
}

int ub_link_write_message(UBLinkState *s, const void *buf, size_t len, Error **errp)
{
    uint32_t plen = cpu_to_le32((uint32_t)len);
    Error *local_err = NULL;
    int ret;

    if (!s) {
        error_setg(errp, "ub_link: link is not configured");
        return -1;
    }
    QEMU_LOCK_GUARD(&s->tx_lock);

    if (s->shmem_ready) {
        return ub_link_shm_write_message(s, buf, len, errp);
    }

    if (!s->ioc) {
        error_setg(errp, "ub_link: socket is not connected");
        return -1;
    }

    if (ub_link_write_all_bounded(s, (const char *)&plen, sizeof(plen), &local_err) == 0 &&
        ub_link_write_all_bounded(s, (const char *)buf, len, &local_err) == 0) {
        ret = 0;
        goto out;
    }

    error_free(local_err);
    local_err = NULL;
    s->write_retries++;
    ub_link_reopen_after_write_error(s);

    if (!s->ioc) {
        error_setg(errp, "ub_link: socket reconnect failed after write error");
        s->write_timeouts++;
        return -1;
    }

    if (ub_link_write_all_bounded(s, (const char *)&plen, sizeof(plen), &local_err) == 0 &&
        ub_link_write_all_bounded(s, (const char *)buf, len, &local_err) == 0) {
        ret = 0;
        goto out;
    }

    ub_link_reopen_after_write_error(s);
    s->write_timeouts++;
    error_propagate(errp, local_err);
    return -1;

out:
    ub_link_account_tx(s, buf, len);
    return ret;
}

static void ub_link_reopen_after_write_error(UBLinkState *s)
{
    UBLinkEndpointDesc *local = NULL;
    UBLinkEndpointDesc *remote = NULL;
    bool is_server;

    if (s->ioc) {
        qio_channel_close(s->ioc, NULL);
        object_unref(OBJECT(s->ioc));
        s->ioc = NULL;
    }
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
}

static int ub_link_write_all_bounded(UBLinkState *s, const char *buf, size_t len,
                                     Error **errp)
{
    gint64 deadline = g_get_monotonic_time() + UB_LINK_WRITE_TIMEOUT_USEC;
    size_t done = 0;

    while (done < len) {
        Error *local_err = NULL;
        ssize_t ret = qio_channel_write(s->ioc, buf + done, len - done,
                                        &local_err);

        if (ret == QIO_CHANNEL_ERR_BLOCK) {
            error_free(local_err);
            if (g_get_monotonic_time() >= deadline) {
                error_setg(errp, "ub_link: bounded write timed out after %zu/%zu bytes",
                           done, len);
                return -1;
            }
            /* The remote peer drains the socket; local AIO is not required. */
            g_usleep(UB_LINK_WRITE_WAIT_USEC);
            continue;
        }
        if (ret < 0) {
            error_propagate(errp, local_err);
            return -1;
        }
        error_free(local_err);
        if (ret == 0) {
            error_setg(errp, "ub_link: bounded write made no progress after %zu/%zu bytes",
                       done, len);
            return -1;
        }
        done += (size_t)ret;
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

static bool ub_link_trace_nodea_nodeh_rx(UBLinkState *s)
{
    const char *node_id = g_getenv("UB_FM_NODE_ID");

    if (!s || g_strcmp0(node_id, "nodeA") != 0) {
        return false;
    }

    return (g_strcmp0(s->a.device_id, "ubcdev0") == 0 &&
            s->a.port_idx == 6 &&
            g_strcmp0(s->b.device_id, "nodeH.ubcdev0") == 0) ||
           (g_strcmp0(s->b.device_id, "ubcdev0") == 0 &&
            s->b.port_idx == 6 &&
            g_strcmp0(s->a.device_id, "nodeH.ubcdev0") == 0);
}

int ub_link_read_message(UBLinkState *s, void **buf, size_t *len, Error **errp)
{
    uint8_t tmp[4096];
    bool trace = ub_link_trace_nodea_nodeh_rx(s);
    size_t before_used;
    size_t bytes_read = 0;
    ssize_t ret;

    if (!s) {
        return 0;
    }
    if (s->shmem_ready) {
        return ub_link_shm_read_message(s, buf, len, errp);
    }
    if (!s->ioc) {
        return 0;
    }

    if (!s->rx_buf && !ub_link_ensure_rx_capacity(s, sizeof(tmp), errp)) {
        return -1;
    }

    /* Non-blocking stream read: accumulate as much as available this turn. */
    before_used = s->rx_buf_used;
    for (;;) {
        Error *read_err = NULL;

        ret = qio_channel_read(s->ioc, (char *)tmp, sizeof(tmp), &read_err);
        if (ret < 0) {
            if (trace && read_err) {
                qemu_log("ub_link rx nodeA<-nodeH read_err: before=%zu used=%zu err=%s\n",
                         before_used, s->rx_buf_used, error_get_pretty(read_err));
            }
            error_free(read_err);
            break;
        }
        if (ret == 0) {
            error_free(read_err);
            break;
        }
        error_free(read_err);

        if (!ub_link_ensure_rx_capacity(s, s->rx_buf_used + (size_t)ret, errp)) {
            s->rx_buf_used = 0;
            return -1;
        }

        memcpy(s->rx_buf + s->rx_buf_used, tmp, (size_t)ret);
        s->rx_buf_used += (size_t)ret;
        bytes_read += (size_t)ret;
    }

    if (trace && bytes_read > 0) {
        qemu_log("ub_link rx nodeA<-nodeH bytes: before=%zu read=%zu used=%zu cap=%zu\n",
                 before_used, bytes_read, s->rx_buf_used, s->rx_buf_cap);
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
        if (trace) {
            qemu_log("ub_link rx nodeA<-nodeH invalid_frame: plen=%u used=%zu cap=%zu\n",
                     plen, s->rx_buf_used, s->rx_buf_cap);
        }
        s->rx_buf_used = 0;
        return -1;
    }

    frame_len = sizeof(uint32_t) + (size_t)plen;
    if (s->rx_buf_used < frame_len) {
        if (trace && bytes_read > 0) {
            qemu_log("ub_link rx nodeA<-nodeH partial_frame: plen=%u frame=%zu used=%zu\n",
                     plen, frame_len, s->rx_buf_used);
        }
        return 0;
    }

    *buf = g_malloc((size_t)plen);
    memcpy(*buf, s->rx_buf + sizeof(uint32_t), (size_t)plen);
    *len = (size_t)plen;

    ub_link_account_rx(s, s->rx_buf + sizeof(uint32_t), plen);

    s->rx_buf_used -= frame_len;
    if (s->rx_buf_used > 0) {
        memmove(s->rx_buf, s->rx_buf + frame_len, s->rx_buf_used);
    }

    if (trace) {
        qemu_log("ub_link rx nodeA<-nodeH complete_frame: plen=%u remain=%zu\n",
                 plen, s->rx_buf_used);
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

    if (s->shmem_ready) {
        return 1;
    }
    
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

    if (!s) {
        return 0;
    }
    if (s->shmem_ready) {
        s->kick_count++;
        return 0;
    }
    if (!s->ioc) {
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

    s->kick_count++;
    return 0;
}

void ub_link_print_stats(UBLinkState *s, const char *prefix)
{
    int i;
    uint64_t tx_total = 0, rx_total = 0;
    uint64_t tx_bytes_total = 0, rx_bytes_total = 0;
    if (!s) {
        return;
    }

    for (i = 0; i < 8; i++) {
        tx_total += s->tx_packets[i];
        rx_total += s->rx_packets[i];
        tx_bytes_total += s->tx_bytes[i];
        rx_bytes_total += s->rx_bytes[i];
    }

    qemu_log("%sLINK_STATS tx_total=%" PRIu64 " rx_total=%" PRIu64
             " tx_bytes=%" PRIu64 " rx_bytes=%" PRIu64
             " retries=%" PRIu64 " timeouts=%" PRIu64 " kicks=%" PRIu64
             " shmem=%d\n",
             prefix ? prefix : "",
             tx_total, rx_total, tx_bytes_total, rx_bytes_total,
             s->write_retries, s->write_timeouts, s->kick_count,
             s->shmem_ready);
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
    g_key_file_set_boolean(keyfile, "status", "shmem_ready", s->shmem_ready);
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
    ub_link_shm_close_notify(&s->shmem_tx_notify_fd);
    ub_link_shm_close_notify(&s->shmem_rx_notify_fd);
    ub_link_shm_unmap_ring(&s->shmem_tx_ring, &s->shmem_tx_fd,
                           &s->shmem_tx_map_size);
    ub_link_shm_unmap_ring(&s->shmem_rx_ring, &s->shmem_rx_fd,
                           &s->shmem_rx_map_size);
    s->shmem_ready = false;
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
    g_free(s->shmem_tx_path);
    g_free(s->shmem_rx_path);
    g_free(s->shmem_tx_notify_path);
    g_free(s->shmem_rx_notify_path);
    qemu_mutex_destroy(&s->tx_lock);
}

void ub_link_configure(UBLinkState *s, const char *a_device_id, uint32_t a_port_idx,
                       const char *b_device_id, uint32_t b_port_idx, bool link_up)
{
    s->a.device_id = g_strdup(a_device_id);
    s->a.port_idx = a_port_idx;
    s->b.device_id = g_strdup(b_device_id);
    s->b.port_idx = b_port_idx;
    s->link_up = link_up;
    s->shmem_tx_fd = -1;
    s->shmem_rx_fd = -1;

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
    if (ub_link_transport_allows_shmem()) {
        Error *local_err = NULL;
        if (ub_link_setup_shmem(s, &local_err) != 0) {
            qemu_log("ub_link: shmem setup failed for %s:%u <-> %s:%u: %s\n",
                     s->a.device_id, s->a.port_idx,
                     s->b.device_id, s->b.port_idx,
                     local_err ? error_get_pretty(local_err) : "unknown");
            error_free(local_err);
            if (ub_link_transport_allows_socket_fallback()) {
                ub_link_setup_socket(s, is_server);
            } else {
                ub_link_mark_failed(s, "shmem transport setup failed");
                return -1;
            }
        }
    } else {
        ub_link_setup_socket(s, is_server);
    }

    /* Check Ready Contract conditions before marking applied */
    uint64_t now_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    bool state_age_ok = (now_ms - s->state_set_ts_ms) >= 200;  /* M1: 200ms stability check */

    qemu_log("ub_link: apply %s:%u <-> %s:%u (shmem=%d sock=%d guid=%d snap=%d age=%d ms_ago=%lu)\n",
             s->a.device_id, s->a.port_idx, s->b.device_id, s->b.port_idx,
             s->shmem_ready, s->socket_connected, s->remote_guid_valid, s->snapshot_reconciled,
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

static void ub_link_instance_init(Object *obj)
{
    UBLinkState *s = UB_LINK(obj);

    qemu_mutex_init(&s->tx_lock);
    s->shmem_tx_fd = -1;
    s->shmem_rx_fd = -1;
    s->shmem_tx_notify_fd = -1;
    s->shmem_rx_notify_fd = -1;
}

static void ub_link_class_init(ObjectClass *klass, void *data) {}

static const TypeInfo ub_link_type_info = {
    .name = TYPE_UB_LINK,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(UBLinkState),
    .instance_init = ub_link_instance_init,
    .instance_finalize = ub_link_finalize,
    .class_init = ub_link_class_init,
};

static void ub_link_register_types(void)
{
    type_register_static(&ub_link_type_info);
}

type_init(ub_link_register_types)

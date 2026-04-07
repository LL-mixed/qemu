/*
 * Minimal standalone UB point-to-point link object.
 *
 * One UBLink instance models exactly one point-to-point port-to-port
 * connection. It exists as a topology object, separate from the UBC model,
 * so future multi-instance UBC<->UBC links can be represented without
 * embedding interconnect behavior inside a controller implementation.
 */

#ifndef UB_LINK_H
#define UB_LINK_H

#include "qom/object.h"
#include "io/channel.h"
#include "io/net-listener.h"
#include "qapi/error.h"

typedef struct UBDevice UBDevice;

#define TYPE_UB_LINK "ub-link"
OBJECT_DECLARE_SIMPLE_TYPE(UBLinkState, UB_LINK)

/*
 * UB spec-aligned constants used by the framing layer.
 * These must match the definitions in ub_common.h.
 */
#define UB_LINK_PKT_HDR_SIZE    32   /* MSG_PKT_HEADER_SIZE */
#define UB_LINK_CLAN_CFG        6    /* UB_CLAN_LINK_CFG */

/* Maximum sizes for the receive buffer */
#define UB_LINK_RX_BUF_MAX      (1 << 20)  /* 1 MiB */

/* A fully received spec-aligned UB packet */
typedef struct UBLinkRxMsg {
    size_t len;      /* total = UB_LINK_PKT_HDR_SIZE + plen */
    void *data;      /* g_malloc'd: MsgPktHeader + payload */
} UBLinkRxMsg;

/* Link state for Ready Contract */
typedef enum UBLinkStateEnum {
    UB_LINK_STATE_PENDING = 0,
    UB_LINK_STATE_READY = 1,
    UB_LINK_STATE_FAILED = -1
} UBLinkStateEnum;

/* ---------- endpoint and link structures ---------- */

typedef struct UBLinkEndpointDesc {
    char *device_id;
    uint32_t port_idx;
    UBDevice *device;
} UBLinkEndpointDesc;

struct UBLinkState {
    Object parent_obj;

    UBLinkEndpointDesc a;
    UBLinkEndpointDesc b;
    bool link_up;
    bool attached;
    bool applied;
    bool pending;
    bool remote_applied;

    /* High-performance socket channel */
    QIOChannel *ioc;
    QIONetListener *lioc; /* Listener, for server side */
    char *socket_path;

    /* Receive buffer for the spec-aligned protocol */
    uint8_t *rx_buf;
    size_t rx_buf_cap;         /* allocated capacity */
    size_t rx_buf_used;        /* valid bytes in rx_buf */

    /*
     * Spec-aligned framing state machine.
     * Phase 1: Collect UB_LINK_PKT_HDR_SIZE bytes (fixed header for cfg==6).
     *          Validate ulh.cfg, extract msgetah.plen.
     * Phase 2: Collect plen bytes of payload.
     * Phase 3: Enqueue complete packet to rx_msgq.
     */
    bool     rx_hdr_done;          /* true = fixed header parsed, collecting payload */
    uint16_t rx_plen;              /* msgetah.plen from parsed header */
    size_t   rx_payload_remaining; /* payload bytes still outstanding */

    /* Queue of fully received packets */
    GQueue *rx_msgq;

    /* Callback invoked when a packet is received via socket AIO */
    void (*rx_cb)(void *opaque, UBLinkState *s);
    void *rx_cb_opaque;

    /* Ready Contract state fields */
    UBLinkStateEnum state;
    bool socket_connected;
    bool remote_guid_valid;
    uint64_t reconcile_ts_ms;
    char *last_error;
    char *status_file_path;
};

void ub_link_configure(UBLinkState *s, const char *a_device_id, uint32_t a_port_idx,
                       const char *b_device_id, uint32_t b_port_idx, bool link_up);
int ub_link_attach_endpoints(UBLinkState *s, Error **errp);
int ub_link_apply(UBLinkState *s, Error **errp);
int ub_link_deactivate(UBLinkState *s, Error **errp);
void ub_link_detach_endpoints(UBLinkState *s);
bool ub_link_is_pending(UBLinkState *s);
int ub_link_kick_remote(UBLinkState *s, Error **errp);
int ub_link_poll_kick(UBLinkState *s);
int ub_link_write_message(UBLinkState *s, const void *buf, size_t len, Error **errp);
int ub_link_read_message(UBLinkState *s, void **buf, size_t *len, Error **errp);
char *ub_link_endpoint_path(const char *device_id, uint32_t port_idx, bool local_scope);

/* Ready Contract management */
int ub_link_update_status_file(UBLinkState *s);
void ub_link_mark_connected(UBLinkState *s);
void ub_link_mark_failed(UBLinkState *s, const char *reason);
bool ub_link_is_ready(UBLinkState *s);
char *ub_link_status_path(const char *device_id, uint32_t port_idx);

#endif

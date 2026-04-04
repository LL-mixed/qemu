/*
 * Minimal standalone UB point-to-point link object.
 *
 * One UBLink instance models exactly one point-to-point port-to-port
 * connection. It exists as a topology object, separate from the UBC model,
 * so future multi-instance UBC<->UBC links can be represented without
 * embedding interconnect behavior inside a controller implementation.
 *
 * Data transport uses a simple framed protocol over a Unix domain socket:
 *
 *   [ UBLinkFrameHeader (16 bytes) ] [ payload (payload_len bytes) ]
 *
 * The header carries a magic value, protocol version, payload length and
 * flags that discriminate control messages from DMA / data frames.
 */

#ifndef UB_LINK_H
#define UB_LINK_H

#include "qom/object.h"
#include "io/channel.h"
#include "io/net-listener.h"

typedef struct UBDevice UBDevice;

#define TYPE_UB_LINK "ub-link"
OBJECT_DECLARE_SIMPLE_TYPE(UBLinkState, UB_LINK)

/* ---------- framed protocol definitions ---------- */

#define UB_LINK_FRAME_MAGIC  0x554C4B46  /* "ULKF" */
#define UB_LINK_FRAME_V1     1

/*
 * Frame flags — carried in UBLinkFrameHeader.flags.
 * Low 16 bits: message class (control / DMA).
 * High 16 bits: reserved.
 */
#define UB_LINK_FRAME_FLAG_CTRL    0x0000  /* control message (MsgPktHeader + payload) */
#define UB_LINK_FRAME_FLAG_DMA_REQ 0x0001  /* DMA request (metadata only) */
#define UB_LINK_FRAME_FLAG_DMA_DAT 0x0002  /* DMA data payload */
#define UB_LINK_FRAME_FLAG_DMA_CPL 0x0003  /* DMA completion */

typedef struct UBLinkFrameHeader {
    uint32_t magic;          /* UB_LINK_FRAME_MAGIC */
    uint32_t version;        /* UB_LINK_FRAME_V1 */
    uint32_t payload_len;    /* bytes following this header */
    uint32_t flags;          /* UB_LINK_FRAME_FLAG_* */
} UBLinkFrameHeader;

#define UB_LINK_FRAME_HDR_SIZE  16

/* Initial and maximum sizes for the receive buffer */
#define UB_LINK_RX_BUF_INITIAL  4096
#define UB_LINK_RX_BUF_MAX      (1 << 20)  /* 1 MiB */

/* A fully received message dequeued from the framing layer */
typedef struct UBLinkRxMsg {
    uint32_t flags;      /* UB_LINK_FRAME_FLAG_* */
    size_t len;          /* payload length */
    void *data;          /* g_malloc'd payload */
} UBLinkRxMsg;

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

    /* Receive buffer for the framed protocol */
    uint8_t *rx_buf;
    size_t rx_buf_cap;         /* allocated capacity */
    size_t rx_buf_used;        /* valid bytes in rx_buf */

    /*
     * Framing state machine.
     * When rx_header_done == false we are collecting the 16-byte header.
     * When rx_header_done == true  we are collecting the payload whose
     * length is stored in rx_hdr.payload_len.  rx_frame_remaining tracks
     * how many payload bytes are still outstanding.
     */
    bool rx_header_done;
    UBLinkFrameHeader rx_hdr;
    size_t rx_frame_remaining;

    /* Queue of fully received frames (each entry is g_malloc'd payload) */
    GQueue *rx_msgq;

    /* AIO watch source ID (0 = not registered) */
    guint aio_watch_id;
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

/* Framed-protocol send (non-static, used by DMA engine and msgq) */
int ub_link_send_frame(UBLinkState *s, uint32_t flags,
                       const void *payload, size_t len, Error **errp);

#endif

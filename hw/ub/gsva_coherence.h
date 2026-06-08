/*
 * GSVA coherence -- header.
 *
 * Owns: GSVA object state, ReadAcquire/WriteAcquire,
 *       revoke/invalidate/downgrade/writeback/fence orchestration,
 *       pending sequence handling, timeout terminal state.
 */

#ifndef GSVA_COHERENCE_H
#define GSVA_COHERENCE_H

#include "qemu/osdep.h"
#include "qemu/queue.h"
#include "hw/ub/gsva_key.h"
#include "hw/ub/gsva_route.h"
#include <stdint.h>
#include <stdbool.h>

/* Forward declaration */
struct BusControllerDev;
typedef struct BusControllerDev BusControllerDev;

/* GSVA coherence message payload */
typedef struct GsvaCohMsgV1 {
    uint32_t version;
    uint32_t op;
    uint64_t seq;
    uint32_t source_cna;
    uint32_t target_cna;
    GsvaKeyV1 key;
    uint64_t access_va;
    uint64_t access_len;
    uint32_t access_flags;
    uint32_t error;
} GsvaCohMsgV1;

/* GSVA coherence message op values */
#define GSVA_COH_MSG_INVALIDATE      1
#define GSVA_COH_MSG_INVALIDATE_ACK  2
#define GSVA_COH_MSG_DOWNGRADE       3
#define GSVA_COH_MSG_DOWNGRADE_ACK   4
#define GSVA_COH_MSG_WRITEBACK       5
#define GSVA_COH_MSG_WRITEBACK_ACK   6
#define GSVA_COH_MSG_FENCE           7
#define GSVA_COH_MSG_FENCE_ACK       8
#define GSVA_COH_MSG_RETIRE          9
#define GSVA_COH_MSG_RETIRE_ACK      10
#define GSVA_COH_MSG_TOKEN_REVOKE    11
#define GSVA_COH_MSG_TOKEN_ACK       12

typedef enum GsvaCohState {
    GSVA_COH_I = 0,
    GSVA_COH_S = 1,
    GSVA_COH_E = 2,
    GSVA_COH_M = 3,
    GSVA_COH_RETIRED = 4,
    GSVA_COH_TIMEOUT = 5,
} GsvaCohState;

#define GSVA_COH_MAX_HOLDERS 64

/* GSVA coherence object state */
typedef struct GsvaCohObject {
    GsvaKeyV1 key;
    GsvaCohState state;
    uint32_t home_cna;
    uint32_t owner_cna;
    uint64_t sharer_bitmap;
    uint32_t sharer_count;
    uint32_t sharer_cnas[GSVA_COH_MAX_HOLDERS];
    uint64_t epoch;
    bool pending;
    uint64_t pending_seq;
    uint32_t pending_op;
    uint32_t pending_target;
    uint64_t pending_ack_bitmap;
    uint32_t pending_ack_count;
    uint32_t pending_ack_cnas[GSVA_COH_MAX_HOLDERS];
    uint64_t pending_start_ms;
    uint64_t map_id;
    uint64_t create_time_ms;
    QTAILQ_ENTRY(GsvaCohObject) next;
} GsvaCohObject;

/* GSVA coherence table */
typedef struct GsvaCohTable {
    QTAILQ_HEAD(, GsvaCohObject) objects;
    uint64_t next_seq;
    int object_count;
} GsvaCohTable;

/* Initialize coherence table */
void gsva_coh_table_init(GsvaCohTable *tbl);

/* Destroy coherence table */
void gsva_coh_table_destroy(GsvaCohTable *tbl);

/* Create coherence object for a route. Returns 0 on success. */
int gsva_coh_object_create(GsvaCohTable *tbl, const GsvaKeyV1 *key,
                           uint32_t home_cna, uint64_t map_id);

/* Remove coherence object. Returns 0 on success. */
int gsva_coh_object_remove(GsvaCohTable *tbl, const GsvaKeyV1 *key);

/* Lookup coherence object by key. Returns object or NULL. */
GsvaCohObject *gsva_coh_lookup(GsvaCohTable *tbl, const GsvaKeyV1 *key);

/* ReadAcquire: acquire shared access. Token validated before state change. */
int gsva_coh_read_acquire(GsvaCohTable *tbl, const GsvaRouteTable *routes,
                          const GsvaKeyV1 *key, uint32_t requester_cna,
                          uint32_t token_id, uint32_t token_value);

/* WriteAcquire: acquire exclusive/modified access. Token validated before state change. */
int gsva_coh_write_acquire(GsvaCohTable *tbl, const GsvaRouteTable *routes,
                           const GsvaKeyV1 *key, uint32_t requester_cna,
                           uint32_t token_id, uint32_t token_value);

/* WriteAcquire with optional UB Link transport for remote invalidation. */
int gsva_coh_write_acquire_tx(GsvaCohTable *tbl, const GsvaRouteTable *routes,
                              BusControllerDev *ubc_dev,
                              const GsvaKeyV1 *key, uint32_t requester_cna,
                              uint32_t token_id, uint32_t token_value);

/* Retire: start retire transaction. Returns GSVA_OK or error. */
int gsva_coh_retire(GsvaCohTable *tbl, const GsvaKeyV1 *key,
                    uint32_t requester_cna);

/* Check and set timeout on pending objects. Returns count of timed-out objects. */
int gsva_coh_check_timeouts(GsvaCohTable *tbl, uint64_t now_ms,
                            uint64_t timeout_ms);

/* Process invalidate ACK from a sharer. Completes pending op when all ACKs received. */
int gsva_coh_inv_ack(GsvaCohTable *tbl, const GsvaKeyV1 *key,
                     uint32_t ack_cna, uint64_t seq);

/* Retry a pending acquire (idempotent). Returns GSVA_OK if op completed. */
int gsva_coh_retry(GsvaCohTable *tbl, const GsvaKeyV1 *key, uint64_t seq);

/* Register process-local default table used by UB Link RX ACK handlers. */
void gsva_coh_set_default_table(GsvaCohTable *tbl);

/* Get object state as string */
const char *gsva_coh_state_name(GsvaCohState state);

/* Send GSVA coherence message over UB Link */
int gsva_coh_send_ub_link_msg(BusControllerDev *ubc_dev, uint32_t dcna,
                               uint8_t sub_msg_code,
                               const GsvaCohMsgV1 *msg);

/* Receive handlers for GSVA coherence messages */
void gsva_coh_handle_rx_inv(BusControllerDev *ubc_dev, const GsvaCohMsgV1 *msg);
void gsva_coh_handle_rx_inv_ack(BusControllerDev *ubc_dev, const GsvaCohMsgV1 *msg);
void gsva_coh_handle_rx_downgrade(BusControllerDev *ubc_dev, const GsvaCohMsgV1 *msg);
void gsva_coh_handle_rx_downgrade_ack(BusControllerDev *ubc_dev, const GsvaCohMsgV1 *msg);
void gsva_coh_handle_rx_wb(BusControllerDev *ubc_dev, const GsvaCohMsgV1 *msg);
void gsva_coh_handle_rx_wb_ack(BusControllerDev *ubc_dev, const GsvaCohMsgV1 *msg);
void gsva_coh_handle_rx_fence(BusControllerDev *ubc_dev, const GsvaCohMsgV1 *msg);
void gsva_coh_handle_rx_fence_ack(BusControllerDev *ubc_dev, const GsvaCohMsgV1 *msg);
void gsva_coh_handle_rx_retire(BusControllerDev *ubc_dev, const GsvaCohMsgV1 *msg);
void gsva_coh_handle_rx_retire_ack(BusControllerDev *ubc_dev, const GsvaCohMsgV1 *msg);
void gsva_coh_handle_rx_token_revoke(BusControllerDev *ubc_dev, const GsvaCohMsgV1 *msg);
void gsva_coh_handle_rx_token_ack(BusControllerDev *ubc_dev, const GsvaCohMsgV1 *msg);

/* Dispatch a received GSVA coherence message by subcode */
void gsva_coh_dispatch_rx(BusControllerDev *ubc_dev, uint8_t sub_msg_code,
                           const void *payload, uint32_t payload_len);

#endif /* GSVA_COHERENCE_H */

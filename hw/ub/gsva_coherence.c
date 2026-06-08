/*
 * GSVA coherence.
 *
 * GSVA object state, ReadAcquire/WriteAcquire,
 * retire orchestration, pending sequence handling.
 */

#include "qemu/osdep.h"
#include "hw/ub/gsva_coherence.h"
#include "hw/ub/gsva_route.h"
#include "hw/ub/ub_ubc.h"
#include "qemu/log.h"
#include "qemu/timer.h"

void gsva_coh_table_init(GsvaCohTable *tbl)
{
    QTAILQ_INIT(&tbl->objects);
    tbl->next_seq = 1;
    tbl->object_count = 0;
}

void gsva_coh_table_destroy(GsvaCohTable *tbl)
{
    GsvaCohObject *obj;
    while ((obj = QTAILQ_FIRST(&tbl->objects)) != NULL) {
        QTAILQ_REMOVE(&tbl->objects, obj, next);
        g_free(obj);
        tbl->object_count--;
    }
}

int gsva_coh_object_create(GsvaCohTable *tbl, const GsvaKeyV1 *key,
                           uint32_t home_cna, uint64_t map_id)
{
    GsvaCohObject *obj;

    if (!tbl || !key) {
        return GSVA_ERR_BAD_VERSION;
    }

    /* Check for existing object with same base identity */
    QTAILQ_FOREACH(obj, &tbl->objects, next) {
        if (gsva_key_base_equal(&obj->key, key)) {
            if (obj->state == GSVA_COH_RETIRED) {
                if (key->epoch <= obj->epoch) {
                    return GSVA_ERR_STALE_EPOCH;
                }
                /* Reuse with higher epoch */
                QTAILQ_REMOVE(&tbl->objects, obj, next);
                g_free(obj);
                tbl->object_count--;
                break;
            }
            return GSVA_ERR_KEY_MISMATCH;
        }
    }

    obj = g_new0(GsvaCohObject, 1);
    obj->key = *key;
    obj->state = GSVA_COH_I;
    obj->home_cna = home_cna;
    obj->owner_cna = 0;
    obj->sharer_bitmap = 0;
    obj->epoch = key->epoch;
    obj->pending = false;
    obj->pending_seq = 0;
    obj->pending_op = 0;
    obj->pending_target = 0;
    obj->pending_ack_bitmap = 0;
    obj->map_id = map_id;
    obj->create_time_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);

    QTAILQ_INSERT_TAIL(&tbl->objects, obj, next);
    tbl->object_count++;

    qemu_log("GSVA_COH: object created segment_id=%#" PRIx64
             " home_va=%#" PRIx64 " epoch=%" PRIu64 " state=%s\n",
             key->segment_id, key->home_va, key->epoch,
             gsva_coh_state_name(obj->state));

    return GSVA_OK;
}

int gsva_coh_object_remove(GsvaCohTable *tbl, const GsvaKeyV1 *key)
{
    GsvaCohObject *obj;

    if (!tbl || !key) {
        return GSVA_ERR_BAD_VERSION;
    }

    QTAILQ_FOREACH(obj, &tbl->objects, next) {
        if (gsva_key_base_equal(&obj->key, key)) {
            if (obj->pending) {
                return GSVA_ERR_COH_PENDING;
            }
            QTAILQ_REMOVE(&tbl->objects, obj, next);
            tbl->object_count--;
            g_free(obj);
            return GSVA_OK;
        }
    }
    return GSVA_ERR_ROUTE_MISSING;
}

GsvaCohObject *gsva_coh_lookup(GsvaCohTable *tbl, const GsvaKeyV1 *key)
{
    GsvaCohObject *obj;

    if (!tbl || !key) {
        return NULL;
    }

    QTAILQ_FOREACH(obj, &tbl->objects, next) {
        if (gsva_key_base_equal(&obj->key, key)) {
            return obj;
        }
    }
    return NULL;
}

int gsva_coh_read_acquire(GsvaCohTable *tbl, const GsvaRouteTable *routes,
                          const GsvaKeyV1 *key, uint32_t requester_cna,
                          uint32_t token_id, uint32_t token_value)
{
    GsvaCohObject *obj;

    if (!tbl || !key) {
        return GSVA_ERR_BAD_VERSION;
    }

    obj = gsva_coh_lookup(tbl, key);
    if (!obj) {
        return GSVA_ERR_ROUTE_MISSING;
    }

    /* Epoch check */
    if (key->epoch != obj->epoch) {
        qemu_log("GSVA_COH: ReadAcquire stale epoch: req=%" PRIu64
                 " active=%" PRIu64 "\n", key->epoch, obj->epoch);
        return GSVA_ERR_STALE_EPOCH;
    }

    if (obj->state == GSVA_COH_RETIRED) {
        qemu_log("GSVA_COH: ReadAcquire retired segment_id=%#" PRIx64
                 " cna=%" PRIu32 "\n",
                 key->segment_id, requester_cna);
        return GSVA_ERR_SEGMENT_RETIRED;
    }

    if (obj->state == GSVA_COH_TIMEOUT) {
        return GSVA_ERR_COH_PENDING;
    }

    if (obj->pending) {
        return GSVA_ERR_COH_PENDING;
    }

    /* Token validation before state change */
    if (routes) {
        GsvaRouteEntry *route = gsva_route_lookup_base((GsvaRouteTable *)routes, key);
        if (route) {
            int tok_rc = gsva_route_validate_token(route, requester_cna,
                                                    token_id, token_value, 1);
            if (tok_rc != GSVA_OK) {
                qemu_log("GSVA_COH: ReadAcquire token denied: cna=%" PRIu32
                         " token_id=%" PRIu32 " rc=%d\n",
                         requester_cna, token_id, tok_rc);
                return GSVA_ERR_TOKEN_DENIED;
            }
        }
    }

    switch (obj->state) {
    case GSVA_COH_I:
        obj->state = GSVA_COH_S;
        obj->sharer_bitmap |= (1ULL << requester_cna);
        qemu_log("GSVA_COH: ReadAcquire I->S cna=%" PRIu32
                 " segment_id=%#" PRIx64 "\n",
                 requester_cna, key->segment_id);
        break;

    case GSVA_COH_S:
        obj->sharer_bitmap |= (1ULL << requester_cna);
        qemu_log("GSVA_COH: ReadAcquire S->S cna=%" PRIu32
                 " segment_id=%#" PRIx64 "\n",
                 requester_cna, key->segment_id);
        break;

    case GSVA_COH_E:
        /* Owner becomes sharer, requester becomes sharer */
        obj->sharer_bitmap |= (1ULL << obj->owner_cna);
        obj->sharer_bitmap |= (1ULL << requester_cna);
        obj->owner_cna = 0;
        obj->state = GSVA_COH_S;
        qemu_log("GSVA_COH: ReadAcquire E->S cna=%" PRIu32
                 " segment_id=%#" PRIx64 "\n",
                 requester_cna, key->segment_id);
        break;

    case GSVA_COH_M:
        /* Owner must writeback or data-forward, then S */
        obj->sharer_bitmap |= (1ULL << obj->owner_cna);
        obj->sharer_bitmap |= (1ULL << requester_cna);
        obj->owner_cna = 0;
        obj->state = GSVA_COH_S;
        qemu_log("GSVA_COH: ReadAcquire M->S cna=%" PRIu32
                 " segment_id=%#" PRIx64 "\n",
                 requester_cna, key->segment_id);
        break;

    default:
        return GSVA_ERR_COH_PENDING;
    }

    return GSVA_OK;
}

int gsva_coh_write_acquire(GsvaCohTable *tbl, const GsvaRouteTable *routes,
                           const GsvaKeyV1 *key, uint32_t requester_cna,
                           uint32_t token_id, uint32_t token_value)
{
    GsvaCohObject *obj;

    if (!tbl || !key) {
        return GSVA_ERR_BAD_VERSION;
    }

    obj = gsva_coh_lookup(tbl, key);
    if (!obj) {
        return GSVA_ERR_ROUTE_MISSING;
    }

    /* Epoch check */
    if (key->epoch != obj->epoch) {
        qemu_log("GSVA_COH: WriteAcquire stale epoch: req=%" PRIu64
                 " active=%" PRIu64 "\n", key->epoch, obj->epoch);
        return GSVA_ERR_STALE_EPOCH;
    }

    if (obj->state == GSVA_COH_RETIRED) {
        qemu_log("GSVA_COH: WriteAcquire retired segment_id=%#" PRIx64
                 " cna=%" PRIu32 "\n",
                 key->segment_id, requester_cna);
        return GSVA_ERR_SEGMENT_RETIRED;
    }

    if (obj->state == GSVA_COH_TIMEOUT) {
        return GSVA_ERR_COH_PENDING;
    }

    if (obj->pending) {
        return GSVA_ERR_COH_PENDING;
    }

    /* Token validation before state change */
    if (routes) {
        GsvaRouteEntry *route = gsva_route_lookup_base((GsvaRouteTable *)routes, key);
        if (route) {
            int tok_rc = gsva_route_validate_token(route, requester_cna,
                                                    token_id, token_value, 2);
            if (tok_rc != GSVA_OK) {
                qemu_log("GSVA_COH: WriteAcquire token denied: cna=%" PRIu32
                         " token_id=%" PRIu32 " rc=%d\n",
                         requester_cna, token_id, tok_rc);
                return GSVA_ERR_TOKEN_DENIED;
            }
        }
    }

    switch (obj->state) {
    case GSVA_COH_I:
        obj->state = GSVA_COH_M;
        obj->owner_cna = requester_cna;
        obj->sharer_bitmap = 0;
        qemu_log("GSVA_COH: WriteAcquire I->M cna=%" PRIu32
                 " segment_id=%#" PRIx64 "\n",
                 requester_cna, key->segment_id);
        break;

    case GSVA_COH_S: {
        /* Invalidate other sharers before granting M.
         * V1 sim: synchronous — record pending, immediately complete. */
        uint64_t other_sharers = obj->sharer_bitmap & ~(1ULL << requester_cna);
        if (other_sharers) {
            obj->pending = true;
            obj->pending_seq = ++tbl->next_seq;
            obj->pending_op = 1; /* invalidate */
            obj->pending_target = requester_cna;
            obj->pending_ack_bitmap = other_sharers;
            qemu_log("GSVA_COH: WriteAcquire S->M pending inv"
                     " cna=%" PRIu32 " waiting_for=%#" PRIx64
                     " seq=%" PRIu64 "\n",
                     requester_cna, other_sharers, obj->pending_seq);

            /* V1 sim: immediately acknowledge all invalidations */
            obj->pending_ack_bitmap = 0;
            obj->pending = false;
        }
        obj->state = GSVA_COH_M;
        obj->owner_cna = requester_cna;
        obj->sharer_bitmap = 0;
        qemu_log("GSVA_COH: WriteAcquire S->M cna=%" PRIu32
                 " segment_id=%#" PRIx64 "\n",
                 requester_cna, key->segment_id);
        break;
    }

    case GSVA_COH_E:
        if (obj->owner_cna == requester_cna) {
            obj->state = GSVA_COH_M;
            qemu_log("GSVA_COH: WriteAcquire E->M (owner) cna=%" PRIu32
                     " segment_id=%#" PRIx64 "\n",
                     requester_cna, key->segment_id);
        } else {
            obj->state = GSVA_COH_M;
            obj->owner_cna = requester_cna;
            obj->sharer_bitmap = 0;
            qemu_log("GSVA_COH: WriteAcquire E->M (other) cna=%" PRIu32
                     " segment_id=%#" PRIx64 "\n",
                     requester_cna, key->segment_id);
        }
        break;

    case GSVA_COH_M:
        if (obj->owner_cna == requester_cna) {
            /* Already owner, no state change */
            qemu_log("GSVA_COH: WriteAcquire M->M (owner) cna=%" PRIu32
                     " segment_id=%#" PRIx64 "\n",
                     requester_cna, key->segment_id);
        } else {
            /* In full impl: need writeback from old owner.
             * For V1 sim: directly transfer ownership. */
            obj->owner_cna = requester_cna;
            qemu_log("GSVA_COH: WriteAcquire M->M (transfer) cna=%" PRIu32
                     " segment_id=%#" PRIx64 "\n",
                     requester_cna, key->segment_id);
        }
        break;

    default:
        return GSVA_ERR_COH_PENDING;
    }

    return GSVA_OK;
}

int gsva_coh_retire(GsvaCohTable *tbl, const GsvaKeyV1 *key,
                    uint32_t requester_cna)
{
    GsvaCohObject *obj;

    if (!tbl || !key) {
        return GSVA_ERR_BAD_VERSION;
    }

    obj = gsva_coh_lookup(tbl, key);
    if (!obj) {
        return GSVA_ERR_ROUTE_MISSING;
    }

    if (key->epoch != obj->epoch) {
        return GSVA_ERR_STALE_EPOCH;
    }

    if (obj->pending) {
        return GSVA_ERR_COH_PENDING;
    }

    if (obj->state == GSVA_COH_RETIRED) {
        return GSVA_ERR_SEGMENT_RETIRED;
    }

    qemu_log("GSVA_COH: Retire revoke holders segment_id=%#" PRIx64
             " state=%s owner=%" PRIu32 " sharers=%#" PRIx64 "\n",
             key->segment_id, gsva_coh_state_name(obj->state),
             obj->owner_cna, obj->sharer_bitmap);

    /* V1 sim: directly retire after holder revoke accounting.
     * Also allow retiring from TIMEOUT state for cleanup. */
    obj->state = GSVA_COH_RETIRED;
    obj->owner_cna = 0;
    obj->sharer_bitmap = 0;
    obj->pending = false;

    qemu_log("GSVA_RETIRE: segment_id=%#" PRIx64 " home_va=%#" PRIx64
             " epoch=%" PRIu64 " RETIRED\n",
             key->segment_id, key->home_va, key->epoch);

    return GSVA_OK;
}

int gsva_coh_inv_ack(GsvaCohTable *tbl, const GsvaKeyV1 *key,
                     uint32_t ack_cna, uint64_t seq)
{
    GsvaCohObject *obj;

    if (!tbl || !key) {
        return GSVA_ERR_BAD_VERSION;
    }

    obj = gsva_coh_lookup(tbl, key);
    if (!obj) {
        return GSVA_ERR_ROUTE_MISSING;
    }

    if (!obj->pending || obj->pending_seq != seq) {
        return GSVA_ERR_COH_PENDING;
    }

    obj->pending_ack_bitmap &= ~(1ULL << ack_cna);
    qemu_log("GSVA_COH: InvAck cna=%" PRIu32 " seq=%" PRIu64
             " remaining=%#" PRIx64 "\n",
             ack_cna, seq, obj->pending_ack_bitmap);

    if (obj->pending_ack_bitmap == 0) {
        obj->pending = false;
        qemu_log("GSVA_COH: pending op complete seq=%" PRIu64
                 " segment_id=%#" PRIx64 "\n",
                 seq, obj->key.segment_id);
    }

    return GSVA_OK;
}

int gsva_coh_retry(GsvaCohTable *tbl, const GsvaKeyV1 *key, uint64_t seq)
{
    GsvaCohObject *obj;

    if (!tbl || !key) {
        return GSVA_ERR_BAD_VERSION;
    }

    obj = gsva_coh_lookup(tbl, key);
    if (!obj) {
        return GSVA_ERR_ROUTE_MISSING;
    }

    if (!obj->pending) {
        return GSVA_OK; /* already complete */
    }

    if (obj->pending_seq != seq) {
        return GSVA_ERR_COH_PENDING;
    }

    /* V1 sim: synchronous — if still pending, treat as timeout check */
    if (obj->pending_ack_bitmap == 0) {
        obj->pending = false;
        return GSVA_OK;
    }

    return GSVA_ERR_COH_PENDING;
}

const char *gsva_coh_state_name(GsvaCohState state)
{
    switch (state) {
    case GSVA_COH_I:        return "I";
    case GSVA_COH_S:        return "S";
    case GSVA_COH_E:        return "E";
    case GSVA_COH_M:        return "M";
    case GSVA_COH_RETIRED:  return "RETIRED";
    case GSVA_COH_TIMEOUT:  return "TIMEOUT";
    default:                return "UNKNOWN";
    }
}

int gsva_coh_check_timeouts(GsvaCohTable *tbl, uint64_t now_ms,
                            uint64_t timeout_ms)
{
    GsvaCohObject *obj;
    int count = 0;

    if (!tbl) {
        return 0;
    }

    QTAILQ_FOREACH(obj, &tbl->objects, next) {
        if (obj->pending && obj->state != GSVA_COH_RETIRED &&
            obj->state != GSVA_COH_TIMEOUT) {
            uint64_t elapsed = now_ms - obj->create_time_ms;
            if (elapsed > timeout_ms) {
                obj->state = GSVA_COH_TIMEOUT;
                obj->pending = false;
                count++;
                qemu_log("GSVA_COH: TIMEOUT segment_id=%#" PRIx64
                         " elapsed=%" PRIu64 "ms\n",
                         obj->key.segment_id, elapsed);
            }
        }
    }
    return count;
}

/*
 * GSVA coherence transport: send/receive over UB Link.
 * Reuses the same obmm_coh_send_ub_link_msg() transport helper.
 */

int gsva_coh_send_ub_link_msg(BusControllerDev *ubc_dev, uint32_t dcna,
                               uint8_t sub_msg_code,
                               const GsvaCohMsgV1 *msg)
{
    if (!ubc_dev || !msg) {
        return -1;
    }
    return obmm_coh_send_ub_link_msg(ubc_dev, dcna, sub_msg_code,
                                     msg, sizeof(*msg));
}

static void gsva_coh_send_ack(BusControllerDev *ubc_dev,
                               const GsvaCohMsgV1 *req,
                               uint8_t ack_subcode, uint32_t error)
{
    GsvaCohMsgV1 ack = *req;
    ack.source_cna = req->target_cna;
    ack.target_cna = req->source_cna;
    ack.error = error;
    gsva_coh_send_ub_link_msg(ubc_dev, req->source_cna, ack_subcode, &ack);
}

void gsva_coh_handle_rx_inv(BusControllerDev *ubc_dev, const GsvaCohMsgV1 *msg)
{
    qemu_log("GSVA_COH: rx INV from cna=%" PRIu32 " segment_id=%#" PRIx64
             " seq=%" PRIu64 "\n", msg->source_cna,
             msg->key.segment_id, msg->seq);
    /* Drop local GSVA state for the range — V1 sim: log and ACK */
    gsva_coh_send_ack(ubc_dev, msg, UBC_MSG_SUB_GSVA_COH_INV_ACK, GSVA_OK);
}

void gsva_coh_handle_rx_inv_ack(BusControllerDev *ubc_dev, const GsvaCohMsgV1 *msg)
{
    qemu_log("GSVA_COH: rx INV_ACK from cna=%" PRIu32 " segment_id=%#" PRIx64
             " seq=%" PRIu64 "\n", msg->source_cna,
             msg->key.segment_id, msg->seq);
}

void gsva_coh_handle_rx_downgrade(BusControllerDev *ubc_dev, const GsvaCohMsgV1 *msg)
{
    qemu_log("GSVA_COH: rx DOWNGRADE from cna=%" PRIu32 " segment_id=%#" PRIx64
             " seq=%" PRIu64 "\n", msg->source_cna,
             msg->key.segment_id, msg->seq);
    gsva_coh_send_ack(ubc_dev, msg, UBC_MSG_SUB_GSVA_COH_DOWNGRADE_ACK, GSVA_OK);
}

void gsva_coh_handle_rx_downgrade_ack(BusControllerDev *ubc_dev, const GsvaCohMsgV1 *msg)
{
    qemu_log("GSVA_COH: rx DOWNGRADE_ACK from cna=%" PRIu32
             " segment_id=%#" PRIx64 " seq=%" PRIu64 "\n",
             msg->source_cna, msg->key.segment_id, msg->seq);
}

void gsva_coh_handle_rx_wb(BusControllerDev *ubc_dev, const GsvaCohMsgV1 *msg)
{
    qemu_log("GSVA_COH: rx WRITEBACK from cna=%" PRIu32 " segment_id=%#" PRIx64
             " seq=%" PRIu64 "\n", msg->source_cna,
             msg->key.segment_id, msg->seq);
    gsva_coh_send_ack(ubc_dev, msg, UBC_MSG_SUB_GSVA_COH_RETIRE_ACK, GSVA_OK);
}

void gsva_coh_handle_rx_wb_ack(BusControllerDev *ubc_dev, const GsvaCohMsgV1 *msg)
{
    qemu_log("GSVA_COH: rx WRITEBACK_ACK from cna=%" PRIu32
             " segment_id=%#" PRIx64 " seq=%" PRIu64 "\n",
             msg->source_cna, msg->key.segment_id, msg->seq);
}

void gsva_coh_handle_rx_fence(BusControllerDev *ubc_dev, const GsvaCohMsgV1 *msg)
{
    qemu_log("GSVA_COH: rx FENCE from cna=%" PRIu32 " segment_id=%#" PRIx64
             " seq=%" PRIu64 "\n", msg->source_cna,
             msg->key.segment_id, msg->seq);
    gsva_coh_send_ack(ubc_dev, msg, UBC_MSG_SUB_GSVA_COH_FENCE_ACK, GSVA_OK);
}

void gsva_coh_handle_rx_fence_ack(BusControllerDev *ubc_dev, const GsvaCohMsgV1 *msg)
{
    qemu_log("GSVA_COH: rx FENCE_ACK from cna=%" PRIu32
             " segment_id=%#" PRIx64 " seq=%" PRIu64 "\n",
             msg->source_cna, msg->key.segment_id, msg->seq);
}

void gsva_coh_handle_rx_retire(BusControllerDev *ubc_dev, const GsvaCohMsgV1 *msg)
{
    qemu_log("GSVA_COH: rx RETIRE from cna=%" PRIu32 " segment_id=%#" PRIx64
             " seq=%" PRIu64 "\n", msg->source_cna,
             msg->key.segment_id, msg->seq);
    gsva_coh_send_ack(ubc_dev, msg, UBC_MSG_SUB_GSVA_COH_RETIRE_ACK, GSVA_OK);
}

void gsva_coh_handle_rx_retire_ack(BusControllerDev *ubc_dev, const GsvaCohMsgV1 *msg)
{
    qemu_log("GSVA_COH: rx RETIRE_ACK from cna=%" PRIu32
             " segment_id=%#" PRIx64 " seq=%" PRIu64 "\n",
             msg->source_cna, msg->key.segment_id, msg->seq);
}

void gsva_coh_handle_rx_token_revoke(BusControllerDev *ubc_dev, const GsvaCohMsgV1 *msg)
{
    qemu_log("GSVA_COH: rx TOKEN_REVOKE from cna=%" PRIu32
             " segment_id=%#" PRIx64 " seq=%" PRIu64 "\n",
             msg->source_cna, msg->key.segment_id, msg->seq);
    gsva_coh_send_ack(ubc_dev, msg, UBC_MSG_SUB_GSVA_COH_TOKEN_ACK, GSVA_OK);
}

void gsva_coh_handle_rx_token_ack(BusControllerDev *ubc_dev, const GsvaCohMsgV1 *msg)
{
    qemu_log("GSVA_COH: rx TOKEN_ACK from cna=%" PRIu32
             " segment_id=%#" PRIx64 " seq=%" PRIu64 "\n",
             msg->source_cna, msg->key.segment_id, msg->seq);
}

void gsva_coh_dispatch_rx(BusControllerDev *ubc_dev, uint8_t sub_msg_code,
                           const void *payload, uint32_t payload_len)
{
    const GsvaCohMsgV1 *msg;

    if (payload_len < sizeof(GsvaCohMsgV1)) {
        qemu_log("GSVA_COH: short payload %u < %zu subcode=%u\n",
                 payload_len, sizeof(GsvaCohMsgV1), sub_msg_code);
        return;
    }
    msg = (const GsvaCohMsgV1 *)payload;

    switch (sub_msg_code) {
    case UBC_MSG_SUB_GSVA_COH_INV:
        gsva_coh_handle_rx_inv(ubc_dev, msg);
        break;
    case UBC_MSG_SUB_GSVA_COH_INV_ACK:
        gsva_coh_handle_rx_inv_ack(ubc_dev, msg);
        break;
    case UBC_MSG_SUB_GSVA_COH_DOWNGRADE:
        gsva_coh_handle_rx_downgrade(ubc_dev, msg);
        break;
    case UBC_MSG_SUB_GSVA_COH_DOWNGRADE_ACK:
        gsva_coh_handle_rx_downgrade_ack(ubc_dev, msg);
        break;
    case UBC_MSG_SUB_GSVA_COH_RETIRE_REQ:
        gsva_coh_handle_rx_retire(ubc_dev, msg);
        break;
    case UBC_MSG_SUB_GSVA_COH_RETIRE_ACK:
        gsva_coh_handle_rx_retire_ack(ubc_dev, msg);
        break;
    case UBC_MSG_SUB_GSVA_COH_FENCE:
        gsva_coh_handle_rx_fence(ubc_dev, msg);
        break;
    case UBC_MSG_SUB_GSVA_COH_FENCE_ACK:
        gsva_coh_handle_rx_fence_ack(ubc_dev, msg);
        break;
    case UBC_MSG_SUB_GSVA_COH_TOKEN_REVOKE:
        gsva_coh_handle_rx_token_revoke(ubc_dev, msg);
        break;
    case UBC_MSG_SUB_GSVA_COH_TOKEN_ACK:
        gsva_coh_handle_rx_token_ack(ubc_dev, msg);
        break;
    default:
        qemu_log("GSVA_COH: unhandled subcode=%u\n", sub_msg_code);
        break;
    }
}

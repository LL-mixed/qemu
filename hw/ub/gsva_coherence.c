/*
 * GSVA coherence.
 *
 * GSVA object state, ReadAcquire/WriteAcquire,
 * retire orchestration, pending sequence handling.
 */

#include "qemu/osdep.h"
#include "hw/ub/gsva_coherence.h"
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

int gsva_coh_read_acquire(GsvaCohTable *tbl, const GsvaKeyV1 *key,
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

    /* Epoch check */
    if (key->epoch != obj->epoch) {
        qemu_log("GSVA_COH: ReadAcquire stale epoch: req=%" PRIu64
                 " active=%" PRIu64 "\n", key->epoch, obj->epoch);
        return GSVA_ERR_STALE_EPOCH;
    }

    if (obj->state == GSVA_COH_RETIRED) {
        return GSVA_ERR_SEGMENT_RETIRED;
    }

    if (obj->state == GSVA_COH_TIMEOUT) {
        return GSVA_ERR_COH_PENDING;
    }

    if (obj->pending) {
        return GSVA_ERR_COH_PENDING;
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

int gsva_coh_write_acquire(GsvaCohTable *tbl, const GsvaKeyV1 *key,
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

    /* Epoch check */
    if (key->epoch != obj->epoch) {
        qemu_log("GSVA_COH: WriteAcquire stale epoch: req=%" PRIu64
                 " active=%" PRIu64 "\n", key->epoch, obj->epoch);
        return GSVA_ERR_STALE_EPOCH;
    }

    if (obj->state == GSVA_COH_RETIRED) {
        return GSVA_ERR_SEGMENT_RETIRED;
    }

    if (obj->state == GSVA_COH_TIMEOUT) {
        return GSVA_ERR_COH_PENDING;
    }

    if (obj->pending) {
        return GSVA_ERR_COH_PENDING;
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

    case GSVA_COH_S:
        /* In full impl: need to invalidate other sharers.
         * For V1 sim: directly grant M to requester. */
        obj->state = GSVA_COH_M;
        obj->owner_cna = requester_cna;
        obj->sharer_bitmap = 0;
        qemu_log("GSVA_COH: WriteAcquire S->M cna=%" PRIu32
                 " segment_id=%#" PRIx64 "\n",
                 requester_cna, key->segment_id);
        break;

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

    /* V1 sim: directly retire, no pending revoke in sim mode.
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

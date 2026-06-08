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

static GsvaCohTable *g_gsva_coh_default_table;
static GsvaRouteTable *g_gsva_route_default_table;

void gsva_coh_set_default_table(GsvaCohTable *tbl)
{
    g_gsva_coh_default_table = tbl;
}

void gsva_coh_set_default_route_table(GsvaRouteTable *tbl)
{
    g_gsva_route_default_table = tbl;
}

void gsva_coh_table_init(GsvaCohTable *tbl)
{
    QTAILQ_INIT(&tbl->objects);
    tbl->next_seq = 1;
    tbl->object_count = 0;
}

static uint64_t gsva_coh_timeout_ms(void)
{
    const char *env = g_getenv("GSVA_COH_TIMEOUT_MS");
    char *end = NULL;
    uint64_t value = 5000;

    if (env && *env) {
        value = g_ascii_strtoull(env, &end, 0);
        if (end == env) {
            value = 5000;
        }
    }
    return value;
}

static bool gsva_coh_hold_pending_enabled(void)
{
    const char *env = g_getenv("GSVA_COH_HOLD_PENDING");

    return env && g_strcmp0(env, "1") == 0;
}

static bool gsva_coh_ub_link_tx_enabled(void)
{
    const char *env = g_getenv("GSVA_COH_UB_LINK_TX");

    return env && g_strcmp0(env, "1") == 0;
}

static void gsva_coh_set_bitmap_bit(uint64_t *bitmap, uint32_t cna)
{
    if (cna < 64) {
        *bitmap |= (1ULL << cna);
    }
}

static void gsva_coh_clear_bitmap_bit(uint64_t *bitmap, uint32_t cna)
{
    if (cna < 64) {
        *bitmap &= ~(1ULL << cna);
    }
}

static bool gsva_coh_sharer_has(const GsvaCohObject *obj, uint32_t cna)
{
    uint32_t i;

    for (i = 0; obj && i < obj->sharer_count; i++) {
        if (obj->sharer_cnas[i] == cna) {
            return true;
        }
    }
    return false;
}

static void gsva_coh_sharer_add(GsvaCohObject *obj, uint32_t cna)
{
    if (!obj || gsva_coh_sharer_has(obj, cna)) {
        return;
    }
    if (obj->sharer_count < GSVA_COH_MAX_HOLDERS) {
        obj->sharer_cnas[obj->sharer_count++] = cna;
    }
    gsva_coh_set_bitmap_bit(&obj->sharer_bitmap, cna);
}

static void gsva_coh_sharers_clear(GsvaCohObject *obj)
{
    if (!obj) {
        return;
    }
    obj->sharer_bitmap = 0;
    obj->sharer_count = 0;
    memset(obj->sharer_cnas, 0, sizeof(obj->sharer_cnas));
}

static void gsva_coh_pending_clear(GsvaCohObject *obj)
{
    if (!obj) {
        return;
    }
    obj->pending_ack_bitmap = 0;
    obj->pending_ack_count = 0;
    memset(obj->pending_ack_cnas, 0, sizeof(obj->pending_ack_cnas));
}

static bool gsva_coh_pending_has(const GsvaCohObject *obj, uint32_t cna)
{
    uint32_t i;

    for (i = 0; obj && i < obj->pending_ack_count; i++) {
        if (obj->pending_ack_cnas[i] == cna) {
            return true;
        }
    }
    return false;
}

static void gsva_coh_pending_add(GsvaCohObject *obj, uint32_t cna)
{
    if (!obj || gsva_coh_pending_has(obj, cna)) {
        return;
    }
    if (obj->pending_ack_count < GSVA_COH_MAX_HOLDERS) {
        obj->pending_ack_cnas[obj->pending_ack_count++] = cna;
    }
    gsva_coh_set_bitmap_bit(&obj->pending_ack_bitmap, cna);
}

static void gsva_coh_pending_remove(GsvaCohObject *obj, uint32_t cna)
{
    uint32_t i;

    if (!obj) {
        return;
    }
    for (i = 0; i < obj->pending_ack_count; i++) {
        if (obj->pending_ack_cnas[i] == cna) {
            obj->pending_ack_cnas[i] =
                obj->pending_ack_cnas[obj->pending_ack_count - 1];
            obj->pending_ack_cnas[obj->pending_ack_count - 1] = 0;
            obj->pending_ack_count--;
            break;
        }
    }
    gsva_coh_clear_bitmap_bit(&obj->pending_ack_bitmap, cna);
}

static bool gsva_coh_object_maybe_timeout(GsvaCohObject *obj,
                                           uint64_t now_ms,
                                           uint64_t timeout_ms)
{
    uint64_t start_ms;
    uint64_t elapsed;

    if (!obj || !obj->pending || obj->state == GSVA_COH_RETIRED ||
        obj->state == GSVA_COH_TIMEOUT || timeout_ms == 0) {
        return false;
    }

    start_ms = obj->pending_start_ms ? obj->pending_start_ms :
                                       obj->create_time_ms;
    elapsed = now_ms >= start_ms ? now_ms - start_ms : 0;
    if (elapsed < timeout_ms) {
        return false;
    }

    qemu_log("GSVA_COH: TIMEOUT segment_id=%#" PRIx64
             " seq=%" PRIu64 " elapsed=%" PRIu64 "ms"
             " waiting_for=%#" PRIx64 "\n",
             obj->key.segment_id, obj->pending_seq, elapsed,
             obj->pending_ack_bitmap);
    obj->state = GSVA_COH_TIMEOUT;
    obj->pending = false;
    gsva_coh_pending_clear(obj);
    obj->pending_start_ms = 0;
    return true;
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
    obj->sharer_count = 0;
    obj->epoch = key->epoch;
    obj->pending = false;
    obj->pending_seq = 0;
    obj->pending_op = 0;
    obj->pending_target = 0;
    obj->pending_ack_bitmap = 0;
    obj->pending_ack_count = 0;
    obj->pending_start_ms = 0;
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

int gsva_coh_read_acquire_tx(GsvaCohTable *tbl, const GsvaRouteTable *routes,
                             BusControllerDev *ubc_dev,
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

    if (gsva_coh_object_maybe_timeout(obj,
                                      qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL),
                                      gsva_coh_timeout_ms())) {
        return GSVA_ERR_COH_TIMEOUT;
    }

    if (obj->state == GSVA_COH_TIMEOUT) {
        qemu_log("GSVA_COH: ReadAcquire timeout segment_id=%#" PRIx64
                 " cna=%" PRIu32 "\n",
                 key->segment_id, requester_cna);
        return GSVA_ERR_COH_TIMEOUT;
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
        gsva_coh_sharer_add(obj, requester_cna);
        qemu_log("GSVA_COH: ReadAcquire I->S cna=%" PRIu32
                 " segment_id=%#" PRIx64 "\n",
                 requester_cna, key->segment_id);
        break;

    case GSVA_COH_S:
        gsva_coh_sharer_add(obj, requester_cna);
        qemu_log("GSVA_COH: ReadAcquire S->S cna=%" PRIu32
                 " segment_id=%#" PRIx64 "\n",
                 requester_cna, key->segment_id);
        break;

    case GSVA_COH_E:
        if (obj->owner_cna != 0 && obj->owner_cna != requester_cna &&
            ubc_dev && gsva_coh_ub_link_tx_enabled()) {
            uint32_t old_owner = obj->owner_cna;
            GsvaCohMsgV1 downgrade = {0};
            int tx_rc;

            obj->pending = true;
            obj->pending_seq = ++tbl->next_seq;
            obj->pending_op = 4; /* downgrade */
            obj->pending_target = requester_cna;
            gsva_coh_pending_clear(obj);
            gsva_coh_pending_add(obj, old_owner);
            obj->pending_start_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
            qemu_log("GSVA_COH: ReadAcquire E->S pending downgrade"
                     " cna=%" PRIu32 " owner=%" PRIu32
                     " waiting_for=%#" PRIx64 " seq=%" PRIu64 "\n",
                     requester_cna, old_owner, obj->pending_ack_bitmap,
                     obj->pending_seq);

            downgrade.version = 1;
            downgrade.op = GSVA_COH_MSG_DOWNGRADE;
            downgrade.seq = obj->pending_seq;
            downgrade.source_cna = requester_cna;
            downgrade.target_cna = old_owner;
            downgrade.key = *key;
            downgrade.access_va = key->home_va;
            downgrade.access_len = key->size;
            downgrade.access_flags = 1;
            tx_rc = gsva_coh_send_ub_link_msg(
                    ubc_dev, old_owner, UBC_MSG_SUB_GSVA_COH, &downgrade);
            qemu_log("GSVA_COH: tx DOWNGRADE target=%" PRIu32
                     " seq=%" PRIu64 " segment_id=%#" PRIx64
                     " rc=%d\n",
                     old_owner, obj->pending_seq, key->segment_id, tx_rc);

            if (gsva_coh_hold_pending_enabled()) {
                qemu_log("GSVA_COH: pending held seq=%" PRIu64
                         " segment_id=%#" PRIx64
                         " timeout_ms=%" PRIu64 "\n",
                         obj->pending_seq, key->segment_id,
                         gsva_coh_timeout_ms());
                return GSVA_ERR_COH_PENDING;
            }

            gsva_coh_pending_clear(obj);
            obj->pending = false;
            obj->pending_start_ms = 0;
            obj->pending_op = 0;
            obj->pending_target = 0;
        }
        /* Owner becomes sharer, requester becomes sharer */
        gsva_coh_sharer_add(obj, obj->owner_cna);
        gsva_coh_sharer_add(obj, requester_cna);
        obj->owner_cna = 0;
        obj->state = GSVA_COH_S;
        qemu_log("GSVA_COH: ReadAcquire E->S cna=%" PRIu32
                 " segment_id=%#" PRIx64 "\n",
                 requester_cna, key->segment_id);
        break;

    case GSVA_COH_M:
        if (obj->owner_cna != 0 && obj->owner_cna != requester_cna &&
            ubc_dev && gsva_coh_ub_link_tx_enabled()) {
            uint32_t old_owner = obj->owner_cna;
            GsvaCohMsgV1 downgrade = {0};
            int tx_rc;

            obj->pending = true;
            obj->pending_seq = ++tbl->next_seq;
            obj->pending_op = 4; /* downgrade */
            obj->pending_target = requester_cna;
            gsva_coh_pending_clear(obj);
            gsva_coh_pending_add(obj, old_owner);
            obj->pending_start_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
            qemu_log("GSVA_COH: ReadAcquire M->S pending downgrade"
                     " cna=%" PRIu32 " owner=%" PRIu32
                     " waiting_for=%#" PRIx64 " seq=%" PRIu64 "\n",
                     requester_cna, old_owner, obj->pending_ack_bitmap,
                     obj->pending_seq);

            downgrade.version = 1;
            downgrade.op = GSVA_COH_MSG_DOWNGRADE;
            downgrade.seq = obj->pending_seq;
            downgrade.source_cna = requester_cna;
            downgrade.target_cna = old_owner;
            downgrade.key = *key;
            downgrade.access_va = key->home_va;
            downgrade.access_len = key->size;
            downgrade.access_flags = 1;
            tx_rc = gsva_coh_send_ub_link_msg(
                    ubc_dev, old_owner, UBC_MSG_SUB_GSVA_COH, &downgrade);
            qemu_log("GSVA_COH: tx DOWNGRADE target=%" PRIu32
                     " seq=%" PRIu64 " segment_id=%#" PRIx64
                     " rc=%d\n",
                     old_owner, obj->pending_seq, key->segment_id, tx_rc);

            if (gsva_coh_hold_pending_enabled()) {
                qemu_log("GSVA_COH: pending held seq=%" PRIu64
                         " segment_id=%#" PRIx64
                         " timeout_ms=%" PRIu64 "\n",
                         obj->pending_seq, key->segment_id,
                         gsva_coh_timeout_ms());
                return GSVA_ERR_COH_PENDING;
            }

            gsva_coh_pending_clear(obj);
            obj->pending = false;
            obj->pending_start_ms = 0;
            obj->pending_op = 0;
            obj->pending_target = 0;
        }
        /* Owner must writeback or data-forward, then S */
        gsva_coh_sharer_add(obj, obj->owner_cna);
        gsva_coh_sharer_add(obj, requester_cna);
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

int gsva_coh_read_acquire(GsvaCohTable *tbl, const GsvaRouteTable *routes,
                          const GsvaKeyV1 *key, uint32_t requester_cna,
                          uint32_t token_id, uint32_t token_value)
{
    return gsva_coh_read_acquire_tx(tbl, routes, NULL, key, requester_cna,
                                    token_id, token_value);
}

int gsva_coh_write_acquire_tx(GsvaCohTable *tbl, const GsvaRouteTable *routes,
                              BusControllerDev *ubc_dev,
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

    if (gsva_coh_object_maybe_timeout(obj,
                                      qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL),
                                      gsva_coh_timeout_ms())) {
        return GSVA_ERR_COH_TIMEOUT;
    }

    if (obj->state == GSVA_COH_TIMEOUT) {
        qemu_log("GSVA_COH: WriteAcquire timeout segment_id=%#" PRIx64
                 " cna=%" PRIu32 "\n",
                 key->segment_id, requester_cna);
        return GSVA_ERR_COH_TIMEOUT;
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
        gsva_coh_sharers_clear(obj);
        qemu_log("GSVA_COH: WriteAcquire I->M cna=%" PRIu32
                 " segment_id=%#" PRIx64 "\n",
                 requester_cna, key->segment_id);
        break;

    case GSVA_COH_S: {
        /* Invalidate other sharers before granting M.
         * V1 keeps bitmap diagnostics, and also tracks full-width CNAs for
         * UB Link targets because simulator CNAs are not restricted to 0..63.
         */
        uint32_t target_count = 0;
        uint32_t targets[GSVA_COH_MAX_HOLDERS] = {0};
        uint64_t other_sharers = obj->sharer_bitmap;
        uint32_t i;

        gsva_coh_clear_bitmap_bit(&other_sharers, requester_cna);
        for (i = 0; i < obj->sharer_count; i++) {
            uint32_t cna = obj->sharer_cnas[i];

            if (cna == requester_cna) {
                continue;
            }
            if (target_count < GSVA_COH_MAX_HOLDERS) {
                targets[target_count++] = cna;
            }
        }

        if (target_count == 0 && other_sharers != 0) {
            for (i = 0; i < 64 && target_count < GSVA_COH_MAX_HOLDERS; i++) {
                if (other_sharers & (1ULL << i)) {
                    targets[target_count++] = i;
                }
            }
        }

        if (target_count > 0) {
            obj->pending = true;
            obj->pending_seq = ++tbl->next_seq;
            obj->pending_op = 1; /* invalidate */
            obj->pending_target = requester_cna;
            gsva_coh_pending_clear(obj);
            for (i = 0; i < target_count; i++) {
                gsva_coh_pending_add(obj, targets[i]);
            }
            obj->pending_start_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
            qemu_log("GSVA_COH: WriteAcquire S->M pending inv"
                     " cna=%" PRIu32 " waiting_for=%#" PRIx64
                     " seq=%" PRIu64 " targets=%" PRIu32 "\n",
                     requester_cna, obj->pending_ack_bitmap,
                     obj->pending_seq, target_count);

            if (ubc_dev && gsva_coh_ub_link_tx_enabled()) {
                for (i = 0; i < target_count; i++) {
                    GsvaCohMsgV1 inv = {0};
                    int tx_rc;

                    if (targets[i] == ubc_dev->parent.cna) {
                        continue;
                    }
                    inv.version = 1;
                    inv.op = GSVA_COH_MSG_INVALIDATE;
                    inv.seq = obj->pending_seq;
                    inv.source_cna = requester_cna;
                    inv.target_cna = targets[i];
                    inv.key = *key;
                    inv.access_va = key->home_va;
                    inv.access_len = key->size;
                    inv.access_flags = 2;
                    tx_rc = gsva_coh_send_ub_link_msg(
                            ubc_dev, targets[i], UBC_MSG_SUB_GSVA_COH,
                            &inv);
                    qemu_log("GSVA_COH: tx INV target=%" PRIu32
                             " seq=%" PRIu64 " segment_id=%#" PRIx64
                             " rc=%d\n",
                             targets[i], obj->pending_seq, key->segment_id,
                             tx_rc);
                }
            }

            if (gsva_coh_hold_pending_enabled()) {
                qemu_log("GSVA_COH: pending held seq=%" PRIu64
                         " segment_id=%#" PRIx64
                         " timeout_ms=%" PRIu64 "\n",
                         obj->pending_seq, key->segment_id,
                         gsva_coh_timeout_ms());
                return GSVA_ERR_COH_PENDING;
            }

            /* V1 sim: immediately acknowledge all invalidations */
            gsva_coh_pending_clear(obj);
            obj->pending = false;
            obj->pending_start_ms = 0;
        }
        obj->state = GSVA_COH_M;
        obj->owner_cna = requester_cna;
        gsva_coh_sharers_clear(obj);
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
            gsva_coh_sharers_clear(obj);
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
            uint32_t old_owner = obj->owner_cna;

            obj->pending = true;
            obj->pending_seq = ++tbl->next_seq;
            obj->pending_op = 2; /* writeback */
            obj->pending_target = requester_cna;
            gsva_coh_pending_clear(obj);
            gsva_coh_pending_add(obj, old_owner);
            obj->pending_start_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
            qemu_log("GSVA_COH: WriteAcquire M->M pending wb"
                     " cna=%" PRIu32 " owner=%" PRIu32
                     " waiting_for=%#" PRIx64 " seq=%" PRIu64 "\n",
                     requester_cna, old_owner, obj->pending_ack_bitmap,
                     obj->pending_seq);

            if (ubc_dev && gsva_coh_ub_link_tx_enabled() &&
                old_owner != ubc_dev->parent.cna) {
                GsvaCohMsgV1 wb = {0};
                int tx_rc;

                wb.version = 1;
                wb.op = GSVA_COH_MSG_WRITEBACK;
                wb.seq = obj->pending_seq;
                wb.source_cna = requester_cna;
                wb.target_cna = old_owner;
                wb.key = *key;
                wb.access_va = key->home_va;
                wb.access_len = key->size;
                wb.access_flags = 2;
                tx_rc = gsva_coh_send_ub_link_msg(
                        ubc_dev, old_owner, UBC_MSG_SUB_GSVA_COH, &wb);
                qemu_log("GSVA_COH: tx WRITEBACK target=%" PRIu32
                         " seq=%" PRIu64 " segment_id=%#" PRIx64
                         " rc=%d\n",
                         old_owner, obj->pending_seq, key->segment_id,
                         tx_rc);
            }

            if (gsva_coh_hold_pending_enabled()) {
                qemu_log("GSVA_COH: pending held seq=%" PRIu64
                         " segment_id=%#" PRIx64
                         " timeout_ms=%" PRIu64 "\n",
                         obj->pending_seq, key->segment_id,
                         gsva_coh_timeout_ms());
                return GSVA_ERR_COH_PENDING;
            }

            gsva_coh_pending_clear(obj);
            obj->pending = false;
            obj->pending_start_ms = 0;
            obj->pending_op = 0;
            obj->pending_target = 0;
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

int gsva_coh_write_acquire(GsvaCohTable *tbl, const GsvaRouteTable *routes,
                           const GsvaKeyV1 *key, uint32_t requester_cna,
                           uint32_t token_id, uint32_t token_value)
{
    return gsva_coh_write_acquire_tx(tbl, routes, NULL, key, requester_cna,
                                     token_id, token_value);
}

int gsva_coh_retire_tx(GsvaCohTable *tbl, BusControllerDev *ubc_dev,
                       const GsvaKeyV1 *key, uint32_t requester_cna)
{
    GsvaCohObject *obj;
    uint32_t targets[GSVA_COH_MAX_HOLDERS] = {0};
    uint32_t target_count = 0;
    uint32_t i;

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

    if ((obj->state == GSVA_COH_E || obj->state == GSVA_COH_M) &&
        obj->owner_cna != 0 && obj->owner_cna != requester_cna) {
        targets[target_count++] = obj->owner_cna;
    }
    if (obj->state == GSVA_COH_S) {
        for (i = 0; i < obj->sharer_count; i++) {
            uint32_t cna = obj->sharer_cnas[i];

            if (cna == requester_cna || gsva_coh_pending_has(obj, cna)) {
                continue;
            }
            if (target_count < GSVA_COH_MAX_HOLDERS) {
                targets[target_count++] = cna;
            }
        }
    }

    if (target_count > 0) {
        obj->pending = true;
        obj->pending_seq = ++tbl->next_seq;
        obj->pending_op = 3; /* retire */
        obj->pending_target = requester_cna;
        gsva_coh_pending_clear(obj);
        for (i = 0; i < target_count; i++) {
            gsva_coh_pending_add(obj, targets[i]);
        }
        obj->pending_start_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
        qemu_log("GSVA_COH: Retire pending revoke"
                 " requester=%" PRIu32 " waiting_for=%#" PRIx64
                 " seq=%" PRIu64 " targets=%" PRIu32 "\n",
                 requester_cna, obj->pending_ack_bitmap,
                 obj->pending_seq, target_count);

        if (ubc_dev && gsva_coh_ub_link_tx_enabled()) {
            for (i = 0; i < target_count; i++) {
                GsvaCohMsgV1 retire = {0};
                int tx_rc;

                if (targets[i] == ubc_dev->parent.cna) {
                    continue;
                }
                retire.version = 1;
                retire.op = GSVA_COH_MSG_RETIRE;
                retire.seq = obj->pending_seq;
                retire.source_cna = requester_cna;
                retire.target_cna = targets[i];
                retire.key = *key;
                retire.access_va = key->home_va;
                retire.access_len = key->size;
                tx_rc = gsva_coh_send_ub_link_msg(
                        ubc_dev, targets[i], UBC_MSG_SUB_GSVA_COH,
                        &retire);
                qemu_log("GSVA_COH: tx RETIRE target=%" PRIu32
                         " seq=%" PRIu64 " segment_id=%#" PRIx64
                         " rc=%d\n",
                         targets[i], obj->pending_seq, key->segment_id,
                         tx_rc);
            }
        }

        if (gsva_coh_hold_pending_enabled()) {
            qemu_log("GSVA_COH: pending held seq=%" PRIu64
                     " segment_id=%#" PRIx64
                     " timeout_ms=%" PRIu64 "\n",
                     obj->pending_seq, key->segment_id,
                     gsva_coh_timeout_ms());
            return GSVA_ERR_COH_PENDING;
        }

        gsva_coh_pending_clear(obj);
        obj->pending = false;
        obj->pending_start_ms = 0;
    }

    /* V1 sim: directly retire after holder revoke accounting.
     * Also allow retiring from TIMEOUT state for cleanup. */
    obj->state = GSVA_COH_RETIRED;
    obj->owner_cna = 0;
    gsva_coh_sharers_clear(obj);
    obj->pending = false;
    gsva_coh_pending_clear(obj);

    qemu_log("GSVA_RETIRE: segment_id=%#" PRIx64 " home_va=%#" PRIx64
             " epoch=%" PRIu64 " RETIRED\n",
             key->segment_id, key->home_va, key->epoch);

    return GSVA_OK;
}

int gsva_coh_retire(GsvaCohTable *tbl, const GsvaKeyV1 *key,
                    uint32_t requester_cna)
{
    return gsva_coh_retire_tx(tbl, NULL, key, requester_cna);
}

int gsva_coh_token_revoke_tx(GsvaCohTable *tbl, BusControllerDev *ubc_dev,
                             const GsvaKeyV1 *key, uint32_t requester_cna,
                             uint32_t token_id, uint32_t new_token_value)
{
    GsvaCohObject *obj;
    uint32_t targets[GSVA_COH_MAX_HOLDERS] = {0};
    uint32_t target_count = 0;
    uint32_t i;

    if (!tbl || !key) {
        return GSVA_ERR_BAD_VERSION;
    }
    if (token_id == 0 || new_token_value == 0) {
        return GSVA_ERR_TOKEN_DENIED;
    }

    obj = gsva_coh_lookup(tbl, key);
    if (!obj) {
        return GSVA_ERR_ROUTE_MISSING;
    }

    if (obj->state == GSVA_COH_E || obj->state == GSVA_COH_M) {
        if (obj->owner_cna != 0 && obj->owner_cna != requester_cna) {
            targets[target_count++] = obj->owner_cna;
        }
    } else if (obj->state == GSVA_COH_S) {
        for (i = 0; i < obj->sharer_count; i++) {
            uint32_t cna = obj->sharer_cnas[i];

            if (cna == requester_cna) {
                continue;
            }
            if (target_count < GSVA_COH_MAX_HOLDERS) {
                targets[target_count++] = cna;
            }
        }
    }

    qemu_log("GSVA_COH: TokenRevoke holders segment_id=%#" PRIx64
             " requester=%" PRIu32 " targets=%" PRIu32
             " token_id=%" PRIu32 "\n",
             key->segment_id, requester_cna, target_count, token_id);

    if (!ubc_dev || !gsva_coh_ub_link_tx_enabled()) {
        return GSVA_OK;
    }

    for (i = 0; i < target_count; i++) {
        GsvaCohMsgV1 revoke = {0};
        int tx_rc;

        if (targets[i] == ubc_dev->parent.cna) {
            continue;
        }
        revoke.version = 1;
        revoke.op = GSVA_COH_MSG_TOKEN_REVOKE;
        revoke.seq = 0;
        revoke.source_cna = requester_cna;
        revoke.target_cna = targets[i];
        revoke.key = *key;
        revoke.access_va = key->home_va;
        revoke.access_len = new_token_value;
        revoke.access_flags = token_id;
        tx_rc = gsva_coh_send_ub_link_msg(
                ubc_dev, targets[i], UBC_MSG_SUB_GSVA_COH, &revoke);
        qemu_log("GSVA_COH: tx TOKEN_REVOKE target=%" PRIu32
                 " segment_id=%#" PRIx64 " token_id=%" PRIu32
                 " rc=%d\n",
                 targets[i], key->segment_id, token_id, tx_rc);
    }

    return GSVA_OK;
}

int gsva_coh_fence_tx(GsvaCohTable *tbl, BusControllerDev *ubc_dev,
                      const GsvaKeyV1 *key, uint32_t requester_cna)
{
    GsvaCohObject *obj;
    uint32_t targets[GSVA_COH_MAX_HOLDERS] = {0};
    uint32_t target_count = 0;
    uint32_t i;

    if (!tbl || !key) {
        return GSVA_ERR_BAD_VERSION;
    }

    obj = gsva_coh_lookup(tbl, key);
    if (!obj) {
        return GSVA_ERR_ROUTE_MISSING;
    }

    if (obj->pending) {
        return GSVA_ERR_COH_PENDING;
    }
    if (obj->state == GSVA_COH_RETIRED) {
        return GSVA_ERR_SEGMENT_RETIRED;
    }
    if (obj->state == GSVA_COH_TIMEOUT) {
        return GSVA_ERR_COH_TIMEOUT;
    }

    if (obj->state == GSVA_COH_E || obj->state == GSVA_COH_M) {
        if (obj->owner_cna != 0 && obj->owner_cna != requester_cna) {
            targets[target_count++] = obj->owner_cna;
        }
    } else if (obj->state == GSVA_COH_S) {
        for (i = 0; i < obj->sharer_count; i++) {
            uint32_t cna = obj->sharer_cnas[i];

            if (cna == requester_cna) {
                continue;
            }
            if (target_count < GSVA_COH_MAX_HOLDERS) {
                targets[target_count++] = cna;
            }
        }
    }

    qemu_log("GSVA_COH: Fence holders segment_id=%#" PRIx64
             " requester=%" PRIu32 " targets=%" PRIu32 "\n",
             key->segment_id, requester_cna, target_count);

    if (target_count == 0) {
        return GSVA_OK;
    }

    obj->pending = true;
    obj->pending_seq = ++tbl->next_seq;
    obj->pending_op = 5; /* fence */
    obj->pending_target = requester_cna;
    gsva_coh_pending_clear(obj);
    for (i = 0; i < target_count; i++) {
        gsva_coh_pending_add(obj, targets[i]);
    }
    obj->pending_start_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    qemu_log("GSVA_COH: Fence pending requester=%" PRIu32
             " waiting_for=%#" PRIx64 " seq=%" PRIu64
             " targets=%" PRIu32 "\n",
             requester_cna, obj->pending_ack_bitmap, obj->pending_seq,
             target_count);

    if (ubc_dev && gsva_coh_ub_link_tx_enabled()) {
        for (i = 0; i < target_count; i++) {
            GsvaCohMsgV1 fence = {0};
            int tx_rc;

            if (targets[i] == ubc_dev->parent.cna) {
                continue;
            }
            fence.version = 1;
            fence.op = GSVA_COH_MSG_FENCE;
            fence.seq = obj->pending_seq;
            fence.source_cna = requester_cna;
            fence.target_cna = targets[i];
            fence.key = *key;
            fence.access_va = key->home_va;
            fence.access_len = key->size;
            fence.access_flags = 0;
            tx_rc = gsva_coh_send_ub_link_msg(
                    ubc_dev, targets[i], UBC_MSG_SUB_GSVA_COH, &fence);
            qemu_log("GSVA_COH: tx FENCE target=%" PRIu32
                     " seq=%" PRIu64 " segment_id=%#" PRIx64
                     " rc=%d\n",
                     targets[i], obj->pending_seq, key->segment_id, tx_rc);
        }
    }

    if (gsva_coh_hold_pending_enabled()) {
        qemu_log("GSVA_COH: pending held seq=%" PRIu64
                 " segment_id=%#" PRIx64
                 " timeout_ms=%" PRIu64 "\n",
                 obj->pending_seq, key->segment_id, gsva_coh_timeout_ms());
        return GSVA_ERR_COH_PENDING;
    }

    gsva_coh_pending_clear(obj);
    obj->pending = false;
    obj->pending_start_ms = 0;
    obj->pending_op = 0;
    obj->pending_target = 0;
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

    gsva_coh_pending_remove(obj, ack_cna);
    qemu_log("GSVA_COH: InvAck cna=%" PRIu32 " seq=%" PRIu64
             " remaining=%#" PRIx64 " remaining_count=%" PRIu32 "\n",
             ack_cna, seq, obj->pending_ack_bitmap,
             obj->pending_ack_count);

    if (obj->pending_ack_count == 0) {
        obj->pending = false;
        obj->pending_start_ms = 0;
        if (obj->pending_op == 1 || obj->pending_op == 2) {
            obj->state = GSVA_COH_M;
            obj->owner_cna = obj->pending_target;
            gsva_coh_sharers_clear(obj);
            qemu_log("GSVA_COH: %s recovery grant M cna=%" PRIu32
                     " seq=%" PRIu64 " segment_id=%#" PRIx64 "\n",
                     obj->pending_op == 1 ? "InvAck" : "WbAck",
                     obj->pending_target, seq, obj->key.segment_id);
        } else if (obj->pending_op == 3) {
            obj->state = GSVA_COH_RETIRED;
            obj->owner_cna = 0;
            gsva_coh_sharers_clear(obj);
            qemu_log("GSVA_COH: RetireAck recovery retire"
                     " requester=%" PRIu32 " seq=%" PRIu64
                     " segment_id=%#" PRIx64 "\n",
                     obj->pending_target, seq, obj->key.segment_id);
        } else if (obj->pending_op == 4) {
            uint32_t requester = obj->pending_target;

            obj->state = GSVA_COH_S;
            obj->owner_cna = 0;
            gsva_coh_sharers_clear(obj);
            gsva_coh_sharer_add(obj, ack_cna);
            gsva_coh_sharer_add(obj, requester);
            qemu_log("GSVA_COH: DowngradeAck recovery grant S"
                     " requester=%" PRIu32 " owner=%" PRIu32
                     " seq=%" PRIu64 " segment_id=%#" PRIx64 "\n",
                     requester, ack_cna, seq, obj->key.segment_id);
        } else if (obj->pending_op == 5) {
            qemu_log("GSVA_COH: FenceAck recovery complete"
                     " requester=%" PRIu32 " seq=%" PRIu64
                     " segment_id=%#" PRIx64 "\n",
                     obj->pending_target, seq, obj->key.segment_id);
        }
        obj->pending_op = 0;
        obj->pending_target = 0;
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

    if (gsva_coh_object_maybe_timeout(obj,
                                      qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL),
                                      gsva_coh_timeout_ms())) {
        qemu_log("GSVA_COH: Retry timeout seq=%" PRIu64
                 " segment_id=%#" PRIx64 "\n",
                 seq, key->segment_id);
        return GSVA_ERR_COH_TIMEOUT;
    }

    if (obj->state == GSVA_COH_TIMEOUT) {
        return GSVA_ERR_COH_TIMEOUT;
    }

    if (!obj->pending) {
        return GSVA_OK; /* already complete */
    }

    if (obj->pending_seq != seq) {
        return GSVA_ERR_COH_PENDING;
    }

    /* V1 sim: synchronous — if still pending, treat as timeout check */
    if (obj->pending_ack_count == 0) {
        obj->pending = false;
        obj->pending_start_ms = 0;
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
            if (gsva_coh_object_maybe_timeout(obj, now_ms, timeout_ms)) {
                count++;
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
    switch (req->op) {
    case GSVA_COH_MSG_INVALIDATE:
        ack.op = GSVA_COH_MSG_INVALIDATE_ACK;
        break;
    case GSVA_COH_MSG_DOWNGRADE:
        ack.op = GSVA_COH_MSG_DOWNGRADE_ACK;
        break;
    case GSVA_COH_MSG_WRITEBACK:
        ack.op = GSVA_COH_MSG_WRITEBACK_ACK;
        break;
    case GSVA_COH_MSG_FENCE:
        ack.op = GSVA_COH_MSG_FENCE_ACK;
        break;
    case GSVA_COH_MSG_RETIRE:
        ack.op = GSVA_COH_MSG_RETIRE_ACK;
        break;
    case GSVA_COH_MSG_TOKEN_REVOKE:
        ack.op = GSVA_COH_MSG_TOKEN_ACK;
        break;
    default:
        break;
    }
    gsva_coh_send_ub_link_msg(ubc_dev, req->source_cna, ack_subcode, &ack);
}

void gsva_coh_handle_rx_inv(BusControllerDev *ubc_dev, const GsvaCohMsgV1 *msg)
{
    qemu_log("GSVA_COH: rx INV from cna=%" PRIu32 " segment_id=%#" PRIx64
             " seq=%" PRIu64 "\n", msg->source_cna,
             msg->key.segment_id, msg->seq);
    /* Drop local GSVA state for the range — V1 sim: log and ACK */
    gsva_coh_send_ack(ubc_dev, msg, UBC_MSG_SUB_GSVA_COH, GSVA_OK);
}

void gsva_coh_handle_rx_inv_ack(BusControllerDev *ubc_dev, const GsvaCohMsgV1 *msg)
{
    int rc = GSVA_ERR_ROUTE_MISSING;

    qemu_log("GSVA_COH: rx INV_ACK from cna=%" PRIu32 " segment_id=%#" PRIx64
             " seq=%" PRIu64 "\n", msg->source_cna,
             msg->key.segment_id, msg->seq);
    if (g_gsva_coh_default_table) {
        rc = gsva_coh_inv_ack(g_gsva_coh_default_table, &msg->key,
                              msg->source_cna, msg->seq);
    }
    qemu_log("GSVA_COH: rx INV_ACK applied from cna=%" PRIu32
             " segment_id=%#" PRIx64 " seq=%" PRIu64 " rc=%d\n",
             msg->source_cna, msg->key.segment_id, msg->seq, rc);
    (void)ubc_dev;
}

void gsva_coh_handle_rx_downgrade(BusControllerDev *ubc_dev, const GsvaCohMsgV1 *msg)
{
    GsvaCohObject *obj = NULL;

    qemu_log("GSVA_COH: rx DOWNGRADE from cna=%" PRIu32 " segment_id=%#" PRIx64
             " seq=%" PRIu64 "\n", msg->source_cna,
             msg->key.segment_id, msg->seq);
    if (g_gsva_coh_default_table) {
        obj = gsva_coh_lookup(g_gsva_coh_default_table, &msg->key);
        if (obj && obj->key.epoch == msg->key.epoch &&
            (obj->state == GSVA_COH_M || obj->state == GSVA_COH_E)) {
            gsva_coh_sharer_add(obj, msg->target_cna);
            gsva_coh_sharer_add(obj, msg->source_cna);
            obj->owner_cna = 0;
            obj->state = GSVA_COH_S;
            qemu_log("GSVA_COH: rx DOWNGRADE local S"
                     " segment_id=%#" PRIx64 " seq=%" PRIu64 "\n",
                     msg->key.segment_id, msg->seq);
        }
    }
    gsva_coh_send_ack(ubc_dev, msg, UBC_MSG_SUB_GSVA_COH, GSVA_OK);
}

void gsva_coh_handle_rx_downgrade_ack(BusControllerDev *ubc_dev, const GsvaCohMsgV1 *msg)
{
    int rc = GSVA_ERR_ROUTE_MISSING;

    qemu_log("GSVA_COH: rx DOWNGRADE_ACK from cna=%" PRIu32
             " segment_id=%#" PRIx64 " seq=%" PRIu64 "\n",
             msg->source_cna, msg->key.segment_id, msg->seq);
    if (g_gsva_coh_default_table) {
        rc = gsva_coh_inv_ack(g_gsva_coh_default_table, &msg->key,
                              msg->source_cna, msg->seq);
    }
    qemu_log("GSVA_COH: rx DOWNGRADE_ACK applied from cna=%" PRIu32
             " segment_id=%#" PRIx64 " seq=%" PRIu64 " rc=%d\n",
             msg->source_cna, msg->key.segment_id, msg->seq, rc);
    (void)ubc_dev;
}

void gsva_coh_handle_rx_wb(BusControllerDev *ubc_dev, const GsvaCohMsgV1 *msg)
{
    qemu_log("GSVA_COH: rx WRITEBACK from cna=%" PRIu32 " segment_id=%#" PRIx64
             " seq=%" PRIu64 "\n", msg->source_cna,
             msg->key.segment_id, msg->seq);
    gsva_coh_send_ack(ubc_dev, msg, UBC_MSG_SUB_GSVA_COH, GSVA_OK);
}

void gsva_coh_handle_rx_wb_ack(BusControllerDev *ubc_dev, const GsvaCohMsgV1 *msg)
{
    int rc = GSVA_ERR_ROUTE_MISSING;

    qemu_log("GSVA_COH: rx WRITEBACK_ACK from cna=%" PRIu32
             " segment_id=%#" PRIx64 " seq=%" PRIu64 "\n",
             msg->source_cna, msg->key.segment_id, msg->seq);
    if (g_gsva_coh_default_table) {
        rc = gsva_coh_inv_ack(g_gsva_coh_default_table, &msg->key,
                              msg->source_cna, msg->seq);
    }
    qemu_log("GSVA_COH: rx WRITEBACK_ACK applied from cna=%" PRIu32
             " segment_id=%#" PRIx64 " seq=%" PRIu64 " rc=%d\n",
             msg->source_cna, msg->key.segment_id, msg->seq, rc);
    (void)ubc_dev;
}

void gsva_coh_handle_rx_fence(BusControllerDev *ubc_dev, const GsvaCohMsgV1 *msg)
{
    qemu_log("GSVA_COH: rx FENCE from cna=%" PRIu32 " segment_id=%#" PRIx64
             " seq=%" PRIu64 "\n", msg->source_cna,
             msg->key.segment_id, msg->seq);
    gsva_coh_send_ack(ubc_dev, msg, UBC_MSG_SUB_GSVA_COH, GSVA_OK);
}

void gsva_coh_handle_rx_fence_ack(BusControllerDev *ubc_dev, const GsvaCohMsgV1 *msg)
{
    int rc = GSVA_ERR_ROUTE_MISSING;

    qemu_log("GSVA_COH: rx FENCE_ACK from cna=%" PRIu32
             " segment_id=%#" PRIx64 " seq=%" PRIu64 "\n",
             msg->source_cna, msg->key.segment_id, msg->seq);
    if (g_gsva_coh_default_table) {
        rc = gsva_coh_inv_ack(g_gsva_coh_default_table, &msg->key,
                              msg->source_cna, msg->seq);
    }
    qemu_log("GSVA_COH: rx FENCE_ACK applied from cna=%" PRIu32
             " segment_id=%#" PRIx64 " seq=%" PRIu64 " rc=%d\n",
             msg->source_cna, msg->key.segment_id, msg->seq, rc);
    (void)ubc_dev;
}

void gsva_coh_handle_rx_retire(BusControllerDev *ubc_dev, const GsvaCohMsgV1 *msg)
{
    GsvaCohObject *obj = NULL;

    qemu_log("GSVA_COH: rx RETIRE from cna=%" PRIu32 " segment_id=%#" PRIx64
             " seq=%" PRIu64 "\n", msg->source_cna,
             msg->key.segment_id, msg->seq);
    if (g_gsva_coh_default_table) {
        obj = gsva_coh_lookup(g_gsva_coh_default_table, &msg->key);
        if (obj && obj->key.epoch == msg->key.epoch) {
            obj->state = GSVA_COH_RETIRED;
            obj->owner_cna = 0;
            obj->pending = false;
            obj->pending_start_ms = 0;
            gsva_coh_sharers_clear(obj);
            gsva_coh_pending_clear(obj);
            qemu_log("GSVA_COH: rx RETIRE local retired"
                     " segment_id=%#" PRIx64 " seq=%" PRIu64 "\n",
                     msg->key.segment_id, msg->seq);
        }
    }
    gsva_coh_send_ack(ubc_dev, msg, UBC_MSG_SUB_GSVA_COH, GSVA_OK);
}

void gsva_coh_handle_rx_retire_ack(BusControllerDev *ubc_dev, const GsvaCohMsgV1 *msg)
{
    int rc = GSVA_ERR_ROUTE_MISSING;

    qemu_log("GSVA_COH: rx RETIRE_ACK from cna=%" PRIu32
             " segment_id=%#" PRIx64 " seq=%" PRIu64 "\n",
             msg->source_cna, msg->key.segment_id, msg->seq);
    if (g_gsva_coh_default_table) {
        rc = gsva_coh_inv_ack(g_gsva_coh_default_table, &msg->key,
                              msg->source_cna, msg->seq);
    }
    qemu_log("GSVA_COH: rx RETIRE_ACK applied from cna=%" PRIu32
             " segment_id=%#" PRIx64 " seq=%" PRIu64 " rc=%d\n",
             msg->source_cna, msg->key.segment_id, msg->seq, rc);
    (void)ubc_dev;
}

void gsva_coh_handle_rx_token_revoke(BusControllerDev *ubc_dev, const GsvaCohMsgV1 *msg)
{
    qemu_log("GSVA_COH: rx TOKEN_REVOKE from cna=%" PRIu32
             " segment_id=%#" PRIx64 " seq=%" PRIu64 "\n",
             msg->source_cna, msg->key.segment_id, msg->seq);
    gsva_coh_send_ack(ubc_dev, msg, UBC_MSG_SUB_GSVA_COH, GSVA_OK);
}

void gsva_coh_handle_rx_token_ack(BusControllerDev *ubc_dev, const GsvaCohMsgV1 *msg)
{
    int rc = GSVA_ERR_ROUTE_MISSING;
    uint32_t token_id = msg->access_flags;
    uint32_t new_token_value = (uint32_t)msg->access_len;

    qemu_log("GSVA_COH: rx TOKEN_ACK from cna=%" PRIu32
             " segment_id=%#" PRIx64 " seq=%" PRIu64
             " token_id=%" PRIu32 "\n",
             msg->source_cna, msg->key.segment_id, msg->seq, token_id);
    if (g_gsva_route_default_table) {
        rc = gsva_route_ack_token_revoke(g_gsva_route_default_table,
                                         &msg->key, token_id,
                                         new_token_value, msg->source_cna);
    }
    qemu_log("GSVA_COH: rx TOKEN_ACK applied from cna=%" PRIu32
             " segment_id=%#" PRIx64 " token_id=%" PRIu32 " rc=%d\n",
             msg->source_cna, msg->key.segment_id, token_id, rc);
    (void)ubc_dev;
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

    if (sub_msg_code == UBC_MSG_SUB_GSVA_COH) {
        switch (msg->op) {
        case GSVA_COH_MSG_INVALIDATE:
            gsva_coh_handle_rx_inv(ubc_dev, msg);
            break;
        case GSVA_COH_MSG_INVALIDATE_ACK:
            gsva_coh_handle_rx_inv_ack(ubc_dev, msg);
            break;
        case GSVA_COH_MSG_DOWNGRADE:
            gsva_coh_handle_rx_downgrade(ubc_dev, msg);
            break;
        case GSVA_COH_MSG_DOWNGRADE_ACK:
            gsva_coh_handle_rx_downgrade_ack(ubc_dev, msg);
            break;
        case GSVA_COH_MSG_WRITEBACK:
            gsva_coh_handle_rx_wb(ubc_dev, msg);
            break;
        case GSVA_COH_MSG_WRITEBACK_ACK:
            gsva_coh_handle_rx_wb_ack(ubc_dev, msg);
            break;
        case GSVA_COH_MSG_RETIRE:
            gsva_coh_handle_rx_retire(ubc_dev, msg);
            break;
        case GSVA_COH_MSG_RETIRE_ACK:
            gsva_coh_handle_rx_retire_ack(ubc_dev, msg);
            break;
        case GSVA_COH_MSG_FENCE:
            gsva_coh_handle_rx_fence(ubc_dev, msg);
            break;
        case GSVA_COH_MSG_FENCE_ACK:
            gsva_coh_handle_rx_fence_ack(ubc_dev, msg);
            break;
        case GSVA_COH_MSG_TOKEN_REVOKE:
            gsva_coh_handle_rx_token_revoke(ubc_dev, msg);
            break;
        case GSVA_COH_MSG_TOKEN_ACK:
            gsva_coh_handle_rx_token_ack(ubc_dev, msg);
            break;
        default:
            qemu_log("GSVA_COH: unhandled op=%u subcode=%u\n",
                     msg->op, sub_msg_code);
            break;
        }
        return;
    }

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

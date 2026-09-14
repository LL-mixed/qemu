/* Production coherence state machine; transport delivery is controlled here.
 * This tests coordinator receipts, not remote CPU/PTO draining. */
#include "qemu/osdep.h"
#include "hw/ub/gsva_coherence.h"
#include "hw/ub/ub_ubc.h"

static uint64_t now_ms = 100;
static unsigned sends;
static int send_result;

int64_t qemu_clock_get_ns(QEMUClockType type)
{
    return now_ms * 1000000;
}

int obmm_coh_send_ub_link_msg(BusControllerDev *ubc, uint32_t dcna,
                            uint8_t subcode, const void *payload, uint32_t len)
{
    const GsvaCohMsgV1 *msg = payload;
    g_assert_cmpuint(len, ==, sizeof(*msg));
    g_assert_cmpuint(msg->op, ==, GSVA_COH_MSG_RETIRE);
    g_assert_cmpuint(msg->target_cna, ==, dcna);
    sends++;
    return send_result;
}

/* Non-retire route entry points are deliberately unavailable in this test. */
GsvaRouteEntry *gsva_route_lookup_base(GsvaRouteTable *tbl, const GsvaKeyV1 *key)
{
    g_assert_not_reached();
}

int gsva_route_validate_token(const GsvaRouteEntry *route, uint32_t requester,
                              uint32_t token_id, uint32_t token_value,
                              uint32_t access_flags)
{
    g_assert_not_reached();
}

int gsva_route_ack_token_revoke(GsvaRouteTable *tbl, const GsvaKeyV1 *key,
                               uint32_t token_id, uint32_t value, uint32_t cna)
{
    g_assert_not_reached();
}

static const GsvaKeyV1 key = {
    .version = 1, .segment_id = 42, .home_va = 0x700000000000,
    .size = 4096, .epoch = 7,
};
static BusControllerDev ubc;
static GsvaCohTable table;

static GsvaCohObject *start(bool transport, bool failure)
{
    GsvaCohObject *obj;

    g_setenv("GSVA_COH_UB_LINK_TX", transport ? "1" : "0", true);
    g_unsetenv("GSVA_COH_HOLD_PENDING");
    gsva_coh_table_init(&table);
    gsva_coh_set_default_table(&table);
    ubc.parent.cna = 7;
    now_ms = 100;
    sends = 0;
    send_result = failure ? -EIO : 0;
    g_assert_cmpint(gsva_coh_object_create(&table, &key, 7, 99), ==, GSVA_OK);
    obj = gsva_coh_lookup(&table, &key);
    obj->state = GSVA_COH_S;
    /* Include a CNA above bitmap width: the explicit holder set is canonical. */
    obj->sharer_count = 3;
    obj->sharer_cnas[0] = 7;
    obj->sharer_cnas[1] = 8;
    obj->sharer_cnas[2] = 100;
    obj->sharer_bitmap = (1ULL << 7) | (1ULL << 8);
    g_assert_cmpint(gsva_coh_retire_tx(&table, &ubc, &key, 7), ==,
                    GSVA_ERR_COH_PENDING);
    g_assert_cmpuint(sends, ==, transport ? 2 : 0);
    g_assert_true(obj->pending);
    g_assert_cmpuint(obj->pending_ack_count, ==, 2);
    g_assert_cmpint(obj->state, ==, GSVA_COH_S);
    return obj;
}

static GsvaCohMsgV1 receipt(GsvaCohObject *obj, uint32_t cna)
{
    return (GsvaCohMsgV1) {
        .version = 1, .op = GSVA_COH_MSG_RETIRE_ACK,
        .seq = obj->pending_seq, .source_cna = cna, .target_cna = 7,
        .key = key,
    };
}

static void deliver(const GsvaCohMsgV1 *msg)
{
    gsva_coh_dispatch_rx(&ubc, UBC_MSG_SUB_GSVA_COH, msg, sizeof(*msg));
}

static void test_exact_receipts(void)
{
    GsvaCohObject *obj = start(true, false);
    GsvaCohMsgV1 good = receipt(obj, 8), bad;

    for (unsigned i = 0; i < 16; i++) {
        bad = good;
        switch (i) {
        case 0: bad.error = GSVA_ERR_COH_TIMEOUT; break;
        case 1: bad.version++; break;
        case 2: bad.seq++; break;
        case 3: bad.source_cna = 9; break;
        case 4: bad.target_cna = 9; break;
        case 5: bad.key.epoch++; break;
        case 6: bad.key.segment_id++; break;
        case 7: bad.key.home_va += 4096; break;
        case 8: bad.key.asid++; break;
        case 9: bad.op = GSVA_COH_MSG_INVALIDATE_ACK; break;
        case 10: bad.op = GSVA_COH_MSG_WRITEBACK_ACK; break;
        case 11: bad.op = GSVA_COH_MSG_DOWNGRADE_ACK; break;
        case 12: bad.op = GSVA_COH_MSG_FENCE_ACK; break;
        case 13: bad.key.size += 4096; break;
        case 14: bad.key.version++; break;
        case 15: bad.key.flags++; break;
        }
        deliver(&bad);
        g_assert_true(obj->pending);
        g_assert_cmpuint(obj->pending_ack_count, ==, 2);
        g_assert_cmpint(obj->state, ==, GSVA_COH_S);
    }
    deliver(&good);
    g_assert_true(obj->pending);
    g_assert_cmpuint(obj->pending_ack_count, ==, 1);
    deliver(&good); /* duplicate cannot stand in for the other holder */
    g_assert_cmpuint(obj->pending_ack_count, ==, 1);
    good = receipt(obj, 100);
    deliver(&good);
    g_assert_false(obj->pending);
    g_assert_cmpint(obj->state, ==, GSVA_COH_RETIRED);
    g_assert_cmpuint(obj->sharer_count, ==, 0);
    g_assert_cmpuint(obj->owner_cna, ==, 0);
    gsva_coh_table_destroy(&table);
}

static void test_missing_receipts(void)
{
    for (unsigned i = 0; i < 3; i++) {
        GsvaCohObject *obj = start(i != 0, i == 1);
        GsvaCohMsgV1 ack = receipt(obj, 8);

        g_assert_cmpint(gsva_coh_retire_tx(&table, &ubc, &key, 7), ==,
                        GSVA_ERR_COH_PENDING);
        now_ms = 5100;
        g_assert_cmpint(gsva_coh_check_timeouts(&table, now_ms, 5000), ==, 1);
        g_assert_cmpint(obj->state, ==, GSVA_COH_TIMEOUT);
        deliver(&ack); /* late completion cannot revive quarantined state */
        g_assert_cmpint(gsva_coh_retire_tx(&table, &ubc, &key, 7), ==,
                        GSVA_ERR_COH_TIMEOUT);
        g_assert_cmpint(obj->state, ==, GSVA_COH_TIMEOUT);
        g_assert_cmpint(gsva_coh_object_remove(&table, &key), ==,
                        GSVA_ERR_COH_TIMEOUT);
        gsva_coh_table_destroy(&table);
    }
}

static void test_late_ack_without_timeout_poll(void)
{
    GsvaCohObject *obj = start(true, false);
    GsvaCohMsgV1 ack = receipt(obj, 8);

    now_ms = 5100;
    deliver(&ack);
    g_assert_cmpint(obj->state, ==, GSVA_COH_TIMEOUT);
    ack = receipt(obj, 100);
    deliver(&ack);
    g_assert_cmpint(obj->state, ==, GSVA_COH_TIMEOUT);
    gsva_coh_table_destroy(&table);
}

static void test_no_remote_holders(void)
{
    gsva_coh_table_init(&table);
    g_assert_cmpint(gsva_coh_object_create(&table, &key, 7, 99), ==, GSVA_OK);
    g_assert_cmpint(gsva_coh_retire_tx(&table, &ubc, &key, 7), ==, GSVA_OK);
    g_assert_cmpint(gsva_coh_lookup(&table, &key)->state, ==, GSVA_COH_RETIRED);
    gsva_coh_table_destroy(&table);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/gsva-retire/exact-receipts", test_exact_receipts);
    g_test_add_func("/gsva-retire/missing-receipts", test_missing_receipts);
    g_test_add_func("/gsva-retire/late-ack", test_late_ack_without_timeout_poll);
    g_test_add_func("/gsva-retire/no-remote-holders", test_no_remote_holders);
    return g_test_run();
}

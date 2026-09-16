/* Production coherence state machine; transport delivery and receiver drain
 * results are controlled here. Platform drain mechanics have a separate
 * extracted production-function test in ub_sim. */
#include "qemu/osdep.h"
#include "hw/ub/gsva_coherence.h"
#include "hw/ub/ub_ubc.h"

static uint64_t now_ms = 100;
static unsigned sends;
static unsigned receipt_sends;
static unsigned quarantine_calls;
static unsigned drain_calls;
static unsigned home_retire_calls;
static int send_result;
static int quarantine_result;
static int drain_result;
static int home_retire_result;
static GsvaCohMsgV1 last_receipt;
static const GsvaKeyV1 key = {
    .version = 1, .segment_id = 42, .home_va = 0x700000000000,
    .size = 4096, .epoch = 7,
};
static BusControllerDev ubc;
static GsvaCohTable table;

int64_t qemu_clock_get_ns(QEMUClockType type)
{
    return now_ms * 1000000;
}

int obmm_coh_send_ub_link_msg(BusControllerDev *ubc, uint32_t dcna,
                            uint8_t subcode, const void *payload, uint32_t len)
{
    const GsvaCohMsgV1 *msg = payload;
    g_assert_cmpuint(len, ==, sizeof(*msg));
    g_assert_cmpuint(msg->target_cna, ==, dcna);
    if (msg->op == GSVA_COH_MSG_RETIRE) {
        sends++;
    } else {
        g_assert_cmpuint(msg->op, ==, GSVA_COH_MSG_RETIRE_ACK);
        receipt_sends++;
        last_receipt = *msg;
    }
    return send_result;
}

int ubc_gsva_quarantine_local_holder(BusControllerDev *dev,
                                     const GsvaKeyV1 *retired,
                                     uint32_t home_cna)
{
    g_assert_true(dev == &ubc);
    g_assert_cmpmem(retired, sizeof(*retired), &key, sizeof(key));
    g_assert_cmpuint(home_cna, ==, 7);
    quarantine_calls++;
    return quarantine_result;
}

int ubc_gsva_drain_local_holder(BusControllerDev *dev,
                                const GsvaKeyV1 *retired,
                                uint32_t home_cna)
{
    g_assert_true(dev == &ubc);
    g_assert_cmpmem(retired, sizeof(*retired), &key, sizeof(key));
    g_assert_cmpuint(home_cna, ==, 7);
    drain_calls++;
    return drain_result;
}

int ubc_gsva_complete_home_retire(BusControllerDev *dev,
                                  const GsvaKeyV1 *retired)
{
    g_assert_true(dev == &ubc);
    g_assert_cmpmem(retired, sizeof(*retired), &key, sizeof(key));
    home_retire_calls++;
    return home_retire_result;
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
    receipt_sends = 0;
    home_retire_calls = 0;
    send_result = failure ? -EIO : 0;
    home_retire_result = GSVA_OK;
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
    g_assert_cmpuint(home_retire_calls, ==, 0);
    deliver(&good); /* duplicate cannot stand in for the other holder */
    g_assert_cmpuint(obj->pending_ack_count, ==, 1);
    g_assert_cmpuint(home_retire_calls, ==, 0);
    good = receipt(obj, 100);
    deliver(&good);
    g_assert_false(obj->pending);
    g_assert_cmpint(obj->state, ==, GSVA_COH_RETIRED);
    g_assert_cmpuint(home_retire_calls, ==, 1);
    g_assert_cmpuint(obj->sharer_count, ==, 0);
    g_assert_cmpuint(obj->owner_cna, ==, 0);
    gsva_coh_table_destroy(&table);
}

static void test_home_finalize_failure_is_fail_closed(void)
{
    GsvaCohObject *obj = start(true, false);
    GsvaCohMsgV1 ack = receipt(obj, 8);

    deliver(&ack);
    home_retire_result = GSVA_ERR_COH_TIMEOUT;
    ack = receipt(obj, 100);
    deliver(&ack);
    g_assert_false(obj->pending);
    g_assert_cmpint(obj->state, ==, GSVA_COH_TIMEOUT);
    g_assert_cmpuint(home_retire_calls, ==, 1);
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

static GsvaCohMsgV1 retire_request(void)
{
    return (GsvaCohMsgV1) {
        .version = 1, .op = GSVA_COH_MSG_RETIRE, .seq = 77,
        .source_cna = 7, .target_cna = 8, .key = key,
        .access_va = key.home_va, .access_len = key.size,
    };
}

static GsvaCohObject *start_receiver(void)
{
    GsvaCohObject *obj;

    gsva_coh_table_init(&table);
    gsva_coh_set_default_table(&table);
    ubc.parent.cna = 8;
    sends = receipt_sends = quarantine_calls = drain_calls = 0;
    quarantine_result = drain_result = GSVA_OK;
    send_result = 0;
    memset(&last_receipt, 0, sizeof(last_receipt));
    g_assert_cmpint(gsva_coh_object_create(&table, &key, 7, 99), ==, GSVA_OK);
    obj = gsva_coh_lookup(&table, &key);
    obj->state = GSVA_COH_S;
    obj->sharer_count = 1;
    obj->sharer_cnas[0] = 8;
    obj->sharer_bitmap = 1ULL << 8;
    return obj;
}

static void deliver_retire(const GsvaCohMsgV1 *msg)
{
    gsva_coh_dispatch_rx(&ubc, UBC_MSG_SUB_GSVA_COH, msg, sizeof(*msg));
}

static void test_receiver_drains_before_receipt(void)
{
    GsvaCohObject *obj = start_receiver();
    GsvaCohMsgV1 msg = retire_request();

    deliver_retire(&msg);
    g_assert_cmpuint(quarantine_calls, ==, 1);
    g_assert_cmpuint(drain_calls, ==, 1);
    g_assert_cmpuint(receipt_sends, ==, 1);
    g_assert_cmpint((int32_t)last_receipt.error, ==, GSVA_OK);
    g_assert_cmpuint(last_receipt.source_cna, ==, 8);
    g_assert_cmpuint(last_receipt.target_cna, ==, 7);
    g_assert_cmpint(obj->state, ==, GSVA_COH_RETIRED);
    g_assert_cmpuint(obj->sharer_count, ==, 0);

    deliver_retire(&msg);
    g_assert_cmpuint(quarantine_calls, ==, 2);
    g_assert_cmpuint(drain_calls, ==, 2);
    g_assert_cmpuint(receipt_sends, ==, 2);
    g_assert_cmpint((int32_t)last_receipt.error, ==, GSVA_OK);
    gsva_coh_table_destroy(&table);
}

static void test_receiver_withholds_unsafe_receipt(void)
{
    GsvaCohObject *obj = start_receiver();
    GsvaCohMsgV1 msg = retire_request();
    GsvaCohMsgV1 bad;
    unsigned receipts;

    obj->pending = true;
    deliver_retire(&msg);
    g_assert_cmpuint(quarantine_calls, ==, 1);
    g_assert_cmpuint(drain_calls, ==, 0);
    g_assert_cmpint((int32_t)last_receipt.error, ==, GSVA_ERR_COH_PENDING);
    g_assert_cmpint(obj->state, ==, GSVA_COH_S);

    obj->pending = false;
    drain_result = GSVA_ERR_COH_TIMEOUT;
    deliver_retire(&msg);
    g_assert_cmpuint(quarantine_calls, ==, 2);
    g_assert_cmpuint(drain_calls, ==, 1);
    g_assert_cmpint((int32_t)last_receipt.error, ==, GSVA_ERR_COH_TIMEOUT);
    g_assert_cmpint(obj->state, ==, GSVA_COH_S);

    bad = msg;
    bad.key.epoch++;
    deliver_retire(&bad);
    g_assert_cmpuint(quarantine_calls, ==, 2);
    g_assert_cmpint((int32_t)last_receipt.error, ==, GSVA_ERR_STALE_EPOCH);

    bad = msg;
    bad.target_cna++;
    receipts = receipt_sends;
    deliver_retire(&bad);
    g_assert_cmpuint(receipt_sends, ==, receipts);
    g_assert_cmpuint(quarantine_calls, ==, 2);
    gsva_coh_table_destroy(&table);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/gsva-retire/exact-receipts", test_exact_receipts);
    g_test_add_func("/gsva-retire/home-finalize-fail-closed",
                    test_home_finalize_failure_is_fail_closed);
    g_test_add_func("/gsva-retire/missing-receipts", test_missing_receipts);
    g_test_add_func("/gsva-retire/late-ack", test_late_ack_without_timeout_poll);
    g_test_add_func("/gsva-retire/no-remote-holders", test_no_remote_holders);
    g_test_add_func("/gsva-retire/receiver-drain",
                    test_receiver_drains_before_receipt);
    g_test_add_func("/gsva-retire/receiver-fail-closed",
                    test_receiver_withholds_unsafe_receipt);
    return g_test_run();
}

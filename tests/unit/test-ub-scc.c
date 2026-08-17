/* Unit tests for the pending-load and direct-EL0-upcall event model. */

#include "qemu/osdep.h"
#include "hw/ub/ub_scc.h"

static const ObmmSccConfig default_config = {
    .context_entries = 64,
    .pending_load_entries = 64,
    .event_queue_depth = 128,
    .clock_mhz = 2000,
};

static ObmmSccLoadDesc test_load(uint64_t pc, uint8_t rt,
                                 uint8_t bytes, bool big_endian)
{
    return (ObmmSccLoadDesc) {
        .fault_pc = pc,
        .effective_va = 0x40000000,
        .map_id = 7,
        .map_generation = 3,
        .remote_offset = 0x1000,
        .submit_cycle = 100,
        .deadline_cycle = 1000,
        .rt = rt,
        .access_bytes = bytes,
        .mmu_index = 1,
        .big_endian = big_endian,
    };
}

static void test_model_spec(void)
{
    ObmmSccConfig config;
    bool enabled;

    g_assert_true(obmm_scc_config_parse(
        "v2|enabled=1|contexts=32|pending=16|events=64|clock_mhz=2500",
        &enabled, &config));
    g_assert_true(enabled);
    g_assert_cmpuint(config.context_entries, ==, 32);
    g_assert_cmpuint(config.pending_load_entries, ==, 16);
    g_assert_cmpuint(config.event_queue_depth, ==, 64);
    g_assert_cmpuint(config.clock_mhz, ==, 2500);
    g_assert_false(obmm_scc_config_parse(
        "v2|enabled=1|contexts=65|pending=16|events=64|clock_mhz=2500",
        &enabled, &config));
    g_assert_false(obmm_scc_config_parse("v1", &enabled, &config));
    g_assert_true(obmm_scc_config_parse(NULL, &enabled, &config));
    g_assert_false(enabled);
}

static void test_context_id_and_logical_ordinal(void)
{
    uint64_t context_id = obmm_scc_context_id_make(9, 5, 3);
    uint64_t ordinal;

    g_assert_cmpuint(obmm_scc_context_id_generation(context_id), ==, 9);
    g_assert_cmpuint(obmm_scc_context_id_home_core(context_id), ==, 5);
    g_assert_cmpuint(obmm_scc_context_id_slot(context_id), ==, 3);
    g_assert_true(obmm_scc_logical_remote_ordinal(3, 8, 4, &ordinal));
    g_assert_cmpuint(ordinal, ==, 35);
    g_assert_false(obmm_scc_logical_remote_ordinal(8, 8, 0, &ordinal));
    g_assert_false(obmm_scc_logical_remote_ordinal(0, 0, 0, &ordinal));
}

static void test_pending_and_complete_events(void)
{
    g_autoptr(ObmmScc) scc = obmm_scc_new(1, 9, &default_config);
    ObmmSccLoadDesc load = test_load(0x1000, 3, 8, false);
    ObmmSccPltToken token;
    ObmmSccEvent event;
    uint64_t context_id = obmm_scc_context_id_make(9, 0, 2);
    uint8_t payload[] = { 0x88, 0x77, 0x66, 0x55,
                          0x44, 0x33, 0x22, 0x11 };

    g_assert_nonnull(scc);
    g_assert_cmpint(obmm_scc_load_pending(
                        scc, context_id, &load, &token),
                    ==, OBMM_SCC_PENDING_ACCEPTED);
    g_assert_true(obmm_scc_event_pending(scc));
    g_assert_true(obmm_scc_event_pop(scc, &event, false));
    g_assert_cmpint(event.kind, ==, OBMM_SCC_EVENT_PENDING);
    g_assert_cmpuint(event.context_id, ==, context_id);
    g_assert_cmpuint(event.plt_token.generation, ==, token.generation);
    g_assert_cmpuint(event.plt_token.slot, ==, token.slot);
    g_assert_cmphex(event.fault_pc, ==, load.fault_pc);
    g_assert_cmpuint(event.rt, ==, 3);
    g_assert_cmpint(obmm_scc_load_complete(
                        scc, token, OBMM_SCC_LOAD_SUCCESS,
                        payload, sizeof(payload), 500),
                    ==, OBMM_SCC_COMPLETION_ACCEPTED);
    g_assert_true(obmm_scc_event_pop(scc, &event, false));
    g_assert_cmpint(event.kind, ==, OBMM_SCC_EVENT_COMPLETE);
    g_assert_cmphex(event.value, ==, 0x1122334455667788ULL);
    g_assert_cmpuint(obmm_scc_pending_count(scc), ==, 0);
    g_assert_cmpuint(obmm_scc_stats(scc)->completion_events_delivered, ==, 1);
    g_assert_cmpuint(obmm_scc_stats(scc)->context_saves, ==, 0);
    g_assert_cmpuint(obmm_scc_stats(scc)->context_restores, ==, 0);
    g_assert_cmpuint(obmm_scc_stats(scc)->context_switches, ==, 0);
    g_assert_false(obmm_scc_event_pending(scc));
}

static void test_scalar_value_matrix(void)
{
    static const uint8_t sizes[] = { 1, 2, 4, 8 };
    static const uint64_t little[] = {
        0x01, 0x0201, 0x04030201, 0x0807060504030201ULL,
    };
    static const uint64_t big[] = {
        0x01, 0x0102, 0x01020304, 0x0102030405060708ULL,
    };
    uint8_t payload[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint32_t endian;
    uint32_t index;

    for (endian = 0; endian < 2; endian++) {
        for (index = 0; index < G_N_ELEMENTS(sizes); index++) {
            g_autoptr(ObmmScc) scc = obmm_scc_new(
                1, 1, &default_config);
            ObmmSccLoadDesc load = test_load(
                0x1000, 0, sizes[index], endian);
            ObmmSccPltToken token;
            ObmmSccEvent event;

            g_assert_cmpint(obmm_scc_load_pending(
                                scc, 1, &load, &token),
                            ==, OBMM_SCC_PENDING_ACCEPTED);
            g_assert_true(obmm_scc_event_pop(scc, &event, false));
            g_assert_cmpint(obmm_scc_load_complete(
                                scc, token, OBMM_SCC_LOAD_SUCCESS,
                                payload, sizes[index], 200),
                            ==, OBMM_SCC_COMPLETION_ACCEPTED);
            g_assert_true(obmm_scc_event_pop(scc, &event, false));
            g_assert_cmphex(event.value, ==,
                            endian ? big[index] : little[index]);
        }
    }
}

static void test_new_pending_precedes_queued_completion(void)
{
    g_autoptr(ObmmScc) scc = obmm_scc_new(1, 9, &default_config);
    ObmmSccLoadDesc first = test_load(0x1000, 3, 8, false);
    ObmmSccLoadDesc second = test_load(0x2000, 4, 8, false);
    ObmmSccPltToken first_token;
    ObmmSccPltToken second_token;
    ObmmSccEvent event;
    uint8_t payload[] = { 0x88, 0x77, 0x66, 0x55,
                          0x44, 0x33, 0x22, 0x11 };

    g_assert_cmpint(obmm_scc_load_pending(
                        scc, obmm_scc_context_id_make(9, 0, 1),
                        &first, &first_token),
                    ==, OBMM_SCC_PENDING_ACCEPTED);
    g_assert_true(obmm_scc_event_pop(scc, &event, false));
    g_assert_cmpint(event.kind, ==, OBMM_SCC_EVENT_PENDING);
    g_assert_cmpuint(event.sequence, ==, 1);
    g_assert_cmpint(obmm_scc_load_complete(
                        scc, first_token, OBMM_SCC_LOAD_SUCCESS,
                        payload, sizeof(payload), 500),
                    ==, OBMM_SCC_COMPLETION_ACCEPTED);

    g_assert_cmpint(obmm_scc_load_pending(
                        scc, obmm_scc_context_id_make(9, 0, 2),
                        &second, &second_token),
                    ==, OBMM_SCC_PENDING_ACCEPTED);
    g_assert_true(obmm_scc_event_pop(scc, &event, false));
    g_assert_cmpint(event.kind, ==, OBMM_SCC_EVENT_PENDING);
    g_assert_cmpuint(event.context_id, ==,
                     obmm_scc_context_id_make(9, 0, 2));
    g_assert_cmpuint(event.sequence, ==, 2);
    g_assert_true(obmm_scc_event_pop(scc, &event, false));
    g_assert_cmpint(event.kind, ==, OBMM_SCC_EVENT_COMPLETE);
    g_assert_cmpuint(event.context_id, ==,
                     obmm_scc_context_id_make(9, 0, 1));
    g_assert_cmpuint(event.sequence, ==, 3);

    g_assert_cmpint(obmm_scc_load_complete(
                        scc, second_token, OBMM_SCC_LOAD_SUCCESS,
                        payload, sizeof(payload), 600),
                    ==, OBMM_SCC_COMPLETION_ACCEPTED);
    g_assert_true(obmm_scc_event_pop(scc, &event, false));
    g_assert_cmpint(event.kind, ==, OBMM_SCC_EVENT_COMPLETE);
    g_assert_cmpuint(event.sequence, ==, 4);
    g_assert_false(obmm_scc_event_pending(scc));
}

static void test_fault_and_stale_completion(void)
{
    g_autoptr(ObmmScc) scc = obmm_scc_new(1, 1, &default_config);
    ObmmSccLoadDesc load = test_load(0x1000, 4, 8, false);
    ObmmSccPltToken token;
    ObmmSccEvent event;

    g_assert_cmpint(obmm_scc_load_pending(scc, 1, &load, &token),
                    ==, OBMM_SCC_PENDING_ACCEPTED);
    g_assert_true(obmm_scc_event_pop(scc, &event, false));
    g_assert_cmpint(obmm_scc_load_complete(
                        scc, token, OBMM_SCC_LOAD_TIMEOUT,
                        NULL, 0, 200),
                    ==, OBMM_SCC_COMPLETION_ACCEPTED);
    g_assert_true(obmm_scc_event_pop(scc, &event, false));
    g_assert_cmpint(event.kind, ==, OBMM_SCC_EVENT_FAULT);
    g_assert_cmpint(event.status, ==, OBMM_SCC_LOAD_TIMEOUT);
    g_assert_cmpint(obmm_scc_load_complete(
                        scc, token, OBMM_SCC_LOAD_SUCCESS,
                        NULL, 0, 201),
                    ==, OBMM_SCC_COMPLETION_STALE);
    g_assert_cmpuint(obmm_scc_stats(scc)->faulted_loads, ==, 1);
    g_assert_cmpuint(obmm_scc_stats(scc)->stale_completions, ==, 1);
}

static void test_replay_retains_and_consumes_completion_once(void)
{
    g_autoptr(ObmmScc) scc = obmm_scc_new(1, 9, &default_config);
    ObmmSccLoadDesc load = test_load(0x1000, 3, 8, false);
    ObmmSccPltToken token;
    ObmmSccEvent event;
    uint64_t context_id = obmm_scc_context_id_make(9, 0, 2);
    uint64_t value = 0;
    uint8_t payload[] = { 0x88, 0x77, 0x66, 0x55,
                          0x44, 0x33, 0x22, 0x11 };

    g_assert_cmpint(obmm_scc_load_pending(
                        scc, context_id, &load, &token),
                    ==, OBMM_SCC_PENDING_ACCEPTED);
    g_assert_true(obmm_scc_event_pop(scc, &event, true));
    g_assert_cmpint(obmm_scc_load_complete(
                        scc, token, OBMM_SCC_LOAD_SUCCESS,
                        payload, sizeof(payload), 500),
                    ==, OBMM_SCC_COMPLETION_ACCEPTED);
    g_assert_true(obmm_scc_event_pop(scc, &event, true));
    g_assert_cmpint(event.kind, ==, OBMM_SCC_EVENT_COMPLETE);
    g_assert_cmpuint(event.flags & OBMM_SCC_EVENT_FLAG_REPLAY_RETIRE,
                     !=, 0);
    g_assert_true(obmm_scc_replay_expected(scc, context_id));
    g_assert_cmpuint(obmm_scc_pending_count(scc), ==, 1);
    g_assert_cmpint(obmm_scc_replay_consume(
                        scc, context_id, &load, &value),
                    ==, OBMM_SCC_REPLAY_CONSUMED);
    g_assert_cmphex(value, ==, 0x1122334455667788ULL);
    g_assert_false(obmm_scc_replay_expected(scc, context_id));
    g_assert_cmpuint(obmm_scc_pending_count(scc), ==, 0);
    g_assert_cmpuint(obmm_scc_stats(scc)->replay_consumed, ==, 1);
    g_assert_cmpuint(obmm_scc_stats(scc)->replay_mismatch, ==, 0);
    g_assert_cmpuint(obmm_scc_stats(scc)->replay_ready_high_water,
                     ==, 1);
    g_assert_cmpint(obmm_scc_replay_consume(
                        scc, context_id, &load, &value),
                    ==, OBMM_SCC_REPLAY_NONE);
}

static void test_replay_mismatch_fails_closed(void)
{
    g_autoptr(ObmmScc) scc = obmm_scc_new(1, 9, &default_config);
    ObmmSccLoadDesc load = test_load(0x1000, 3, 8, false);
    ObmmSccLoadDesc mismatch = load;
    ObmmSccPltToken token;
    ObmmSccEvent event;
    uint64_t context_id = obmm_scc_context_id_make(9, 0, 2);
    uint64_t value = 0;
    uint8_t payload[8] = { 0 };

    g_assert_cmpint(obmm_scc_load_pending(
                        scc, context_id, &load, &token),
                    ==, OBMM_SCC_PENDING_ACCEPTED);
    g_assert_true(obmm_scc_event_pop(scc, &event, true));
    g_assert_cmpint(obmm_scc_load_complete(
                        scc, token, OBMM_SCC_LOAD_SUCCESS,
                        payload, sizeof(payload), 500),
                    ==, OBMM_SCC_COMPLETION_ACCEPTED);
    g_assert_true(obmm_scc_event_pop(scc, &event, true));
    mismatch.effective_va += 8;
    mismatch.remote_offset += 8;
    g_assert_cmpint(obmm_scc_replay_consume(
                        scc, context_id, &mismatch, &value),
                    ==, OBMM_SCC_REPLAY_MISMATCH);
    g_assert_true(obmm_scc_fail_stop(scc));
    g_assert_cmpuint(obmm_scc_stats(scc)->replay_consumed, ==, 0);
    g_assert_cmpuint(obmm_scc_stats(scc)->replay_mismatch, ==, 1);
}

static void test_capacity_and_fail_stop(void)
{
    ObmmSccConfig config = default_config;
    ObmmSccLoadDesc first = test_load(0x1000, 0, 8, false);
    ObmmSccLoadDesc second = test_load(0x2000, 1, 8, false);
    ObmmSccPltToken token;
    ObmmSccPltToken unused;

    config.pending_load_entries = 1;
    config.event_queue_depth = 1;
    g_autoptr(ObmmScc) scc = obmm_scc_new(1, 1, &config);

    g_assert_cmpint(obmm_scc_load_pending(scc, 1, &first, &token),
                    ==, OBMM_SCC_PENDING_ACCEPTED);
    g_assert_cmpint(obmm_scc_load_pending(scc, 2, &second, &unused),
                    ==, OBMM_SCC_PENDING_SYNC_STALL);
    g_assert_cmpuint(obmm_scc_stats(scc)->capacity_stalls, ==, 1);
    obmm_scc_mark_fail_stop(scc);
    g_assert_true(obmm_scc_fail_stop(scc));
    g_assert_cmpint(obmm_scc_load_pending(scc, 2, &second, &unused),
                    ==, OBMM_SCC_PENDING_INVALID);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/ub/scc/model-spec", test_model_spec);
    g_test_add_func("/ub/scc/context-id-logical-ordinal",
                    test_context_id_and_logical_ordinal);
    g_test_add_func("/ub/scc/pending-complete-events",
                    test_pending_and_complete_events);
    g_test_add_func("/ub/scc/scalar-value-matrix",
                    test_scalar_value_matrix);
    g_test_add_func("/ub/scc/pending-priority",
                    test_new_pending_precedes_queued_completion);
    g_test_add_func("/ub/scc/fault-stale", test_fault_and_stale_completion);
    g_test_add_func("/ub/scc/replay-consume-once",
                    test_replay_retains_and_consumes_completion_once);
    g_test_add_func("/ub/scc/replay-mismatch",
                    test_replay_mismatch_fails_closed);
    g_test_add_func("/ub/scc/capacity-fail-stop",
                    test_capacity_and_fail_stop);
    return g_test_run();
}

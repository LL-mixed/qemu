/* Unit tests for the pending-load and direct-EL0-upcall event model. */

#include "qemu/osdep.h"
#include "hw/ub/ub_async_load.h"

static const UbAsyncLoadConfig default_config = {
    .context_entries = 64,
    .pending_load_entries = 64,
    .event_queue_depth = 128,
    .clock_mhz = 2000,
};

static UbAsyncLoadDesc test_load(uint64_t pc, uint8_t rt,
                                 uint8_t bytes, bool big_endian)
{
    return (UbAsyncLoadDesc) {
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
    UbAsyncLoadConfig config;
    bool enabled;

    g_assert_true(ub_async_load_config_parse(
        "v3|enabled=1|contexts=32|pending=16|events=64|clock_mhz=2500",
        &enabled, &config));
    g_assert_true(enabled);
    g_assert_cmpuint(config.context_entries, ==, 32);
    g_assert_cmpuint(config.pending_load_entries, ==, 16);
    g_assert_cmpuint(config.event_queue_depth, ==, 64);
    g_assert_cmpuint(config.clock_mhz, ==, 2500);
    g_assert_false(ub_async_load_config_parse(
        "v3|enabled=1|contexts=65|pending=16|events=64|clock_mhz=2500",
        &enabled, &config));
    g_assert_false(ub_async_load_config_parse("v1", &enabled, &config));
    g_assert_true(ub_async_load_config_parse(NULL, &enabled, &config));
    g_assert_false(enabled);
}

static void test_context_id_and_logical_ordinal(void)
{
    uint64_t context_id = ub_async_load_context_id_make(9, 5, 3);
    uint64_t ordinal;

    g_assert_cmpuint(ub_async_load_context_id_generation(context_id), ==, 9);
    g_assert_cmpuint(ub_async_load_context_id_home_core(context_id), ==, 5);
    g_assert_cmpuint(ub_async_load_context_id_slot(context_id), ==, 3);
    g_assert_true(ub_async_load_logical_remote_ordinal(3, 8, 4, &ordinal));
    g_assert_cmpuint(ordinal, ==, 35);
    g_assert_false(ub_async_load_logical_remote_ordinal(8, 8, 0, &ordinal));
    g_assert_false(ub_async_load_logical_remote_ordinal(0, 0, 0, &ordinal));
}

static void test_pending_and_complete_events(void)
{
    g_autoptr(UbAsyncLoad) async_load = ub_async_load_new(1, 9, &default_config);
    UbAsyncLoadDesc load = test_load(0x1000, 3, 8, false);
    UbAsyncLoadPltToken token;
    UbAsyncLoadEvent event;
    uint64_t context_id = ub_async_load_context_id_make(9, 0, 2);
    uint8_t payload[] = { 0x88, 0x77, 0x66, 0x55,
                          0x44, 0x33, 0x22, 0x11 };

    g_assert_nonnull(async_load);
    g_assert_cmpint(ub_async_load_load_pending(
                        async_load, context_id, &load, &token),
                    ==, UB_ASYNC_LOAD_PENDING_ACCEPTED);
    g_assert_true(ub_async_load_event_pending(async_load));
    g_assert_true(ub_async_load_event_pop(async_load, &event, false));
    g_assert_cmpint(event.kind, ==, UB_ASYNC_LOAD_EVENT_PENDING);
    g_assert_cmpuint(event.context_id, ==, context_id);
    g_assert_cmpuint(event.plt_token.generation, ==, token.generation);
    g_assert_cmpuint(event.plt_token.slot, ==, token.slot);
    g_assert_cmphex(event.fault_pc, ==, load.fault_pc);
    g_assert_cmpuint(event.rt, ==, 3);
    g_assert_cmpint(ub_async_load_load_complete(
                        async_load, token, UB_ASYNC_LOAD_STATUS_SUCCESS,
                        payload, sizeof(payload), 500),
                    ==, UB_ASYNC_LOAD_COMPLETION_ACCEPTED);
    g_assert_true(ub_async_load_event_pop(async_load, &event, false));
    g_assert_cmpint(event.kind, ==, UB_ASYNC_LOAD_EVENT_COMPLETE);
    g_assert_cmphex(event.value, ==, 0x1122334455667788ULL);
    g_assert_cmpuint(ub_async_load_pending_count(async_load), ==, 0);
    g_assert_cmpuint(ub_async_load_stats(async_load)->completion_events_delivered, ==, 1);
    g_assert_cmpuint(ub_async_load_stats(async_load)->context_saves, ==, 0);
    g_assert_cmpuint(ub_async_load_stats(async_load)->context_restores, ==, 0);
    g_assert_cmpuint(ub_async_load_stats(async_load)->context_switches, ==, 0);
    g_assert_false(ub_async_load_event_pending(async_load));
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
            g_autoptr(UbAsyncLoad) async_load = ub_async_load_new(
                1, 1, &default_config);
            UbAsyncLoadDesc load = test_load(
                0x1000, 0, sizes[index], endian);
            UbAsyncLoadPltToken token;
            UbAsyncLoadEvent event;

            g_assert_cmpint(ub_async_load_load_pending(
                                async_load, 1, &load, &token),
                            ==, UB_ASYNC_LOAD_PENDING_ACCEPTED);
            g_assert_true(ub_async_load_event_pop(async_load, &event, false));
            g_assert_cmpint(ub_async_load_load_complete(
                                async_load, token, UB_ASYNC_LOAD_STATUS_SUCCESS,
                                payload, sizes[index], 200),
                            ==, UB_ASYNC_LOAD_COMPLETION_ACCEPTED);
            g_assert_true(ub_async_load_event_pop(async_load, &event, false));
            g_assert_cmphex(event.value, ==,
                            endian ? big[index] : little[index]);
        }
    }
}

static void test_new_pending_precedes_queued_completion(void)
{
    g_autoptr(UbAsyncLoad) async_load = ub_async_load_new(1, 9, &default_config);
    UbAsyncLoadDesc first = test_load(0x1000, 3, 8, false);
    UbAsyncLoadDesc second = test_load(0x2000, 4, 8, false);
    UbAsyncLoadPltToken first_token;
    UbAsyncLoadPltToken second_token;
    UbAsyncLoadEvent event;
    uint8_t payload[] = { 0x88, 0x77, 0x66, 0x55,
                          0x44, 0x33, 0x22, 0x11 };

    g_assert_cmpint(ub_async_load_load_pending(
                        async_load, ub_async_load_context_id_make(9, 0, 1),
                        &first, &first_token),
                    ==, UB_ASYNC_LOAD_PENDING_ACCEPTED);
    g_assert_true(ub_async_load_event_pop(async_load, &event, false));
    g_assert_cmpint(event.kind, ==, UB_ASYNC_LOAD_EVENT_PENDING);
    g_assert_cmpuint(event.sequence, ==, 1);
    g_assert_cmpint(ub_async_load_load_complete(
                        async_load, first_token, UB_ASYNC_LOAD_STATUS_SUCCESS,
                        payload, sizeof(payload), 500),
                    ==, UB_ASYNC_LOAD_COMPLETION_ACCEPTED);

    g_assert_cmpint(ub_async_load_load_pending(
                        async_load, ub_async_load_context_id_make(9, 0, 2),
                        &second, &second_token),
                    ==, UB_ASYNC_LOAD_PENDING_ACCEPTED);
    g_assert_true(ub_async_load_event_pop(async_load, &event, false));
    g_assert_cmpint(event.kind, ==, UB_ASYNC_LOAD_EVENT_PENDING);
    g_assert_cmpuint(event.context_id, ==,
                     ub_async_load_context_id_make(9, 0, 2));
    g_assert_cmpuint(event.sequence, ==, 2);
    g_assert_true(ub_async_load_event_pop(async_load, &event, false));
    g_assert_cmpint(event.kind, ==, UB_ASYNC_LOAD_EVENT_COMPLETE);
    g_assert_cmpuint(event.context_id, ==,
                     ub_async_load_context_id_make(9, 0, 1));
    g_assert_cmpuint(event.sequence, ==, 3);

    g_assert_cmpint(ub_async_load_load_complete(
                        async_load, second_token, UB_ASYNC_LOAD_STATUS_SUCCESS,
                        payload, sizeof(payload), 600),
                    ==, UB_ASYNC_LOAD_COMPLETION_ACCEPTED);
    g_assert_true(ub_async_load_event_pop(async_load, &event, false));
    g_assert_cmpint(event.kind, ==, UB_ASYNC_LOAD_EVENT_COMPLETE);
    g_assert_cmpuint(event.sequence, ==, 4);
    g_assert_false(ub_async_load_event_pending(async_load));
}

static void test_fault_and_stale_completion(void)
{
    g_autoptr(UbAsyncLoad) async_load = ub_async_load_new(1, 1, &default_config);
    UbAsyncLoadDesc load = test_load(0x1000, 4, 8, false);
    UbAsyncLoadPltToken token;
    UbAsyncLoadEvent event;

    g_assert_cmpint(ub_async_load_load_pending(async_load, 1, &load, &token),
                    ==, UB_ASYNC_LOAD_PENDING_ACCEPTED);
    g_assert_true(ub_async_load_event_pop(async_load, &event, false));
    g_assert_cmpint(ub_async_load_load_complete(
                        async_load, token, UB_ASYNC_LOAD_STATUS_TIMEOUT,
                        NULL, 0, 200),
                    ==, UB_ASYNC_LOAD_COMPLETION_ACCEPTED);
    g_assert_true(ub_async_load_event_pop(async_load, &event, false));
    g_assert_cmpint(event.kind, ==, UB_ASYNC_LOAD_EVENT_FAULT);
    g_assert_cmpint(event.status, ==, UB_ASYNC_LOAD_STATUS_TIMEOUT);
    g_assert_cmpint(ub_async_load_load_complete(
                        async_load, token, UB_ASYNC_LOAD_STATUS_SUCCESS,
                        NULL, 0, 201),
                    ==, UB_ASYNC_LOAD_COMPLETION_STALE);
    g_assert_cmpuint(ub_async_load_stats(async_load)->faulted_loads, ==, 1);
    g_assert_cmpuint(ub_async_load_stats(async_load)->stale_completions, ==, 1);
}

static void test_replay_retains_and_consumes_completion_once(void)
{
    g_autoptr(UbAsyncLoad) async_load = ub_async_load_new(1, 9, &default_config);
    UbAsyncLoadDesc load = test_load(0x1000, 3, 8, false);
    UbAsyncLoadPltToken token;
    UbAsyncLoadEvent event;
    uint64_t context_id = ub_async_load_context_id_make(9, 0, 2);
    uint64_t value = 0;
    uint8_t payload[] = { 0x88, 0x77, 0x66, 0x55,
                          0x44, 0x33, 0x22, 0x11 };

    g_assert_cmpint(ub_async_load_load_pending(
                        async_load, context_id, &load, &token),
                    ==, UB_ASYNC_LOAD_PENDING_ACCEPTED);
    g_assert_true(ub_async_load_event_pop(async_load, &event, true));
    g_assert_cmpint(ub_async_load_load_complete(
                        async_load, token, UB_ASYNC_LOAD_STATUS_SUCCESS,
                        payload, sizeof(payload), 500),
                    ==, UB_ASYNC_LOAD_COMPLETION_ACCEPTED);
    g_assert_true(ub_async_load_event_pop(async_load, &event, true));
    g_assert_cmpint(event.kind, ==, UB_ASYNC_LOAD_EVENT_COMPLETE);
    g_assert_cmpuint(event.flags & UB_ASYNC_LOAD_EVENT_FLAG_REPLAY_RETIRE,
                     !=, 0);
    g_assert_true(ub_async_load_replay_expected(async_load, context_id));
    g_assert_cmpuint(ub_async_load_pending_count(async_load), ==, 1);
    g_assert_cmpint(ub_async_load_replay_consume(
                        async_load, context_id, &load, &value),
                    ==, UB_ASYNC_LOAD_REPLAY_CONSUMED);
    g_assert_cmphex(value, ==, 0x1122334455667788ULL);
    g_assert_false(ub_async_load_replay_expected(async_load, context_id));
    g_assert_cmpuint(ub_async_load_pending_count(async_load), ==, 0);
    g_assert_cmpuint(ub_async_load_stats(async_load)->replay_consumed, ==, 1);
    g_assert_cmpuint(ub_async_load_stats(async_load)->replay_mismatch, ==, 0);
    g_assert_cmpuint(ub_async_load_stats(async_load)->replay_ready_high_water,
                     ==, 1);
    g_assert_cmpint(ub_async_load_replay_consume(
                        async_load, context_id, &load, &value),
                    ==, UB_ASYNC_LOAD_REPLAY_NONE);
}

static void test_replay_mismatch_fails_closed(void)
{
    g_autoptr(UbAsyncLoad) async_load = ub_async_load_new(1, 9, &default_config);
    UbAsyncLoadDesc load = test_load(0x1000, 3, 8, false);
    UbAsyncLoadDesc mismatch = load;
    UbAsyncLoadPltToken token;
    UbAsyncLoadEvent event;
    uint64_t context_id = ub_async_load_context_id_make(9, 0, 2);
    uint64_t value = 0;
    uint8_t payload[8] = { 0 };

    g_assert_cmpint(ub_async_load_load_pending(
                        async_load, context_id, &load, &token),
                    ==, UB_ASYNC_LOAD_PENDING_ACCEPTED);
    g_assert_true(ub_async_load_event_pop(async_load, &event, true));
    g_assert_cmpint(ub_async_load_load_complete(
                        async_load, token, UB_ASYNC_LOAD_STATUS_SUCCESS,
                        payload, sizeof(payload), 500),
                    ==, UB_ASYNC_LOAD_COMPLETION_ACCEPTED);
    g_assert_true(ub_async_load_event_pop(async_load, &event, true));
    mismatch.effective_va += 8;
    mismatch.remote_offset += 8;
    g_assert_cmpint(ub_async_load_replay_consume(
                        async_load, context_id, &mismatch, &value),
                    ==, UB_ASYNC_LOAD_REPLAY_MISMATCH);
    g_assert_true(ub_async_load_fail_stop(async_load));
    g_assert_cmpuint(ub_async_load_stats(async_load)->replay_consumed, ==, 0);
    g_assert_cmpuint(ub_async_load_stats(async_load)->replay_mismatch, ==, 1);
}

static void test_capacity_and_fail_stop(void)
{
    UbAsyncLoadConfig config = default_config;
    UbAsyncLoadDesc first = test_load(0x1000, 0, 8, false);
    UbAsyncLoadDesc second = test_load(0x2000, 1, 8, false);
    UbAsyncLoadPltToken token;
    UbAsyncLoadPltToken unused;

    config.pending_load_entries = 1;
    config.event_queue_depth = 1;
    g_autoptr(UbAsyncLoad) async_load = ub_async_load_new(1, 1, &config);

    g_assert_cmpint(ub_async_load_load_pending(async_load, 1, &first, &token),
                    ==, UB_ASYNC_LOAD_PENDING_ACCEPTED);
    g_assert_cmpint(ub_async_load_load_pending(async_load, 2, &second, &unused),
                    ==, UB_ASYNC_LOAD_PENDING_SYNC_STALL);
    g_assert_cmpuint(ub_async_load_stats(async_load)->capacity_stalls, ==, 1);
    ub_async_load_mark_fail_stop(async_load);
    g_assert_true(ub_async_load_fail_stop(async_load));
    g_assert_cmpint(ub_async_load_load_pending(async_load, 2, &second, &unused),
                    ==, UB_ASYNC_LOAD_PENDING_INVALID);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/ub/async_load/model-spec", test_model_spec);
    g_test_add_func("/ub/async_load/context-id-logical-ordinal",
                    test_context_id_and_logical_ordinal);
    g_test_add_func("/ub/async_load/pending-complete-events",
                    test_pending_and_complete_events);
    g_test_add_func("/ub/async_load/scalar-value-matrix",
                    test_scalar_value_matrix);
    g_test_add_func("/ub/async_load/pending-priority",
                    test_new_pending_precedes_queued_completion);
    g_test_add_func("/ub/async_load/fault-stale", test_fault_and_stale_completion);
    g_test_add_func("/ub/async_load/replay-consume-once",
                    test_replay_retains_and_consumes_completion_once);
    g_test_add_func("/ub/async_load/replay-mismatch",
                    test_replay_mismatch_fails_closed);
    g_test_add_func("/ub/async_load/capacity-fail-stop",
                    test_capacity_and_fail_stop);
    return g_test_run();
}

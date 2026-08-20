/*
 * Unit tests for the provider-neutral OBMM remote-read backend.
 */

#include "qemu/osdep.h"
#include "hw/ub/ub_obmm_remote.h"

typedef enum TestProviderMode {
    TEST_PROVIDER_INLINE,
    TEST_PROVIDER_PENDING,
    TEST_PROVIDER_REJECT,
} TestProviderMode;

typedef struct TestProvider {
    TestProviderMode mode;
    uint32_t submitted;
    uint32_t cancelled;
    bool map_valid;
} TestProvider;

typedef struct TestSink {
    bool valid;
    ObmmRemoteSinkKind kind;
    uint32_t calls;
    ObmmRemoteResult result;
    uint8_t payload[OBMM_REMOTE_MAX_BYTES];
} TestSink;

static bool test_map_validate(void *opaque, uint64_t map_id,
                              uint64_t map_generation)
{
    TestProvider *provider = opaque;

    return provider->map_valid && map_id == 7 && map_generation == 3;
}

static ObmmProviderChildDisposition test_submit_child(
    void *opaque, ObmmRemoteToken token, uint16_t child_index,
    uint64_t map_id, uint64_t map_generation, uint64_t remote_offset,
    uint32_t length, uint64_t operation_ordinal, void *inline_payload,
    ObmmRemoteStatus *inline_status)
{
    TestProvider *provider = opaque;
    uint8_t *bytes = inline_payload;
    uint32_t index;

    provider->submitted++;
    g_assert_cmpuint(operation_ordinal, ==, 0);
    g_assert_cmpuint(map_id, ==, 7);
    g_assert_cmpuint(map_generation, ==, 3);
    if (provider->mode == TEST_PROVIDER_REJECT) {
        return OBMM_PROVIDER_CHILD_REJECTED;
    }
    if (provider->mode == TEST_PROVIDER_PENDING) {
        return OBMM_PROVIDER_CHILD_PENDING;
    }
    for (index = 0; index < length; index++) {
        bytes[index] = (remote_offset + index) & 0xff;
    }
    *inline_status = OBMM_REMOTE_STATUS_SUCCESS;
    g_assert_cmpuint(token.generation, >, 0);
    g_assert_cmpuint(child_index, <, OBMM_REMOTE_MAX_CHILDREN);
    return OBMM_PROVIDER_CHILD_INLINE;
}

static void test_cancel_provider(void *opaque, ObmmRemoteToken token)
{
    TestProvider *provider = opaque;

    provider->cancelled++;
    g_assert_cmpuint(token.generation, >, 0);
}

static bool test_sink_validate(void *opaque, uint64_t sink_id,
                               uint64_t sink_generation)
{
    TestSink *sink = opaque;

    return sink->valid && sink_id == 9 && sink_generation == 5;
}

static void test_sink_complete(void *opaque,
                               const ObmmRemoteResult *result)
{
    TestSink *sink = opaque;

    sink->calls++;
    sink->result = *result;
    if (result->status == OBMM_REMOTE_STATUS_SUCCESS) {
        memcpy(sink->payload, result->payload, result->bytes_done);
    }
}

static ObmmRemoteBackend *test_backend(TestProvider *provider,
                                       uint32_t chunk_bytes)
{
    ObmmRemoteBackendOps ops = {
        .map_validate = test_map_validate,
        .submit_child = test_submit_child,
        .cancel = test_cancel_provider,
        .opaque = provider,
    };

    return obmm_remote_backend_new(11, chunk_bytes, &ops);
}

static ObmmRemoteRequest test_request(TestSink *sink, uint32_t length,
                                      uint64_t deadline)
{
    return (ObmmRemoteRequest) {
        .map_id = 7,
        .map_generation = 3,
        .remote_offset = 0x1000,
        .length = length,
        .deadline_model_ns = deadline,
        .operation_key = 0x1234,
        .sink = {
            .kind = sink->kind,
            .sink_id = 9,
            .sink_generation = 5,
            .owner_id = 11,
            .validate = test_sink_validate,
            .complete = test_sink_complete,
            .adapter_state = sink,
        },
    };
}

static void test_complete_children(ObmmRemoteBackend *backend,
                                   ObmmRemoteToken token,
                                   uint32_t length, bool reverse,
                                   uint64_t publish_ns)
{
    uint8_t payload[4096];
    uint16_t child_count = DIV_ROUND_UP(length, sizeof(payload));
    uint16_t ordinal;

    for (ordinal = 0; ordinal < child_count; ordinal++) {
        uint16_t child = reverse ? child_count - ordinal - 1 : ordinal;
        uint32_t offset = child * sizeof(payload);
        uint32_t child_length = MIN((uint32_t)sizeof(payload),
                                    length - offset);
        uint32_t index;

        for (index = 0; index < child_length; index++) {
            payload[index] = (0x1000 + offset + index) & 0xff;
        }
        g_assert_true(obmm_remote_child_complete(
            backend, token, child, OBMM_REMOTE_STATUS_SUCCESS,
            payload, child_length, publish_ns + ordinal));
    }
}

static bool test_parse_sink(const char *name, ObmmRemoteSinkKind *kind)
{
    if (g_str_equal(name, "test")) {
        *kind = OBMM_REMOTE_SINK_TEST;
    } else if (g_str_equal(name, "p2a")) {
        *kind = OBMM_REMOTE_SINK_P2A;
    } else if (g_str_equal(name, "async-load")) {
        *kind = OBMM_REMOTE_SINK_ASYNC_LOAD;
    } else {
        return false;
    }
    return true;
}

static int test_run_conformance(const char *sink_name,
                                const char *case_name,
                                uint32_t access_bytes)
{
    TestProvider provider = {
        .mode = g_str_equal(case_name, "inline") ?
            TEST_PROVIDER_INLINE : TEST_PROVIDER_PENDING,
        .map_valid = true,
    };
    ObmmRemoteSinkKind sink_kind;
    g_autoptr(ObmmRemoteBackend) backend = NULL;
    g_autofree TestSink *sinks = NULL;
    ObmmRemoteToken tokens[OBMM_REMOTE_PARENT_CAPACITY] = { 0 };
    ObmmRemoteResult inline_result;
    ObmmRemoteStatus rejected = OBMM_REMOTE_STATUS_INTERNAL;
    const ObmmRemoteBackendStats *stats;
    uint64_t checksum = 0;
    uint32_t request_count = 1;
    uint32_t index;
    bool pass = true;

    if (!test_parse_sink(sink_name, &sink_kind) || !access_bytes ||
        access_bytes > OBMM_REMOTE_MAX_BYTES ||
        (sink_kind == OBMM_REMOTE_SINK_ASYNC_LOAD && access_bytes > 8)) {
        return 2;
    }
    if (g_str_equal(case_name, "inflight64") ||
        g_str_equal(case_name, "capacity")) {
        request_count = OBMM_REMOTE_PARENT_CAPACITY;
    } else if (g_str_equal(case_name, "cancel-race")) {
        request_count = 2;
    }
    sinks = g_new0(TestSink, request_count + 1);
    backend = test_backend(&provider, 4096);
    for (index = 0; index < request_count; index++) {
        ObmmRemoteRequest request;
        ObmmSubmitDisposition disposition;

        sinks[index].valid = true;
        sinks[index].kind = sink_kind;
        request = test_request(&sinks[index], access_bytes,
                               g_str_equal(case_name, "timeout") ? 50 : 0);
        request.remote_offset += (uint64_t)index * access_bytes;
        request.operation_key += index;
        disposition = obmm_remote_submit(
            backend, &request, 1, &tokens[index],
            &inline_result, &rejected);
        if (provider.mode == TEST_PROVIDER_INLINE) {
            pass &= disposition == OBMM_SUBMIT_INLINE &&
                inline_result.status == OBMM_REMOTE_STATUS_SUCCESS;
            checksum ^= inline_result.checksum64;
        } else {
            pass &= disposition == OBMM_SUBMIT_PENDING;
        }
    }
    if (g_str_equal(case_name, "capacity")) {
        ObmmRemoteRequest request;
        ObmmRemoteToken token;

        sinks[request_count].valid = true;
        sinks[request_count].kind = sink_kind;
        request = test_request(&sinks[request_count], access_bytes, 0);
        pass &= obmm_remote_submit(
                    backend, &request, 1, &token,
                    &inline_result, &rejected) == OBMM_SUBMIT_REJECTED &&
            rejected == OBMM_REMOTE_STATUS_CAPACITY;
    }
    if (provider.mode == TEST_PROVIDER_PENDING) {
        if (g_str_equal(case_name, "timeout")) {
            pass &= obmm_remote_run_deadlines(backend, 50) == 1;
            pass &= obmm_remote_deliver_ready(backend) == 1;
            pass &= !obmm_remote_child_complete(
                backend, tokens[0], 0, OBMM_REMOTE_STATUS_SUCCESS,
                "12345678", MIN(access_bytes, 8), 51);
        } else if (g_str_equal(case_name, "cancel-race")) {
            test_complete_children(backend, tokens[0], access_bytes,
                                   false, 20);
            pass &= obmm_remote_cancel(
                        backend, tokens[0], OBMM_REMOTE_STATUS_CANCELLED,
                        21) == OBMM_CANCEL_ALREADY_TERMINAL;
            pass &= obmm_remote_cancel(
                        backend, tokens[1], OBMM_REMOTE_STATUS_CANCELLED,
                        20) == OBMM_CANCELLED_NOW;
            pass &= !obmm_remote_child_complete(
                backend, tokens[1], 0, OBMM_REMOTE_STATUS_SUCCESS,
                "12345678", MIN(access_bytes, 8), 21);
            pass &= obmm_remote_deliver_ready(backend) == 2;
        } else if (g_str_equal(case_name, "retire")) {
            pass &= obmm_remote_retire_map(backend, 7, 3, 20) == 1;
            pass &= obmm_remote_deliver_ready(backend) == 1;
        } else {
            for (index = request_count; index > 0; index--) {
                test_complete_children(
                    backend, tokens[index - 1], access_bytes,
                    g_str_equal(case_name, "reorder"), 20 + index);
                if (g_str_equal(case_name, "duplicate") && index == 1) {
                    pass &= !obmm_remote_child_complete(
                        backend, tokens[0], 0,
                        OBMM_REMOTE_STATUS_SUCCESS,
                        "12345678", MIN(access_bytes, 8), 40);
                }
            }
            pass &= obmm_remote_deliver_ready(backend) == request_count;
        }
        for (index = 0; index < request_count; index++) {
            if (sinks[index].calls == 1) {
                checksum ^= sinks[index].result.checksum64;
            } else {
                pass = false;
            }
        }
    }
    stats = obmm_remote_backend_stats(backend);
    pass &= obmm_remote_pending(backend) == 0;
    printf("OBMM_P1_SUMMARY schema=1 sink=%s case=%s accepted=%" PRIu64
           " delivered=%" PRIu64 " late=%" PRIu64
           " duplicate=%" PRIu64 " checksum=%016" PRIx64
           " pending=%u status=%s\n",
           sink_name, case_name, stats->accepted, stats->delivered,
           stats->late, stats->duplicate, checksum,
           obmm_remote_pending(backend), pass ? "pass" : "fail");
    return pass ? 0 : 1;
}

static void test_inline(void)
{
    TestProvider provider = {
        .mode = TEST_PROVIDER_INLINE,
        .map_valid = true,
    };
    TestSink sink = { .valid = true };
    g_autoptr(ObmmRemoteBackend) backend = test_backend(&provider, 4096);
    ObmmRemoteRequest request = test_request(&sink, 8, 0);
    ObmmRemoteResult result;
    ObmmRemoteStatus rejected;
    ObmmRemoteToken token;
    uint8_t expected[] = { 0, 1, 2, 3, 4, 5, 6, 7 };

    g_assert_nonnull(backend);
    g_assert_cmpint(obmm_remote_submit(backend, &request, 10, &token,
                                      &result, &rejected),
                    ==, OBMM_SUBMIT_INLINE);
    g_assert_cmpint(result.status, ==, OBMM_REMOTE_STATUS_SUCCESS);
    g_assert_cmpuint(result.bytes_done, ==, 8);
    g_assert_cmpmem(result.payload, 8, expected, sizeof(expected));
    g_assert_cmpuint(sink.calls, ==, 0);
    g_assert_cmpuint(obmm_remote_pending(backend), ==, 0);
}

static void test_stats_reset_requires_drain(void)
{
    TestProvider provider = {
        .mode = TEST_PROVIDER_INLINE,
        .map_valid = true,
    };
    TestSink sink = { .valid = true };
    g_autoptr(ObmmRemoteBackend) backend = test_backend(&provider, 4096);
    ObmmRemoteRequest request = test_request(&sink, 8, 0);
    ObmmRemoteResult result;
    ObmmRemoteStatus rejected;
    ObmmRemoteToken token;
    const ObmmRemoteBackendStats *stats;

    g_assert_cmpint(obmm_remote_submit(backend, &request, 10, &token,
                                      &result, &rejected),
                    ==, OBMM_SUBMIT_INLINE);
    stats = obmm_remote_backend_stats(backend);
    g_assert_cmpuint(stats->accepted, ==, 1);
    g_assert_cmpuint(stats->pending_high_water, ==, 1);
    g_assert_true(obmm_remote_backend_reset_stats(backend));
    g_assert_cmpuint(stats->accepted, ==, 0);
    g_assert_cmpuint(stats->pending_high_water, ==, 0);

    provider.mode = TEST_PROVIDER_PENDING;
    g_assert_cmpint(obmm_remote_submit(backend, &request, 10, &token,
                                      &result, &rejected),
                    ==, OBMM_SUBMIT_PENDING);
    g_assert_false(obmm_remote_backend_reset_stats(backend));
    g_assert_cmpuint(stats->accepted, ==, 1);
}

static void test_capacity_and_stale_token(void)
{
    TestProvider provider = {
        .mode = TEST_PROVIDER_PENDING,
        .map_valid = true,
    };
    TestSink sinks[OBMM_REMOTE_PARENT_CAPACITY + 1] = { 0 };
    ObmmRemoteToken tokens[OBMM_REMOTE_PARENT_CAPACITY];
    g_autoptr(ObmmRemoteBackend) backend = test_backend(&provider, 4096);
    ObmmRemoteResult inline_result;
    ObmmRemoteStatus rejected;
    uint8_t payload[8] = { 0 };
    uint32_t index;

    for (index = 0; index < OBMM_REMOTE_PARENT_CAPACITY; index++) {
        ObmmRemoteRequest request;

        sinks[index].valid = true;
        request = test_request(&sinks[index], sizeof(payload), 0);
        g_assert_cmpint(obmm_remote_submit(
                            backend, &request, 10, &tokens[index],
                            &inline_result, &rejected),
                        ==, OBMM_SUBMIT_PENDING);
    }
    sinks[OBMM_REMOTE_PARENT_CAPACITY].valid = true;
    {
        ObmmRemoteRequest request = test_request(
            &sinks[OBMM_REMOTE_PARENT_CAPACITY], sizeof(payload), 0);
        ObmmRemoteToken rejected_token;

        g_assert_cmpint(obmm_remote_submit(
                            backend, &request, 10, &rejected_token,
                            &inline_result, &rejected),
                        ==, OBMM_SUBMIT_REJECTED);
        g_assert_cmpint(rejected, ==, OBMM_REMOTE_STATUS_CAPACITY);
    }

    for (index = OBMM_REMOTE_PARENT_CAPACITY; index > 0; index--) {
        g_assert_true(obmm_remote_child_complete(
            backend, tokens[index - 1], 0, OBMM_REMOTE_STATUS_SUCCESS,
            payload, sizeof(payload), 20));
    }
    g_assert_cmpuint(obmm_remote_deliver_ready(backend), ==,
                     OBMM_REMOTE_PARENT_CAPACITY);
    g_assert_cmpuint(obmm_remote_pending(backend), ==, 0);
    g_assert_false(obmm_remote_child_complete(
        backend, tokens[0], 0, OBMM_REMOTE_STATUS_SUCCESS,
        payload, sizeof(payload), 30));
    g_assert_cmpuint(obmm_remote_backend_stats(backend)->late, ==, 1);
}

static void test_multichild_reverse_completion(void)
{
    TestProvider provider = {
        .mode = TEST_PROVIDER_PENDING,
        .map_valid = true,
    };
    TestSink sink = { .valid = true };
    g_autoptr(ObmmRemoteBackend) backend = test_backend(&provider, 4096);
    ObmmRemoteRequest request = test_request(
        &sink, OBMM_REMOTE_MAX_BYTES, 0);
    ObmmRemoteResult inline_result;
    ObmmRemoteStatus rejected;
    ObmmRemoteToken token;
    uint8_t child_payload[4096];
    uint32_t child;

    g_assert_cmpint(obmm_remote_submit(backend, &request, 1, &token,
                                      &inline_result, &rejected),
                    ==, OBMM_SUBMIT_PENDING);
    g_assert_cmpuint(provider.submitted, ==, 16);
    for (child = 16; child > 0; child--) {
        uint32_t index;

        for (index = 0; index < sizeof(child_payload); index++) {
            child_payload[index] = ((child - 1) * 4096 + index) & 0xff;
        }
        g_assert_true(obmm_remote_child_complete(
            backend, token, child - 1, OBMM_REMOTE_STATUS_SUCCESS,
            child_payload, sizeof(child_payload), 100 + child));
    }
    g_assert_cmpuint(obmm_remote_deliver_ready(backend), ==, 1);
    g_assert_cmpuint(sink.calls, ==, 1);
    g_assert_cmpint(sink.result.status, ==, OBMM_REMOTE_STATUS_SUCCESS);
    for (child = 0; child < OBMM_REMOTE_MAX_BYTES; child++) {
        g_assert_cmpuint(sink.payload[child], ==, child & 0xff);
    }
}

static void test_timeout_wins_once(void)
{
    TestProvider provider = {
        .mode = TEST_PROVIDER_PENDING,
        .map_valid = true,
    };
    TestSink sink = { .valid = true };
    g_autoptr(ObmmRemoteBackend) backend = test_backend(&provider, 4096);
    ObmmRemoteRequest request = test_request(&sink, 8, 50);
    ObmmRemoteResult inline_result;
    ObmmRemoteStatus rejected;
    ObmmRemoteToken token;
    uint8_t payload[8] = { 0 };

    g_assert_cmpint(obmm_remote_submit(backend, &request, 1, &token,
                                      &inline_result, &rejected),
                    ==, OBMM_SUBMIT_PENDING);
    g_assert_cmpuint(obmm_remote_run_deadlines(backend, 50), ==, 1);
    g_assert_false(obmm_remote_child_complete(
        backend, token, 0, OBMM_REMOTE_STATUS_SUCCESS,
        payload, sizeof(payload), 51));
    g_assert_cmpuint(obmm_remote_deliver_ready(backend), ==, 1);
    g_assert_cmpuint(sink.calls, ==, 1);
    g_assert_cmpint(sink.result.status, ==, OBMM_REMOTE_STATUS_TIMEOUT);
    g_assert_cmpuint(provider.cancelled, ==, 1);
    g_assert_cmpint(obmm_remote_cancel(
                        backend, token, OBMM_REMOTE_STATUS_CANCELLED, 52),
                    ==, OBMM_CANCEL_STALE_TOKEN);
}

static void test_stale_sink_and_retire(void)
{
    TestProvider provider = {
        .mode = TEST_PROVIDER_PENDING,
        .map_valid = true,
    };
    TestSink sink = { .valid = false };
    g_autoptr(ObmmRemoteBackend) backend = test_backend(&provider, 4096);
    ObmmRemoteRequest request = test_request(&sink, 8, 0);
    ObmmRemoteResult inline_result;
    ObmmRemoteStatus rejected;
    ObmmRemoteToken token;

    g_assert_cmpint(obmm_remote_submit(backend, &request, 1, &token,
                                      &inline_result, &rejected),
                    ==, OBMM_SUBMIT_PENDING);
    g_assert_cmpuint(obmm_remote_retire_map(backend, 7, 3, 10), ==, 1);
    g_assert_cmpuint(obmm_remote_deliver_ready(backend), ==, 1);
    g_assert_cmpuint(sink.calls, ==, 1);
    g_assert_cmpint(sink.result.status, ==, OBMM_REMOTE_STATUS_STALE_SINK);
}

int main(int argc, char **argv)
{
    if (argc == 7 && g_str_equal(argv[1], "--conformance") &&
        g_str_equal(argv[2], "--sink") &&
        g_str_equal(argv[4], "--case") &&
        g_str_has_prefix(argv[6], "--access-bytes=")) {
        uint64_t access_bytes = g_ascii_strtoull(
            argv[6] + strlen("--access-bytes="), NULL, 10);

        return access_bytes <= UINT32_MAX ?
            test_run_conformance(argv[3], argv[5], access_bytes) : 2;
    }
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/obmm-remote/inline", test_inline);
    g_test_add_func("/obmm-remote/stats-reset",
                    test_stats_reset_requires_drain);
    g_test_add_func("/obmm-remote/capacity-and-stale-token",
                    test_capacity_and_stale_token);
    g_test_add_func("/obmm-remote/multichild-reverse-completion",
                    test_multichild_reverse_completion);
    g_test_add_func("/obmm-remote/timeout-wins-once",
                    test_timeout_wins_once);
    g_test_add_func("/obmm-remote/stale-sink-and-retire",
                    test_stale_sink_and_retire);
    return g_test_run();
}

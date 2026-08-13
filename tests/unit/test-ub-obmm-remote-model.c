/*
 * Unit tests for the deterministic OBMM remote-memory model.
 */

#include "qemu/osdep.h"
#include "hw/ub/ub_obmm_remote_model.h"

static UbObmmRemoteModelConfig test_config(void)
{
    return (UbObmmRemoteModelConfig) {
        .enabled = true,
        .fixed_latency_ns = 100000,
        .jitter_mode = UB_OBMM_REMOTE_JITTER_UNIFORM,
        .jitter_max_abs_ns = 20000,
        .tail_probability_ppm = 10000,
        .tail_extra_latency_ns = 900000,
        .queue_depth = 2,
        .reorder_window = 2,
        .drop_ppm = 1000,
        .error_ppm = 2000,
        .duplicate_ppm = 3000,
        .duplicate_delay_ns = 1000,
        .seed = 7,
    };
}

static UbObmmRemoteOperation test_operation(uint64_t ordinal)
{
    return (UbObmmRemoteOperation) {
        .map_id = 4,
        .map_generation = 2,
        .remote_offset = 0x123400 + ordinal * 8,
        .length = 8,
        .per_range_ordinal = ordinal,
    };
}

static void test_golden_decision(void)
{
    UbObmmRemoteModelConfig config = test_config();
    UbObmmRemoteOperation operation = test_operation(3);
    UbObmmRemoteDecision decision;

    decision = ub_obmm_remote_model_decide(&config, &operation);

    g_assert_cmphex(decision.operation_key, ==, 0xa38f126beeafbc89ULL);
    g_assert_cmpint(decision.outcome, ==, UB_OBMM_REMOTE_SUCCESS);
    g_assert_cmpint(decision.jitter_ns, ==, -15157);
    g_assert_false(decision.tail_applied);
    g_assert_cmpuint(decision.service_ns, ==, 84843);
    g_assert_false(decision.duplicate);
    g_assert_cmphex(decision.reorder_key, ==, 0xf60772f583d5d19eULL);
}

static void test_capacity(void)
{
    UbObmmRemoteModelState model;
    UbObmmRemoteDecision decision;
    UbObmmRemoteOperation operation;

    ub_obmm_remote_model_init(&model);
    model.config = test_config();

    operation = test_operation(0);
    g_assert_true(ub_obmm_remote_model_try_accept(&model, &operation,
                                                   &decision));
    operation = test_operation(1);
    g_assert_true(ub_obmm_remote_model_try_accept(&model, &operation,
                                                   &decision));
    operation = test_operation(2);
    g_assert_false(ub_obmm_remote_model_try_accept(&model, &operation,
                                                    &decision));
    g_assert_cmpuint(model.pending, ==, 2);
    g_assert_cmpuint(model.capacity_rejected, ==, 1);

    g_assert_true(ub_obmm_remote_model_release(&model));
    g_assert_true(ub_obmm_remote_model_try_accept(&model, &operation,
                                                   &decision));
}

typedef struct TestPublishCapture {
    uint32_t calls;
    uint32_t duplicate_calls;
    UbObmmRemoteOutcome outcome;
    uint64_t accept_ns;
    uint64_t due_ns;
    uint64_t publish_ns;
} TestPublishCapture;

static void capture_publish(void *opaque,
                            const UbObmmRemoteDecision *decision,
                            bool duplicate, uint64_t accept_ns,
                            uint64_t due_ns, uint64_t publish_ns)
{
    TestPublishCapture *capture = opaque;

    capture->calls++;
    capture->duplicate_calls += duplicate;
    capture->outcome = decision->outcome;
    capture->accept_ns = accept_ns;
    capture->due_ns = due_ns;
    capture->publish_ns = publish_ns;
}

static void test_virtual_due_and_duplicate(void)
{
    UbObmmRemoteModelState model;
    UbObmmRemoteModelConfig config = test_config();
    UbObmmRemoteOperation operation = test_operation(3);
    UbObmmRemoteDecision decision;
    TestPublishCapture capture = { 0 };

    config.fixed_latency_ns = 1000;
    config.jitter_mode = UB_OBMM_REMOTE_JITTER_NONE;
    config.jitter_max_abs_ns = 0;
    config.tail_probability_ppm = 0;
    config.drop_ppm = 0;
    config.error_ppm = 0;
    config.duplicate_ppm = UB_OBMM_REMOTE_MODEL_PPM_SCALE;
    config.duplicate_delay_ns = 50;
    ub_obmm_remote_model_init(&model);
    model.config = config;

    g_assert_true(ub_obmm_remote_model_enqueue(
        &model, &operation, 100, NULL, capture_publish, NULL, &capture,
        &decision));
    g_assert_cmpuint(ub_obmm_remote_model_next_due_ns(&model), ==, 1100);
    g_assert_cmpuint(ub_obmm_remote_model_run_due(&model, 1099), ==, 0);
    g_assert_cmpuint(capture.calls, ==, 0);
    g_assert_cmpuint(ub_obmm_remote_model_run_due(&model, 1100), ==, 1);
    g_assert_cmpuint(capture.calls, ==, 1);
    g_assert_cmpuint(capture.duplicate_calls, ==, 0);
    g_assert_cmpuint(capture.accept_ns, ==, 100);
    g_assert_cmpuint(capture.due_ns, ==, 1100);
    g_assert_cmpuint(capture.publish_ns, ==, 1100);
    g_assert_cmpuint(model.pending, ==, 1);
    g_assert_cmpuint(ub_obmm_remote_model_next_due_ns(&model), ==, 1150);

    g_assert_cmpuint(ub_obmm_remote_model_run_due(&model, 1150), ==, 1);
    g_assert_cmpuint(capture.calls, ==, 2);
    g_assert_cmpuint(capture.duplicate_calls, ==, 1);
    g_assert_cmpuint(model.pending, ==, 0);
    g_assert_cmpuint(model.completed, ==, 1);
    ub_obmm_remote_model_cleanup(&model);
}

static void test_drop_does_not_publish(void)
{
    UbObmmRemoteModelState model;
    UbObmmRemoteModelConfig config = test_config();
    UbObmmRemoteOperation operation = test_operation(0);
    UbObmmRemoteDecision decision;
    TestPublishCapture capture = { 0 };

    config.fixed_latency_ns = 10;
    config.jitter_mode = UB_OBMM_REMOTE_JITTER_NONE;
    config.jitter_max_abs_ns = 0;
    config.tail_probability_ppm = 0;
    config.drop_ppm = UB_OBMM_REMOTE_MODEL_PPM_SCALE;
    config.error_ppm = 0;
    ub_obmm_remote_model_init(&model);
    model.config = config;

    g_assert_true(ub_obmm_remote_model_enqueue(
        &model, &operation, 5, NULL, capture_publish, NULL, &capture,
        &decision));
    g_assert_cmpint(decision.outcome, ==, UB_OBMM_REMOTE_DROP);
    g_assert_cmpuint(ub_obmm_remote_model_run_due(&model, 15), ==, 1);
    g_assert_cmpuint(capture.calls, ==, 0);
    g_assert_cmpuint(model.pending, ==, 0);
    g_assert_cmpuint(model.dropped, ==, 1);
    ub_obmm_remote_model_cleanup(&model);
}

static void test_observability_reset_requires_drain(void)
{
    UbObmmRemoteModelState model;
    UbObmmRemoteModelConfig config = test_config();
    UbObmmRemoteOperation operation = test_operation(0);
    UbObmmRemoteDecision decision;
    TestPublishCapture capture = { 0 };

    config.fixed_latency_ns = 1000;
    config.jitter_mode = UB_OBMM_REMOTE_JITTER_NONE;
    config.jitter_max_abs_ns = 0;
    config.tail_probability_ppm = 0;
    config.drop_ppm = 0;
    config.error_ppm = 0;
    config.duplicate_ppm = 0;
    ub_obmm_remote_model_init(&model);
    model.config = config;

    g_assert_true(ub_obmm_remote_model_enqueue(
        &model, &operation, 100, NULL, capture_publish, NULL, &capture,
        &decision));
    g_assert_false(ub_obmm_remote_model_reset_stats(&model));
    g_assert_cmpuint(model.total_service_ns, ==, 1000);
    g_assert_cmpuint(ub_obmm_remote_model_run_due(&model, 1100), ==, 1);
    g_assert_cmpuint(model.total_accept_to_publish_ns, ==, 1000);
    g_assert_true(ub_obmm_remote_model_reset_stats(&model));
    g_assert_cmpuint(model.accepted, ==, 0);
    g_assert_cmpuint(model.completed, ==, 0);
    g_assert_cmpuint(model.total_service_ns, ==, 0);
    g_assert_cmpuint(model.total_accept_to_publish_ns, ==, 0);
    ub_obmm_remote_model_cleanup(&model);
}

static void test_manifest_load(void)
{
    static const char manifest[] =
        "{\"schema\":1,"
        "\"manifest_hash\":\"fnv1a64:54431162a1abe3be\","
        "\"scenario_name\":\"mvp_2host_single_domain\","
        "\"scenario_seed\":42,"
        "\"remote_memory_model\":{"
        "\"enabled\":true,\"time_source\":\"qemu_virtual\","
        "\"fixed_latency_ns\":100000,"
        "\"jitter\":{\"mode\":\"uniform\",\"max_abs_ns\":20000},"
        "\"tail\":{\"probability_ppm\":0,\"extra_latency_ns\":0},"
        "\"queue_depth\":64,\"reorder_window\":8,"
        "\"drop_ppm\":0,\"error_ppm\":0,\"duplicate_ppm\":0,"
        "\"duplicate_delay_ns\":1000,\"seed\":1}}";
    g_autofree char *path = NULL;
    Error *error = NULL;
    UbObmmRemoteModelState model;
    int fd;

    fd = g_file_open_tmp("obmm-remote-model-XXXXXX.json", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    g_assert_true(g_file_set_contents(path, manifest, -1, NULL));

    ub_obmm_remote_model_init(&model);
    g_assert_true(ub_obmm_remote_model_load(&model, path, &error));
    g_assert_null(error);
    g_assert_true(model.loaded);
    g_assert_true(model.config.enabled);
    g_assert_cmpuint(model.config.fixed_latency_ns, ==, 100000);
    g_assert_cmpstr(model.manifest_hash, ==,
                    "fnv1a64:54431162a1abe3be");

    ub_obmm_remote_model_cleanup(&model);
    error_free(error);
    unlink(path);
}

static void test_manifest_hash_mismatch(void)
{
    static const char manifest[] =
        "{\"schema\":1,\"manifest_hash\":\"fnv1a64:0000000000000000\","
        "\"scenario_name\":\"x\",\"scenario_seed\":1,"
        "\"remote_memory_model\":{\"enabled\":false,"
        "\"time_source\":\"qemu_virtual\",\"fixed_latency_ns\":0,"
        "\"jitter\":{\"mode\":\"none\",\"max_abs_ns\":0},"
        "\"tail\":{\"probability_ppm\":0,\"extra_latency_ns\":0},"
        "\"queue_depth\":64,\"reorder_window\":1,\"drop_ppm\":0,"
        "\"error_ppm\":0,\"duplicate_ppm\":0,"
        "\"duplicate_delay_ns\":1000,\"seed\":1}}";
    g_autofree char *path = NULL;
    Error *error = NULL;
    UbObmmRemoteModelState model;
    int fd;

    fd = g_file_open_tmp("obmm-remote-model-XXXXXX.json", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    g_assert_true(g_file_set_contents(path, manifest, -1, NULL));

    ub_obmm_remote_model_init(&model);
    g_assert_false(ub_obmm_remote_model_load(&model, path, &error));
    g_assert_nonnull(error);
    g_assert_nonnull(strstr(error_get_pretty(error), "hash mismatch"));

    error_free(error);
    ub_obmm_remote_model_cleanup(&model);
    unlink(path);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/obmm-remote-model/golden-decision",
                    test_golden_decision);
    g_test_add_func("/obmm-remote-model/capacity", test_capacity);
    g_test_add_func("/obmm-remote-model/virtual-due-and-duplicate",
                    test_virtual_due_and_duplicate);
    g_test_add_func("/obmm-remote-model/drop-does-not-publish",
                    test_drop_does_not_publish);
    g_test_add_func("/obmm-remote-model/observability-reset",
                    test_observability_reset_requires_drain);
    g_test_add_func("/obmm-remote-model/manifest-load", test_manifest_load);
    g_test_add_func("/obmm-remote-model/manifest-hash-mismatch",
                    test_manifest_hash_mismatch);
    return g_test_run();
}

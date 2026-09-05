/*
 * Unit tests for the reusable UB void-response policy.
 */

#include "qemu/osdep.h"
#include "hw/ub/ub_void_response_policy.h"

static UbVoidResponseRequest test_request(uint32_t request_id)
{
    return (UbVoidResponseRequest) {
        .source_cna = 0x11,
        .request_id = request_id,
        .remote_address = 0x40000000,
        .length = 8,
        .arrival_ns = 100,
    };
}

static void test_disabled(void)
{
    UbVoidResponsePolicy policy;
    UbVoidResponseRequest request = test_request(1);
    UbVoidResponseDecision decision;

    ub_void_response_policy_init(&policy);
    decision = ub_void_response_policy_decide(&policy, &request);
    g_assert_false(decision.send_void);
    g_assert_cmpuint(decision.completion_delay_ns, ==, 0);
    g_assert_cmpuint(policy.stats.normal_selected, ==, 1);
}

static void test_threshold(void)
{
    UbVoidResponsePolicy policy;
    UbVoidResponseRequest request = test_request(2);
    UbVoidResponseDecision decision;
    Error *error = NULL;

    g_assert_true(ub_void_response_policy_configure(
        &policy,
        "v1|enabled=1|threshold_ns=1000|latency_ns=2000|jitter_ns=0|"
        "fault_voids=0|seed=1",
        &error));
    g_assert_null(error);
    decision = ub_void_response_policy_decide(&policy, &request);
    g_assert_true(decision.send_void);
    g_assert_cmpuint(decision.completion_delay_ns, ==, 2000);
    g_assert_cmpint(decision.reason, ==,
                    UB_VOID_RESPONSE_LATENCY_THRESHOLD);
}

static void test_fault_then_recover(void)
{
    UbVoidResponsePolicy policy;
    UbVoidResponseRequest first = test_request(3);
    UbVoidResponseRequest second = test_request(4);
    UbVoidResponseRequest third = test_request(5);
    Error *error = NULL;

    g_assert_true(ub_void_response_policy_configure(
        &policy,
        "v1|enabled=1|threshold_ns=1000|latency_ns=500|jitter_ns=0|"
        "fault_voids=2|seed=7",
        &error));
    g_assert_true(ub_void_response_policy_decide(&policy, &first).send_void);
    g_assert_true(ub_void_response_policy_decide(&policy, &second).send_void);
    g_assert_false(ub_void_response_policy_decide(&policy, &third).send_void);
    g_assert_cmpuint(policy.stats.fault_injected, ==, 2);
    g_assert_cmpuint(policy.stats.normal_selected, ==, 1);
}

static void test_jitter_is_deterministic(void)
{
    UbVoidResponsePolicy first_policy;
    UbVoidResponsePolicy second_policy;
    UbVoidResponseRequest request = test_request(9);
    UbVoidResponseDecision first;
    UbVoidResponseDecision second;
    const char *spec =
        "v1|enabled=1|threshold_ns=1000|latency_ns=800|jitter_ns=200|"
        "fault_voids=0|seed=99";

    g_assert_true(ub_void_response_policy_configure(
        &first_policy, spec, &error_abort));
    g_assert_true(ub_void_response_policy_configure(
        &second_policy, spec, &error_abort));
    first = ub_void_response_policy_decide(&first_policy, &request);
    second = ub_void_response_policy_decide(&second_policy, &request);
    g_assert_cmpint(first.jitter_ns, ==, second.jitter_ns);
    g_assert_cmpuint(first.completion_delay_ns, ==,
                     second.completion_delay_ns);
}

static void test_invalid_spec(void)
{
    UbVoidResponsePolicy policy;
    Error *error = NULL;

    g_assert_false(ub_void_response_policy_configure(
        &policy,
        "v1|enabled=1|threshold_ns=1|latency_ns=10|jitter_ns=11|"
        "fault_voids=0|seed=1",
        &error));
    g_assert_nonnull(error);
    error_free(error);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/ub/void-response/disabled", test_disabled);
    g_test_add_func("/ub/void-response/threshold", test_threshold);
    g_test_add_func("/ub/void-response/fault-recovery",
                    test_fault_then_recover);
    g_test_add_func("/ub/void-response/deterministic-jitter",
                    test_jitter_is_deterministic);
    g_test_add_func("/ub/void-response/invalid-spec", test_invalid_spec);
    return g_test_run();
}

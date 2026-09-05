/*
 * Reusable source- or destination-side policy for UB remote-read void
 * responses.
 */

#include "qemu/osdep.h"
#include "hw/ub/ub_void_response_policy.h"

#define UB_VOID_RESPONSE_DEFAULT_SEED 1ULL
#define UB_VOID_RESPONSE_JITTER_LANE 0x6a09e667f3bcc909ULL

static uint64_t ub_void_response_mix64(uint64_t value)
{
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

static uint64_t ub_void_response_request_key(
    const UbVoidResponsePolicyConfig *config,
    const UbVoidResponseRequest *request)
{
    uint64_t key = config->seed;

    key = ub_void_response_mix64(key ^ request->source_cna);
    key = ub_void_response_mix64(key ^ request->request_id);
    key = ub_void_response_mix64(key ^ request->remote_address);
    key = ub_void_response_mix64(key ^ request->length);
    return key;
}

static int64_t ub_void_response_jitter(
    const UbVoidResponsePolicyConfig *config,
    const UbVoidResponseRequest *request)
{
    uint64_t width;
    uint64_t draw;

    if (!config->jitter_ns) {
        return 0;
    }
    if (config->jitter_ns > (UINT64_MAX - 1) / 2) {
        return 0;
    }
    width = config->jitter_ns * 2 + 1;
    draw = ub_void_response_mix64(
        ub_void_response_request_key(config, request) ^
        UB_VOID_RESPONSE_JITTER_LANE);
    return (int64_t)(draw % width) - (int64_t)config->jitter_ns;
}

void ub_void_response_policy_init(UbVoidResponsePolicy *policy)
{
    if (!policy) {
        return;
    }
    memset(policy, 0, sizeof(*policy));
    policy->config.seed = UB_VOID_RESPONSE_DEFAULT_SEED;
}

bool ub_void_response_policy_configure(UbVoidResponsePolicy *policy,
                                       const char *spec, Error **errp)
{
    unsigned int enabled;
    uint64_t threshold_ns;
    uint64_t latency_ns;
    uint64_t jitter_ns;
    uint64_t fault_voids;
    uint64_t seed;
    int consumed = 0;

    if (!policy) {
        error_setg(errp, "void-response policy state is missing");
        return false;
    }
    ub_void_response_policy_init(policy);
    if (!spec || !*spec || strcmp(spec, "off") == 0) {
        return true;
    }
    if (sscanf(spec,
               "v1|enabled=%u|threshold_ns=%" SCNu64
               "|latency_ns=%" SCNu64 "|jitter_ns=%" SCNu64
               "|fault_voids=%" SCNu64 "|seed=%" SCNu64 "%n",
               &enabled, &threshold_ns, &latency_ns, &jitter_ns,
               &fault_voids, &seed, &consumed) != 6 ||
        spec[consumed] != '\0' || enabled > 1 || seed == 0 ||
        threshold_ns > UB_VOID_RESPONSE_POLICY_MAX_LATENCY_NS ||
        latency_ns > UB_VOID_RESPONSE_POLICY_MAX_LATENCY_NS ||
        jitter_ns > UB_VOID_RESPONSE_POLICY_MAX_LATENCY_NS ||
        jitter_ns > latency_ns) {
        error_setg(errp,
                   "invalid void-response policy; expected "
                   "v1|enabled=0|1|threshold_ns=N|latency_ns=N|"
                   "jitter_ns=N|fault_voids=N|seed=N");
        return false;
    }
    policy->config = (UbVoidResponsePolicyConfig) {
        .enabled = enabled,
        .threshold_ns = threshold_ns,
        .latency_ns = latency_ns,
        .jitter_ns = jitter_ns,
        .fault_voids = fault_voids,
        .seed = seed,
    };
    return true;
}

UbVoidResponseDecision ub_void_response_policy_decide(
    UbVoidResponsePolicy *policy, const UbVoidResponseRequest *request)
{
    UbVoidResponseDecision decision = {
        .reason = UB_VOID_RESPONSE_DISABLED,
    };
    uint64_t evaluated;

    if (!policy || !request) {
        return decision;
    }
    evaluated = policy->stats.evaluated++;
    if (!policy->config.enabled) {
        policy->stats.normal_selected++;
        return decision;
    }
    decision.jitter_ns = ub_void_response_jitter(&policy->config, request);
    if (decision.jitter_ns < 0 &&
        (uint64_t)-decision.jitter_ns > policy->config.latency_ns) {
        decision.completion_delay_ns = 0;
    } else if (decision.jitter_ns < 0) {
        decision.completion_delay_ns = policy->config.latency_ns -
            (uint64_t)-decision.jitter_ns;
    } else if (policy->config.latency_ns >
               UINT64_MAX - (uint64_t)decision.jitter_ns) {
        decision.completion_delay_ns = UINT64_MAX;
    } else {
        decision.completion_delay_ns = policy->config.latency_ns +
            (uint64_t)decision.jitter_ns;
    }
    if (evaluated < policy->config.fault_voids) {
        decision.send_void = true;
        decision.fault_injected = true;
        decision.reason = UB_VOID_RESPONSE_FAULT_INJECTION;
        policy->stats.fault_injected++;
    } else if (decision.completion_delay_ns > policy->config.threshold_ns) {
        decision.send_void = true;
        decision.reason = UB_VOID_RESPONSE_LATENCY_THRESHOLD;
    } else {
        decision.reason = UB_VOID_RESPONSE_WITHIN_THRESHOLD;
    }
    if (decision.send_void) {
        policy->stats.void_selected++;
    } else {
        policy->stats.normal_selected++;
    }
    return decision;
}

const UbVoidResponsePolicyStats *ub_void_response_policy_stats(
    const UbVoidResponsePolicy *policy)
{
    return policy ? &policy->stats : NULL;
}

const char *ub_void_response_reason_name(UbVoidResponseReason reason)
{
    switch (reason) {
    case UB_VOID_RESPONSE_DISABLED:
        return "disabled";
    case UB_VOID_RESPONSE_WITHIN_THRESHOLD:
        return "within-threshold";
    case UB_VOID_RESPONSE_LATENCY_THRESHOLD:
        return "latency-threshold";
    case UB_VOID_RESPONSE_FAULT_INJECTION:
        return "fault-injection";
    default:
        return "invalid";
    }
}

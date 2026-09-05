/*
 * Reusable source- or destination-side policy for UB remote-read void
 * responses.
 */

#ifndef HW_UB_VOID_RESPONSE_POLICY_H
#define HW_UB_VOID_RESPONSE_POLICY_H

#include "qapi/error.h"
#include "qemu/osdep.h"

#define UB_VOID_RESPONSE_POLICY_MAX_LATENCY_NS 10000000000ULL

typedef enum UbVoidResponseReason {
    UB_VOID_RESPONSE_DISABLED,
    UB_VOID_RESPONSE_WITHIN_THRESHOLD,
    UB_VOID_RESPONSE_LATENCY_THRESHOLD,
    UB_VOID_RESPONSE_FAULT_INJECTION,
} UbVoidResponseReason;

typedef struct UbVoidResponsePolicyConfig {
    bool enabled;
    uint64_t threshold_ns;
    uint64_t latency_ns;
    uint64_t jitter_ns;
    uint64_t fault_voids;
    uint64_t seed;
} UbVoidResponsePolicyConfig;

typedef struct UbVoidResponseRequest {
    uint32_t source_cna;
    uint32_t request_id;
    uint64_t remote_address;
    uint32_t length;
    uint64_t arrival_ns;
} UbVoidResponseRequest;

typedef struct UbVoidResponseDecision {
    bool send_void;
    bool fault_injected;
    int64_t jitter_ns;
    uint64_t completion_delay_ns;
    UbVoidResponseReason reason;
} UbVoidResponseDecision;

typedef struct UbVoidResponsePolicyStats {
    uint64_t evaluated;
    uint64_t void_selected;
    uint64_t normal_selected;
    uint64_t fault_injected;
} UbVoidResponsePolicyStats;

typedef struct UbVoidResponsePolicy {
    UbVoidResponsePolicyConfig config;
    UbVoidResponsePolicyStats stats;
} UbVoidResponsePolicy;

void ub_void_response_policy_init(UbVoidResponsePolicy *policy);
bool ub_void_response_policy_configure(UbVoidResponsePolicy *policy,
                                       const char *spec, Error **errp);
UbVoidResponseDecision ub_void_response_policy_decide(
    UbVoidResponsePolicy *policy, const UbVoidResponseRequest *request);
const UbVoidResponsePolicyStats *ub_void_response_policy_stats(
    const UbVoidResponsePolicy *policy);
const char *ub_void_response_reason_name(UbVoidResponseReason reason);

#endif

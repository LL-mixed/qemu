/*
 * Deterministic OBMM remote-memory latency and failure model.
 */

#ifndef HW_UB_OBMM_REMOTE_MODEL_H
#define HW_UB_OBMM_REMOTE_MODEL_H

#include "qapi/error.h"

#define UB_OBMM_REMOTE_MODEL_SCHEMA 1
#define UB_OBMM_REMOTE_MODEL_MAX_LATENCY_NS 10000000000ULL
#define UB_OBMM_REMOTE_MODEL_MAX_QUEUE_DEPTH 65535U
#define UB_OBMM_REMOTE_MODEL_PPM_SCALE 1000000U
#define UB_OBMM_REMOTE_MODEL_HASH_MAX 32

typedef enum UbObmmRemoteOutcome {
    UB_OBMM_REMOTE_SUCCESS,
    UB_OBMM_REMOTE_ERROR,
    UB_OBMM_REMOTE_DROP,
} UbObmmRemoteOutcome;

typedef enum UbObmmRemoteJitterMode {
    UB_OBMM_REMOTE_JITTER_NONE,
    UB_OBMM_REMOTE_JITTER_UNIFORM,
} UbObmmRemoteJitterMode;

typedef struct UbObmmRemoteModelConfig {
    bool enabled;
    uint64_t fixed_latency_ns;
    UbObmmRemoteJitterMode jitter_mode;
    uint64_t jitter_max_abs_ns;
    uint32_t tail_probability_ppm;
    uint64_t tail_extra_latency_ns;
    uint32_t queue_depth;
    uint32_t reorder_window;
    uint32_t drop_ppm;
    uint32_t error_ppm;
    uint32_t duplicate_ppm;
    uint64_t duplicate_delay_ns;
    uint64_t seed;
} UbObmmRemoteModelConfig;

typedef struct UbObmmRemoteOperation {
    uint64_t map_id;
    uint64_t map_generation;
    uint64_t remote_offset;
    uint32_t length;
    uint64_t per_range_ordinal;
} UbObmmRemoteOperation;

typedef struct UbObmmRemoteDecision {
    uint64_t operation_key;
    UbObmmRemoteOutcome outcome;
    int64_t jitter_ns;
    bool tail_applied;
    uint64_t service_ns;
    bool duplicate;
    uint64_t duplicate_delay_ns;
    uint64_t reorder_key;
} UbObmmRemoteDecision;

typedef void (*UbObmmRemotePublishFn)(
    void *opaque, const UbObmmRemoteDecision *decision, bool duplicate,
    uint64_t model_accept_ns, uint64_t model_due_ns,
    uint64_t model_publish_ns);

typedef void (*UbObmmRemoteDueFn)(
    void *opaque, const UbObmmRemoteDecision *decision,
    uint64_t model_accept_ns, uint64_t model_due_ns);

typedef void (*UbObmmRemoteDestroyFn)(void *opaque);

typedef struct UbObmmRemoteQueuedEvent UbObmmRemoteQueuedEvent;

typedef struct UbObmmRemoteModelState {
    UbObmmRemoteModelConfig config;
    char manifest_hash[UB_OBMM_REMOTE_MODEL_HASH_MAX];
    bool loaded;
    uint32_t pending;
    uint64_t accepted;
    uint64_t capacity_rejected;
    uint64_t completed;
    uint64_t dropped;
    uint64_t errored;
    uint64_t duplicated;
    uint64_t published;
    uint64_t duplicate_published;
    uint64_t total_service_ns;
    uint64_t total_accept_to_publish_ns;
    UbObmmRemoteQueuedEvent *events;
    uint32_t event_capacity;
} UbObmmRemoteModelState;

void ub_obmm_remote_model_init(UbObmmRemoteModelState *model);
bool ub_obmm_remote_model_load(UbObmmRemoteModelState *model,
                               const char *path, Error **errp);
void ub_obmm_remote_model_cleanup(UbObmmRemoteModelState *model);
uint64_t ub_obmm_remote_operation_key(
    const UbObmmRemoteModelConfig *config,
    const UbObmmRemoteOperation *operation);
UbObmmRemoteDecision ub_obmm_remote_model_decide(
    const UbObmmRemoteModelConfig *config,
    const UbObmmRemoteOperation *operation);
bool ub_obmm_remote_model_try_accept(UbObmmRemoteModelState *model,
                                     const UbObmmRemoteOperation *operation,
                                     UbObmmRemoteDecision *decision);
bool ub_obmm_remote_model_release(UbObmmRemoteModelState *model);
bool ub_obmm_remote_model_enqueue(
    UbObmmRemoteModelState *model, const UbObmmRemoteOperation *operation,
    uint64_t model_accept_ns, UbObmmRemoteDueFn due,
    UbObmmRemotePublishFn publish,
    UbObmmRemoteDestroyFn destroy, void *opaque,
    UbObmmRemoteDecision *decision);
uint64_t ub_obmm_remote_model_next_due_ns(
    const UbObmmRemoteModelState *model);
uint32_t ub_obmm_remote_model_run_due(UbObmmRemoteModelState *model,
                                      uint64_t model_now_ns);
bool ub_obmm_remote_model_reset_stats(UbObmmRemoteModelState *model);

#endif

/*
 * Provider-neutral split-phase OBMM remote-read backend.
 */

#ifndef HW_UB_OBMM_REMOTE_H
#define HW_UB_OBMM_REMOTE_H

#include "qemu/osdep.h"

#define OBMM_REMOTE_PARENT_CAPACITY 64
#define OBMM_REMOTE_MAX_BYTES 65536
#define OBMM_REMOTE_MAX_CHILDREN 64

typedef struct ObmmRemoteBackend ObmmRemoteBackend;

typedef struct ObmmRemoteToken {
    uint32_t generation;
    uint16_t owner_id;
    uint16_t slot;
} ObmmRemoteToken;

typedef enum ObmmRemoteStatus {
    OBMM_REMOTE_STATUS_SUCCESS,
    OBMM_REMOTE_STATUS_CAPACITY,
    OBMM_REMOTE_STATUS_BOUNDS,
    OBMM_REMOTE_STATUS_PERMISSION,
    OBMM_REMOTE_STATUS_STALE_MAP,
    OBMM_REMOTE_STATUS_STALE_SINK,
    OBMM_REMOTE_STATUS_TOKEN_DENIED,
    OBMM_REMOTE_STATUS_COHERENCE,
    OBMM_REMOTE_STATUS_REMOTE_IO,
    OBMM_REMOTE_STATUS_CHECKSUM,
    OBMM_REMOTE_STATUS_TIMEOUT,
    OBMM_REMOTE_STATUS_CANCELLED,
    OBMM_REMOTE_STATUS_RETIRED,
    OBMM_REMOTE_STATUS_UNSUPPORTED,
    OBMM_REMOTE_STATUS_INTERNAL,
} ObmmRemoteStatus;

typedef enum ObmmRemoteSinkKind {
    OBMM_REMOTE_SINK_TEST,
    OBMM_REMOTE_SINK_P2A,
    OBMM_REMOTE_SINK_P2B,
} ObmmRemoteSinkKind;

typedef struct ObmmRemoteResult ObmmRemoteResult;

typedef bool (*ObmmRemoteSinkValidateFn)(void *adapter_state,
                                         uint64_t sink_id,
                                         uint64_t sink_generation);
typedef void (*ObmmRemoteCompleteFn)(void *adapter_state,
                                     const ObmmRemoteResult *result);

typedef struct ObmmCompletionSink {
    ObmmRemoteSinkKind kind;
    uint64_t sink_id;
    uint64_t sink_generation;
    uint16_t owner_id;
    ObmmRemoteSinkValidateFn validate;
    ObmmRemoteCompleteFn complete;
    void *adapter_state;
} ObmmCompletionSink;

typedef struct ObmmRemoteRequest {
    uint64_t map_id;
    uint64_t map_generation;
    uint64_t remote_offset;
    uint32_t length;
    uint32_t flags;
    uint64_t deadline_model_ns;
    uint64_t operation_key;
    uint64_t operation_ordinal;
    ObmmCompletionSink sink;
} ObmmRemoteRequest;

struct ObmmRemoteResult {
    ObmmRemoteToken token;
    ObmmRemoteStatus status;
    uint32_t bytes_done;
    uint64_t checksum64;
    uint64_t model_accept_ns;
    uint64_t model_publish_ns;
    const void *payload;
};

typedef enum ObmmSubmitDisposition {
    OBMM_SUBMIT_INLINE,
    OBMM_SUBMIT_PENDING,
    OBMM_SUBMIT_REJECTED,
} ObmmSubmitDisposition;

typedef enum ObmmProviderChildDisposition {
    OBMM_PROVIDER_CHILD_INLINE,
    OBMM_PROVIDER_CHILD_PENDING,
    OBMM_PROVIDER_CHILD_REJECTED,
} ObmmProviderChildDisposition;

typedef enum ObmmCancelDisposition {
    OBMM_CANCELLED_NOW,
    OBMM_CANCEL_ALREADY_TERMINAL,
    OBMM_CANCEL_STALE_TOKEN,
} ObmmCancelDisposition;

typedef bool (*ObmmRemoteMapValidateFn)(void *opaque, uint64_t map_id,
                                        uint64_t map_generation);
typedef ObmmProviderChildDisposition (*ObmmRemoteSubmitChildFn)(
    void *opaque, ObmmRemoteToken token, uint16_t child_index,
    uint64_t map_id, uint64_t map_generation, uint64_t remote_offset,
    uint32_t length, uint64_t operation_ordinal, void *inline_payload,
    ObmmRemoteStatus *inline_status);
typedef void (*ObmmRemoteCancelProviderFn)(void *opaque,
                                           ObmmRemoteToken token);

typedef struct ObmmRemoteBackendOps {
    ObmmRemoteMapValidateFn map_validate;
    ObmmRemoteSubmitChildFn submit_child;
    ObmmRemoteCancelProviderFn cancel;
    void *opaque;
} ObmmRemoteBackendOps;

typedef struct ObmmRemoteBackendStats {
    uint64_t accepted;
    uint64_t rejected;
    uint64_t delivered;
    uint64_t inline_completed;
    uint64_t late;
    uint64_t duplicate;
    uint64_t capacity;
    uint64_t pending_high_water;
    uint64_t sink_copy_bytes;
    uint64_t sink_copy_ns;
} ObmmRemoteBackendStats;

ObmmRemoteBackend *obmm_remote_backend_new(
    uint16_t owner_id, uint32_t child_chunk_bytes,
    const ObmmRemoteBackendOps *ops);
void obmm_remote_backend_free(ObmmRemoteBackend *backend);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(ObmmRemoteBackend,
                              obmm_remote_backend_free)
ObmmSubmitDisposition obmm_remote_submit(
    ObmmRemoteBackend *backend, const ObmmRemoteRequest *request,
    uint64_t model_accept_ns, ObmmRemoteToken *token,
    ObmmRemoteResult *inline_result, ObmmRemoteStatus *rejected_status);
bool obmm_remote_child_complete(
    ObmmRemoteBackend *backend, ObmmRemoteToken token,
    uint16_t child_index, ObmmRemoteStatus status,
    const void *payload, uint32_t bytes_done, uint64_t model_publish_ns);
ObmmCancelDisposition obmm_remote_cancel(ObmmRemoteBackend *backend,
                                         ObmmRemoteToken token,
                                         ObmmRemoteStatus reason,
                                         uint64_t model_now_ns);
uint32_t obmm_remote_retire_map(ObmmRemoteBackend *backend,
                                uint64_t map_id,
                                uint64_t map_generation,
                                uint64_t model_now_ns);
uint32_t obmm_remote_run_deadlines(ObmmRemoteBackend *backend,
                                   uint64_t model_now_ns);
uint64_t obmm_remote_next_deadline(const ObmmRemoteBackend *backend);
uint32_t obmm_remote_deliver_ready(ObmmRemoteBackend *backend);
uint32_t obmm_remote_pending(const ObmmRemoteBackend *backend);
const ObmmRemoteBackendStats *obmm_remote_backend_stats(
    const ObmmRemoteBackend *backend);
bool obmm_remote_backend_reset_stats(ObmmRemoteBackend *backend);
const char *obmm_remote_status_name(ObmmRemoteStatus status);

#endif

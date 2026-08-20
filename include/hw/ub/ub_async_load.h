/*
 * Provider-neutral pending-load and EL0-upcall event model.
 *
 * Coroutine contexts, ready queues and scheduling policy deliberately do not
 * live here. They are owned by the guest EL0 runtime.
 */

#ifndef HW_UB_ASYNC_LOAD_H
#define HW_UB_ASYNC_LOAD_H

#include "qemu/osdep.h"

#define UB_ASYNC_LOAD_MAX_CONTEXTS 64
#define UB_ASYNC_LOAD_MAX_PENDING_LOADS 64
#define UB_ASYNC_LOAD_MAX_EVENTS 128
#define UB_ASYNC_LOAD_EVENT_FLAG_REPLAY_RETIRE (1U << 1)

typedef struct UbAsyncLoad UbAsyncLoad;

typedef enum UbAsyncLoadPltState {
    UB_ASYNC_LOAD_PLT_FREE,
    UB_ASYNC_LOAD_PLT_PENDING,
    UB_ASYNC_LOAD_PLT_COMPLETE,
    UB_ASYNC_LOAD_PLT_REPLAY_READY,
    UB_ASYNC_LOAD_PLT_FAULTED,
} UbAsyncLoadPltState;

typedef enum UbAsyncLoadEventKind {
    UB_ASYNC_LOAD_EVENT_PENDING = 1,
    UB_ASYNC_LOAD_EVENT_COMPLETE = 2,
    UB_ASYNC_LOAD_EVENT_FAULT = 3,
    UB_ASYNC_LOAD_EVENT_OWNER_STOP = 4,
} UbAsyncLoadEventKind;

typedef enum UbAsyncLoadPendingResult {
    UB_ASYNC_LOAD_PENDING_ACCEPTED,
    UB_ASYNC_LOAD_PENDING_SYNC_STALL,
    UB_ASYNC_LOAD_PENDING_INVALID,
} UbAsyncLoadPendingResult;

typedef enum UbAsyncLoadCompletionResult {
    UB_ASYNC_LOAD_COMPLETION_ACCEPTED,
    UB_ASYNC_LOAD_COMPLETION_STALE,
    UB_ASYNC_LOAD_COMPLETION_DUPLICATE,
} UbAsyncLoadCompletionResult;

typedef enum UbAsyncLoadReplayResult {
    UB_ASYNC_LOAD_REPLAY_NONE,
    UB_ASYNC_LOAD_REPLAY_CONSUMED,
    UB_ASYNC_LOAD_REPLAY_MISMATCH,
} UbAsyncLoadReplayResult;

typedef enum UbAsyncLoadStatus {
    UB_ASYNC_LOAD_STATUS_SUCCESS,
    UB_ASYNC_LOAD_STATUS_TIMEOUT,
    UB_ASYNC_LOAD_STATUS_PERMISSION,
    UB_ASYNC_LOAD_STATUS_STALE_MAP,
    UB_ASYNC_LOAD_STATUS_REMOTE_IO,
    UB_ASYNC_LOAD_STATUS_CANCELLED,
    UB_ASYNC_LOAD_STATUS_INTERNAL,
} UbAsyncLoadStatus;

typedef struct UbAsyncLoadConfig {
    uint16_t context_entries;
    uint16_t pending_load_entries;
    uint16_t event_queue_depth;
    uint32_t clock_mhz;
} UbAsyncLoadConfig;

typedef struct UbAsyncLoadPltToken {
    uint32_t generation;
    uint16_t owner_id;
    uint16_t slot;
} UbAsyncLoadPltToken;

typedef struct UbAsyncLoadDesc {
    uint64_t fault_pc;
    uint64_t effective_va;
    uint64_t map_id;
    uint64_t map_generation;
    uint64_t remote_offset;
    uint64_t submit_cycle;
    uint64_t deadline_cycle;
    uint8_t rt;
    uint8_t access_bytes;
    uint8_t mmu_index;
    bool sign_extend;
    bool big_endian;
} UbAsyncLoadDesc;

typedef struct UbAsyncLoadEvent {
    uint64_t sequence;
    uint64_t context_id;
    UbAsyncLoadPltToken plt_token;
    uint64_t interrupted_pc;
    uint64_t fault_pc;
    uint64_t effective_va;
    uint64_t value;
    UbAsyncLoadEventKind kind;
    UbAsyncLoadStatus status;
    uint32_t flags;
    uint16_t rt;
    uint16_t access_bytes;
    uint64_t guest_cycle;
} UbAsyncLoadEvent;

typedef struct UbAsyncLoadStats {
    uint64_t pending_loads;
    uint64_t completed_loads;
    uint64_t completion_events_delivered;
    uint64_t faulted_loads;
    uint64_t stale_completions;
    uint64_t duplicate_completions;
    uint64_t context_saves;
    uint64_t context_restores;
    uint64_t context_switches;
    uint64_t context_bytes_moved;
    uint64_t modeled_cycles;
    uint64_t capacity_stalls;
    uint64_t no_ready_idle;
    uint64_t event_overflow;
    uint64_t direct_upcalls;
    uint16_t pending_high_water;
    uint16_t ready_high_water;
    uint16_t event_high_water;
    uint64_t replay_consumed;
    uint64_t replay_mismatch;
    uint16_t replay_ready_high_water;
} UbAsyncLoadStats;

bool ub_async_load_config_parse(const char *spec, bool *enabled,
                           UbAsyncLoadConfig *config);

UbAsyncLoad *ub_async_load_new(uint16_t owner_id, uint32_t owner_generation,
                      const UbAsyncLoadConfig *config);
void ub_async_load_free(UbAsyncLoad *async_load);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(UbAsyncLoad, ub_async_load_free)

UbAsyncLoadPendingResult ub_async_load_load_pending(
    UbAsyncLoad *async_load, uint64_t context_id, const UbAsyncLoadDesc *load,
    UbAsyncLoadPltToken *plt_token);
UbAsyncLoadCompletionResult ub_async_load_load_complete(
    UbAsyncLoad *async_load, UbAsyncLoadPltToken plt_token, UbAsyncLoadStatus status,
    const void *payload, uint8_t bytes_done, uint64_t complete_cycle);

bool ub_async_load_event_pop(UbAsyncLoad *async_load, UbAsyncLoadEvent *event,
                        bool replay_retire);
bool ub_async_load_event_pending(const UbAsyncLoad *async_load);
bool ub_async_load_replay_expected(const UbAsyncLoad *async_load,
                              uint64_t context_id);
UbAsyncLoadReplayResult ub_async_load_replay_consume(
    UbAsyncLoad *async_load, uint64_t context_id, const UbAsyncLoadDesc *load,
    uint64_t *value);
void ub_async_load_record_direct_upcall(UbAsyncLoad *async_load);
void ub_async_load_mark_fail_stop(UbAsyncLoad *async_load);
bool ub_async_load_fail_stop(const UbAsyncLoad *async_load);
uint16_t ub_async_load_pending_count(const UbAsyncLoad *async_load);
const UbAsyncLoadStats *ub_async_load_stats(const UbAsyncLoad *async_load);

uint64_t ub_async_load_context_id_make(uint32_t generation,
                                  uint16_t home_core, uint16_t slot);
uint32_t ub_async_load_context_id_generation(uint64_t context_id);
uint16_t ub_async_load_context_id_home_core(uint64_t context_id);
uint16_t ub_async_load_context_id_slot(uint64_t context_id);
bool ub_async_load_logical_remote_ordinal(uint16_t context_slot,
                                     uint16_t context_count,
                                     uint64_t remote_local_ordinal,
                                     uint64_t *operation_ordinal);

#endif

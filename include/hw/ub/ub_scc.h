/*
 * Provider-neutral pending-load and EL0-upcall event model.
 *
 * Coroutine contexts, ready queues and scheduling policy deliberately do not
 * live here. They are owned by the guest EL0 runtime.
 */

#ifndef HW_UB_SCC_H
#define HW_UB_SCC_H

#include "qemu/osdep.h"

#define OBMM_SCC_MAX_CONTEXTS 64
#define OBMM_SCC_MAX_PENDING_LOADS 64
#define OBMM_SCC_MAX_EVENTS 128

typedef struct ObmmScc ObmmScc;

typedef enum ObmmSccPltState {
    OBMM_SCC_PLT_FREE,
    OBMM_SCC_PLT_PENDING,
    OBMM_SCC_PLT_COMPLETE,
    OBMM_SCC_PLT_FAULTED,
} ObmmSccPltState;

typedef enum ObmmSccEventKind {
    OBMM_SCC_EVENT_PENDING = 1,
    OBMM_SCC_EVENT_COMPLETE = 2,
    OBMM_SCC_EVENT_FAULT = 3,
    OBMM_SCC_EVENT_OWNER_STOP = 4,
} ObmmSccEventKind;

typedef enum ObmmSccPendingResult {
    OBMM_SCC_PENDING_ACCEPTED,
    OBMM_SCC_PENDING_SYNC_STALL,
    OBMM_SCC_PENDING_INVALID,
} ObmmSccPendingResult;

typedef enum ObmmSccCompletionResult {
    OBMM_SCC_COMPLETION_ACCEPTED,
    OBMM_SCC_COMPLETION_STALE,
    OBMM_SCC_COMPLETION_DUPLICATE,
} ObmmSccCompletionResult;

typedef enum ObmmSccLoadStatus {
    OBMM_SCC_LOAD_SUCCESS,
    OBMM_SCC_LOAD_TIMEOUT,
    OBMM_SCC_LOAD_PERMISSION,
    OBMM_SCC_LOAD_STALE_MAP,
    OBMM_SCC_LOAD_REMOTE_IO,
    OBMM_SCC_LOAD_CANCELLED,
    OBMM_SCC_LOAD_INTERNAL,
} ObmmSccLoadStatus;

typedef struct ObmmSccConfig {
    uint16_t context_entries;
    uint16_t pending_load_entries;
    uint16_t event_queue_depth;
    uint32_t clock_mhz;
} ObmmSccConfig;

typedef struct ObmmSccPltToken {
    uint32_t generation;
    uint16_t owner_id;
    uint16_t slot;
} ObmmSccPltToken;

typedef struct ObmmSccLoadDesc {
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
} ObmmSccLoadDesc;

typedef struct ObmmSccEvent {
    uint64_t sequence;
    uint64_t context_id;
    ObmmSccPltToken plt_token;
    uint64_t interrupted_pc;
    uint64_t fault_pc;
    uint64_t effective_va;
    uint64_t value;
    ObmmSccEventKind kind;
    ObmmSccLoadStatus status;
    uint32_t flags;
    uint16_t rt;
    uint16_t access_bytes;
    uint64_t guest_cycle;
} ObmmSccEvent;

typedef struct ObmmSccStats {
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
} ObmmSccStats;

bool obmm_scc_config_parse(const char *spec, bool *enabled,
                           ObmmSccConfig *config);

ObmmScc *obmm_scc_new(uint16_t owner_id, uint32_t owner_generation,
                      const ObmmSccConfig *config);
void obmm_scc_free(ObmmScc *scc);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(ObmmScc, obmm_scc_free)

ObmmSccPendingResult obmm_scc_load_pending(
    ObmmScc *scc, uint64_t context_id, const ObmmSccLoadDesc *load,
    ObmmSccPltToken *plt_token);
ObmmSccCompletionResult obmm_scc_load_complete(
    ObmmScc *scc, ObmmSccPltToken plt_token, ObmmSccLoadStatus status,
    const void *payload, uint8_t bytes_done, uint64_t complete_cycle);

bool obmm_scc_event_pop(ObmmScc *scc, ObmmSccEvent *event);
bool obmm_scc_event_pending(const ObmmScc *scc);
void obmm_scc_record_direct_upcall(ObmmScc *scc);
void obmm_scc_mark_fail_stop(ObmmScc *scc);
bool obmm_scc_fail_stop(const ObmmScc *scc);
uint16_t obmm_scc_pending_count(const ObmmScc *scc);
const ObmmSccStats *obmm_scc_stats(const ObmmScc *scc);

uint64_t obmm_scc_context_id_make(uint32_t generation,
                                  uint16_t home_core, uint16_t slot);
uint32_t obmm_scc_context_id_generation(uint64_t context_id);
uint16_t obmm_scc_context_id_home_core(uint64_t context_id);
uint16_t obmm_scc_context_id_slot(uint64_t context_id);
bool obmm_scc_logical_remote_ordinal(uint16_t context_slot,
                                     uint16_t context_count,
                                     uint64_t remote_local_ordinal,
                                     uint64_t *operation_ordinal);

#endif

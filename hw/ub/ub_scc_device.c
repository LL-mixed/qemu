/*
 * QEMU adapter for the OBMM direct-to-EL0 upcall mechanism.
 */

#include "qemu/osdep.h"
#include "hw/ub/ub_obmm_remote.h"
#include "hw/ub/ub_scc_device.h"
#include "hw/ub/ub_ubc.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "sysemu/cpus.h"
#include "trace.h"

#define UB_SCC_BACKEND_OWNER_ID 2
#define UB_SCC_CHILD_CHUNK_BYTES 8

#define SCC_REG_VERSION_CAPS 0x000
#define SCC_REG_STATUS 0x008
#define SCC_REG_LAST_ERROR 0x010
#define SCC_REG_OWNER_GENERATION 0x018
#define SCC_REG_MAP_GSVA_BASE 0x020
#define SCC_REG_MAP_LOCAL_PA 0x028
#define SCC_REG_MAP_LENGTH 0x030
#define SCC_REG_MAP_ID 0x038
#define SCC_REG_MAP_GENERATION 0x040
#define SCC_REG_MAP_COMMAND 0x048
#define SCC_REG_SESSION_COMMAND 0x0a0
#define SCC_REG_CLOCK_MHZ 0x0f8
#define SCC_REG_LOAD_TIMEOUT_NS 0x100
#define SCC_REG_OWNER_TTBR0 0x108
#define SCC_REG_MAP_MODEL_GENERATION 0x110
#define SCC_REG_MAP_FLAGS 0x118
#define SCC_REG_UPCALL_ENTRY 0x120
#define SCC_REG_LOGICAL_CONTEXTS 0x128
#define SCC_REG_ACTIVE_CONTEXT_ID 0x130
#define SCC_REG_EVENT_SEQUENCE 0x138
#define SCC_REG_EVENT_CONTEXT_ID 0x140
#define SCC_REG_EVENT_PLT_TOKEN 0x148
#define SCC_REG_EVENT_INTERRUPTED_PC 0x150
#define SCC_REG_EVENT_FAULT_PC 0x158
#define SCC_REG_EVENT_VA 0x160
#define SCC_REG_EVENT_VALUE 0x168
#define SCC_REG_EVENT_KIND_STATUS 0x170
#define SCC_REG_EVENT_META 0x178
#define SCC_REG_EVENT_COMMAND 0x180
#define SCC_REG_STATS_BASE 0x200

#define SCC_STATUS_ACTIVE BIT(0)
#define SCC_STATUS_FAIL_STOP BIT(1)
#define SCC_STATUS_ENABLED BIT(2)
#define SCC_STATUS_EVENT_PENDING BIT(3)
#define SCC_STATUS_EVENT_DELIVERED BIT(4)
#define SCC_STATUS_UPCALL_ACTIVE BIT(5)
#define SCC_MAP_LOGICAL_MIXED BIT(0)

typedef enum UbSccError {
    UB_SCC_ERROR_NONE,
    UB_SCC_ERROR_INVALID,
    UB_SCC_ERROR_BUSY,
    UB_SCC_ERROR_NO_MAP,
    UB_SCC_ERROR_STALE,
    UB_SCC_ERROR_REMOTE,
    UB_SCC_ERROR_CPU,
} UbSccError;

typedef struct UbSccMap {
    bool active;
    uint64_t generation;
    uint64_t model_generation;
    uint32_t flags;
    uint64_t gsva_base;
    uint64_t length;
    UbcObmmResolvedMap resolved;
} UbSccMap;

typedef struct UbSccFuture {
    UbSccDeviceState *state;
    bool active;
    uint64_t context_id;
    ObmmSccPltToken plt_token;
    ObmmRemoteToken backend_token;
} UbSccFuture;

struct UbSccDeviceState {
    BusControllerDev *ubc_dev;
    ObmmScc *model;
    ObmmRemoteBackend *backend;
    QEMUBH *bh;
    QEMUTimer *deadline_timer;
    CPUState *home_cpu;
    ObmmSccConfig config;
    uint32_t owner_generation;
    bool enabled;
    bool session_active;
    bool upcall_active;
    bool delivered_event_valid;
    uint64_t last_error;
    uint64_t map_gsva_base;
    uint64_t map_local_pa;
    uint64_t map_length;
    uint64_t map_id;
    uint64_t map_generation;
    uint64_t map_model_generation;
    uint64_t map_flags;
    uint64_t load_timeout_ns;
    uint64_t owner_ttbr0;
    uint64_t upcall_entry;
    uint64_t active_context_id;
    uint16_t logical_context_count;
    uint64_t context_next_ordinal[OBMM_SCC_MAX_CONTEXTS];
    ObmmSccEvent delivered_event;
    UbSccMap maps[OBMM_SCC_MAX_PENDING_LOADS];
    UbSccFuture futures[OBMM_SCC_MAX_PENDING_LOADS];
};

static UbSccDeviceState *ub_scc_global;

static uint64_t ub_scc_ns_to_cycles(const UbSccDeviceState *state,
                                    uint64_t ns)
{
    if (ns > UINT64_MAX / state->config.clock_mhz) {
        return UINT64_MAX;
    }
    return ns * state->config.clock_mhz / 1000;
}

static uint64_t ub_scc_access_extract(uint64_t value, hwaddr reg,
                                      unsigned int size)
{
    if (size == sizeof(uint64_t)) {
        return value;
    }
    if (size == sizeof(uint32_t)) {
        return reg & 4 ? value >> 32 : value & UINT32_MAX;
    }
    return 0;
}

static uint64_t ub_scc_access_merge(uint64_t current, hwaddr reg,
                                    uint64_t value, unsigned int size)
{
    if (size == sizeof(uint64_t)) {
        return value;
    }
    if (size == sizeof(uint32_t)) {
        if (reg & 4) {
            return (current & UINT32_MAX) |
                ((value & UINT32_MAX) << 32);
        }
        return (current & ~((uint64_t)UINT32_MAX)) |
            (value & UINT32_MAX);
    }
    return current;
}

static uint64_t ub_scc_plt_token_pack(ObmmSccPltToken token)
{
    return (uint64_t)token.generation << 32 |
        (uint64_t)token.owner_id << 16 | token.slot;
}

static ObmmSccLoadStatus ub_scc_status_from_backend(
    ObmmRemoteStatus status)
{
    switch (status) {
    case OBMM_REMOTE_STATUS_SUCCESS:
        return OBMM_SCC_LOAD_SUCCESS;
    case OBMM_REMOTE_STATUS_TIMEOUT:
        return OBMM_SCC_LOAD_TIMEOUT;
    case OBMM_REMOTE_STATUS_PERMISSION:
    case OBMM_REMOTE_STATUS_TOKEN_DENIED:
    case OBMM_REMOTE_STATUS_COHERENCE:
        return OBMM_SCC_LOAD_PERMISSION;
    case OBMM_REMOTE_STATUS_STALE_MAP:
    case OBMM_REMOTE_STATUS_RETIRED:
        return OBMM_SCC_LOAD_STALE_MAP;
    case OBMM_REMOTE_STATUS_CANCELLED:
        return OBMM_SCC_LOAD_CANCELLED;
    case OBMM_REMOTE_STATUS_REMOTE_IO:
    case OBMM_REMOTE_STATUS_CHECKSUM:
        return OBMM_SCC_LOAD_REMOTE_IO;
    default:
        return OBMM_SCC_LOAD_INTERNAL;
    }
}

static bool ub_scc_map_valid(void *opaque, uint64_t map_id,
                             uint64_t map_generation)
{
    UbSccDeviceState *state = opaque;
    UbSccMap *map;

    if (!map_id || map_id > OBMM_SCC_MAX_PENDING_LOADS) {
        return false;
    }
    map = &state->maps[map_id - 1];
    return map->active && map->generation == map_generation;
}

static void ub_scc_child_complete(
    void *opaque, ObmmRemoteToken token, uint16_t child_index,
    ObmmRemoteStatus status, const void *payload, uint32_t bytes_done,
    uint64_t model_publish_ns);

static ObmmProviderChildDisposition ub_scc_submit_child(
    void *opaque, ObmmRemoteToken token, uint16_t child_index,
    uint64_t map_id, uint64_t map_generation, uint64_t remote_offset,
    uint32_t length, uint64_t operation_ordinal, void *inline_payload,
    ObmmRemoteStatus *inline_status)
{
    UbSccDeviceState *state = opaque;
    UbSccMap *map;
    UbObmmRemoteOperation operation;

    (void)inline_payload;
    if (!ub_scc_map_valid(state, map_id, map_generation)) {
        *inline_status = OBMM_REMOTE_STATUS_STALE_MAP;
        return OBMM_PROVIDER_CHILD_REJECTED;
    }
    map = &state->maps[map_id - 1];
    operation = (UbObmmRemoteOperation) {
        .map_id = map->resolved.map_id,
        .map_generation = map->resolved.map_generation,
        .remote_offset = remote_offset,
        .length = length,
        .per_range_ordinal = operation_ordinal,
    };
    if (!ubc_sim_dec_remote_read_async_submit(
            state->ubc_dev, &map->resolved, remote_offset, length,
            &operation, token, child_index, ub_scc_child_complete,
            state)) {
        *inline_status = OBMM_REMOTE_STATUS_REMOTE_IO;
        return OBMM_PROVIDER_CHILD_REJECTED;
    }
    return OBMM_PROVIDER_CHILD_PENDING;
}

static void ub_scc_cancel_provider(void *opaque, ObmmRemoteToken token)
{
    UbSccDeviceState *state = opaque;

    ubc_sim_dec_remote_read_async_cancel(state->ubc_dev, token);
}

static void ub_scc_arm_deadline(UbSccDeviceState *state)
{
    uint64_t deadline = obmm_remote_next_deadline(state->backend);

    if (deadline == UINT64_MAX) {
        timer_del(state->deadline_timer);
    } else {
        timer_mod_ns(state->deadline_timer, deadline);
    }
}

static void ub_scc_bh(void *opaque)
{
    UbSccDeviceState *state = opaque;

    obmm_remote_deliver_ready(state->backend);
    ub_scc_arm_deadline(state);
}

static void ub_scc_schedule_bh(UbSccDeviceState *state)
{
    qemu_bh_schedule(state->bh);
}

static void ub_scc_deadline(void *opaque)
{
    UbSccDeviceState *state = opaque;

    obmm_remote_run_deadlines(
        state->backend, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    ub_scc_schedule_bh(state);
}

static void ub_scc_child_complete(
    void *opaque, ObmmRemoteToken token, uint16_t child_index,
    ObmmRemoteStatus status, const void *payload, uint32_t bytes_done,
    uint64_t model_publish_ns)
{
    UbSccDeviceState *state = opaque;

    obmm_remote_child_complete(state->backend, token, child_index,
                               status, payload, bytes_done,
                               model_publish_ns);
    ub_scc_schedule_bh(state);
}

static bool ub_scc_backend_new(UbSccDeviceState *state)
{
    ObmmRemoteBackendOps ops = {
        .map_validate = ub_scc_map_valid,
        .submit_child = ub_scc_submit_child,
        .cancel = ub_scc_cancel_provider,
        .opaque = state,
    };

    state->backend = obmm_remote_backend_new(
        UB_SCC_BACKEND_OWNER_ID, UB_SCC_CHILD_CHUNK_BYTES, &ops);
    return state->backend != NULL;
}

static bool ub_scc_reset(UbSccDeviceState *state)
{
    uint16_t slot;
    uint64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    qemu_bh_cancel(state->bh);
    timer_del(state->deadline_timer);
    for (slot = 0; slot < G_N_ELEMENTS(state->futures); slot++) {
        UbSccFuture *future = &state->futures[slot];

        if (future->active && future->backend_token.owner_id) {
            obmm_remote_cancel(state->backend, future->backend_token,
                               OBMM_REMOTE_STATUS_CANCELLED, now_ns);
        }
    }
    obmm_remote_deliver_ready(state->backend);
    obmm_scc_free(state->model);
    state->owner_generation++;
    if (!state->owner_generation) {
        state->owner_generation = 1;
    }
    state->model = obmm_scc_new(UB_SCC_BACKEND_OWNER_ID,
                                state->owner_generation,
                                &state->config);
    memset(state->maps, 0, sizeof(state->maps));
    memset(state->futures, 0, sizeof(state->futures));
    memset(state->context_next_ordinal, 0,
           sizeof(state->context_next_ordinal));
    memset(&state->delivered_event, 0, sizeof(state->delivered_event));
    state->delivered_event_valid = false;
    state->upcall_active = false;
    state->active_context_id = 0;
    state->logical_context_count = 0;
    state->upcall_entry = 0;
    state->load_timeout_ns = 0;
    state->owner_ttbr0 = 0;
    state->last_error = 0;
    return state->model && state->backend &&
        obmm_remote_pending(state->backend) == 0;
}

UbSccDeviceState *ub_scc_device_new(BusControllerDev *ubc_dev,
                                    const char *model_spec,
                                    Error **errp)
{
    UbSccDeviceState *state;

    if (!ubc_dev || ub_scc_global) {
        error_setg(errp, "OBMM scheduler-core endpoint already exists");
        return NULL;
    }
    state = g_new0(UbSccDeviceState, 1);
    state->ubc_dev = ubc_dev;
    state->owner_generation = 1;
    if (!obmm_scc_config_parse(model_spec, &state->enabled,
                               &state->config)) {
        error_setg(errp, "invalid OBMM scheduler-core model spec: %s",
                   model_spec ? model_spec : "(null)");
        g_free(state);
        return NULL;
    }
    state->model = obmm_scc_new(UB_SCC_BACKEND_OWNER_ID,
                                state->owner_generation,
                                &state->config);
    if (!state->model || !ub_scc_backend_new(state)) {
        error_setg(errp, "failed to allocate OBMM EL0-upcall state");
        obmm_scc_free(state->model);
        g_free(state);
        return NULL;
    }
    state->bh = qemu_bh_new_guarded(
        ub_scc_bh, state, &DEVICE(ubc_dev)->mem_reentrancy_guard);
    state->deadline_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                         ub_scc_deadline, state);
    ub_scc_global = state;
    return state;
}

void ub_scc_device_free(UbSccDeviceState *state)
{
    if (!state) {
        return;
    }
    if (state->home_cpu) {
        state->home_cpu->halted = 0;
        arm_obmm_scc_set_active(state->home_cpu, false);
    }
    if (ub_scc_global == state) {
        ub_scc_global = NULL;
    }
    qemu_bh_delete(state->bh);
    timer_free(state->deadline_timer);
    obmm_remote_backend_free(state->backend);
    obmm_scc_free(state->model);
    g_free(state);
}

bool ub_scc_device_decode(hwaddr address, hwaddr *reg)
{
    if (address < UB_SCC_ENDPOINT_BASE ||
        address >= UB_SCC_ENDPOINT_BASE + UB_SCC_ENDPOINT_BYTES) {
        return false;
    }
    *reg = address - UB_SCC_ENDPOINT_BASE;
    return true;
}

static uint64_t ub_scc_stats_read(const ObmmSccStats *stats,
                                  uint32_t index)
{
    switch (index) {
    case 0: return stats->pending_loads;
    case 1: return stats->completed_loads;
    case 2: return stats->completion_events_delivered;
    case 3: return stats->faulted_loads;
    case 4: return stats->stale_completions;
    case 5: return stats->duplicate_completions;
    case 6: return stats->context_saves;
    case 7: return stats->context_restores;
    case 8: return stats->context_switches;
    case 9: return stats->context_bytes_moved;
    case 10: return stats->modeled_cycles;
    case 11: return stats->capacity_stalls;
    case 12: return stats->no_ready_idle;
    case 13: return stats->event_overflow;
    case 14: return stats->pending_high_water;
    case 15: return stats->ready_high_water;
    case 16: return stats->event_high_water;
    default: return 0;
    }
}

static uint64_t ub_scc_observability_read(UbSccDeviceState *state,
                                          const ObmmSccStats *stats,
                                          uint32_t index)
{
    const ObmmRemoteBackendStats *backend =
        obmm_remote_backend_stats(state->backend);

    switch (index) {
    case 0: return obmm_scc_pending_count(state->model);
    case 1: return obmm_remote_pending(state->backend);
    case 2: return backend->accepted;
    case 3: return backend->rejected;
    case 4: return backend->delivered;
    case 5: return backend->late;
    case 6: return backend->duplicate;
    case 7: return backend->capacity;
    case 8: return backend->pending_high_water;
    case 9: return backend->sink_copy_bytes;
    case 10: return backend->sink_copy_ns;
    case 11: return 0;
    case 12: return 0;
    case 13: return 0;
    case 14: return 0;
    case 15: return state->logical_context_count;
    case 16: return stats->direct_upcalls;
    default: return 0;
    }
}

uint64_t ub_scc_device_read(UbSccDeviceState *state, hwaddr reg,
                            unsigned int size)
{
    const ObmmSccStats *stats;
    const ObmmSccEvent *event;
    uint64_t value = 0;

    if (!state || (size != sizeof(uint32_t) &&
                   size != sizeof(uint64_t))) {
        return 0;
    }
    stats = obmm_scc_stats(state->model);
    event = &state->delivered_event;
    if ((reg & ~7ULL) >= SCC_REG_STATS_BASE) {
        uint32_t index = ((reg & ~7ULL) - SCC_REG_STATS_BASE) / 8;

        value = index < 17 ? ub_scc_stats_read(stats, index) :
            ub_scc_observability_read(state, stats, index - 17);
        return ub_scc_access_extract(value, reg, size);
    }
    switch (reg & ~7ULL) {
    case SCC_REG_VERSION_CAPS:
        value = UB_SCC_ABI_VERSION |
            ((uint64_t)state->config.context_entries << 16) |
            ((uint64_t)state->config.pending_load_entries << 32) |
            ((uint64_t)state->config.event_queue_depth << 48);
        break;
    case SCC_REG_STATUS:
        value = (state->session_active ? SCC_STATUS_ACTIVE : 0) |
            (state->enabled ? SCC_STATUS_ENABLED : 0) |
            (obmm_scc_fail_stop(state->model) ? SCC_STATUS_FAIL_STOP : 0) |
            (obmm_scc_event_pending(state->model) ?
             SCC_STATUS_EVENT_PENDING : 0) |
            (state->delivered_event_valid ?
             SCC_STATUS_EVENT_DELIVERED : 0) |
            (state->upcall_active ? SCC_STATUS_UPCALL_ACTIVE : 0);
        break;
    case SCC_REG_LAST_ERROR:
        value = state->last_error;
        break;
    case SCC_REG_OWNER_GENERATION:
        value = state->owner_generation;
        break;
    case SCC_REG_MAP_ID:
        value = state->map_id;
        break;
    case SCC_REG_MAP_GENERATION:
        value = state->map_generation;
        break;
    case SCC_REG_MAP_MODEL_GENERATION:
        value = state->map_model_generation;
        break;
    case SCC_REG_MAP_FLAGS:
        value = state->map_flags;
        break;
    case SCC_REG_CLOCK_MHZ:
        value = state->config.clock_mhz;
        break;
    case SCC_REG_LOAD_TIMEOUT_NS:
        value = state->load_timeout_ns;
        break;
    case SCC_REG_OWNER_TTBR0:
        value = state->owner_ttbr0;
        break;
    case SCC_REG_UPCALL_ENTRY:
        value = state->upcall_entry;
        break;
    case SCC_REG_LOGICAL_CONTEXTS:
        value = state->logical_context_count;
        break;
    case SCC_REG_ACTIVE_CONTEXT_ID:
        value = state->active_context_id;
        break;
    case SCC_REG_EVENT_SEQUENCE:
        value = event->sequence;
        break;
    case SCC_REG_EVENT_CONTEXT_ID:
        value = event->context_id;
        break;
    case SCC_REG_EVENT_PLT_TOKEN:
        value = ub_scc_plt_token_pack(event->plt_token);
        break;
    case SCC_REG_EVENT_INTERRUPTED_PC:
        value = event->interrupted_pc;
        break;
    case SCC_REG_EVENT_FAULT_PC:
        value = event->fault_pc;
        break;
    case SCC_REG_EVENT_VA:
        value = event->effective_va;
        break;
    case SCC_REG_EVENT_VALUE:
        value = event->value;
        break;
    case SCC_REG_EVENT_KIND_STATUS:
        value = (uint64_t)event->status << 32 | event->kind;
        break;
    case SCC_REG_EVENT_META:
        value = (uint64_t)event->flags << 32 |
            (uint64_t)event->access_bytes << 16 | event->rt;
        break;
    default:
        break;
    }
    return ub_scc_access_extract(value, reg, size);
}

static void ub_scc_map_command(UbSccDeviceState *state, uint64_t command)
{
    UbSccMap *map;
    UbcObmmResolvedMap resolved;

    state->last_error = UB_SCC_ERROR_NONE;
    if (!state->map_id ||
        state->map_id > state->config.pending_load_entries ||
        !state->map_generation || !state->map_model_generation ||
        state->map_flags & ~SCC_MAP_LOGICAL_MIXED) {
        state->last_error = UB_SCC_ERROR_INVALID;
        return;
    }
    map = &state->maps[state->map_id - 1];
    if (command == 1) {
        if (map->active || !state->map_gsva_base ||
            !state->map_local_pa || !state->map_length ||
            state->map_gsva_base > UINT64_MAX - state->map_length ||
            !ubc_obmm_resolve_async_map(
                state->ubc_dev, state->map_local_pa,
                state->map_length, &resolved)) {
            state->last_error = UB_SCC_ERROR_NO_MAP;
            return;
        }
        *map = (UbSccMap) {
            .active = true,
            .generation = state->map_generation,
            .model_generation = state->map_model_generation,
            .flags = state->map_flags,
            .gsva_base = state->map_gsva_base,
            .length = state->map_length,
            .resolved = resolved,
        };
    } else if (command == 2) {
        if (!map->active || map->generation != state->map_generation) {
            state->last_error = UB_SCC_ERROR_STALE;
            return;
        }
        obmm_remote_retire_map(
            state->backend, state->map_id, state->map_generation,
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        memset(map, 0, sizeof(*map));
        ub_scc_schedule_bh(state);
    } else {
        state->last_error = UB_SCC_ERROR_INVALID;
    }
}

static void ub_scc_session_command(UbSccDeviceState *state,
                                   uint64_t command)
{
    state->last_error = UB_SCC_ERROR_NONE;
    if (command == 1) {
        if (!state->enabled || state->session_active || !current_cpu ||
            !state->owner_ttbr0 || !state->upcall_entry ||
            !state->logical_context_count ||
            state->logical_context_count > state->config.context_entries) {
            state->last_error = UB_SCC_ERROR_BUSY;
            return;
        }
        state->home_cpu = current_cpu;
        state->session_active = true;
        state->active_context_id = 0;
        state->upcall_active = false;
        arm_obmm_scc_set_active(state->home_cpu, true);
    } else if (command == 2) {
        if (state->home_cpu) {
            state->home_cpu->halted = 0;
            arm_obmm_scc_set_active(state->home_cpu, false);
        }
        state->session_active = false;
        state->home_cpu = NULL;
        if (!ub_scc_reset(state)) {
            state->last_error = UB_SCC_ERROR_REMOTE;
        }
    } else {
        state->last_error = UB_SCC_ERROR_INVALID;
    }
}

static void ub_scc_event_command(UbSccDeviceState *state,
                                 uint64_t command)
{
    state->last_error = UB_SCC_ERROR_NONE;
    if (command == 1) {
        if (!state->delivered_event_valid) {
            state->last_error = UB_SCC_ERROR_STALE;
            return;
        }
        memset(&state->delivered_event, 0,
               sizeof(state->delivered_event));
        state->delivered_event_valid = false;
    } else if (command == 2) {
        if (!state->session_active || state->delivered_event_valid ||
            !obmm_scc_event_pop(state->model,
                                &state->delivered_event)) {
            state->last_error = UB_SCC_ERROR_BUSY;
            return;
        }
        state->delivered_event.interrupted_pc = 0;
        state->delivered_event_valid = true;
    } else {
        state->last_error = UB_SCC_ERROR_INVALID;
    }
}

bool ub_scc_device_write(UbSccDeviceState *state, hwaddr reg,
                         uint64_t value, unsigned int size)
{
    hwaddr base = reg & ~7ULL;

    if (!state || (size != sizeof(uint32_t) &&
                   size != sizeof(uint64_t))) {
        return false;
    }
    switch (base) {
    case SCC_REG_MAP_GSVA_BASE:
        state->map_gsva_base = ub_scc_access_merge(
            state->map_gsva_base, reg, value, size);
        return true;
    case SCC_REG_MAP_LOCAL_PA:
        state->map_local_pa = ub_scc_access_merge(
            state->map_local_pa, reg, value, size);
        return true;
    case SCC_REG_MAP_LENGTH:
        state->map_length = value;
        return true;
    case SCC_REG_MAP_ID:
        state->map_id = value;
        return true;
    case SCC_REG_MAP_GENERATION:
        state->map_generation = value;
        return true;
    case SCC_REG_MAP_MODEL_GENERATION:
        state->map_model_generation = value;
        return true;
    case SCC_REG_MAP_FLAGS:
        state->map_flags = value;
        return true;
    case SCC_REG_MAP_COMMAND:
        ub_scc_map_command(state, value);
        return true;
    case SCC_REG_SESSION_COMMAND:
        ub_scc_session_command(state, value);
        return true;
    case SCC_REG_LOAD_TIMEOUT_NS:
        if (state->session_active) {
            state->last_error = UB_SCC_ERROR_BUSY;
        } else {
            state->load_timeout_ns = value;
            state->last_error = UB_SCC_ERROR_NONE;
        }
        return true;
    case SCC_REG_OWNER_TTBR0:
        if (state->session_active) {
            state->last_error = UB_SCC_ERROR_BUSY;
        } else {
            state->owner_ttbr0 = ub_scc_access_merge(
                state->owner_ttbr0, reg, value, size);
            state->last_error = UB_SCC_ERROR_NONE;
        }
        return true;
    case SCC_REG_UPCALL_ENTRY:
        if (state->session_active) {
            state->last_error = UB_SCC_ERROR_BUSY;
        } else {
            state->upcall_entry = ub_scc_access_merge(
                state->upcall_entry, reg, value, size);
        }
        return true;
    case SCC_REG_LOGICAL_CONTEXTS:
        if (state->session_active || !value ||
            value > state->config.context_entries) {
            state->last_error = UB_SCC_ERROR_INVALID;
        } else {
            state->logical_context_count = value;
        }
        return true;
    case SCC_REG_EVENT_COMMAND:
        ub_scc_event_command(state, value);
        return true;
    default:
        return false;
    }
}

bool ub_scc_cpu_enabled(CPUState *cpu)
{
    return ub_scc_global && ub_scc_global->session_active &&
        ub_scc_global->home_cpu == cpu;
}

bool ub_scc_cpu_owner_matches(CPUState *cpu, uint64_t ttbr0_el1)
{
    return ub_scc_cpu_enabled(cpu) &&
        ub_scc_global->owner_ttbr0 == ttbr0_el1;
}

uint64_t ub_scc_cpu_cycle(CPUState *cpu)
{
    UbSccDeviceState *state = ub_scc_global;

    if (!state || state->home_cpu != cpu) {
        return 0;
    }
    return ub_scc_ns_to_cycles(
        state, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

static UbSccMap *ub_scc_find_map(UbSccDeviceState *state,
                                 uint64_t va, uint8_t bytes,
                                 uint64_t *remote_offset,
                                 uint64_t *map_id)
{
    uint16_t slot;

    for (slot = 0; slot < G_N_ELEMENTS(state->maps); slot++) {
        UbSccMap *map = &state->maps[slot];

        if (!map->active || va < map->gsva_base ||
            va - map->gsva_base > map->length ||
            bytes > map->length - (va - map->gsva_base)) {
            continue;
        }
        *remote_offset = va - map->gsva_base;
        *map_id = slot + 1;
        return map;
    }
    return NULL;
}

bool ub_scc_cpu_address_is_remote(CPUState *cpu, uint64_t va,
                                  uint8_t bytes)
{
    UbSccDeviceState *state = ub_scc_global;
    uint64_t remote_offset;
    uint64_t map_id;

    return state && state->session_active && state->home_cpu == cpu &&
        ub_scc_find_map(state, va, bytes, &remote_offset, &map_id);
}

bool ub_scc_cpu_take_upcall(CPUState *cpu, uint64_t interrupted_pc,
                            uint64_t *upcall_entry)
{
    UbSccDeviceState *state = ub_scc_global;

    if (!state || !upcall_entry || !state->session_active ||
        state->home_cpu != cpu || state->upcall_active ||
        state->delivered_event_valid ||
        !obmm_scc_event_pop(state->model, &state->delivered_event)) {
        return false;
    }
    state->delivered_event.interrupted_pc = interrupted_pc;
    state->delivered_event_valid = true;
    state->upcall_active = true;
    obmm_scc_record_direct_upcall(state->model);
    *upcall_entry = state->upcall_entry;
    return true;
}

bool ub_scc_cpu_resume(CPUState *cpu, uint64_t context_id)
{
    UbSccDeviceState *state = ub_scc_global;
    uint16_t slot;

    if (!state || !state->session_active || state->home_cpu != cpu ||
        !context_id || state->delivered_event_valid) {
        return false;
    }
    slot = obmm_scc_context_id_slot(context_id);
    if (cpu->cpu_index < 0 || cpu->cpu_index > UINT16_MAX ||
        obmm_scc_context_id_generation(context_id) !=
            state->owner_generation ||
        obmm_scc_context_id_home_core(context_id) !=
            (uint16_t)cpu->cpu_index ||
        slot >= state->logical_context_count) {
        return false;
    }
    state->active_context_id = context_id;
    state->upcall_active = false;
    cpu->halted = 0;
    return true;
}

void ub_scc_cpu_fail_stop(CPUState *cpu)
{
    UbSccDeviceState *state = ub_scc_global;

    if (state && state->home_cpu == cpu) {
        state->last_error = UB_SCC_ERROR_CPU;
        obmm_scc_mark_fail_stop(state->model);
        state->session_active = false;
    }
}

static bool ub_scc_sink_validate(void *opaque, uint64_t sink_id,
                                 uint64_t sink_generation)
{
    UbSccFuture *future = opaque;

    return future->active && future->state->session_active &&
        ub_scc_plt_token_pack(future->plt_token) == sink_id &&
        future->plt_token.generation == sink_generation;
}

static void ub_scc_complete_plt(UbSccFuture *future,
                                ObmmRemoteStatus backend_status,
                                const void *payload, uint8_t bytes_done,
                                uint64_t publish_ns)
{
    UbSccDeviceState *state = future->state;
    ObmmSccLoadStatus status =
        ub_scc_status_from_backend(backend_status);
    ObmmSccCompletionResult completion;
    uint64_t cycle = ub_scc_ns_to_cycles(state, publish_ns);

    completion = obmm_scc_load_complete(
        state->model, future->plt_token, status, payload,
        bytes_done, cycle);
    if (completion != OBMM_SCC_COMPLETION_ACCEPTED) {
        trace_scc_stale_completion(
            state->owner_generation, future->plt_token.generation,
            future->plt_token.slot, cycle);
    } else if (status == OBMM_SCC_LOAD_SUCCESS) {
        trace_scc_load_complete(
            state->owner_generation, future->context_id,
            future->plt_token.generation, future->plt_token.slot,
            status, cycle);
    } else {
        trace_scc_load_fault(
            state->owner_generation, future->context_id,
            future->plt_token.generation, future->plt_token.slot,
            status, cycle);
    }
}

static void ub_scc_load_complete(void *opaque,
                                 const ObmmRemoteResult *result)
{
    UbSccFuture *future = opaque;
    UbSccDeviceState *state = future->state;

    if (!future->active) {
        return;
    }
    ub_scc_complete_plt(future, result->status, result->payload,
                        result->bytes_done, result->model_publish_ns);
    future->active = false;
    if (state->home_cpu) {
        state->home_cpu->halted = 0;
        cpu_exit(state->home_cpu);
        qemu_cpu_kick(state->home_cpu);
    }
}

UbSccLoadTryResult ub_scc_cpu_remote_load(
    CPUState *cpu, const ObmmSccLoadDesc *load)
{
    UbSccDeviceState *state = ub_scc_global;
    ObmmSccLoadDesc resolved_load;
    ObmmSccPltToken plt_token;
    ObmmSccPendingResult pending;
    ObmmRemoteRequest request;
    ObmmRemoteResult inline_result;
    ObmmRemoteToken backend_token;
    ObmmRemoteStatus status;
    ObmmSubmitDisposition disposition;
    UbSccFuture *future;
    UbSccMap *map;
    uint64_t remote_offset;
    uint64_t map_id;
    uint64_t operation_ordinal;
    uint64_t now_ns;
    uint16_t context_slot;

    if (!state || !load || !state->session_active ||
        state->home_cpu != cpu || !state->active_context_id ||
        state->upcall_active) {
        return UB_SCC_LOAD_NOT_REMOTE;
    }
    map = ub_scc_find_map(state, load->effective_va,
                          load->access_bytes, &remote_offset, &map_id);
    if (!map) {
        return UB_SCC_LOAD_NOT_REMOTE;
    }
    context_slot = obmm_scc_context_id_slot(state->active_context_id);
    if (context_slot >= state->logical_context_count ||
        !obmm_scc_logical_remote_ordinal(
            context_slot, state->logical_context_count,
            state->context_next_ordinal[context_slot],
            &operation_ordinal)) {
        return UB_SCC_LOAD_FAIL_STOP;
    }
    now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    resolved_load = *load;
    resolved_load.map_id = map_id;
    resolved_load.map_generation = map->generation;
    resolved_load.remote_offset = remote_offset;
    if (state->load_timeout_ns) {
        uint64_t deadline_ns = now_ns >
            UINT64_MAX - state->load_timeout_ns ? UINT64_MAX :
            now_ns + state->load_timeout_ns;

        resolved_load.deadline_cycle = ub_scc_ns_to_cycles(
            state, deadline_ns);
    }
    pending = obmm_scc_load_pending(
        state->model, state->active_context_id,
        &resolved_load, &plt_token);
    if (pending == OBMM_SCC_PENDING_SYNC_STALL) {
        trace_scc_capacity_stall(
            state->owner_generation, state->active_context_id,
            load->submit_cycle);
        return UB_SCC_LOAD_SYNC_STALL;
    }
    if (pending != OBMM_SCC_PENDING_ACCEPTED) {
        return UB_SCC_LOAD_FAIL_STOP;
    }
    state->context_next_ordinal[context_slot]++;
    future = &state->futures[plt_token.slot];
    if (future->active) {
        obmm_scc_load_complete(state->model, plt_token,
                               OBMM_SCC_LOAD_INTERNAL,
                               NULL, 0, load->submit_cycle);
        return UB_SCC_LOAD_FAIL_STOP;
    }
    *future = (UbSccFuture) {
        .state = state,
        .active = true,
        .context_id = state->active_context_id,
        .plt_token = plt_token,
    };
    trace_scc_load_pending(
        state->owner_generation, state->active_context_id,
        plt_token.generation, plt_token.slot, load->submit_cycle);
    request = (ObmmRemoteRequest) {
        .map_id = map_id,
        .map_generation = map->generation,
        .remote_offset = remote_offset,
        .length = load->access_bytes,
        .deadline_model_ns = state->load_timeout_ns ?
            (now_ns > UINT64_MAX - state->load_timeout_ns ?
             UINT64_MAX : now_ns + state->load_timeout_ns) : 0,
        .operation_ordinal = operation_ordinal,
        .operation_key = ub_obmm_remote_operation_key(
            &state->ubc_dev->remote_memory_model.config,
            &(UbObmmRemoteOperation) {
                .map_id = map->resolved.map_id,
                .map_generation = map->resolved.map_generation,
                .remote_offset = remote_offset,
                .length = load->access_bytes,
                .per_range_ordinal = operation_ordinal,
            }),
        .sink = {
            .kind = OBMM_REMOTE_SINK_P2B,
            .sink_id = ub_scc_plt_token_pack(plt_token),
            .sink_generation = plt_token.generation,
            .owner_id = UB_SCC_BACKEND_OWNER_ID,
            .validate = ub_scc_sink_validate,
            .complete = ub_scc_load_complete,
            .adapter_state = future,
        },
    };
    disposition = obmm_remote_submit(
        state->backend, &request, now_ns, &backend_token,
        &inline_result, &status);
    if (disposition == OBMM_SUBMIT_REJECTED) {
        ub_scc_complete_plt(future, status, NULL, 0, now_ns);
        future->active = false;
    } else if (disposition == OBMM_SUBMIT_INLINE) {
        trace_scc_load_inline(
            state->owner_generation, state->active_context_id,
            plt_token.generation, plt_token.slot, load->submit_cycle);
        ub_scc_complete_plt(future, inline_result.status,
                            inline_result.payload,
                            inline_result.bytes_done, now_ns);
        future->active = false;
    } else {
        future->backend_token = backend_token;
    }
    ub_scc_arm_deadline(state);
    return obmm_scc_fail_stop(state->model) ?
        UB_SCC_LOAD_FAIL_STOP : UB_SCC_LOAD_PENDING;
}

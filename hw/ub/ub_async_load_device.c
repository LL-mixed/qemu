/*
 * QEMU adapter for the OBMM direct-to-EL0 upcall mechanism.
 */

#include "qemu/osdep.h"
#include "hw/ub/ub_obmm_remote.h"
#include "hw/ub/ub_async_load_device.h"
#include "hw/ub/ub_ubc.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "sysemu/cpus.h"
#include "sysemu/dma.h"
#include "trace.h"

#define UB_ASYNC_LOAD_BACKEND_OWNER_ID 2
#define UB_ASYNC_LOAD_CACHEABLE_WAIT_OWNER_ID 3
#define UB_ASYNC_LOAD_CHILD_CHUNK_BYTES 64

#define ASYNC_LOAD_REG_VERSION_CAPS 0x000
#define ASYNC_LOAD_REG_STATUS 0x008
#define ASYNC_LOAD_REG_LAST_ERROR 0x010
#define ASYNC_LOAD_REG_OWNER_GENERATION 0x018
#define ASYNC_LOAD_REG_MAP_GSVA_BASE 0x020
#define ASYNC_LOAD_REG_MAP_LOCAL_PA 0x028
#define ASYNC_LOAD_REG_MAP_LENGTH 0x030
#define ASYNC_LOAD_REG_MAP_ID 0x038
#define ASYNC_LOAD_REG_MAP_GENERATION 0x040
#define ASYNC_LOAD_REG_MAP_COMMAND 0x048
#define ASYNC_LOAD_REG_SESSION_COMMAND 0x0a0
#define ASYNC_LOAD_REG_CLOCK_MHZ 0x0f8
#define ASYNC_LOAD_REG_LOAD_TIMEOUT_NS 0x100
#define ASYNC_LOAD_REG_OWNER_TTBR0 0x108
#define ASYNC_LOAD_REG_MAP_MODEL_GENERATION 0x110
#define ASYNC_LOAD_REG_MAP_FLAGS 0x118
#define ASYNC_LOAD_REG_UPCALL_ENTRY 0x120
#define ASYNC_LOAD_REG_LOGICAL_CONTEXTS 0x128
#define ASYNC_LOAD_REG_ACTIVE_CONTEXT_ID 0x130
#define ASYNC_LOAD_REG_EVENT_RING_BASE 0x138
#define ASYNC_LOAD_REG_EVENT_RING_BYTES 0x140
#define ASYNC_LOAD_REG_EVENT_CONSUMER_BASE 0x148
#define ASYNC_LOAD_REG_EVENT_CONSUMER_BYTES 0x150
#define ASYNC_LOAD_REG_EVENT_SLOT_BYTES 0x158
#define ASYNC_LOAD_REG_EVENT_PRODUCER_SEQUENCE 0x160
#define ASYNC_LOAD_REG_EVENT_CONSUMER_SEQUENCE 0x168
#define ASYNC_LOAD_REG_EVENT_RING_PUBLISHED 0x170
#define ASYNC_LOAD_REG_EVENT_WAIT_WAKEUPS 0x180
#define ASYNC_LOAD_REG_SCHEDULER_ENTERS 0x188
#define ASYNC_LOAD_REG_SESSION_FLAGS 0x190
#define ASYNC_LOAD_REG_CAPABILITIES 0x198
#define ASYNC_LOAD_REG_IRQ_STATUS 0x1a0
#define ASYNC_LOAD_REG_IRQ_ACK 0x1a8
#define ASYNC_LOAD_REG_REPLAY_CONTEXT_ID 0x1b0
#define ASYNC_LOAD_REG_REPLAY_TOKEN 0x1b8
#define ASYNC_LOAD_REG_REPLAY_PC 0x1c0
#define ASYNC_LOAD_REG_REPLAY_COMMAND 0x1c8
#define ASYNC_LOAD_REG_STATS_BASE 0x200
#define ASYNC_LOAD_REG_REPLAY_STATS_BASE 0x400
#define ASYNC_LOAD_REG_PATH_STATS_BASE 0x420

#define ASYNC_LOAD_STATUS_ACTIVE BIT(0)
#define ASYNC_LOAD_STATUS_FAIL_STOP BIT(1)
#define ASYNC_LOAD_STATUS_ENABLED BIT(2)
#define ASYNC_LOAD_STATUS_EVENT_PENDING BIT(3)
#define ASYNC_LOAD_STATUS_EVENT_DELIVERED BIT(4)
#define ASYNC_LOAD_STATUS_UPCALL_ACTIVE BIT(5)
#define ASYNC_LOAD_MAP_LOGICAL_MIXED BIT(0)
#define ASYNC_LOAD_EVENT_PRODUCER_HEADER_BYTES 64
#define ASYNC_LOAD_EVENT_SLOT_BYTES 128
#define ASYNC_LOAD_EVENT_CONSUMER_BYTES 64
#define ASYNC_LOAD_IRQ_COMPLETION BIT(0)

typedef struct QEMU_PACKED UbAsyncLoadEventProducerV3 {
    uint32_t abi_version;
    uint16_t event_depth;
    uint16_t event_slot_bytes;
    uint64_t owner_generation;
    uint64_t producer_sequence;
    uint64_t published_events;
    uint64_t wait_wakeups;
    uint64_t reserved[3];
} UbAsyncLoadEventProducerV3;

typedef struct QEMU_PACKED UbAsyncLoadEventConsumerV3 {
    uint32_t abi_version;
    uint32_t flags;
    uint64_t owner_generation;
    uint64_t consumer_sequence;
    uint64_t wait_count;
    uint64_t scheduler_enter_count;
    uint64_t reserved[3];
} UbAsyncLoadEventConsumerV3;

typedef struct QEMU_PACKED UbAsyncLoadEventV3 {
    uint64_t sequence;
    uint64_t owner_generation;
    uint64_t context_id;
    uint64_t plt_token;
    uint64_t interrupted_pc;
    uint64_t fault_pc;
    uint64_t effective_va;
    uint64_t value;
    uint64_t map_id;
    uint64_t map_generation;
    uint64_t model_phase_generation;
    uint32_t kind;
    uint32_t status;
    uint16_t rt;
    uint16_t access_bytes;
    uint32_t flags;
    uint64_t reserved[3];
} UbAsyncLoadEventV3;

QEMU_BUILD_BUG_ON(sizeof(UbAsyncLoadEventProducerV3) !=
                  ASYNC_LOAD_EVENT_PRODUCER_HEADER_BYTES);
QEMU_BUILD_BUG_ON(sizeof(UbAsyncLoadEventConsumerV3) !=
                  ASYNC_LOAD_EVENT_CONSUMER_BYTES);
QEMU_BUILD_BUG_ON(sizeof(UbAsyncLoadEventV3) != ASYNC_LOAD_EVENT_SLOT_BYTES);

typedef enum UbAsyncLoadError {
    UB_ASYNC_LOAD_ERROR_NONE,
    UB_ASYNC_LOAD_ERROR_INVALID,
    UB_ASYNC_LOAD_ERROR_BUSY,
    UB_ASYNC_LOAD_ERROR_NO_MAP,
    UB_ASYNC_LOAD_ERROR_STALE,
    UB_ASYNC_LOAD_ERROR_REMOTE,
    UB_ASYNC_LOAD_ERROR_CPU,
} UbAsyncLoadError;

typedef struct UbAsyncLoadMap {
    bool active;
    uint64_t generation;
    uint64_t model_generation;
    uint32_t flags;
    uint64_t gsva_base;
    uint64_t length;
    UbcObmmResolvedMap resolved;
} UbAsyncLoadMap;

typedef enum UbAsyncLoadFutureKind {
    UB_ASYNC_LOAD_FUTURE_NC,
    UB_ASYNC_LOAD_FUTURE_CACHEABLE,
} UbAsyncLoadFutureKind;

typedef struct UbAsyncLoadFuture {
    UbAsyncLoadDeviceState *state;
    bool active;
    UbAsyncLoadFutureKind kind;
    uint32_t generation;
    uint16_t slot;
    uint64_t context_id;
    UbAsyncLoadPltToken wait_key;
    UbAsyncLoadPltToken plt_token;
    UbAsyncLoadDesc load;
    uint64_t fill_remote_offset;
    uint32_t fill_bytes;
    ObmmRemoteToken backend_token;
} UbAsyncLoadFuture;

struct UbAsyncLoadDeviceState {
    BusControllerDev *ubc_dev;
    UbAsyncLoad *model;
    ObmmRemoteBackend *backend;
    QEMUBH *bh;
    QEMUTimer *deadline_timer;
    CPUState *home_cpu;
    UbAsyncLoadConfig config;
    uint32_t owner_generation;
    bool enabled;
    bool session_active;
    bool upcall_active;
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
    uint64_t session_flags;
    uint64_t active_context_id;
    uint64_t event_ring_base;
    uint64_t event_ring_bytes;
    uint64_t event_consumer_base;
    uint64_t event_consumer_bytes;
    uint64_t event_producer_sequence;
    uint64_t event_consumer_sequence;
    uint64_t event_ring_published;
    uint64_t event_wait_wakeups;
    uint64_t scheduler_enters;
    uint64_t irq_status;
    uint64_t replay_context_id;
    uint64_t replay_token;
    uint64_t replay_pc;
    bool replay_armed;
    UbAsyncLoadPltToken armed_replay_token;
    uint16_t logical_context_count;
    uint64_t context_next_ordinal[UB_ASYNC_LOAD_MAX_CONTEXTS];
    uint64_t context_cookies[UB_ASYNC_LOAD_MAX_CONTEXTS];
    bool context_cookie_used[UB_ASYNC_LOAD_MAX_CONTEXTS];
    UbAsyncLoadMap maps[UB_ASYNC_LOAD_MAX_PENDING_LOADS];
    UbAsyncLoadFuture futures[UB_ASYNC_LOAD_MAX_PENDING_LOADS];
};

static UbAsyncLoadDeviceState *ub_async_load_global;

static uint64_t ub_async_load_ns_to_cycles(const UbAsyncLoadDeviceState *state,
                                    uint64_t ns)
{
    if (ns > UINT64_MAX / state->config.clock_mhz) {
        return UINT64_MAX;
    }
    return ns * state->config.clock_mhz / 1000;
}

static uint64_t ub_async_load_access_extract(uint64_t value, hwaddr reg,
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

static uint64_t ub_async_load_access_merge(uint64_t current, hwaddr reg,
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

static void ub_async_load_ring_fail_stop(UbAsyncLoadDeviceState *state,
                                   UbAsyncLoadError error)
{
    state->last_error = error;
    ub_async_load_mark_fail_stop(state->model);
}

static bool ub_async_load_ring_configured(const UbAsyncLoadDeviceState *state)
{
    uint64_t required = ASYNC_LOAD_EVENT_PRODUCER_HEADER_BYTES +
        (uint64_t)state->config.event_queue_depth *
        ASYNC_LOAD_EVENT_SLOT_BYTES;

    return state->event_ring_base && state->event_consumer_base &&
        state->event_ring_bytes >= required &&
        state->event_consumer_bytes >= ASYNC_LOAD_EVENT_CONSUMER_BYTES;
}

static bool ub_async_load_ring_sync_consumer(UbAsyncLoadDeviceState *state)
{
    UbAsyncLoadEventConsumerV3 consumer;
    uint64_t owner_generation;
    uint64_t sequence;

    if (dma_memory_read(&address_space_memory, state->event_consumer_base,
                        &consumer, sizeof(consumer),
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        ub_async_load_ring_fail_stop(state, UB_ASYNC_LOAD_ERROR_REMOTE);
        return false;
    }
    owner_generation = le64_to_cpu(consumer.owner_generation);
    sequence = le64_to_cpu(consumer.consumer_sequence);
    if (le32_to_cpu(consumer.abi_version) !=
            UB_ASYNC_LOAD_EVENT_ABI_VERSION ||
        le32_to_cpu(consumer.flags) ||
        owner_generation != state->owner_generation ||
        sequence < state->event_consumer_sequence ||
        sequence > state->event_producer_sequence) {
        ub_async_load_ring_fail_stop(state, UB_ASYNC_LOAD_ERROR_STALE);
        return false;
    }
    state->event_consumer_sequence = sequence;
    return true;
}

static bool ub_async_load_ring_start(UbAsyncLoadDeviceState *state)
{
    UbAsyncLoadEventProducerV3 producer = {
        .abi_version = cpu_to_le32(UB_ASYNC_LOAD_EVENT_ABI_VERSION),
        .event_depth = cpu_to_le16(state->config.event_queue_depth),
        .event_slot_bytes = cpu_to_le16(ASYNC_LOAD_EVENT_SLOT_BYTES),
        .owner_generation = cpu_to_le64(state->owner_generation),
    };

    if (!ub_async_load_ring_configured(state) ||
        dma_memory_write(&address_space_memory, state->event_ring_base,
                         &producer, sizeof(producer),
                         MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        ub_async_load_ring_fail_stop(state, UB_ASYNC_LOAD_ERROR_REMOTE);
        return false;
    }
    state->event_producer_sequence = 0;
    state->event_consumer_sequence = 0;
    state->event_ring_published = 0;
    state->event_wait_wakeups = 0;
    state->scheduler_enters = 0;
    return ub_async_load_ring_sync_consumer(state);
}

static bool ub_async_load_ring_update_u64(UbAsyncLoadDeviceState *state,
                                    uint64_t offset, uint64_t value)
{
    uint64_t wire = cpu_to_le64(value);

    if (dma_memory_write(&address_space_memory,
                         state->event_ring_base + offset,
                         &wire, sizeof(wire),
                         MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        ub_async_load_ring_fail_stop(state, UB_ASYNC_LOAD_ERROR_REMOTE);
        return false;
    }
    return true;
}

static void ub_async_load_event_encode(UbAsyncLoadDeviceState *state,
                                 const UbAsyncLoadEvent *event,
                                 UbAsyncLoadEventV3 *wire)
{
    *wire = (UbAsyncLoadEventV3) {
        .sequence = cpu_to_le64(event->sequence),
        .owner_generation = cpu_to_le64(state->owner_generation),
        .context_id = cpu_to_le64(event->context_id),
        .plt_token = cpu_to_le64(
            (uint64_t)event->plt_token.generation << 32 |
            (uint64_t)event->plt_token.owner_id << 16 |
            event->plt_token.slot),
        .interrupted_pc = cpu_to_le64(event->interrupted_pc),
        .fault_pc = cpu_to_le64(event->fault_pc),
        .effective_va = cpu_to_le64(event->effective_va),
        .value = cpu_to_le64(event->value),
        .map_id = cpu_to_le64(event->map_id),
        .map_generation = cpu_to_le64(event->map_generation),
        .model_phase_generation =
            cpu_to_le64(event->map_model_generation),
        .kind = cpu_to_le32(event->kind),
        .status = cpu_to_le32(event->status),
        .rt = cpu_to_le16(event->rt),
        .access_bytes = cpu_to_le16(event->access_bytes),
        .flags = cpu_to_le32(event->flags),
        .reserved[0] = cpu_to_le64(
            state->session_flags & UB_ASYNC_LOAD_START_KERNEL_TASK ?
            event->context_cookie : 0),
    };
}

static bool ub_async_load_ring_publish_one(UbAsyncLoadDeviceState *state,
                                     uint64_t interrupted_pc)
{
    UbAsyncLoadEvent event;
    UbAsyncLoadEventV3 wire;
    uint64_t slot;

    if (!ub_async_load_event_pending(state->model)) {
        return false;
    }
    if (!ub_async_load_ring_sync_consumer(state)) {
        return false;
    }
    if (state->event_producer_sequence - state->event_consumer_sequence >=
        state->config.event_queue_depth) {
        ub_async_load_ring_fail_stop(state, UB_ASYNC_LOAD_ERROR_BUSY);
        return false;
    }
    if (!ub_async_load_event_pop(state->model, &event) ||
        event.sequence != state->event_producer_sequence + 1) {
        ub_async_load_ring_fail_stop(state, UB_ASYNC_LOAD_ERROR_STALE);
        return false;
    }
    event.interrupted_pc = interrupted_pc;
    ub_async_load_event_encode(state, &event, &wire);
    slot = (event.sequence - 1) % state->config.event_queue_depth;
    if (dma_memory_write(
            &address_space_memory,
            state->event_ring_base +
                ASYNC_LOAD_EVENT_PRODUCER_HEADER_BYTES +
                slot * ASYNC_LOAD_EVENT_SLOT_BYTES,
            &wire, sizeof(wire), MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        ub_async_load_ring_fail_stop(state, UB_ASYNC_LOAD_ERROR_REMOTE);
        return false;
    }
    state->event_ring_published++;
    if (!ub_async_load_ring_update_u64(
            state, offsetof(UbAsyncLoadEventProducerV3, published_events),
            state->event_ring_published)) {
        return false;
    }
    smp_wmb();
    if (!ub_async_load_ring_update_u64(
            state, offsetof(UbAsyncLoadEventProducerV3, producer_sequence),
            event.sequence)) {
        return false;
    }
    state->event_producer_sequence = event.sequence;
    return true;
}

static uint64_t ub_async_load_plt_token_pack(UbAsyncLoadPltToken token)
{
    return (uint64_t)token.generation << 32 |
        (uint64_t)token.owner_id << 16 | token.slot;
}

static UbAsyncLoadPltToken ub_async_load_plt_token_unpack(uint64_t token)
{
    return (UbAsyncLoadPltToken) {
        .generation = token >> 32,
        .owner_id = token >> 16,
        .slot = token,
    };
}

static uint32_t ub_async_load_generation_next(uint32_t generation)
{
    generation++;
    return generation ? generation : 1;
}

static void ub_async_load_futures_init(UbAsyncLoadDeviceState *state)
{
    uint16_t slot;

    memset(state->futures, 0, sizeof(state->futures));
    for (slot = 0; slot < G_N_ELEMENTS(state->futures); slot++) {
        state->futures[slot].state = state;
        state->futures[slot].generation = 1;
        state->futures[slot].slot = slot;
    }
}

static UbAsyncLoadFuture *ub_async_load_future_alloc(
    UbAsyncLoadDeviceState *state)
{
    uint16_t slot;

    for (slot = 0; slot < G_N_ELEMENTS(state->futures); slot++) {
        UbAsyncLoadFuture *future = &state->futures[slot];
        uint32_t generation;

        if (future->active) {
            continue;
        }
        generation = future->generation ? future->generation : 1;
        memset(future, 0, sizeof(*future));
        future->state = state;
        future->generation = generation;
        future->slot = slot;
        future->active = true;
        return future;
    }
    return NULL;
}

static void ub_async_load_future_release(UbAsyncLoadFuture *future)
{
    UbAsyncLoadDeviceState *state = future->state;
    uint32_t generation = ub_async_load_generation_next(future->generation);
    uint16_t slot = future->slot;

    memset(future, 0, sizeof(*future));
    future->state = state;
    future->generation = generation;
    future->slot = slot;
}

static UbAsyncLoadPltToken ub_async_load_future_backend_key(
    const UbAsyncLoadFuture *future)
{
    return (UbAsyncLoadPltToken) {
        .generation = future->generation,
        .owner_id = UB_ASYNC_LOAD_BACKEND_OWNER_ID,
        .slot = future->slot,
    };
}

static UbAsyncLoadPltToken ub_async_load_future_cacheable_wait_key(
    const UbAsyncLoadFuture *future)
{
    return (UbAsyncLoadPltToken) {
        .generation = future->generation,
        .owner_id = UB_ASYNC_LOAD_CACHEABLE_WAIT_OWNER_ID,
        .slot = future->slot,
    };
}

static UbAsyncLoadStatus ub_async_load_status_from_backend(
    ObmmRemoteStatus status)
{
    switch (status) {
    case OBMM_REMOTE_STATUS_SUCCESS:
        return UB_ASYNC_LOAD_STATUS_SUCCESS;
    case OBMM_REMOTE_STATUS_TIMEOUT:
        return UB_ASYNC_LOAD_STATUS_TIMEOUT;
    case OBMM_REMOTE_STATUS_PERMISSION:
    case OBMM_REMOTE_STATUS_TOKEN_DENIED:
    case OBMM_REMOTE_STATUS_COHERENCE:
        return UB_ASYNC_LOAD_STATUS_PERMISSION;
    case OBMM_REMOTE_STATUS_STALE_MAP:
    case OBMM_REMOTE_STATUS_RETIRED:
        return UB_ASYNC_LOAD_STATUS_STALE_MAP;
    case OBMM_REMOTE_STATUS_CANCELLED:
        return UB_ASYNC_LOAD_STATUS_CANCELLED;
    case OBMM_REMOTE_STATUS_REMOTE_IO:
    case OBMM_REMOTE_STATUS_CHECKSUM:
        return UB_ASYNC_LOAD_STATUS_REMOTE_IO;
    default:
        return UB_ASYNC_LOAD_STATUS_INTERNAL;
    }
}

static bool ub_async_load_map_valid(void *opaque, uint64_t map_id,
                             uint64_t map_generation)
{
    UbAsyncLoadDeviceState *state = opaque;
    UbAsyncLoadMap *map;

    if (!map_id || map_id > UB_ASYNC_LOAD_MAX_PENDING_LOADS) {
        return false;
    }
    map = &state->maps[map_id - 1];
    return map->active && map->generation == map_generation;
}

static void ub_async_load_child_complete(
    void *opaque, ObmmRemoteToken token, uint16_t child_index,
    ObmmRemoteStatus status, const void *payload, uint32_t bytes_done,
    uint64_t model_publish_ns);

static ObmmProviderChildDisposition ub_async_load_submit_child(
    void *opaque, ObmmRemoteToken token, uint16_t child_index,
    uint64_t map_id, uint64_t map_generation, uint64_t remote_offset,
    uint32_t length, uint64_t operation_ordinal, void *inline_payload,
    ObmmRemoteStatus *inline_status)
{
    UbAsyncLoadDeviceState *state = opaque;
    UbAsyncLoadMap *map;
    UbObmmRemoteOperation operation;

    if (!ub_async_load_map_valid(state, map_id, map_generation)) {
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
    return ubc_sim_dec_remote_read_async_submit(
        state->ubc_dev, &map->resolved, remote_offset, length,
        &operation, token, child_index, ub_async_load_child_complete,
        state, ub_async_load_cpu_kernel_task_mode(current_cpu),
        inline_payload, inline_status);
}

static void ub_async_load_cancel_provider(void *opaque, ObmmRemoteToken token)
{
    UbAsyncLoadDeviceState *state = opaque;

    ubc_sim_dec_remote_read_async_cancel(state->ubc_dev, token);
}

static void ub_async_load_arm_deadline(UbAsyncLoadDeviceState *state)
{
    uint64_t deadline = obmm_remote_next_deadline(state->backend);

    if (deadline == UINT64_MAX) {
        timer_del(state->deadline_timer);
    } else {
        timer_mod_ns(state->deadline_timer, deadline);
    }
}

static void ub_async_load_bh(void *opaque)
{
    UbAsyncLoadDeviceState *state = opaque;

    obmm_remote_deliver_ready(state->backend);
    ub_async_load_arm_deadline(state);
}

static void ub_async_load_schedule_bh(UbAsyncLoadDeviceState *state)
{
    qemu_bh_schedule(state->bh);
}

static void ub_async_load_deadline(void *opaque)
{
    UbAsyncLoadDeviceState *state = opaque;

    obmm_remote_run_deadlines(
        state->backend, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    ub_async_load_schedule_bh(state);
}

static void ub_async_load_child_complete(
    void *opaque, ObmmRemoteToken token, uint16_t child_index,
    ObmmRemoteStatus status, const void *payload, uint32_t bytes_done,
    uint64_t model_publish_ns)
{
    UbAsyncLoadDeviceState *state = opaque;

    obmm_remote_child_complete(state->backend, token, child_index,
                               status, payload, bytes_done,
                               model_publish_ns);
    ub_async_load_schedule_bh(state);
}

static bool ub_async_load_backend_new(UbAsyncLoadDeviceState *state)
{
    ObmmRemoteBackendOps ops = {
        .map_validate = ub_async_load_map_valid,
        .submit_child = ub_async_load_submit_child,
        .cancel = ub_async_load_cancel_provider,
        .opaque = state,
    };

    state->backend = obmm_remote_backend_new(
        UB_ASYNC_LOAD_BACKEND_OWNER_ID, UB_ASYNC_LOAD_CHILD_CHUNK_BYTES, &ops);
    return state->backend != NULL;
}

static bool ub_async_load_reset(UbAsyncLoadDeviceState *state)
{
    uint16_t slot;
    uint64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    qemu_bh_cancel(state->bh);
    timer_del(state->deadline_timer);
    ubc_async_load_irq_set(state->ubc_dev, false);
    for (slot = 0; slot < G_N_ELEMENTS(state->futures); slot++) {
        UbAsyncLoadFuture *future = &state->futures[slot];

        if (future->active && future->backend_token.owner_id) {
            obmm_remote_cancel(state->backend, future->backend_token,
                               OBMM_REMOTE_STATUS_CANCELLED, now_ns);
        }
    }
    obmm_remote_deliver_ready(state->backend);
    ub_async_load_free(state->model);
    state->owner_generation++;
    if (!state->owner_generation) {
        state->owner_generation = 1;
    }
    state->model = ub_async_load_new(UB_ASYNC_LOAD_BACKEND_OWNER_ID,
                                state->owner_generation,
                                &state->config);
    memset(state->maps, 0, sizeof(state->maps));
    ub_async_load_futures_init(state);
    memset(state->context_next_ordinal, 0,
           sizeof(state->context_next_ordinal));
    memset(state->context_cookies, 0, sizeof(state->context_cookies));
    memset(state->context_cookie_used, 0,
           sizeof(state->context_cookie_used));
    state->upcall_active = false;
    state->active_context_id = 0;
    state->logical_context_count = 0;
    state->upcall_entry = 0;
    state->session_flags = 0;
    state->event_ring_base = 0;
    state->event_ring_bytes = 0;
    state->event_consumer_base = 0;
    state->event_consumer_bytes = 0;
    state->event_producer_sequence = 0;
    state->event_consumer_sequence = 0;
    state->event_ring_published = 0;
    state->event_wait_wakeups = 0;
    state->scheduler_enters = 0;
    state->irq_status = 0;
    state->replay_context_id = 0;
    state->replay_token = 0;
    state->replay_pc = 0;
    state->replay_armed = false;
    memset(&state->armed_replay_token, 0,
           sizeof(state->armed_replay_token));
    state->load_timeout_ns = 0;
    state->owner_ttbr0 = 0;
    state->last_error = 0;
    return state->model && state->backend &&
        obmm_remote_pending(state->backend) == 0;
}

UbAsyncLoadDeviceState *ub_async_load_device_new(BusControllerDev *ubc_dev,
                                    const char *model_spec,
                                    Error **errp)
{
    UbAsyncLoadDeviceState *state;

    if (!ubc_dev || ub_async_load_global) {
        error_setg(errp, "OBMM async-load endpoint already exists");
        return NULL;
    }
    state = g_new0(UbAsyncLoadDeviceState, 1);
    state->ubc_dev = ubc_dev;
    state->owner_generation = 1;
    if (!ub_async_load_config_parse(model_spec, &state->enabled,
                               &state->config)) {
        error_setg(errp, "invalid OBMM async-load model spec: %s",
                   model_spec ? model_spec : "(null)");
        g_free(state);
        return NULL;
    }
    state->model = ub_async_load_new(UB_ASYNC_LOAD_BACKEND_OWNER_ID,
                                state->owner_generation,
                                &state->config);
    if (!state->model || !ub_async_load_backend_new(state)) {
        error_setg(errp, "failed to allocate OBMM EL0-upcall state");
        ub_async_load_free(state->model);
        g_free(state);
        return NULL;
    }
    ub_async_load_futures_init(state);
    state->bh = qemu_bh_new_guarded(
        ub_async_load_bh, state, &DEVICE(ubc_dev)->mem_reentrancy_guard);
    state->deadline_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                         ub_async_load_deadline, state);
    ub_async_load_global = state;
    return state;
}

void ub_async_load_device_free(UbAsyncLoadDeviceState *state)
{
    if (!state) {
        return;
    }
    if (state->home_cpu) {
        state->home_cpu->halted = 0;
        arm_async_load_set_active(state->home_cpu, false);
    }
    if (ub_async_load_global == state) {
        ub_async_load_global = NULL;
    }
    qemu_bh_delete(state->bh);
    timer_free(state->deadline_timer);
    obmm_remote_backend_free(state->backend);
    ub_async_load_free(state->model);
    g_free(state);
}

bool ub_async_load_device_decode(hwaddr address, hwaddr *reg)
{
    if (address < UB_ASYNC_LOAD_ENDPOINT_BASE ||
        address >= UB_ASYNC_LOAD_ENDPOINT_BASE + UB_ASYNC_LOAD_ENDPOINT_BYTES) {
        return false;
    }
    *reg = address - UB_ASYNC_LOAD_ENDPOINT_BASE;
    return true;
}

static uint64_t ub_async_load_stats_read(const UbAsyncLoadStats *stats,
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

static uint64_t ub_async_load_observability_read(UbAsyncLoadDeviceState *state,
                                          const UbAsyncLoadStats *stats,
                                          uint32_t index)
{
    const ObmmRemoteBackendStats *backend =
        obmm_remote_backend_stats(state->backend);

    switch (index) {
    case 0: return ub_async_load_pending_count(state->model);
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

uint64_t ub_async_load_device_read(UbAsyncLoadDeviceState *state, hwaddr reg,
                            unsigned int size)
{
    const UbAsyncLoadStats *stats;
    uint64_t value = 0;

    if (!state || (size != sizeof(uint32_t) &&
                   size != sizeof(uint64_t))) {
        return 0;
    }
    stats = ub_async_load_stats(state->model);
    if ((reg & ~7ULL) >= ASYNC_LOAD_REG_PATH_STATS_BASE &&
        (reg & ~7ULL) < ASYNC_LOAD_REG_PATH_STATS_BASE + 6 * 8) {
        uint32_t index = ((reg & ~7ULL) -
                          ASYNC_LOAD_REG_PATH_STATS_BASE) / 8;

        switch (index) {
        case 0: value = stats->nc_plt_allocations; break;
        case 1: value = ub_async_load_pending_count(state->model); break;
        case 2: value = stats->cacheable_fill_pending; break;
        case 3: value = stats->cacheable_fill_completed; break;
        case 4: value = stats->cacheable_replay_hits; break;
        case 5: value = stats->cacheable_fill_bytes; break;
        default: value = 0; break;
        }
        return ub_async_load_access_extract(value, reg, size);
    }
    if ((reg & ~7ULL) >= ASYNC_LOAD_REG_REPLAY_STATS_BASE &&
        (reg & ~7ULL) < ASYNC_LOAD_REG_REPLAY_STATS_BASE + 4 * 8) {
        uint32_t index = ((reg & ~7ULL) -
                          ASYNC_LOAD_REG_REPLAY_STATS_BASE) / 8;

        switch (index) {
        case 0: value = stats->replay_consumed; break;
        case 1: value = stats->replay_mismatch; break;
        case 2: value = stats->replay_ready_high_water; break;
        default: value = 0; break;
        }
        return ub_async_load_access_extract(value, reg, size);
    }
    if ((reg & ~7ULL) >= ASYNC_LOAD_REG_STATS_BASE) {
        uint32_t index = ((reg & ~7ULL) - ASYNC_LOAD_REG_STATS_BASE) / 8;

        value = index < 17 ? ub_async_load_stats_read(stats, index) :
            ub_async_load_observability_read(state, stats, index - 17);
        return ub_async_load_access_extract(value, reg, size);
    }
    switch (reg & ~7ULL) {
    case ASYNC_LOAD_REG_VERSION_CAPS:
        value = UB_ASYNC_LOAD_ABI_VERSION |
            ((uint64_t)state->config.context_entries << 16) |
            ((uint64_t)state->config.pending_load_entries << 32) |
            ((uint64_t)state->config.event_queue_depth << 48);
        break;
    case ASYNC_LOAD_REG_STATUS:
        /* Keep status reads useful for setup, teardown and diagnostics. */
        obmm_remote_run_deadlines(
            state->backend, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        obmm_remote_deliver_ready(state->backend);
        ub_async_load_arm_deadline(state);
        value = (state->session_active ? ASYNC_LOAD_STATUS_ACTIVE : 0) |
            (state->enabled ? ASYNC_LOAD_STATUS_ENABLED : 0) |
            (ub_async_load_fail_stop(state->model) ? ASYNC_LOAD_STATUS_FAIL_STOP : 0) |
            (ub_async_load_event_pending(state->model) ?
             ASYNC_LOAD_STATUS_EVENT_PENDING : 0) |
            (state->event_producer_sequence !=
                 state->event_consumer_sequence ?
             ASYNC_LOAD_STATUS_EVENT_DELIVERED : 0) |
            (state->upcall_active ? ASYNC_LOAD_STATUS_UPCALL_ACTIVE : 0);
        break;
    case ASYNC_LOAD_REG_LAST_ERROR:
        value = state->last_error;
        break;
    case ASYNC_LOAD_REG_OWNER_GENERATION:
        value = state->owner_generation;
        break;
    case ASYNC_LOAD_REG_MAP_ID:
        value = state->map_id;
        break;
    case ASYNC_LOAD_REG_MAP_GENERATION:
        value = state->map_generation;
        break;
    case ASYNC_LOAD_REG_MAP_MODEL_GENERATION:
        value = state->map_model_generation;
        break;
    case ASYNC_LOAD_REG_MAP_FLAGS:
        value = state->map_flags;
        break;
    case ASYNC_LOAD_REG_CLOCK_MHZ:
        value = state->config.clock_mhz;
        break;
    case ASYNC_LOAD_REG_LOAD_TIMEOUT_NS:
        value = state->load_timeout_ns;
        break;
    case ASYNC_LOAD_REG_OWNER_TTBR0:
        value = state->owner_ttbr0;
        break;
    case ASYNC_LOAD_REG_UPCALL_ENTRY:
        value = state->upcall_entry;
        break;
    case ASYNC_LOAD_REG_LOGICAL_CONTEXTS:
        value = state->logical_context_count;
        break;
    case ASYNC_LOAD_REG_SESSION_FLAGS:
        value = state->session_flags;
        break;
    case ASYNC_LOAD_REG_CAPABILITIES:
        value = UB_ASYNC_LOAD_CAP_REPLAY_RETIRE |
            UB_ASYNC_LOAD_CAP_KERNEL_FREE_EVENT_RING |
            UB_ASYNC_LOAD_CAP_EL0_WAIT_WAKE |
            UB_ASYNC_LOAD_CAP_EL0_SCHEDULER_ENTER |
            UB_ASYNC_LOAD_CAP_KERNEL_TASK_REPLAY |
            UB_ASYNC_LOAD_CAP_NC_REPLAY_TOKEN |
            UB_ASYNC_LOAD_CAP_SVC_CONTEXT_RESUME |
            UB_ASYNC_LOAD_CAP_WFE_WAIT |
            UB_ASYNC_LOAD_CAP_CACHEABLE_FILL_REPLAY;
        if (ubc_void_response_policy_enabled(state->ubc_dev)) {
            value |= UB_ASYNC_LOAD_CAP_VOID_RESPONSE_RETRY;
        }
        break;
    case ASYNC_LOAD_REG_IRQ_STATUS:
        value = state->irq_status;
        break;
    case ASYNC_LOAD_REG_REPLAY_CONTEXT_ID:
        value = state->replay_context_id;
        break;
    case ASYNC_LOAD_REG_REPLAY_TOKEN:
        value = state->replay_token;
        break;
    case ASYNC_LOAD_REG_REPLAY_PC:
        value = state->replay_pc;
        break;
    case ASYNC_LOAD_REG_ACTIVE_CONTEXT_ID:
        value = state->active_context_id;
        break;
    case ASYNC_LOAD_REG_EVENT_RING_BASE:
        value = state->event_ring_base;
        break;
    case ASYNC_LOAD_REG_EVENT_RING_BYTES:
        value = state->event_ring_bytes;
        break;
    case ASYNC_LOAD_REG_EVENT_CONSUMER_BASE:
        value = state->event_consumer_base;
        break;
    case ASYNC_LOAD_REG_EVENT_CONSUMER_BYTES:
        value = state->event_consumer_bytes;
        break;
    case ASYNC_LOAD_REG_EVENT_SLOT_BYTES:
        value = ASYNC_LOAD_EVENT_SLOT_BYTES;
        break;
    case ASYNC_LOAD_REG_EVENT_PRODUCER_SEQUENCE:
        value = state->event_producer_sequence;
        break;
    case ASYNC_LOAD_REG_EVENT_CONSUMER_SEQUENCE:
        value = state->event_consumer_sequence;
        break;
    case ASYNC_LOAD_REG_EVENT_RING_PUBLISHED:
        value = state->event_ring_published;
        break;
    case ASYNC_LOAD_REG_EVENT_WAIT_WAKEUPS:
        value = state->event_wait_wakeups;
        break;
    case ASYNC_LOAD_REG_SCHEDULER_ENTERS:
        value = state->scheduler_enters;
        break;
    default:
        break;
    }
    return ub_async_load_access_extract(value, reg, size);
}

static void ub_async_load_map_command(UbAsyncLoadDeviceState *state, uint64_t command)
{
    UbAsyncLoadMap *map;
    UbcObmmResolvedMap resolved;

    state->last_error = UB_ASYNC_LOAD_ERROR_NONE;
    if (!state->map_id ||
        state->map_id > state->config.pending_load_entries ||
        !state->map_generation || !state->map_model_generation ||
        state->map_flags & ~ASYNC_LOAD_MAP_LOGICAL_MIXED) {
        state->last_error = UB_ASYNC_LOAD_ERROR_INVALID;
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
            state->last_error = UB_ASYNC_LOAD_ERROR_NO_MAP;
            return;
        }
        *map = (UbAsyncLoadMap) {
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
            state->last_error = UB_ASYNC_LOAD_ERROR_STALE;
            return;
        }
        obmm_remote_retire_map(
            state->backend, state->map_id, state->map_generation,
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        memset(map, 0, sizeof(*map));
        ub_async_load_schedule_bh(state);
    } else {
        state->last_error = UB_ASYNC_LOAD_ERROR_INVALID;
    }
}

static void ub_async_load_session_command(UbAsyncLoadDeviceState *state,
                                   uint64_t command)
{
    state->last_error = UB_ASYNC_LOAD_ERROR_NONE;
    if (command == 1) {
        if (!state->enabled || state->session_active || !current_cpu ||
            !state->owner_ttbr0 ||
            (!(state->session_flags & UB_ASYNC_LOAD_START_KERNEL_TASK) &&
             !state->upcall_entry) ||
            (state->session_flags & UB_ASYNC_LOAD_START_KERNEL_TASK &&
             state->upcall_entry) ||
            !state->logical_context_count ||
            state->logical_context_count > state->config.context_entries ||
            !ub_async_load_ring_configured(state)) {
            state->last_error = UB_ASYNC_LOAD_ERROR_BUSY;
            return;
        }
        state->home_cpu = current_cpu;
        state->session_active = true;
        state->active_context_id = 0;
        state->upcall_active = false;
        if (!ub_async_load_ring_start(state)) {
            state->session_active = false;
            state->home_cpu = NULL;
            return;
        }
        arm_async_load_set_active(state->home_cpu, true);
    } else if (command == 2) {
        if (state->home_cpu) {
            state->home_cpu->halted = 0;
            arm_async_load_set_active(state->home_cpu, false);
        }
        state->session_active = false;
        state->home_cpu = NULL;
        if (!ub_async_load_reset(state)) {
            state->last_error = UB_ASYNC_LOAD_ERROR_REMOTE;
        }
    } else {
        state->last_error = UB_ASYNC_LOAD_ERROR_INVALID;
    }
}

static void ub_async_load_replay_command(UbAsyncLoadDeviceState *state,
                                  uint64_t command)
{
    bool accepted = false;

    state->last_error = UB_ASYNC_LOAD_ERROR_NONE;
    if (!state->session_active || !current_cpu ||
        state->home_cpu != current_cpu) {
        state->last_error = UB_ASYNC_LOAD_ERROR_CPU;
        goto out;
    }
    if (command == 1) {
        accepted = ub_async_load_cpu_resume(
            current_cpu, state->replay_context_id,
            state->replay_token, state->replay_pc);
    } else if (command == 2) {
        accepted = ub_async_load_cpu_scheduler_enter(current_cpu);
    }
    if (!accepted) {
        state->last_error = command == 1 || command == 2 ?
            UB_ASYNC_LOAD_ERROR_CPU : UB_ASYNC_LOAD_ERROR_INVALID;
        ub_async_load_mark_fail_stop(state->model);
    }

out:
    state->replay_context_id = 0;
    state->replay_token = 0;
    state->replay_pc = 0;
}

bool ub_async_load_device_write(UbAsyncLoadDeviceState *state, hwaddr reg,
                         uint64_t value, unsigned int size)
{
    hwaddr base = reg & ~7ULL;

    if (!state || (size != sizeof(uint32_t) &&
                   size != sizeof(uint64_t))) {
        return false;
    }
    switch (base) {
    case ASYNC_LOAD_REG_MAP_GSVA_BASE:
        state->map_gsva_base = ub_async_load_access_merge(
            state->map_gsva_base, reg, value, size);
        return true;
    case ASYNC_LOAD_REG_MAP_LOCAL_PA:
        state->map_local_pa = ub_async_load_access_merge(
            state->map_local_pa, reg, value, size);
        return true;
    case ASYNC_LOAD_REG_MAP_LENGTH:
        state->map_length = value;
        return true;
    case ASYNC_LOAD_REG_MAP_ID:
        state->map_id = value;
        return true;
    case ASYNC_LOAD_REG_MAP_GENERATION:
        state->map_generation = value;
        return true;
    case ASYNC_LOAD_REG_MAP_MODEL_GENERATION:
        state->map_model_generation = value;
        return true;
    case ASYNC_LOAD_REG_MAP_FLAGS:
        state->map_flags = value;
        return true;
    case ASYNC_LOAD_REG_MAP_COMMAND:
        ub_async_load_map_command(state, value);
        return true;
    case ASYNC_LOAD_REG_SESSION_COMMAND:
        ub_async_load_session_command(state, value);
        return true;
    case ASYNC_LOAD_REG_LOAD_TIMEOUT_NS:
        if (state->session_active) {
            state->last_error = UB_ASYNC_LOAD_ERROR_BUSY;
        } else {
            state->load_timeout_ns = value;
            state->last_error = UB_ASYNC_LOAD_ERROR_NONE;
        }
        return true;
    case ASYNC_LOAD_REG_OWNER_TTBR0:
        if (state->session_active) {
            state->last_error = UB_ASYNC_LOAD_ERROR_BUSY;
        } else {
            state->owner_ttbr0 = ub_async_load_access_merge(
                state->owner_ttbr0, reg, value, size);
            state->last_error = UB_ASYNC_LOAD_ERROR_NONE;
        }
        return true;
    case ASYNC_LOAD_REG_UPCALL_ENTRY:
        if (state->session_active) {
            state->last_error = UB_ASYNC_LOAD_ERROR_BUSY;
        } else {
            state->upcall_entry = ub_async_load_access_merge(
                state->upcall_entry, reg, value, size);
        }
        return true;
    case ASYNC_LOAD_REG_LOGICAL_CONTEXTS:
        if (state->session_active || !value ||
            value > state->config.context_entries) {
            state->last_error = UB_ASYNC_LOAD_ERROR_INVALID;
        } else {
            state->logical_context_count = value;
        }
        return true;
    case ASYNC_LOAD_REG_SESSION_FLAGS:
        if (state->session_active ||
            value & ~(UB_ASYNC_LOAD_START_REPLAY_RETIRE |
                      UB_ASYNC_LOAD_START_KERNEL_TASK) ||
            !(value & UB_ASYNC_LOAD_START_REPLAY_RETIRE)) {
            state->last_error = UB_ASYNC_LOAD_ERROR_INVALID;
        } else {
            state->session_flags = value;
            state->last_error = UB_ASYNC_LOAD_ERROR_NONE;
        }
        return true;
    case ASYNC_LOAD_REG_IRQ_ACK:
        state->irq_status &= ~value;
        if (state->session_active &&
            (state->session_flags & UB_ASYNC_LOAD_START_KERNEL_TASK) &&
            ub_async_load_ring_sync_consumer(state) &&
            state->event_consumer_sequence !=
                state->event_producer_sequence) {
            state->irq_status |= ASYNC_LOAD_IRQ_COMPLETION;
        }
        ubc_async_load_irq_set(state->ubc_dev, state->irq_status != 0);
        return true;
    case ASYNC_LOAD_REG_REPLAY_CONTEXT_ID:
        state->replay_context_id = ub_async_load_access_merge(
            state->replay_context_id, reg, value, size);
        return true;
    case ASYNC_LOAD_REG_REPLAY_TOKEN:
        state->replay_token = ub_async_load_access_merge(
            state->replay_token, reg, value, size);
        return true;
    case ASYNC_LOAD_REG_REPLAY_PC:
        state->replay_pc = ub_async_load_access_merge(
            state->replay_pc, reg, value, size);
        return true;
    case ASYNC_LOAD_REG_REPLAY_COMMAND:
        ub_async_load_replay_command(state, value);
        return true;
    case ASYNC_LOAD_REG_EVENT_RING_BASE:
        if (state->session_active) {
            state->last_error = UB_ASYNC_LOAD_ERROR_BUSY;
        } else {
            state->event_ring_base = ub_async_load_access_merge(
                state->event_ring_base, reg, value, size);
            state->last_error = UB_ASYNC_LOAD_ERROR_NONE;
        }
        return true;
    case ASYNC_LOAD_REG_EVENT_RING_BYTES:
        if (state->session_active) {
            state->last_error = UB_ASYNC_LOAD_ERROR_BUSY;
        } else {
            state->event_ring_bytes = value;
            state->last_error = UB_ASYNC_LOAD_ERROR_NONE;
        }
        return true;
    case ASYNC_LOAD_REG_EVENT_CONSUMER_BASE:
        if (state->session_active) {
            state->last_error = UB_ASYNC_LOAD_ERROR_BUSY;
        } else {
            state->event_consumer_base = ub_async_load_access_merge(
                state->event_consumer_base, reg, value, size);
            state->last_error = UB_ASYNC_LOAD_ERROR_NONE;
        }
        return true;
    case ASYNC_LOAD_REG_EVENT_CONSUMER_BYTES:
        if (state->session_active) {
            state->last_error = UB_ASYNC_LOAD_ERROR_BUSY;
        } else {
            state->event_consumer_bytes = value;
            state->last_error = UB_ASYNC_LOAD_ERROR_NONE;
        }
        return true;
    default:
        return false;
    }
}

bool ub_async_load_cpu_enabled(CPUState *cpu)
{
    return ub_async_load_global && ub_async_load_global->session_active &&
        ub_async_load_global->home_cpu == cpu;
}

bool ub_async_load_cpu_owner_matches(CPUState *cpu, uint64_t ttbr0_el1)
{
    return ub_async_load_cpu_enabled(cpu) &&
        ub_async_load_global->owner_ttbr0 == ttbr0_el1;
}

uint64_t ub_async_load_cpu_cycle(CPUState *cpu)
{
    UbAsyncLoadDeviceState *state = ub_async_load_global;

    if (!state || state->home_cpu != cpu) {
        return 0;
    }
    return ub_async_load_ns_to_cycles(
        state, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

static UbAsyncLoadMap *ub_async_load_find_map(UbAsyncLoadDeviceState *state,
                                 uint64_t va, uint8_t bytes,
                                 uint64_t *remote_offset,
                                 uint64_t *map_id)
{
    uint16_t slot;

    for (slot = 0; slot < G_N_ELEMENTS(state->maps); slot++) {
        UbAsyncLoadMap *map = &state->maps[slot];

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

bool ub_async_load_cpu_address_is_remote(CPUState *cpu, uint64_t va,
                                  uint8_t bytes)
{
    UbAsyncLoadDeviceState *state = ub_async_load_global;
    uint64_t remote_offset;
    uint64_t map_id;

    return state && state->session_active && state->home_cpu == cpu &&
        ub_async_load_find_map(state, va, bytes, &remote_offset, &map_id);
}

bool ub_async_load_cpu_replay_expected(CPUState *cpu)
{
    UbAsyncLoadDeviceState *state = ub_async_load_global;

    return state && state->session_active && state->home_cpu == cpu &&
        !state->upcall_active &&
        state->session_flags & UB_ASYNC_LOAD_START_REPLAY_RETIRE &&
        ub_async_load_replay_expected(state->model,
                                 state->active_context_id);
}

bool ub_async_load_cpu_kernel_task_mode(CPUState *cpu)
{
    UbAsyncLoadDeviceState *state = ub_async_load_global;

    return state && state->session_active && state->home_cpu == cpu &&
        state->session_flags & UB_ASYNC_LOAD_START_KERNEL_TASK;
}

bool ub_async_load_cpu_select_kernel_context(CPUState *cpu,
                                       uint64_t context_cookie)
{
    UbAsyncLoadDeviceState *state = ub_async_load_global;
    uint16_t slot;

    if (!ub_async_load_cpu_kernel_task_mode(cpu) ||
        cpu->cpu_index < 0 || cpu->cpu_index > UINT16_MAX) {
        return false;
    }
    for (slot = 0; slot < state->logical_context_count; slot++) {
        if (state->context_cookie_used[slot] &&
            state->context_cookies[slot] == context_cookie) {
            state->active_context_id = ub_async_load_context_id_make(
                state->owner_generation, (uint16_t)cpu->cpu_index, slot);
            return true;
        }
    }
    return false;
}

bool ub_async_load_cpu_prepare_kernel_context(CPUState *cpu,
                                        uint64_t context_cookie)
{
    UbAsyncLoadDeviceState *state = ub_async_load_global;
    uint16_t slot;

    if (ub_async_load_cpu_select_kernel_context(cpu, context_cookie)) {
        return true;
    }
    if (!ub_async_load_cpu_kernel_task_mode(cpu) ||
        cpu->cpu_index < 0 || cpu->cpu_index > UINT16_MAX) {
        return false;
    }
    for (slot = 0; slot < state->logical_context_count; slot++) {
        if (!state->context_cookie_used[slot]) {
            state->context_cookie_used[slot] = true;
            state->context_cookies[slot] = context_cookie;
            state->active_context_id = ub_async_load_context_id_make(
                state->owner_generation, (uint16_t)cpu->cpu_index, slot);
            return true;
        }
    }
    return false;
}

bool ub_async_load_cpu_take_kernel_fault(CPUState *cpu,
                                   uint64_t interrupted_pc)
{
    UbAsyncLoadDeviceState *state = ub_async_load_global;
    bool completion_ready;

    if (ub_async_load_cpu_kernel_task_mode(cpu) &&
        ubc_void_response_policy_enabled(state->ubc_dev)) {
        return true;
    }
    if (!ub_async_load_cpu_kernel_task_mode(cpu) ||
        !ub_async_load_ring_publish_one(state, interrupted_pc)) {
        return false;
    }
    completion_ready = ub_async_load_event_pending(state->model);
    if (completion_ready && !ub_async_load_ring_publish_one(state, 0)) {
        return false;
    }
    if (completion_ready) {
        state->irq_status |= ASYNC_LOAD_IRQ_COMPLETION;
        ubc_async_load_irq_set(state->ubc_dev, true);
    }
    return true;
}

bool ub_async_load_cpu_take_upcall(CPUState *cpu, uint64_t interrupted_pc,
                            uint64_t *upcall_entry)
{
    UbAsyncLoadDeviceState *state = ub_async_load_global;

    if (!state || !upcall_entry || !state->session_active ||
        state->session_flags & UB_ASYNC_LOAD_START_KERNEL_TASK ||
        state->home_cpu != cpu || state->upcall_active ||
        !ub_async_load_ring_publish_one(state, interrupted_pc)) {
        return false;
    }
    state->upcall_active = true;
    ub_async_load_record_direct_upcall(state->model);
    *upcall_entry = state->upcall_entry;
    return true;
}

bool ub_async_load_cpu_resume(CPUState *cpu, uint64_t context_id,
                         uint64_t replay_token, uint64_t replay_pc)
{
    UbAsyncLoadDeviceState *state = ub_async_load_global;
    UbAsyncLoadPltToken token;
    uint16_t slot;

    if (!state || !state->session_active || state->home_cpu != cpu ||
        !context_id ||
        !ub_async_load_ring_sync_consumer(state) ||
        state->event_consumer_sequence != state->event_producer_sequence) {
        return false;
    }
    slot = ub_async_load_context_id_slot(context_id);
    if (cpu->cpu_index < 0 || cpu->cpu_index > UINT16_MAX ||
        ub_async_load_context_id_generation(context_id) !=
            state->owner_generation ||
        ub_async_load_context_id_home_core(context_id) !=
            (uint16_t)cpu->cpu_index ||
        slot >= state->logical_context_count) {
        return false;
    }
    if (state->replay_armed) {
        return false;
    }
    if (replay_token) {
        token = ub_async_load_plt_token_unpack(replay_token);
        if (!replay_pc || !ub_async_load_replay_arm(
                state->model, context_id, token, replay_pc)) {
            return false;
        }
        state->armed_replay_token = token;
        state->replay_armed = true;
    } else if (replay_pc ||
               ub_async_load_replay_expected(state->model, context_id)) {
        return false;
    }
    state->active_context_id = context_id;
    state->upcall_active = false;
    cpu->halted = 0;
    return true;
}

bool ub_async_load_cpu_scheduler_enter(CPUState *cpu)
{
    UbAsyncLoadDeviceState *state = ub_async_load_global;

    if (!state || !state->session_active || state->home_cpu != cpu ||
        state->upcall_active ||
        !state->active_context_id ||
        !ub_async_load_ring_sync_consumer(state) ||
        state->event_consumer_sequence != state->event_producer_sequence) {
        return false;
    }
    state->active_context_id = 0;
    state->upcall_active = true;
    state->scheduler_enters++;
    if (ub_async_load_event_pending(state->model) &&
        !ub_async_load_ring_publish_one(state, 0)) {
        return false;
    }
    return true;
}

void ub_async_load_cpu_fail_stop(CPUState *cpu)
{
    UbAsyncLoadDeviceState *state = ub_async_load_global;

    if (state && state->home_cpu == cpu) {
        state->last_error = UB_ASYNC_LOAD_ERROR_CPU;
        ub_async_load_mark_fail_stop(state->model);
        state->session_active = false;
    }
}

static bool ub_async_load_sink_validate(void *opaque, uint64_t sink_id,
                                 uint64_t sink_generation)
{
    UbAsyncLoadFuture *future = opaque;
    UbAsyncLoadPltToken backend_key =
        ub_async_load_future_backend_key(future);

    return future->active && future->state->session_active &&
        ub_async_load_plt_token_pack(backend_key) == sink_id &&
        backend_key.generation == sink_generation;
}

static void ub_async_load_complete_plt(UbAsyncLoadFuture *future,
                                ObmmRemoteStatus backend_status,
                                const void *payload, uint8_t bytes_done,
                                uint64_t publish_ns)
{
    UbAsyncLoadDeviceState *state = future->state;
    UbAsyncLoadStatus status =
        ub_async_load_status_from_backend(backend_status);
    UbAsyncLoadCompletionResult completion;
    uint64_t cycle = ub_async_load_ns_to_cycles(state, publish_ns);

    completion = ub_async_load_load_complete(
        state->model, future->plt_token, status, payload,
        bytes_done, cycle);
    if (completion != UB_ASYNC_LOAD_COMPLETION_ACCEPTED) {
        trace_async_load_stale_completion(
            state->owner_generation, future->plt_token.generation,
            future->plt_token.slot, cycle);
    } else if (status == UB_ASYNC_LOAD_STATUS_SUCCESS) {
        trace_async_load_load_complete(
            state->owner_generation, future->context_id,
            future->plt_token.generation, future->plt_token.slot,
            status, cycle);
    } else {
        trace_async_load_load_fault(
            state->owner_generation, future->context_id,
            future->plt_token.generation, future->plt_token.slot,
            status, cycle);
    }
}

static void ub_async_load_complete_cacheable(
    UbAsyncLoadFuture *future, ObmmRemoteStatus backend_status,
    const void *payload, uint32_t bytes_done, uint64_t publish_ns)
{
    UbAsyncLoadDeviceState *state = future->state;
    UbAsyncLoadStatus status =
        ub_async_load_status_from_backend(backend_status);
    uint64_t cycle = ub_async_load_ns_to_cycles(state, publish_ns);

    if (status == UB_ASYNC_LOAD_STATUS_SUCCESS &&
        (bytes_done != future->fill_bytes ||
         !ubc_obmm_cacheable_fill_complete(
             state->ubc_dev,
             &state->maps[future->load.map_id - 1].resolved,
             future->fill_remote_offset, payload, bytes_done))) {
        status = UB_ASYNC_LOAD_STATUS_INTERNAL;
    }
    if (status == UB_ASYNC_LOAD_STATUS_SUCCESS) {
        ub_async_load_record_cacheable_fill_bytes(state->model, bytes_done);
        qemu_log("ASYNC_LOAD_CACHEABLE_FILL context=%#" PRIx64
                 " wait_key=%#" PRIx64 " va=%#" PRIx64
                 " fill_offset=%#" PRIx64 " fill_bytes=%u"
                 " nc_plt_pending=%u\n",
                 future->context_id,
                 ub_async_load_plt_token_pack(future->wait_key),
                 future->load.effective_va, future->fill_remote_offset,
                 bytes_done, ub_async_load_pending_count(state->model));
    }
    ub_async_load_cacheable_complete(
        state->model, future->context_id, &future->load, future->wait_key,
        status, cycle);
}

static void ub_async_load_complete_future(
    UbAsyncLoadFuture *future, ObmmRemoteStatus backend_status,
    const void *payload, uint32_t bytes_done, uint64_t publish_ns)
{
    if (future->kind == UB_ASYNC_LOAD_FUTURE_CACHEABLE) {
        ub_async_load_complete_cacheable(
            future, backend_status, payload, bytes_done, publish_ns);
    } else {
        ub_async_load_complete_plt(
            future, backend_status, payload, bytes_done, publish_ns);
    }
}

static void ub_async_load_remote_complete(void *opaque,
                                          const ObmmRemoteResult *result)
{
    UbAsyncLoadFuture *future = opaque;
    UbAsyncLoadDeviceState *state = future->state;

    if (!future->active) {
        return;
    }
    if (result->status == OBMM_REMOTE_STATUS_VOIDED) {
        ub_async_load_future_release(future);
        return;
    }
    ub_async_load_complete_future(
        future, result->status, result->payload,
        result->bytes_done, result->model_publish_ns);
    ub_async_load_future_release(future);
    if (ub_async_load_event_pending(state->model)) {
        if (ub_async_load_ring_publish_one(state, 0)) {
            state->irq_status |= ASYNC_LOAD_IRQ_COMPLETION;
            ubc_async_load_irq_set(state->ubc_dev, true);
            state->event_wait_wakeups++;
            ub_async_load_ring_update_u64(
                state,
                offsetof(UbAsyncLoadEventProducerV3, wait_wakeups),
                state->event_wait_wakeups);
        } else {
            ub_async_load_ring_fail_stop(
                state, UB_ASYNC_LOAD_ERROR_REMOTE);
        }
    }
    if (state->home_cpu) {
        state->home_cpu->halted = 0;
        cpu_exit(state->home_cpu);
        qemu_cpu_kick(state->home_cpu);
    }
}

static bool ub_async_load_inline_value(const UbAsyncLoadDesc *load,
                                       const void *payload,
                                       uint32_t bytes_done,
                                       uint64_t *value)
{
    const uint8_t *bytes = payload;
    uint64_t result = 0;
    uint8_t index;

    if (!load || !payload || !value || bytes_done != load->access_bytes ||
        (bytes_done != 1 && bytes_done != 2 && bytes_done != 4 &&
         bytes_done != 8)) {
        return false;
    }
    if (load->big_endian) {
        for (index = 0; index < bytes_done; index++) {
            result = result << 8 | bytes[index];
        }
    } else {
        for (index = 0; index < bytes_done; index++) {
            result |= (uint64_t)bytes[index] << (index * 8);
        }
    }
    *value = result;
    return true;
}

UbAsyncLoadTryResult ub_async_load_cpu_remote_load(
    CPUState *cpu, const UbAsyncLoadDesc *load, uint64_t *replay_value)
{
    UbAsyncLoadDeviceState *state = ub_async_load_global;
    UbAsyncLoadDesc resolved_load;
    UbAsyncLoadPltToken plt_token;
    UbAsyncLoadPendingResult pending;
    ObmmRemoteRequest request;
    ObmmRemoteResult inline_result;
    ObmmRemoteToken backend_token;
    ObmmRemoteStatus status;
    ObmmSubmitDisposition disposition;
    UbAsyncLoadFuture *future;
    UbAsyncLoadMap *map;
    uint64_t remote_offset;
    uint64_t map_id;
    uint64_t operation_ordinal;
    uint64_t now_ns;
    uint64_t request_remote_offset;
    uint32_t request_bytes;
    uint16_t context_slot;
    bool predicated_void;
    int cache_lookup;
    UbAsyncLoadReplayResult replay;

    if (!state || !load || !replay_value || !state->session_active ||
        state->home_cpu != cpu || !state->active_context_id ||
        state->upcall_active) {
        return UB_ASYNC_LOAD_TRY_NOT_REMOTE;
    }
    map = ub_async_load_find_map(state, load->effective_va,
                          load->access_bytes, &remote_offset, &map_id);
    if (!map) {
        return ub_async_load_cpu_replay_expected(cpu) ?
            UB_ASYNC_LOAD_TRY_FAIL_STOP : UB_ASYNC_LOAD_TRY_NOT_REMOTE;
    }
    resolved_load = *load;
    resolved_load.map_id = map_id;
    resolved_load.map_generation = map->generation;
    resolved_load.map_model_generation = map->model_generation;
    resolved_load.remote_offset = remote_offset;
    if (resolved_load.normal_cacheable) {
        cache_lookup = ubc_obmm_cacheable_fill_lookup(
            state->ubc_dev, &map->resolved, remote_offset,
            resolved_load.access_bytes, &request_remote_offset,
            &request_bytes);
        if (cache_lookup > 0) {
            ub_async_load_record_cacheable_replay_hit(state->model);
            qemu_log("ASYNC_LOAD_CACHEABLE_REPLAY_HIT context=%#" PRIx64
                     " pc=%#" PRIx64 " va=%#" PRIx64
                     " nc_plt_pending=%u\n",
                     state->active_context_id, resolved_load.fault_pc,
                     resolved_load.effective_va,
                     ub_async_load_pending_count(state->model));
            return UB_ASYNC_LOAD_TRY_NOT_REMOTE;
        }
        if (cache_lookup < 0 || state->replay_armed) {
            ub_async_load_mark_fail_stop(state->model);
            return UB_ASYNC_LOAD_TRY_FAIL_STOP;
        }
    } else if (state->replay_armed) {
        replay = ub_async_load_replay_consume_token(
            state->model, state->armed_replay_token,
            &resolved_load, replay_value);
        state->replay_armed = false;
        memset(&state->armed_replay_token, 0,
               sizeof(state->armed_replay_token));
        if (replay == UB_ASYNC_LOAD_REPLAY_CONSUMED) {
            trace_async_load_load_replay(
                state->owner_generation, state->active_context_id,
                load->fault_pc, load->effective_va, *replay_value);
            return UB_ASYNC_LOAD_TRY_REPLAYED;
        }
        if (replay == UB_ASYNC_LOAD_REPLAY_MISMATCH) {
            return UB_ASYNC_LOAD_TRY_FAIL_STOP;
        }
        return UB_ASYNC_LOAD_TRY_FAIL_STOP;
    }
    if (!resolved_load.normal_cacheable && ub_async_load_replay_expected(
            state->model, state->active_context_id)) {
        ub_async_load_mark_fail_stop(state->model);
        return UB_ASYNC_LOAD_TRY_FAIL_STOP;
    }
    context_slot = ub_async_load_context_id_slot(state->active_context_id);
    if (context_slot >= state->logical_context_count ||
        !ub_async_load_logical_remote_ordinal(
            context_slot, state->logical_context_count,
            state->context_next_ordinal[context_slot],
            &operation_ordinal)) {
        return UB_ASYNC_LOAD_TRY_FAIL_STOP;
    }
    now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (state->load_timeout_ns) {
        uint64_t deadline_ns = now_ns >
            UINT64_MAX - state->load_timeout_ns ? UINT64_MAX :
            now_ns + state->load_timeout_ns;

        resolved_load.deadline_cycle = ub_async_load_ns_to_cycles(
            state, deadline_ns);
    }
    future = ub_async_load_future_alloc(state);
    if (!future) {
        trace_async_load_capacity_stall(
            state->owner_generation, state->active_context_id,
            load->submit_cycle);
        return UB_ASYNC_LOAD_TRY_SYNC_STALL;
    }
    future->context_id = state->active_context_id;
    future->load = resolved_load;
    predicated_void = ubc_void_response_policy_enabled(state->ubc_dev) &&
        ub_async_load_cpu_kernel_task_mode(cpu);
    if (predicated_void) {
        if (resolved_load.normal_cacheable) {
            future->kind = UB_ASYNC_LOAD_FUTURE_CACHEABLE;
            future->fill_remote_offset = request_remote_offset;
            future->fill_bytes = request_bytes;
        } else {
            future->kind = UB_ASYNC_LOAD_FUTURE_NC;
            request_remote_offset = remote_offset;
            request_bytes = load->access_bytes;
        }
    } else if (resolved_load.normal_cacheable) {
        future->kind = UB_ASYNC_LOAD_FUTURE_CACHEABLE;
        future->wait_key = ub_async_load_future_cacheable_wait_key(future);
        future->fill_remote_offset = request_remote_offset;
        future->fill_bytes = request_bytes;
        pending = ub_async_load_cacheable_pending(
            state->model, future->context_id, &resolved_load,
            future->wait_key);
    } else {
        future->kind = UB_ASYNC_LOAD_FUTURE_NC;
        pending = ub_async_load_load_pending(
            state->model, future->context_id, &resolved_load, &plt_token);
        future->plt_token = plt_token;
        future->wait_key = plt_token;
        request_remote_offset = remote_offset;
        request_bytes = load->access_bytes;
    }
    if (pending == UB_ASYNC_LOAD_PENDING_SYNC_STALL) {
        ub_async_load_future_release(future);
        trace_async_load_capacity_stall(
            state->owner_generation, state->active_context_id,
            load->submit_cycle);
        return UB_ASYNC_LOAD_TRY_SYNC_STALL;
    }
    if (pending != UB_ASYNC_LOAD_PENDING_ACCEPTED) {
        ub_async_load_future_release(future);
        return UB_ASYNC_LOAD_TRY_FAIL_STOP;
    }
    state->context_next_ordinal[context_slot]++;
    if (!predicated_void && resolved_load.normal_cacheable) {
        qemu_log("ASYNC_LOAD_CACHEABLE_PENDING context=%#" PRIx64
                 " wait_key=%#" PRIx64 " pc=%#" PRIx64
                 " va=%#" PRIx64 " fill_offset=%#" PRIx64
                 " fill_bytes=%u nc_plt_pending=%u\n",
                 future->context_id,
                 ub_async_load_plt_token_pack(future->wait_key),
                 resolved_load.fault_pc, resolved_load.effective_va,
                 request_remote_offset, request_bytes,
                 ub_async_load_pending_count(state->model));
    } else if (!predicated_void) {
        trace_async_load_load_pending(
            state->owner_generation, state->active_context_id,
            plt_token.generation, plt_token.slot, load->submit_cycle);
    }
    request = (ObmmRemoteRequest) {
        .map_id = map_id,
        .map_generation = map->generation,
        .remote_offset = request_remote_offset,
        .length = request_bytes,
        .deadline_model_ns = !predicated_void && state->load_timeout_ns ?
            (now_ns > UINT64_MAX - state->load_timeout_ns ?
             UINT64_MAX : now_ns + state->load_timeout_ns) : 0,
        .operation_ordinal = operation_ordinal,
        .operation_key = ub_obmm_remote_operation_key(
            &state->ubc_dev->remote_memory_model.config,
            &(UbObmmRemoteOperation) {
                .map_id = map->resolved.map_id,
                .map_generation = map->resolved.map_generation,
                .remote_offset = request_remote_offset,
                .length = request_bytes,
                .per_range_ordinal = operation_ordinal,
            }),
        .sink = {
            .kind = OBMM_REMOTE_SINK_ASYNC_LOAD,
            .sink_id = ub_async_load_plt_token_pack(
                ub_async_load_future_backend_key(future)),
            .sink_generation = future->generation,
            .owner_id = UB_ASYNC_LOAD_BACKEND_OWNER_ID,
            .validate = ub_async_load_sink_validate,
            .complete = ub_async_load_remote_complete,
            .adapter_state = future,
        },
    };
    disposition = obmm_remote_submit(
        state->backend, &request, now_ns, &backend_token,
        &inline_result, &status);
    if (disposition == OBMM_SUBMIT_REJECTED) {
        if (!predicated_void) {
            ub_async_load_complete_future(future, status, NULL, 0, now_ns);
        }
        ub_async_load_future_release(future);
        if (predicated_void) {
            return UB_ASYNC_LOAD_TRY_FAIL_STOP;
        }
    } else if (disposition == OBMM_SUBMIT_INLINE) {
        if (predicated_void && resolved_load.normal_cacheable) {
            bool filled = inline_result.status == OBMM_REMOTE_STATUS_SUCCESS &&
                inline_result.bytes_done == future->fill_bytes &&
                ubc_obmm_cacheable_fill_complete(
                    state->ubc_dev,
                    &state->maps[future->load.map_id - 1].resolved,
                    future->fill_remote_offset, inline_result.payload,
                    inline_result.bytes_done);

            if (filled) {
                ub_async_load_record_cacheable_fill_bytes(
                    state->model, inline_result.bytes_done);
            }
            ub_async_load_future_release(future);
            return filled ? UB_ASYNC_LOAD_TRY_NOT_REMOTE :
                UB_ASYNC_LOAD_TRY_FAIL_STOP;
        }
        if (predicated_void) {
            bool valid = inline_result.status == OBMM_REMOTE_STATUS_SUCCESS &&
                ub_async_load_inline_value(
                    &resolved_load, inline_result.payload,
                    inline_result.bytes_done, replay_value);

            ub_async_load_future_release(future);
            return valid ? UB_ASYNC_LOAD_TRY_REPLAYED :
                UB_ASYNC_LOAD_TRY_FAIL_STOP;
        }
        if (!resolved_load.normal_cacheable) {
            trace_async_load_load_inline(
                state->owner_generation, state->active_context_id,
                plt_token.generation, plt_token.slot, load->submit_cycle);
        }
        ub_async_load_complete_future(
            future, inline_result.status, inline_result.payload,
            inline_result.bytes_done, now_ns);
        ub_async_load_future_release(future);
    } else {
        future->backend_token = backend_token;
    }
    ub_async_load_arm_deadline(state);
    if (predicated_void) {
        return disposition == OBMM_SUBMIT_PENDING ?
            UB_ASYNC_LOAD_TRY_PENDING : UB_ASYNC_LOAD_TRY_FAIL_STOP;
    }
    return ub_async_load_fail_stop(state->model) ?
        UB_ASYNC_LOAD_TRY_FAIL_STOP : UB_ASYNC_LOAD_TRY_PENDING;
}

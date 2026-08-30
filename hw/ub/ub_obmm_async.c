/*
 * OBMM asynchronous queue endpoint for the P2A software path.
 */

#include "qemu/osdep.h"
#include "hw/ub/ub_obmm_async.h"
#include "hw/ub/linqu_shmem_pto_abi.h"
#include "qemu/log.h"
#include "hw/ub/ub_obmm_remote.h"
#include "hw/ub/ub_ubc.h"
#include "qemu/atomic.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "sysemu/dma.h"

#define OBMM_ASYNC_REG_VERSION_CAPS 0x000
#define OBMM_ASYNC_REG_STATUS 0x008
#define OBMM_ASYNC_REG_SQ_BASE 0x010
#define OBMM_ASYNC_REG_SQ_SIZE 0x018
#define OBMM_ASYNC_REG_SQ_HEAD 0x020
#define OBMM_ASYNC_REG_SQ_TAIL 0x028
#define OBMM_ASYNC_REG_CQ_BASE 0x030
#define OBMM_ASYNC_REG_CQ_SIZE 0x038
#define OBMM_ASYNC_REG_CQ_HEAD 0x040
#define OBMM_ASYNC_REG_CQ_TAIL 0x048
#define OBMM_ASYNC_REG_DOORBELL 0x050
#define OBMM_ASYNC_REG_IRQ_STATUS 0x058
#define OBMM_ASYNC_REG_IRQ_ACK 0x060
#define OBMM_ASYNC_REG_LAST_ERROR 0x068
#define OBMM_ASYNC_REG_QUEUE_ID 0x070
#define OBMM_ASYNC_REG_MAP_LOCAL_PA 0x100
#define OBMM_ASYNC_REG_MAP_LENGTH 0x108
#define OBMM_ASYNC_REG_MAP_ID 0x110
#define OBMM_ASYNC_REG_MAP_GENERATION 0x118
#define OBMM_ASYNC_REG_MAP_CMD 0x120
#define OBMM_ASYNC_REG_BUFFER_BASE 0x128
#define OBMM_ASYNC_REG_BUFFER_LENGTH 0x130
#define OBMM_ASYNC_REG_BUFFER_ID 0x138
#define OBMM_ASYNC_REG_BUFFER_GENERATION 0x140
#define OBMM_ASYNC_REG_BUFFER_CMD 0x148
#define OBMM_ASYNC_REG_CANCEL_TOKEN 0x150
#define OBMM_ASYNC_REG_CANCEL_CMD 0x158
#define OBMM_ASYNC_REG_GUEST_MONOTONIC_NS 0x160
#define OBMM_ASYNC_REG_OBSERVABILITY_RESET 0x1f8
#define OBMM_ASYNC_REG_OBSERVABILITY_BASE 0x200

#define OBMM_ASYNC_STATUS_ENABLED 1
#define OBMM_ASYNC_IRQ_COMPLETION 1
#define OBMM_ASYNC_IRQ_ERROR 2
#define OBMM_ASYNC_OPCODE_READ 1
#define OBMM_ASYNC_OPCODE_READ_COMPLETE 0x81
#define OBMM_ASYNC_BACKEND_OWNER_ID 1
#define OBMM_ASYNC_CHILD_CHUNK_BYTES 4079

typedef enum UbObmmAsyncStatus {
    UB_OBMM_ASYNC_OK,
    UB_OBMM_ASYNC_INVALID,
    UB_OBMM_ASYNC_NO_MAP,
    UB_OBMM_ASYNC_BOUNDS,
    UB_OBMM_ASYNC_PERMISSION,
    UB_OBMM_ASYNC_STALE,
    UB_OBMM_ASYNC_RETIRED,
    UB_OBMM_ASYNC_TIMEOUT,
    UB_OBMM_ASYNC_REMOTE_IO,
    UB_OBMM_ASYNC_CHECKSUM,
    UB_OBMM_ASYNC_CANCELLED,
    UB_OBMM_ASYNC_UNSUPPORTED,
} UbObmmAsyncStatus;

typedef struct UbObmmAsyncMap {
    bool active;
    uint64_t generation;
    uint64_t length;
    uint64_t next_ordinal;
    UbcObmmResolvedMap resolved;
} UbObmmAsyncMap;

typedef struct UbObmmAsyncBuffer {
    bool active;
    uint32_t generation;
    uint64_t base;
    uint64_t length;
} UbObmmAsyncBuffer;

typedef struct UbObmmAsyncFuture {
    bool active;
    bool terminal;
    uint64_t public_token;
    uint32_t generation;
    uint32_t buffer_id;
    uint32_t buffer_generation;
    uint32_t dst_offset;
    uint32_t length;
    uint64_t map_id;
    uint64_t map_generation;
    uint64_t user_data;
    ObmmRemoteToken backend_token;
    UbObmmAsyncCqEntryV1 cqe;
} UbObmmAsyncFuture;

typedef struct UbObmmAsyncParsedSq {
    uint16_t abi_version;
    uint8_t opcode;
    uint8_t flags;
    uint32_t length;
    uint64_t token;
    uint64_t map_id;
    uint64_t map_generation;
    uint64_t remote_offset;
    uint32_t dst_buffer_id;
    uint32_t dst_offset;
    uint64_t deadline_ns;
    uint64_t user_data;
} UbObmmAsyncParsedSq;

struct UbObmmAsyncState {
    BusControllerDev *ubc_dev;
    ObmmRemoteBackend *backend;
    QEMUBH *bh;
    QEMUTimer *deadline_timer;
    bool enabled;
    bool bh_pending;
    uint32_t queue_id;
    uint64_t sq_base;
    uint64_t sq_depth;
    uint64_t sq_head;
    uint64_t sq_tail;
    uint64_t cq_base;
    uint64_t cq_depth;
    uint64_t cq_head;
    uint64_t cq_tail;
    uint64_t irq_status;
    uint64_t last_error;
    uint64_t map_local_pa;
    uint64_t map_length;
    uint64_t map_id;
    uint64_t map_generation;
    uint64_t buffer_base;
    uint64_t buffer_length;
    uint64_t buffer_id;
    uint64_t buffer_generation;
    uint64_t cancel_token;
    uint64_t guest_monotonic_ns;
    UbObmmAsyncMap maps[UB_OBMM_ASYNC_QUEUE_DEPTH];
    UbObmmAsyncBuffer buffers[UB_OBMM_ASYNC_QUEUE_DEPTH];
    UbObmmAsyncFuture futures[UB_OBMM_ASYNC_QUEUE_DEPTH];
};

QEMU_BUILD_BUG_ON(sizeof(UbObmmAsyncSqEntryV1) !=
                  UB_OBMM_ASYNC_SLOT_BYTES);
QEMU_BUILD_BUG_ON(sizeof(UbObmmAsyncCqEntryV1) !=
                  UB_OBMM_ASYNC_SLOT_BYTES);

static uint64_t ub_obmm_async_token_generation(uint64_t token)
{
    return token >> 32;
}

static uint16_t ub_obmm_async_token_queue(uint64_t token)
{
    return token >> 16;
}

static uint16_t ub_obmm_async_token_slot(uint64_t token)
{
    return token;
}

static uint64_t ub_obmm_async_access_extract(uint64_t value, hwaddr reg,
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

static uint64_t ub_obmm_async_access_merge(uint64_t current, hwaddr reg,
                                           uint64_t value,
                                           unsigned int size)
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

static UbObmmAsyncStatus ub_obmm_async_status_from_backend(
    ObmmRemoteStatus status)
{
    switch (status) {
    case OBMM_REMOTE_STATUS_SUCCESS:
        return UB_OBMM_ASYNC_OK;
    case OBMM_REMOTE_STATUS_BOUNDS:
        return UB_OBMM_ASYNC_BOUNDS;
    case OBMM_REMOTE_STATUS_PERMISSION:
    case OBMM_REMOTE_STATUS_TOKEN_DENIED:
    case OBMM_REMOTE_STATUS_COHERENCE:
        return UB_OBMM_ASYNC_PERMISSION;
    case OBMM_REMOTE_STATUS_STALE_MAP:
        return UB_OBMM_ASYNC_NO_MAP;
    case OBMM_REMOTE_STATUS_STALE_SINK:
        return UB_OBMM_ASYNC_STALE;
    case OBMM_REMOTE_STATUS_RETIRED:
        return UB_OBMM_ASYNC_RETIRED;
    case OBMM_REMOTE_STATUS_TIMEOUT:
        return UB_OBMM_ASYNC_TIMEOUT;
    case OBMM_REMOTE_STATUS_REMOTE_IO:
    case OBMM_REMOTE_STATUS_CAPACITY:
    case OBMM_REMOTE_STATUS_INTERNAL:
        return UB_OBMM_ASYNC_REMOTE_IO;
    case OBMM_REMOTE_STATUS_CHECKSUM:
        return UB_OBMM_ASYNC_CHECKSUM;
    case OBMM_REMOTE_STATUS_CANCELLED:
        return UB_OBMM_ASYNC_CANCELLED;
    case OBMM_REMOTE_STATUS_UNSUPPORTED:
        return UB_OBMM_ASYNC_UNSUPPORTED;
    default:
        return UB_OBMM_ASYNC_INVALID;
    }
}

static void ub_obmm_async_decode_sq(const UbObmmAsyncSqEntryV1 *wire,
                                    UbObmmAsyncParsedSq *entry)
{
    const uint8_t *bytes = (const uint8_t *)wire;

    *entry = (UbObmmAsyncParsedSq) {
        .abi_version = lduw_le_p(bytes),
        .opcode = bytes[2],
        .flags = bytes[3],
        .length = ldl_le_p(bytes + 4),
        .token = ldq_le_p(bytes + 8),
        .map_id = ldq_le_p(bytes + 16),
        .map_generation = ldq_le_p(bytes + 24),
        .remote_offset = ldq_le_p(bytes + 32),
        .dst_buffer_id = ldl_le_p(bytes + 40),
        .dst_offset = ldl_le_p(bytes + 44),
        .deadline_ns = ldq_le_p(bytes + 48),
        .user_data = ldq_le_p(bytes + 56),
    };
}

static void ub_obmm_async_encode_cqe(const UbObmmAsyncCqEntryV1 *entry,
                                     uint8_t bytes[UB_OBMM_ASYNC_SLOT_BYTES])
{
    memset(bytes, 0, UB_OBMM_ASYNC_SLOT_BYTES);
    stw_le_p(bytes, entry->abi_version);
    bytes[2] = entry->opcode;
    bytes[3] = entry->flags;
    stl_le_p(bytes + 4, entry->status);
    stq_le_p(bytes + 8, entry->token);
    stq_le_p(bytes + 16, entry->user_data);
    stl_le_p(bytes + 24, entry->bytes_done);
    stl_le_p(bytes + 28, entry->provider_status);
    stq_le_p(bytes + 32, entry->checksum64);
    stq_le_p(bytes + 40, entry->completed_ns);
    stq_le_p(bytes + 48, entry->map_generation);
    stq_le_p(bytes + 56, 0);
}

static bool ub_obmm_async_map_valid(void *opaque, uint64_t map_id,
                                    uint64_t map_generation)
{
    UbObmmAsyncState *state = opaque;
    UbObmmAsyncMap *map;

    if (map_id == 0 || map_id > UB_OBMM_ASYNC_QUEUE_DEPTH) {
        return false;
    }
    map = &state->maps[map_id - 1];
    return map->active && map->generation == map_generation;
}

bool ub_obmm_async_resolve_mapping_ref(UbObmmAsyncState *state,
                                       uint64_t mapping_ref,
                                       uint64_t local_pa,
                                       uint64_t length,
                                       UbcObmmResolvedMap *resolved)
{
    UbcObmmResolvedMap current;
    UbObmmAsyncMap *map;
    uint64_t map_generation;
    uint64_t map_id;
    uint64_t offset;

    if (!state || !resolved || mapping_ref == 0 || length == 0 ||
        local_pa > UINT64_MAX - length) {
        return false;
    }
    map_id = lingqu_pto_obmm_mapping_ref_map_id(mapping_ref);
    map_generation =
        lingqu_pto_obmm_mapping_ref_generation(mapping_ref);
    if (lingqu_pto_obmm_mapping_ref_encode(map_id, map_generation) !=
            mapping_ref ||
        map_id > UB_OBMM_ASYNC_QUEUE_DEPTH ||
        !ub_obmm_async_map_valid(state, map_id, map_generation)) {
        return false;
    }

    map = &state->maps[map_id - 1];
    if (local_pa < map->resolved.local_pa) {
        return false;
    }
    offset = local_pa - map->resolved.local_pa;
    if (offset > map->length || length > map->length - offset ||
        map->resolved.remote_uba > UINT64_MAX - offset ||
        map->resolved.remote_uba + offset > UINT64_MAX - length ||
        !ubc_obmm_resolve_async_map(state->ubc_dev, local_pa, length,
                                    &current)) {
        return false;
    }
    if (current.map_id != map->resolved.map_id ||
        current.map_generation != map->resolved.map_generation ||
        current.local_pa != local_pa ||
        current.remote_uba != map->resolved.remote_uba + offset ||
        current.token_id != map->resolved.token_id ||
        current.peer_cna != map->resolved.peer_cna ||
        current.access_flags != map->resolved.access_flags) {
        return false;
    }
    *resolved = current;
    return true;
}

bool ub_obmm_async_crosses_mapping_boundary(
    UbObmmAsyncState *state, uint64_t mapping_ref, uint64_t local_pa,
    uint64_t length, UbObmmAsyncBoundaryCrossing *crossing)
{
    UbObmmAsyncMap *source;
    uint64_t source_generation;
    uint64_t source_map_id;
    uint64_t request_end;
    uint64_t boundary;
    uint32_t index;

    if (!state || !crossing || mapping_ref == 0 || length == 0 ||
        local_pa > UINT64_MAX - length) {
        return false;
    }
    source_map_id = lingqu_pto_obmm_mapping_ref_map_id(mapping_ref);
    source_generation =
        lingqu_pto_obmm_mapping_ref_generation(mapping_ref);
    if (lingqu_pto_obmm_mapping_ref_encode(source_map_id,
                                           source_generation) !=
            mapping_ref ||
        source_map_id == 0 || source_map_id > UB_OBMM_ASYNC_QUEUE_DEPTH ||
        !ub_obmm_async_map_valid(state, source_map_id,
                                 source_generation)) {
        return false;
    }
    source = &state->maps[source_map_id - 1];
    if (source->resolved.local_pa > UINT64_MAX - source->length) {
        return false;
    }
    boundary = source->resolved.local_pa + source->length;
    request_end = local_pa + length;
    if (local_pa < source->resolved.local_pa || local_pa >= boundary ||
        request_end <= boundary) {
        return false;
    }
    for (index = 0; index < UB_OBMM_ASYNC_QUEUE_DEPTH; index++) {
        UbObmmAsyncMap *adjacent = &state->maps[index];
        uint64_t adjacent_end;

        if (!adjacent->active || index + 1 == source_map_id ||
            adjacent->resolved.local_pa != boundary ||
            adjacent->resolved.local_pa > UINT64_MAX - adjacent->length) {
            continue;
        }
        adjacent_end = adjacent->resolved.local_pa + adjacent->length;
        if (request_end > adjacent_end) {
            continue;
        }
        *crossing = (UbObmmAsyncBoundaryCrossing) {
            .source_map_id = source_map_id,
            .source_map_generation = source_generation,
            .source_base = source->resolved.local_pa,
            .source_length = source->length,
            .boundary = boundary,
            .adjacent_map_id = index + 1,
            .adjacent_map_generation = adjacent->generation,
        };
        return true;
    }
    return false;
}

static void ub_obmm_async_child_complete(
    void *opaque, ObmmRemoteToken token, uint16_t child_index,
    ObmmRemoteStatus status, const void *payload, uint32_t bytes_done,
    uint64_t model_publish_ns);
static void ub_obmm_async_process_sq(UbObmmAsyncState *state);
static void ub_obmm_async_disable(UbObmmAsyncState *state);

static ObmmProviderChildDisposition ub_obmm_async_submit_child(
    void *opaque, ObmmRemoteToken token, uint16_t child_index,
    uint64_t map_id, uint64_t map_generation, uint64_t remote_offset,
    uint32_t length, uint64_t operation_ordinal, void *inline_payload,
    ObmmRemoteStatus *inline_status)
{
    UbObmmAsyncState *state = opaque;
    UbObmmAsyncMap *map;
    UbObmmRemoteOperation operation;

    (void)inline_payload;

    if (!ub_obmm_async_map_valid(state, map_id, map_generation)) {
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
            &operation, token, child_index,
            ub_obmm_async_child_complete, state)) {
        *inline_status = OBMM_REMOTE_STATUS_REMOTE_IO;
        return OBMM_PROVIDER_CHILD_REJECTED;
    }
    return OBMM_PROVIDER_CHILD_PENDING;
}

static void ub_obmm_async_cancel_provider(void *opaque,
                                          ObmmRemoteToken token)
{
    UbObmmAsyncState *state = opaque;

    ubc_sim_dec_remote_read_async_cancel(state->ubc_dev, token);
}

static bool ub_obmm_async_sink_valid(void *opaque, uint64_t sink_id,
                                     uint64_t sink_generation)
{
    UbObmmAsyncState *state = opaque;
    uint16_t slot = ub_obmm_async_token_slot(sink_id);

    if (slot >= UB_OBMM_ASYNC_QUEUE_DEPTH ||
        ub_obmm_async_token_queue(sink_id) != state->queue_id ||
        ub_obmm_async_token_generation(sink_id) != sink_generation) {
        return false;
    }
    return state->futures[slot].active &&
        state->futures[slot].public_token == sink_id;
}

static bool ub_obmm_async_buffer_owned(const UbObmmAsyncState *state,
                                       const UbObmmAsyncFuture *future)
{
    const UbObmmAsyncBuffer *buffer;

    if (future->buffer_id == 0 ||
        future->buffer_id > UB_OBMM_ASYNC_QUEUE_DEPTH) {
        return false;
    }
    buffer = &state->buffers[future->buffer_id - 1];
    return buffer->active &&
        buffer->generation == future->buffer_generation &&
        future->dst_offset <= buffer->length &&
        future->length <= buffer->length - future->dst_offset;
}

static void ub_obmm_async_stage_completion(
    UbObmmAsyncState *state, UbObmmAsyncFuture *future,
    ObmmRemoteStatus backend_status, uint32_t bytes_done,
    uint64_t checksum64, uint64_t completed_ns, const void *payload)
{
    UbObmmAsyncStatus status =
        ub_obmm_async_status_from_backend(backend_status);
    UbObmmAsyncBuffer *buffer = NULL;

    if (status == UB_OBMM_ASYNC_OK) {
        if (!ub_obmm_async_map_valid(state, future->map_id,
                                     future->map_generation) ||
            !ub_obmm_async_buffer_owned(state, future)) {
            status = UB_OBMM_ASYNC_STALE;
            bytes_done = 0;
        } else {
            buffer = &state->buffers[future->buffer_id - 1];
            if (!payload || bytes_done != future->length ||
                dma_memory_write(
                    &address_space_memory,
                    buffer->base + future->dst_offset, payload, bytes_done,
                    MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
                status = UB_OBMM_ASYNC_REMOTE_IO;
                bytes_done = 0;
            }
        }
    }
    future->cqe = (UbObmmAsyncCqEntryV1) {
        .abi_version = UB_OBMM_ASYNC_ABI_VERSION,
        .opcode = OBMM_ASYNC_OPCODE_READ_COMPLETE,
        .status = status,
        .token = future->public_token,
        .user_data = future->user_data,
        .bytes_done = status == UB_OBMM_ASYNC_OK ? bytes_done : 0,
        .provider_status = backend_status,
        .checksum64 = status == UB_OBMM_ASYNC_OK ? checksum64 : 0,
        .completed_ns = completed_ns,
        .map_generation = future->map_generation,
    };
    future->terminal = true;
}

static void ub_obmm_async_sink_complete(void *opaque,
                                        const ObmmRemoteResult *result)
{
    UbObmmAsyncState *state = opaque;
    uint16_t backend_slot = result->token.slot;
    UbObmmAsyncFuture *future = NULL;
    uint16_t index;

    for (index = 0; index < UB_OBMM_ASYNC_QUEUE_DEPTH; index++) {
        if (state->futures[index].active &&
            state->futures[index].backend_token.owner_id ==
                result->token.owner_id &&
            state->futures[index].backend_token.slot == result->token.slot &&
            state->futures[index].backend_token.generation ==
                result->token.generation) {
            future = &state->futures[index];
            break;
        }
    }
    if (!future) {
        qemu_log("obmm_async stale backend completion slot=%u\n",
                 backend_slot);
        return;
    }
    ub_obmm_async_stage_completion(
        state, future, result->status, result->bytes_done,
        result->checksum64, result->model_publish_ns, result->payload);
}

static void ub_obmm_async_flush_cq(UbObmmAsyncState *state)
{
    bool published = false;
    uint16_t slot;

    for (slot = 0; slot < UB_OBMM_ASYNC_QUEUE_DEPTH; slot++) {
        UbObmmAsyncFuture *future = &state->futures[slot];
        uint8_t wire[UB_OBMM_ASYNC_SLOT_BYTES];
        uint64_t cq_slot;

        if (!future->active || !future->terminal) {
            continue;
        }
        if (state->cq_tail - state->cq_head >= state->cq_depth) {
            break;
        }
        cq_slot = state->cq_tail & (state->cq_depth - 1);
        ub_obmm_async_encode_cqe(&future->cqe, wire);
        if (dma_memory_write(
                &address_space_memory,
                state->cq_base + cq_slot * UB_OBMM_ASYNC_SLOT_BYTES,
                wire, sizeof(wire), MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            state->last_error = UB_OBMM_ASYNC_REMOTE_IO;
            state->irq_status |= OBMM_ASYNC_IRQ_ERROR;
            break;
        }
        /* Publish CQE contents before exposing the updated CQ tail. */
        smp_wmb();
        state->cq_tail++;
        future->active = false;
        future->terminal = false;
        published = true;
    }
    if (published) {
        state->irq_status |= OBMM_ASYNC_IRQ_COMPLETION;
        ubc_obmm_async_irq_notify(state->ubc_dev);
    }
}

static void ub_obmm_async_arm_deadline(UbObmmAsyncState *state)
{
    uint64_t deadline = obmm_remote_next_deadline(state->backend);

    if (deadline == UINT64_MAX) {
        timer_del(state->deadline_timer);
    } else {
        timer_mod_ns(state->deadline_timer, deadline);
    }
}

static void ub_obmm_async_bh(void *opaque)
{
    UbObmmAsyncState *state = opaque;

    state->bh_pending = false;
    ub_obmm_async_process_sq(state);
}

static void ub_obmm_async_schedule_bh(UbObmmAsyncState *state)
{
    if (state->bh_pending) {
        return;
    }
    state->bh_pending = true;
    qemu_bh_schedule(state->bh);
}

static void ub_obmm_async_deadline(void *opaque)
{
    UbObmmAsyncState *state = opaque;
    uint64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    obmm_remote_run_deadlines(state->backend, now_ns);
    ub_obmm_async_schedule_bh(state);
}

static void ub_obmm_async_child_complete(
    void *opaque, ObmmRemoteToken token, uint16_t child_index,
    ObmmRemoteStatus status, const void *payload, uint32_t bytes_done,
    uint64_t model_publish_ns)
{
    UbObmmAsyncState *state = opaque;

    obmm_remote_child_complete(state->backend, token, child_index,
                               status, payload, bytes_done,
                               model_publish_ns);
    ub_obmm_async_schedule_bh(state);
}

static bool ub_obmm_async_future_prepare(
    UbObmmAsyncState *state, const UbObmmAsyncParsedSq *entry,
    UbObmmAsyncFuture **future_out, ObmmRemoteStatus *status)
{
    UbObmmAsyncMap *map;
    UbObmmAsyncBuffer *buffer;
    UbObmmAsyncFuture *future;
    uint16_t slot = ub_obmm_async_token_slot(entry->token);
    uint64_t generation = ub_obmm_async_token_generation(entry->token);

    if (entry->abi_version != UB_OBMM_ASYNC_ABI_VERSION ||
        entry->opcode != OBMM_ASYNC_OPCODE_READ || entry->flags != 0 ||
        entry->length == 0 || entry->length > OBMM_REMOTE_MAX_BYTES ||
        ub_obmm_async_token_queue(entry->token) != state->queue_id ||
        slot >= UB_OBMM_ASYNC_QUEUE_DEPTH || generation == 0) {
        *status = OBMM_REMOTE_STATUS_UNSUPPORTED;
        return false;
    }
    if (entry->map_id == 0 ||
        entry->map_id > UB_OBMM_ASYNC_QUEUE_DEPTH) {
        *status = OBMM_REMOTE_STATUS_STALE_MAP;
        return false;
    }
    map = &state->maps[entry->map_id - 1];
    if (!map->active || map->generation != entry->map_generation) {
        *status = OBMM_REMOTE_STATUS_STALE_MAP;
        return false;
    }
    if (entry->remote_offset > map->length ||
        entry->length > map->length - entry->remote_offset) {
        *status = OBMM_REMOTE_STATUS_BOUNDS;
        return false;
    }
    if (entry->dst_buffer_id == 0 ||
        entry->dst_buffer_id > UB_OBMM_ASYNC_QUEUE_DEPTH) {
        *status = OBMM_REMOTE_STATUS_BOUNDS;
        return false;
    }
    buffer = &state->buffers[entry->dst_buffer_id - 1];
    if (!buffer->active || entry->dst_offset > buffer->length ||
        entry->length > buffer->length - entry->dst_offset) {
        *status = OBMM_REMOTE_STATUS_BOUNDS;
        return false;
    }
    future = &state->futures[slot];
    if (future->active || generation <= future->generation) {
        *status = OBMM_REMOTE_STATUS_STALE_SINK;
        return false;
    }
    *future = (UbObmmAsyncFuture) {
        .active = true,
        .public_token = entry->token,
        .generation = generation,
        .buffer_id = entry->dst_buffer_id,
        .buffer_generation = buffer->generation,
        .dst_offset = entry->dst_offset,
        .length = entry->length,
        .map_id = entry->map_id,
        .map_generation = entry->map_generation,
        .user_data = entry->user_data,
    };
    *future_out = future;
    return true;
}

static uint64_t ub_obmm_async_model_deadline(
    const UbObmmAsyncState *state, uint64_t guest_deadline_ns,
    uint64_t model_now_ns)
{
    uint64_t delta;

    if (guest_deadline_ns == 0) {
        return 0;
    }
    if (guest_deadline_ns <= state->guest_monotonic_ns) {
        return model_now_ns;
    }
    delta = guest_deadline_ns - state->guest_monotonic_ns;
    if (delta >= UINT64_MAX - model_now_ns) {
        return UINT64_MAX - 1;
    }
    return model_now_ns + delta;
}

static void ub_obmm_async_reject_entry(UbObmmAsyncState *state,
                                       const UbObmmAsyncParsedSq *entry,
                                       ObmmRemoteStatus status)
{
    uint16_t slot = ub_obmm_async_token_slot(entry->token);
    UbObmmAsyncFuture *future;

    if (slot >= UB_OBMM_ASYNC_QUEUE_DEPTH ||
        state->futures[slot].active) {
        state->last_error = ub_obmm_async_status_from_backend(status);
        state->irq_status |= OBMM_ASYNC_IRQ_ERROR;
        return;
    }
    future = &state->futures[slot];
    *future = (UbObmmAsyncFuture) {
        .active = true,
        .public_token = entry->token,
        .generation = ub_obmm_async_token_generation(entry->token),
        .map_generation = entry->map_generation,
        .user_data = entry->user_data,
    };
    ub_obmm_async_stage_completion(
        state, future, status, 0, 0,
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), NULL);
}

static void ub_obmm_async_process_sq(UbObmmAsyncState *state)
{
    while (state->enabled && state->sq_head != state->sq_tail) {
        UbObmmAsyncSqEntryV1 wire;
        UbObmmAsyncParsedSq entry;
        UbObmmAsyncFuture *future;
        ObmmRemoteRequest request;
        ObmmRemoteResult inline_result;
        ObmmRemoteStatus status = OBMM_REMOTE_STATUS_INTERNAL;
        ObmmRemoteToken backend_token;
        ObmmSubmitDisposition disposition;
        uint64_t model_now_ns;
        uint64_t sq_slot;

        if (state->sq_tail - state->sq_head > state->sq_depth) {
            state->last_error = UB_OBMM_ASYNC_INVALID;
            state->irq_status |= OBMM_ASYNC_IRQ_ERROR;
            break;
        }
        sq_slot = state->sq_head & (state->sq_depth - 1);
        if (dma_memory_read(
                &address_space_memory,
                state->sq_base + sq_slot * UB_OBMM_ASYNC_SLOT_BYTES,
                &wire, sizeof(wire), MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            state->last_error = UB_OBMM_ASYNC_REMOTE_IO;
            state->irq_status |= OBMM_ASYNC_IRQ_ERROR;
            break;
        }
        /* Observe SQE contents after reading the guest-published SQ tail. */
        smp_rmb();
        ub_obmm_async_decode_sq(&wire, &entry);
        if (!ub_obmm_async_future_prepare(state, &entry, &future, &status)) {
            ub_obmm_async_reject_entry(state, &entry, status);
            state->sq_head++;
            continue;
        }
        model_now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        request = (ObmmRemoteRequest) {
            .map_id = entry.map_id,
            .map_generation = entry.map_generation,
            .remote_offset = entry.remote_offset,
            .length = entry.length,
            .deadline_model_ns = ub_obmm_async_model_deadline(
                state, entry.deadline_ns, model_now_ns),
            .operation_ordinal = entry.user_data,
            .sink = {
                .kind = OBMM_REMOTE_SINK_P2A,
                .sink_id = entry.token,
                .sink_generation = future->generation,
                .owner_id = OBMM_ASYNC_BACKEND_OWNER_ID,
                .validate = ub_obmm_async_sink_valid,
                .complete = ub_obmm_async_sink_complete,
                .adapter_state = state,
            },
        };
        if (state->maps[entry.map_id - 1].active) {
            const UbObmmAsyncMap *map =
                &state->maps[entry.map_id - 1];
            UbObmmRemoteOperation operation = {
                .map_id = map->resolved.map_id,
                .map_generation = map->resolved.map_generation,
                .remote_offset = entry.remote_offset,
                .length = entry.length,
                .per_range_ordinal = entry.user_data,
            };
            request.operation_key = ub_obmm_remote_operation_key(
                &state->ubc_dev->remote_memory_model.config, &operation);
        }
        disposition = obmm_remote_submit(
            state->backend, &request, model_now_ns, &backend_token,
            &inline_result, &status);
        if (disposition == OBMM_SUBMIT_REJECTED) {
            ub_obmm_async_stage_completion(
                state, future, status, 0, 0,
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), NULL);
        } else if (disposition == OBMM_SUBMIT_INLINE) {
            ub_obmm_async_stage_completion(
                state, future, inline_result.status,
                inline_result.bytes_done, inline_result.checksum64,
                inline_result.model_publish_ns, inline_result.payload);
        } else {
            future->backend_token = backend_token;
        }
        state->sq_head++;
    }
    obmm_remote_deliver_ready(state->backend);
    ub_obmm_async_flush_cq(state);
    ub_obmm_async_arm_deadline(state);
}

static void ub_obmm_async_schedule_doorbell(UbObmmAsyncState *state)
{
    ub_obmm_async_schedule_bh(state);
}

UbObmmAsyncState *ub_obmm_async_new(BusControllerDev *ubc_dev)
{
    ObmmRemoteBackendOps ops;
    UbObmmAsyncState *state;

    if (!ubc_dev) {
        return NULL;
    }
    state = g_new0(UbObmmAsyncState, 1);
    state->ubc_dev = ubc_dev;
    state->sq_depth = UB_OBMM_ASYNC_QUEUE_DEPTH;
    state->cq_depth = UB_OBMM_ASYNC_QUEUE_DEPTH;
    ops = (ObmmRemoteBackendOps) {
        .map_validate = ub_obmm_async_map_valid,
        .submit_child = ub_obmm_async_submit_child,
        .cancel = ub_obmm_async_cancel_provider,
        .opaque = state,
    };
    state->backend = obmm_remote_backend_new(
        OBMM_ASYNC_BACKEND_OWNER_ID, OBMM_ASYNC_CHILD_CHUNK_BYTES, &ops);
    if (!state->backend) {
        g_free(state);
        return NULL;
    }
    state->bh = qemu_bh_new_guarded(
        ub_obmm_async_bh, state,
        &DEVICE(ubc_dev)->mem_reentrancy_guard);
    state->deadline_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                         ub_obmm_async_deadline, state);
    return state;
}

void ub_obmm_async_free(UbObmmAsyncState *state)
{
    if (!state) {
        return;
    }
    ub_obmm_async_disable(state);
    qemu_bh_delete(state->bh);
    timer_free(state->deadline_timer);
    obmm_remote_backend_free(state->backend);
    g_free(state);
}

bool ub_obmm_async_decode(hwaddr address, hwaddr *reg)
{
    if (address < UB_OBMM_ASYNC_ENDPOINT_BASE ||
        address >= UB_OBMM_ASYNC_ENDPOINT_BASE +
            UB_OBMM_ASYNC_ENDPOINT_BYTES) {
        return false;
    }
    *reg = address - UB_OBMM_ASYNC_ENDPOINT_BASE;
    return true;
}

static uint64_t ub_obmm_async_observability_read(
    const UbObmmAsyncState *state, uint32_t index)
{
    const UbObmmRemoteModelState *model =
        &state->ubc_dev->remote_memory_model;
    const ObmmRemoteBackendStats *backend =
        obmm_remote_backend_stats(state->backend);

    switch (index) {
    case 0: return model->accepted;
    case 1: return model->total_service_ns;
    case 2: return model->total_accept_to_publish_ns;
    case 3: return model->pending;
    case 4: return model->completed;
    case 5: return model->capacity_rejected;
    case 6: return model->dropped;
    case 7: return model->errored;
    case 8: return model->duplicated;
    case 9: return model->published;
    case 10: return model->duplicate_published;
    case 11: return backend->accepted;
    case 12: return backend->rejected;
    case 13: return backend->delivered;
    case 14: return backend->late;
    case 15: return backend->duplicate;
    case 16: return backend->capacity;
    case 17: return obmm_remote_pending(state->backend);
    case 18: return backend->pending_high_water;
    case 19: return backend->sink_copy_bytes;
    case 20: return backend->sink_copy_ns;
    default: return 0;
    }
}

uint64_t ub_obmm_async_read(UbObmmAsyncState *state, hwaddr reg,
                            unsigned int size)
{
    uint64_t value = 0;

    if (!state || (size != sizeof(uint32_t) &&
                   size != sizeof(uint64_t))) {
        return 0;
    }
    if ((reg & ~7ULL) >= OBMM_ASYNC_REG_OBSERVABILITY_BASE) {
        value = ub_obmm_async_observability_read(
            state, ((reg & ~7ULL) -
                    OBMM_ASYNC_REG_OBSERVABILITY_BASE) / 8);
        return ub_obmm_async_access_extract(value, reg, size);
    }
    switch (reg & ~7ULL) {
    case OBMM_ASYNC_REG_VERSION_CAPS:
        value = UB_OBMM_ASYNC_ABI_VERSION |
            ((uint64_t)UB_OBMM_ASYNC_QUEUE_DEPTH << 32);
        break;
    case OBMM_ASYNC_REG_STATUS:
        value = state->enabled ? OBMM_ASYNC_STATUS_ENABLED : 0;
        break;
    case OBMM_ASYNC_REG_SQ_BASE:
        value = state->sq_base;
        break;
    case OBMM_ASYNC_REG_SQ_SIZE:
        value = state->sq_depth;
        break;
    case OBMM_ASYNC_REG_SQ_HEAD:
        value = state->sq_head;
        break;
    case OBMM_ASYNC_REG_SQ_TAIL:
        value = state->sq_tail;
        break;
    case OBMM_ASYNC_REG_CQ_BASE:
        value = state->cq_base;
        break;
    case OBMM_ASYNC_REG_CQ_SIZE:
        value = state->cq_depth;
        break;
    case OBMM_ASYNC_REG_CQ_HEAD:
        value = state->cq_head;
        break;
    case OBMM_ASYNC_REG_CQ_TAIL:
        ub_obmm_async_flush_cq(state);
        value = state->cq_tail;
        break;
    case OBMM_ASYNC_REG_IRQ_STATUS:
        value = state->irq_status;
        break;
    case OBMM_ASYNC_REG_LAST_ERROR:
        value = state->last_error;
        break;
    case OBMM_ASYNC_REG_QUEUE_ID:
        value = state->queue_id;
        break;
    default:
        break;
    }
    return ub_obmm_async_access_extract(value, reg, size);
}

static bool ub_obmm_async_future_uses_buffer(
    const UbObmmAsyncState *state, uint32_t buffer_id,
    uint32_t generation)
{
    uint16_t slot;

    for (slot = 0; slot < UB_OBMM_ASYNC_QUEUE_DEPTH; slot++) {
        const UbObmmAsyncFuture *future = &state->futures[slot];

        if (future->active && future->buffer_id == buffer_id &&
            future->buffer_generation == generation) {
            return true;
        }
    }
    return false;
}

static void ub_obmm_async_disable(UbObmmAsyncState *state)
{
    uint16_t slot;
    uint64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    for (slot = 0; slot < UB_OBMM_ASYNC_QUEUE_DEPTH; slot++) {
        UbObmmAsyncFuture *future = &state->futures[slot];

        if (future->active && !future->terminal &&
            future->backend_token.owner_id != 0) {
            obmm_remote_cancel(state->backend, future->backend_token,
                               OBMM_REMOTE_STATUS_CANCELLED, now_ns);
        }
    }
    obmm_remote_deliver_ready(state->backend);
    memset(state->futures, 0, sizeof(state->futures));
    state->enabled = false;
    state->bh_pending = false;
    state->sq_head = 0;
    state->sq_tail = 0;
    state->cq_head = 0;
    state->cq_tail = 0;
    state->irq_status = 0;
    state->last_error = 0;
    qemu_bh_cancel(state->bh);
    timer_del(state->deadline_timer);
}

static void ub_obmm_async_map_command(UbObmmAsyncState *state,
                                      uint64_t command)
{
    UbObmmAsyncMap *map;

    state->last_error = 0;
    if (state->map_id == 0 ||
        state->map_id > UB_OBMM_ASYNC_QUEUE_DEPTH ||
        state->map_generation == 0) {
        state->last_error = UB_OBMM_ASYNC_INVALID;
        return;
    }
    map = &state->maps[state->map_id - 1];
    if (command == 1) {
        if (map->active || state->map_length == 0 ||
            !ubc_obmm_resolve_async_map(
                state->ubc_dev, state->map_local_pa,
                state->map_length, &map->resolved)) {
            state->last_error = UB_OBMM_ASYNC_NO_MAP;
            return;
        }
        map->active = true;
        map->generation = state->map_generation;
        map->length = state->map_length;
    } else if (command == 2) {
        if (!map->active || map->generation != state->map_generation) {
            state->last_error = UB_OBMM_ASYNC_STALE;
            return;
        }
        obmm_remote_retire_map(
            state->backend, state->map_id, state->map_generation,
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        memset(map, 0, sizeof(*map));
        ub_obmm_async_schedule_bh(state);
    } else {
        state->last_error = UB_OBMM_ASYNC_INVALID;
    }
}

static void ub_obmm_async_buffer_command(UbObmmAsyncState *state,
                                         uint64_t command)
{
    UbObmmAsyncBuffer *buffer;

    state->last_error = 0;
    if (state->buffer_id == 0 ||
        state->buffer_id > UB_OBMM_ASYNC_QUEUE_DEPTH ||
        state->buffer_generation == 0) {
        state->last_error = UB_OBMM_ASYNC_INVALID;
        return;
    }
    buffer = &state->buffers[state->buffer_id - 1];
    if (command == 1) {
        if (buffer->active || state->buffer_length == 0 ||
            state->buffer_length > OBMM_REMOTE_MAX_BYTES) {
            state->last_error = UB_OBMM_ASYNC_BOUNDS;
            return;
        }
        *buffer = (UbObmmAsyncBuffer) {
            .active = true,
            .generation = state->buffer_generation,
            .base = state->buffer_base,
            .length = state->buffer_length,
        };
    } else if (command == 2) {
        if (!buffer->active ||
            buffer->generation != state->buffer_generation ||
            ub_obmm_async_future_uses_buffer(
                state, state->buffer_id, state->buffer_generation)) {
            state->last_error = UB_OBMM_ASYNC_STALE;
            return;
        }
        memset(buffer, 0, sizeof(*buffer));
    } else {
        state->last_error = UB_OBMM_ASYNC_INVALID;
    }
}

static void ub_obmm_async_cancel_command(UbObmmAsyncState *state)
{
    uint16_t slot = ub_obmm_async_token_slot(state->cancel_token);
    UbObmmAsyncFuture *future;

    state->last_error = 0;
    if (slot >= UB_OBMM_ASYNC_QUEUE_DEPTH) {
        state->last_error = UB_OBMM_ASYNC_STALE;
        return;
    }
    future = &state->futures[slot];
    if (!future->active || future->public_token != state->cancel_token ||
        obmm_remote_cancel(
            state->backend, future->backend_token,
            OBMM_REMOTE_STATUS_CANCELLED,
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)) ==
                OBMM_CANCEL_STALE_TOKEN) {
        state->last_error = UB_OBMM_ASYNC_STALE;
        return;
    }
    ub_obmm_async_schedule_bh(state);
}

bool ub_obmm_async_write(UbObmmAsyncState *state, hwaddr reg,
                         uint64_t value, unsigned int size)
{
    hwaddr base = reg & ~7ULL;

    if (!state || (size != sizeof(uint32_t) &&
                   size != sizeof(uint64_t))) {
        return false;
    }
    switch (base) {
    case OBMM_ASYNC_REG_STATUS:
        if (!(value & OBMM_ASYNC_STATUS_ENABLED)) {
            ub_obmm_async_disable(state);
        } else if (state->queue_id == 0 || state->sq_base == 0 ||
                   state->cq_base == 0) {
            state->last_error = UB_OBMM_ASYNC_INVALID;
        } else {
            state->enabled = true;
            state->last_error = 0;
        }
        return true;
    case OBMM_ASYNC_REG_SQ_BASE:
        state->sq_base = ub_obmm_async_access_merge(
            state->sq_base, reg, value, size);
        return true;
    case OBMM_ASYNC_REG_SQ_SIZE:
        if (value == UB_OBMM_ASYNC_QUEUE_DEPTH) {
            state->sq_depth = value;
        } else {
            state->last_error = UB_OBMM_ASYNC_INVALID;
        }
        return true;
    case OBMM_ASYNC_REG_SQ_TAIL:
        state->sq_tail = value;
        return true;
    case OBMM_ASYNC_REG_CQ_BASE:
        state->cq_base = ub_obmm_async_access_merge(
            state->cq_base, reg, value, size);
        return true;
    case OBMM_ASYNC_REG_CQ_SIZE:
        if (value == UB_OBMM_ASYNC_QUEUE_DEPTH) {
            state->cq_depth = value;
        } else {
            state->last_error = UB_OBMM_ASYNC_INVALID;
        }
        return true;
    case OBMM_ASYNC_REG_CQ_HEAD:
        if (value <= state->cq_tail &&
            state->cq_tail - value <= state->cq_depth) {
            state->cq_head = value;
            ub_obmm_async_flush_cq(state);
        } else {
            state->last_error = UB_OBMM_ASYNC_INVALID;
        }
        return true;
    case OBMM_ASYNC_REG_DOORBELL:
        state->last_error = 0;
        ub_obmm_async_schedule_doorbell(state);
        return true;
    case OBMM_ASYNC_REG_IRQ_ACK:
        state->irq_status &= ~value;
        return true;
    case OBMM_ASYNC_REG_QUEUE_ID:
        if (!state->enabled && value > 0 && value <= UINT16_MAX) {
            state->queue_id = value;
        } else {
            state->last_error = UB_OBMM_ASYNC_INVALID;
        }
        return true;
    case OBMM_ASYNC_REG_MAP_LOCAL_PA:
        state->map_local_pa = ub_obmm_async_access_merge(
            state->map_local_pa, reg, value, size);
        return true;
    case OBMM_ASYNC_REG_MAP_LENGTH:
        state->map_length = value;
        return true;
    case OBMM_ASYNC_REG_MAP_ID:
        state->map_id = value;
        return true;
    case OBMM_ASYNC_REG_MAP_GENERATION:
        state->map_generation = value;
        return true;
    case OBMM_ASYNC_REG_MAP_CMD:
        ub_obmm_async_map_command(state, value);
        return true;
    case OBMM_ASYNC_REG_BUFFER_BASE:
        state->buffer_base = ub_obmm_async_access_merge(
            state->buffer_base, reg, value, size);
        return true;
    case OBMM_ASYNC_REG_BUFFER_LENGTH:
        state->buffer_length = value;
        return true;
    case OBMM_ASYNC_REG_BUFFER_ID:
        state->buffer_id = value;
        return true;
    case OBMM_ASYNC_REG_BUFFER_GENERATION:
        state->buffer_generation = value;
        return true;
    case OBMM_ASYNC_REG_BUFFER_CMD:
        ub_obmm_async_buffer_command(state, value);
        return true;
    case OBMM_ASYNC_REG_CANCEL_TOKEN:
        state->cancel_token = value;
        return true;
    case OBMM_ASYNC_REG_CANCEL_CMD:
        ub_obmm_async_cancel_command(state);
        return true;
    case OBMM_ASYNC_REG_GUEST_MONOTONIC_NS:
        state->guest_monotonic_ns = ub_obmm_async_access_merge(
            state->guest_monotonic_ns, reg, value, size);
        return true;
    case OBMM_ASYNC_REG_OBSERVABILITY_RESET:
        state->last_error = 0;
        if (value != 1 || state->ubc_dev->remote_memory_model.pending ||
            obmm_remote_pending(state->backend) ||
            !ub_obmm_remote_model_reset_stats(
                &state->ubc_dev->remote_memory_model) ||
            !obmm_remote_backend_reset_stats(state->backend)) {
            state->last_error = UB_OBMM_ASYNC_INVALID;
        }
        return true;
    default:
        return false;
    }
}

/*
 * Provider-neutral split-phase OBMM remote-read backend.
 */

#include "qemu/osdep.h"
#include "hw/ub/ub_obmm_remote.h"
#include "qemu/log.h"
#include "qemu/timer.h"

#define OBMM_FNV1A_OFFSET_BASIS 0xcbf29ce484222325ULL
#define OBMM_FNV1A_PRIME 0x00000100000001b3ULL

typedef enum ObmmRemoteParentState {
    OBMM_PARENT_FREE,
    OBMM_PARENT_ACCEPTED,
    OBMM_PARENT_IN_FLIGHT,
    OBMM_PARENT_TERMINAL,
    OBMM_PARENT_DELIVERING,
} ObmmRemoteParentState;

typedef struct ObmmRemoteChild {
    bool issued;
    bool terminal;
    uint32_t offset;
    uint32_t length;
} ObmmRemoteChild;

typedef struct ObmmRemoteParent {
    ObmmRemoteParentState state;
    uint32_t generation;
    ObmmRemoteRequest request;
    ObmmRemoteStatus terminal_status;
    uint32_t bytes_done;
    uint16_t child_count;
    uint16_t terminal_children;
    uint64_t checksum64;
    uint64_t model_accept_ns;
    uint64_t model_publish_ns;
    uint8_t *payload;
    ObmmRemoteChild children[OBMM_REMOTE_MAX_CHILDREN];
} ObmmRemoteParent;

struct ObmmRemoteBackend {
    uint16_t owner_id;
    uint32_t child_chunk_bytes;
    uint32_t pending;
    ObmmRemoteBackendOps ops;
    ObmmRemoteBackendStats stats;
    uint8_t *payload_pool;
    ObmmRemoteParent parents[OBMM_REMOTE_PARENT_CAPACITY];
};

static uint64_t obmm_remote_checksum(const void *payload, uint32_t length)
{
    const uint8_t *bytes = payload;
    uint64_t hash = OBMM_FNV1A_OFFSET_BASIS;
    uint32_t index;

    for (index = 0; index < length; index++) {
        hash ^= bytes[index];
        hash *= OBMM_FNV1A_PRIME;
    }
    return hash;
}

static ObmmRemoteToken obmm_remote_parent_token(
    const ObmmRemoteBackend *backend, uint16_t slot)
{
    return (ObmmRemoteToken) {
        .generation = backend->parents[slot].generation,
        .owner_id = backend->owner_id,
        .slot = slot,
    };
}

static ObmmRemoteParent *obmm_remote_find_parent(
    ObmmRemoteBackend *backend, ObmmRemoteToken token)
{
    ObmmRemoteParent *parent;

    if (!backend || token.owner_id != backend->owner_id ||
        token.slot >= OBMM_REMOTE_PARENT_CAPACITY) {
        return NULL;
    }
    parent = &backend->parents[token.slot];
    if (parent->state == OBMM_PARENT_FREE ||
        parent->generation != token.generation) {
        return NULL;
    }
    return parent;
}

static bool obmm_remote_try_terminal(ObmmRemoteBackend *backend,
                                     ObmmRemoteParent *parent,
                                     ObmmRemoteStatus status,
                                     uint64_t model_publish_ns,
                                     const char *winner)
{
    uint16_t slot = parent - backend->parents;
    ObmmRemoteToken token = obmm_remote_parent_token(backend, slot);

    if (parent->state == OBMM_PARENT_TERMINAL ||
        parent->state == OBMM_PARENT_DELIVERING) {
        backend->stats.late++;
        qemu_log("obmm_p1_late token=%u:%u:%u source=%s\n",
                 token.owner_id, token.slot, token.generation, winner);
        return false;
    }
    if (parent->state != OBMM_PARENT_ACCEPTED &&
        parent->state != OBMM_PARENT_IN_FLIGHT) {
        return false;
    }
    parent->terminal_status = status;
    parent->model_publish_ns = model_publish_ns;
    if (status == OBMM_REMOTE_STATUS_SUCCESS) {
        parent->bytes_done = parent->request.length;
        parent->checksum64 = obmm_remote_checksum(parent->payload,
                                                   parent->request.length);
    }
    parent->state = OBMM_PARENT_TERMINAL;
    qemu_log("obmm_p1_terminal token=%u:%u:%u winner=%s status=%s\n",
             token.owner_id, token.slot, token.generation, winner,
             obmm_remote_status_name(status));
    return true;
}

static void obmm_remote_cancel_children(ObmmRemoteBackend *backend,
                                        ObmmRemoteParent *parent)
{
    uint16_t child_index;

    if (backend->ops.cancel) {
        uint16_t slot = parent - backend->parents;

        backend->ops.cancel(backend->ops.opaque,
                            obmm_remote_parent_token(backend, slot));
    }
    for (child_index = 0; child_index < parent->child_count;
         child_index++) {
        ObmmRemoteChild *child = &parent->children[child_index];

        if (child->issued && !child->terminal) {
            child->terminal = true;
            parent->terminal_children++;
        }
    }
}

static void obmm_remote_recycle_parent(ObmmRemoteBackend *backend,
                                       ObmmRemoteParent *parent)
{
    uint32_t generation = parent->generation + 1;
    uint8_t *payload = parent->payload;

    if (generation == 0) {
        generation = 1;
    }
    memset(parent, 0, sizeof(*parent));
    parent->generation = generation;
    parent->payload = payload;
    parent->state = OBMM_PARENT_FREE;
    backend->pending--;
}

static void obmm_remote_fill_result(const ObmmRemoteBackend *backend,
                                    uint16_t slot,
                                    const ObmmRemoteParent *parent,
                                    ObmmRemoteResult *result)
{
    *result = (ObmmRemoteResult) {
        .token = obmm_remote_parent_token(backend, slot),
        .status = parent->terminal_status,
        .bytes_done = parent->bytes_done,
        .checksum64 = parent->checksum64,
        .model_accept_ns = parent->model_accept_ns,
        .model_publish_ns = parent->model_publish_ns,
        .payload = parent->payload,
    };
}

ObmmRemoteBackend *obmm_remote_backend_new(
    uint16_t owner_id, uint32_t child_chunk_bytes,
    const ObmmRemoteBackendOps *ops)
{
    ObmmRemoteBackend *backend;
    uint16_t slot;

    if (owner_id == 0 || child_chunk_bytes == 0 ||
        child_chunk_bytes > OBMM_REMOTE_MAX_BYTES || !ops ||
        !ops->submit_child) {
        return NULL;
    }
    backend = g_new0(ObmmRemoteBackend, 1);
    backend->owner_id = owner_id;
    backend->child_chunk_bytes = child_chunk_bytes;
    backend->ops = *ops;
    backend->payload_pool = g_malloc0(
        OBMM_REMOTE_PARENT_CAPACITY * OBMM_REMOTE_MAX_BYTES);
    for (slot = 0; slot < OBMM_REMOTE_PARENT_CAPACITY; slot++) {
        backend->parents[slot].generation = 1;
        backend->parents[slot].payload = backend->payload_pool +
            slot * OBMM_REMOTE_MAX_BYTES;
    }
    return backend;
}

void obmm_remote_backend_free(ObmmRemoteBackend *backend)
{
    if (!backend) {
        return;
    }
    g_free(backend->payload_pool);
    g_free(backend);
}

static ObmmRemoteStatus obmm_remote_validate_request(
    const ObmmRemoteBackend *backend, const ObmmRemoteRequest *request,
    uint16_t *child_count)
{
    uint64_t end;
    uint64_t count;

    if (!request || !request->sink.complete ||
        request->sink.owner_id != backend->owner_id) {
        return OBMM_REMOTE_STATUS_STALE_SINK;
    }
    if (request->length == 0 || request->length > OBMM_REMOTE_MAX_BYTES ||
        request->remote_offset > UINT64_MAX - request->length) {
        return OBMM_REMOTE_STATUS_BOUNDS;
    }
    end = request->remote_offset + request->length;
    if (end < request->remote_offset) {
        return OBMM_REMOTE_STATUS_BOUNDS;
    }
    if (request->flags != 0) {
        return OBMM_REMOTE_STATUS_UNSUPPORTED;
    }
    if (backend->ops.map_validate &&
        !backend->ops.map_validate(backend->ops.opaque, request->map_id,
                                   request->map_generation)) {
        return OBMM_REMOTE_STATUS_STALE_MAP;
    }
    count = (request->length + backend->child_chunk_bytes - 1) /
        backend->child_chunk_bytes;
    if (count == 0 || count > OBMM_REMOTE_MAX_CHILDREN) {
        return OBMM_REMOTE_STATUS_UNSUPPORTED;
    }
    *child_count = count;
    return OBMM_REMOTE_STATUS_SUCCESS;
}

ObmmSubmitDisposition obmm_remote_submit(
    ObmmRemoteBackend *backend, const ObmmRemoteRequest *request,
    uint64_t model_accept_ns, ObmmRemoteToken *token,
    ObmmRemoteResult *inline_result, ObmmRemoteStatus *rejected_status)
{
    ObmmRemoteStatus status;
    ObmmRemoteParent *parent = NULL;
    uint16_t child_count;
    uint16_t slot;
    uint16_t child_index;
    bool any_pending = false;

    if (!backend || !token || !inline_result || !rejected_status) {
        return OBMM_SUBMIT_REJECTED;
    }
    status = obmm_remote_validate_request(backend, request, &child_count);
    if (status != OBMM_REMOTE_STATUS_SUCCESS) {
        backend->stats.rejected++;
        *rejected_status = status;
        return OBMM_SUBMIT_REJECTED;
    }
    for (slot = 0; slot < OBMM_REMOTE_PARENT_CAPACITY; slot++) {
        if (backend->parents[slot].state == OBMM_PARENT_FREE) {
            parent = &backend->parents[slot];
            break;
        }
    }
    if (!parent) {
        backend->stats.rejected++;
        backend->stats.capacity++;
        *rejected_status = OBMM_REMOTE_STATUS_CAPACITY;
        return OBMM_SUBMIT_REJECTED;
    }

    parent->state = OBMM_PARENT_ACCEPTED;
    parent->request = *request;
    parent->child_count = child_count;
    parent->model_accept_ns = model_accept_ns;
    backend->pending++;
    backend->stats.accepted++;
    backend->stats.pending_high_water = MAX(
        backend->stats.pending_high_water, backend->pending);
    *token = obmm_remote_parent_token(backend, slot);
    qemu_log("obmm_p1_submit token=%u:%u:%u operation_key=%016" PRIx64
             " chunks=%u bytes=%u\n",
             token->owner_id, token->slot, token->generation,
             request->operation_key, child_count, request->length);

    for (child_index = 0; child_index < child_count; child_index++) {
        ObmmRemoteChild *child = &parent->children[child_index];
        ObmmRemoteStatus inline_status = OBMM_REMOTE_STATUS_SUCCESS;
        ObmmProviderChildDisposition disposition;
        uint32_t offset = child_index * backend->child_chunk_bytes;
        uint32_t length = MIN(backend->child_chunk_bytes,
                              request->length - offset);

        *child = (ObmmRemoteChild) {
            .issued = true,
            .offset = offset,
            .length = length,
        };
        disposition = backend->ops.submit_child(
            backend->ops.opaque, *token, child_index,
            request->map_id, request->map_generation,
            request->remote_offset + offset, length,
            request->operation_ordinal,
            parent->payload + offset, &inline_status);
        if (disposition == OBMM_PROVIDER_CHILD_PENDING) {
            any_pending = true;
            continue;
        }
        child->terminal = true;
        parent->terminal_children++;
        if (disposition == OBMM_PROVIDER_CHILD_REJECTED ||
            inline_status != OBMM_REMOTE_STATUS_SUCCESS) {
            status = disposition == OBMM_PROVIDER_CHILD_REJECTED ?
                OBMM_REMOTE_STATUS_REMOTE_IO : inline_status;
            obmm_remote_try_terminal(backend, parent, status,
                                     model_accept_ns, "provider-submit");
            break;
        }
    }

    if (parent->state == OBMM_PARENT_TERMINAL) {
        if (any_pending) {
            obmm_remote_cancel_children(backend, parent);
            return OBMM_SUBMIT_PENDING;
        }
        *rejected_status = parent->terminal_status;
        obmm_remote_recycle_parent(backend, parent);
        backend->stats.rejected++;
        return OBMM_SUBMIT_REJECTED;
    }
    if (!any_pending && parent->terminal_children == parent->child_count) {
        obmm_remote_try_terminal(backend, parent,
                                 OBMM_REMOTE_STATUS_SUCCESS,
                                 model_accept_ns, "inline");
        obmm_remote_fill_result(backend, slot, parent, inline_result);
        backend->stats.inline_completed++;
        obmm_remote_recycle_parent(backend, parent);
        return OBMM_SUBMIT_INLINE;
    }
    parent->state = OBMM_PARENT_IN_FLIGHT;
    return OBMM_SUBMIT_PENDING;
}

bool obmm_remote_child_complete(
    ObmmRemoteBackend *backend, ObmmRemoteToken token,
    uint16_t child_index, ObmmRemoteStatus status,
    const void *payload, uint32_t bytes_done, uint64_t model_publish_ns)
{
    ObmmRemoteParent *parent = obmm_remote_find_parent(backend, token);
    ObmmRemoteChild *child;

    if (!parent || child_index >= parent->child_count) {
        if (backend) {
            backend->stats.late++;
        }
        return false;
    }
    child = &parent->children[child_index];
    if (!child->issued || child->terminal ||
        parent->state == OBMM_PARENT_TERMINAL ||
        parent->state == OBMM_PARENT_DELIVERING) {
        backend->stats.duplicate++;
        qemu_log("obmm_p1_late token=%u:%u:%u source=response\n",
                 token.owner_id, token.slot, token.generation);
        return false;
    }
    qemu_log("obmm_p1_child_response token=%u:%u:%u child=%u"
             " status=%s bytes=%u\n",
             token.owner_id, token.slot, token.generation, child_index,
             obmm_remote_status_name(status), bytes_done);
    child->terminal = true;
    parent->terminal_children++;
    if (status != OBMM_REMOTE_STATUS_SUCCESS || !payload ||
        bytes_done != child->length) {
        status = status == OBMM_REMOTE_STATUS_SUCCESS ?
            OBMM_REMOTE_STATUS_REMOTE_IO : status;
        obmm_remote_try_terminal(backend, parent, status,
                                 model_publish_ns, "response");
        obmm_remote_cancel_children(backend, parent);
        return true;
    }
    memcpy(parent->payload + child->offset, payload, bytes_done);
    if (parent->terminal_children == parent->child_count) {
        obmm_remote_try_terminal(backend, parent,
                                 OBMM_REMOTE_STATUS_SUCCESS,
                                 model_publish_ns, "response");
    }
    return true;
}

ObmmCancelDisposition obmm_remote_cancel(ObmmRemoteBackend *backend,
                                         ObmmRemoteToken token,
                                         ObmmRemoteStatus reason,
                                         uint64_t model_now_ns)
{
    ObmmRemoteParent *parent = obmm_remote_find_parent(backend, token);

    if (!parent) {
        return OBMM_CANCEL_STALE_TOKEN;
    }
    if (!obmm_remote_try_terminal(backend, parent, reason, model_now_ns,
                                  "cancel")) {
        return OBMM_CANCEL_ALREADY_TERMINAL;
    }
    obmm_remote_cancel_children(backend, parent);
    return OBMM_CANCELLED_NOW;
}

uint32_t obmm_remote_retire_map(ObmmRemoteBackend *backend,
                                uint64_t map_id,
                                uint64_t map_generation,
                                uint64_t model_now_ns)
{
    uint32_t retired = 0;
    uint16_t slot;

    if (!backend) {
        return 0;
    }
    for (slot = 0; slot < OBMM_REMOTE_PARENT_CAPACITY; slot++) {
        ObmmRemoteParent *parent = &backend->parents[slot];

        if ((parent->state == OBMM_PARENT_ACCEPTED ||
             parent->state == OBMM_PARENT_IN_FLIGHT) &&
            parent->request.map_id == map_id &&
            parent->request.map_generation == map_generation &&
            obmm_remote_try_terminal(backend, parent,
                                     OBMM_REMOTE_STATUS_RETIRED,
                                     model_now_ns, "retire")) {
            obmm_remote_cancel_children(backend, parent);
            retired++;
        }
    }
    return retired;
}

uint32_t obmm_remote_run_deadlines(ObmmRemoteBackend *backend,
                                   uint64_t model_now_ns)
{
    uint32_t timed_out = 0;
    uint16_t slot;

    if (!backend) {
        return 0;
    }
    for (slot = 0; slot < OBMM_REMOTE_PARENT_CAPACITY; slot++) {
        ObmmRemoteParent *parent = &backend->parents[slot];

        if ((parent->state == OBMM_PARENT_ACCEPTED ||
             parent->state == OBMM_PARENT_IN_FLIGHT) &&
            parent->request.deadline_model_ns != 0 &&
            parent->request.deadline_model_ns <= model_now_ns &&
            obmm_remote_try_terminal(backend, parent,
                                     OBMM_REMOTE_STATUS_TIMEOUT,
                                     model_now_ns, "deadline")) {
            obmm_remote_cancel_children(backend, parent);
            timed_out++;
        }
    }
    return timed_out;
}

uint64_t obmm_remote_next_deadline(const ObmmRemoteBackend *backend)
{
    uint64_t deadline = UINT64_MAX;
    uint16_t slot;

    if (!backend) {
        return deadline;
    }
    for (slot = 0; slot < OBMM_REMOTE_PARENT_CAPACITY; slot++) {
        const ObmmRemoteParent *parent = &backend->parents[slot];

        if ((parent->state == OBMM_PARENT_ACCEPTED ||
             parent->state == OBMM_PARENT_IN_FLIGHT) &&
            parent->request.deadline_model_ns != 0) {
            deadline = MIN(deadline,
                           parent->request.deadline_model_ns);
        }
    }
    return deadline;
}

uint32_t obmm_remote_deliver_ready(ObmmRemoteBackend *backend)
{
    uint32_t delivered = 0;
    uint16_t slot;

    if (!backend) {
        return 0;
    }
    for (slot = 0; slot < OBMM_REMOTE_PARENT_CAPACITY; slot++) {
        ObmmRemoteParent *parent = &backend->parents[slot];
        ObmmRemoteResult result;
        bool sink_valid;
        uint64_t copy_start_ns;
        uint64_t copy_ns;

        if (parent->state != OBMM_PARENT_TERMINAL) {
            continue;
        }
        parent->state = OBMM_PARENT_DELIVERING;
        sink_valid = !parent->request.sink.validate ||
            parent->request.sink.validate(
                parent->request.sink.adapter_state,
                parent->request.sink.sink_id,
                parent->request.sink.sink_generation);
        if (!sink_valid) {
            parent->terminal_status = OBMM_REMOTE_STATUS_STALE_SINK;
        }
        obmm_remote_fill_result(backend, slot, parent, &result);
        copy_start_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        parent->request.sink.complete(
            parent->request.sink.adapter_state, &result);
        copy_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) -
            copy_start_ns;
        qemu_log("obmm_p1_deliver token=%u:%u:%u sink=%u"
                 " copy_bytes=%u copy_ns=%" PRIu64 " status=%s\n",
                 result.token.owner_id, result.token.slot,
                 result.token.generation, parent->request.sink.kind,
                 result.status == OBMM_REMOTE_STATUS_SUCCESS ?
                     result.bytes_done : 0,
                 copy_ns,
                 obmm_remote_status_name(result.status));
        backend->stats.delivered++;
        if (result.status == OBMM_REMOTE_STATUS_SUCCESS) {
            backend->stats.sink_copy_bytes += result.bytes_done;
        }
        backend->stats.sink_copy_ns += copy_ns;
        delivered++;
        obmm_remote_recycle_parent(backend, parent);
    }
    return delivered;
}

uint32_t obmm_remote_pending(const ObmmRemoteBackend *backend)
{
    return backend ? backend->pending : 0;
}

const ObmmRemoteBackendStats *obmm_remote_backend_stats(
    const ObmmRemoteBackend *backend)
{
    return backend ? &backend->stats : NULL;
}

bool obmm_remote_backend_reset_stats(ObmmRemoteBackend *backend)
{
    if (!backend || backend->pending != 0) {
        return false;
    }
    memset(&backend->stats, 0, sizeof(backend->stats));
    return true;
}

const char *obmm_remote_status_name(ObmmRemoteStatus status)
{
    static const char *const names[] = {
        [OBMM_REMOTE_STATUS_SUCCESS] = "success",
        [OBMM_REMOTE_STATUS_CAPACITY] = "capacity",
        [OBMM_REMOTE_STATUS_BOUNDS] = "bounds",
        [OBMM_REMOTE_STATUS_PERMISSION] = "permission",
        [OBMM_REMOTE_STATUS_STALE_MAP] = "stale-map",
        [OBMM_REMOTE_STATUS_STALE_SINK] = "stale-sink",
        [OBMM_REMOTE_STATUS_TOKEN_DENIED] = "token-denied",
        [OBMM_REMOTE_STATUS_COHERENCE] = "coherence",
        [OBMM_REMOTE_STATUS_REMOTE_IO] = "remote-io",
        [OBMM_REMOTE_STATUS_CHECKSUM] = "checksum",
        [OBMM_REMOTE_STATUS_TIMEOUT] = "timeout",
        [OBMM_REMOTE_STATUS_CANCELLED] = "cancelled",
        [OBMM_REMOTE_STATUS_RETIRED] = "retired",
        [OBMM_REMOTE_STATUS_UNSUPPORTED] = "unsupported",
        [OBMM_REMOTE_STATUS_INTERNAL] = "internal",
    };

    if (status >= ARRAY_SIZE(names) || !names[status]) {
        return "invalid";
    }
    return names[status];
}

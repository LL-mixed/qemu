/*
 * Provider-neutral pending-load and EL0-upcall event model.
 */

#include "qemu/osdep.h"
#include "hw/ub/ub_async_load.h"

#define ASYNC_LOAD_MAX_CLOCK_MHZ 100000U

typedef struct UbAsyncLoadPltEntry {
    uint32_t generation;
    UbAsyncLoadPltState state;
    uint64_t context_id;
    UbAsyncLoadDesc load;
    UbAsyncLoadStatus status;
    uint64_t complete_cycle;
    uint8_t bytes_done;
    uint8_t payload[8];
} UbAsyncLoadPltEntry;

struct UbAsyncLoad {
    uint16_t owner_id;
    uint32_t owner_generation;
    UbAsyncLoadConfig config;
    UbAsyncLoadPltEntry plt[UB_ASYNC_LOAD_MAX_PENDING_LOADS];
    UbAsyncLoadEvent events[UB_ASYNC_LOAD_MAX_EVENTS];
    uint16_t event_head;
    uint16_t event_tail;
    uint16_t event_count;
    uint16_t pending_count;
    uint64_t next_event_sequence;
    bool fail_stop;
    UbAsyncLoadStats stats;
};

bool ub_async_load_config_parse(const char *spec, bool *enabled,
                           UbAsyncLoadConfig *config)
{
    unsigned int values[5];
    int consumed = 0;

    if (!enabled || !config) {
        return false;
    }
    *enabled = false;
    *config = (UbAsyncLoadConfig) {
        .context_entries = 64,
        .pending_load_entries = 64,
        .event_queue_depth = 128,
        .clock_mhz = 2000,
    };
    if (!spec || !*spec) {
        return true;
    }
    if (sscanf(spec,
               "v3|enabled=%u|contexts=%u|pending=%u|events=%u|"
               "clock_mhz=%u%n",
               &values[0], &values[1], &values[2], &values[3],
               &values[4], &consumed) != 5 || spec[consumed] != '\0' ||
        values[0] > 1 || values[1] == 0 ||
        values[1] > UB_ASYNC_LOAD_MAX_CONTEXTS || values[2] == 0 ||
        values[2] > UB_ASYNC_LOAD_MAX_PENDING_LOADS || values[3] == 0 ||
        values[3] > UB_ASYNC_LOAD_MAX_EVENTS || values[4] == 0 ||
        values[4] > ASYNC_LOAD_MAX_CLOCK_MHZ) {
        return false;
    }
    *enabled = values[0];
    *config = (UbAsyncLoadConfig) {
        .context_entries = values[1],
        .pending_load_entries = values[2],
        .event_queue_depth = values[3],
        .clock_mhz = values[4],
    };
    return true;
}

uint64_t ub_async_load_context_id_make(uint32_t generation,
                                  uint16_t home_core, uint16_t slot)
{
    return (uint64_t)generation << 32 | (uint64_t)home_core << 16 | slot;
}

uint32_t ub_async_load_context_id_generation(uint64_t context_id)
{
    return context_id >> 32;
}

uint16_t ub_async_load_context_id_home_core(uint64_t context_id)
{
    return context_id >> 16;
}

uint16_t ub_async_load_context_id_slot(uint64_t context_id)
{
    return context_id;
}

static uint32_t ub_async_load_next_generation(uint32_t generation)
{
    generation++;
    return generation ? generation : 1;
}

static bool ub_async_load_config_valid(const UbAsyncLoadConfig *config)
{
    return config && config->context_entries > 0 &&
        config->context_entries <= UB_ASYNC_LOAD_MAX_CONTEXTS &&
        config->pending_load_entries > 0 &&
        config->pending_load_entries <= UB_ASYNC_LOAD_MAX_PENDING_LOADS &&
        config->event_queue_depth > 0 &&
        config->event_queue_depth <= UB_ASYNC_LOAD_MAX_EVENTS &&
        config->clock_mhz > 0;
}

UbAsyncLoad *ub_async_load_new(uint16_t owner_id, uint32_t owner_generation,
                      const UbAsyncLoadConfig *config)
{
    UbAsyncLoad *async_load;
    uint16_t slot;

    if (!owner_id || !owner_generation || !ub_async_load_config_valid(config)) {
        return NULL;
    }
    async_load = g_new0(UbAsyncLoad, 1);
    async_load->owner_id = owner_id;
    async_load->owner_generation = owner_generation;
    async_load->config = *config;
    async_load->next_event_sequence = 1;
    for (slot = 0; slot < config->pending_load_entries; slot++) {
        async_load->plt[slot].generation = 1;
    }
    return async_load;
}

void ub_async_load_free(UbAsyncLoad *async_load)
{
    g_free(async_load);
}

static UbAsyncLoadPltToken ub_async_load_plt_token(const UbAsyncLoad *async_load,
                                          uint16_t slot)
{
    return (UbAsyncLoadPltToken) {
        .generation = async_load->plt[slot].generation,
        .owner_id = async_load->owner_id,
        .slot = slot,
    };
}

static UbAsyncLoadPltEntry *ub_async_load_find_plt(UbAsyncLoad *async_load,
                                          UbAsyncLoadPltToken token)
{
    UbAsyncLoadPltEntry *entry;

    if (!async_load || token.owner_id != async_load->owner_id ||
        token.slot >= async_load->config.pending_load_entries) {
        return NULL;
    }
    entry = &async_load->plt[token.slot];
    if (entry->state == UB_ASYNC_LOAD_PLT_FREE ||
        entry->generation != token.generation) {
        return NULL;
    }
    return entry;
}

static bool ub_async_load_event_push(UbAsyncLoad *async_load, UbAsyncLoadEventKind kind,
                                const UbAsyncLoadPltEntry *entry,
                                UbAsyncLoadPltToken token,
                                UbAsyncLoadStatus status,
                                uint64_t value, uint64_t guest_cycle)
{
    UbAsyncLoadEvent *event;

    if (async_load->event_count == async_load->config.event_queue_depth) {
        async_load->stats.event_overflow++;
        async_load->fail_stop = true;
        return false;
    }
    if (kind == UB_ASYNC_LOAD_EVENT_PENDING) {
        async_load->event_head = (async_load->event_head +
                           async_load->config.event_queue_depth - 1) %
            async_load->config.event_queue_depth;
        event = &async_load->events[async_load->event_head];
    } else {
        event = &async_load->events[async_load->event_tail];
        async_load->event_tail = (async_load->event_tail + 1) %
            async_load->config.event_queue_depth;
    }
    *event = (UbAsyncLoadEvent) {
        .context_id = entry->context_id,
        .plt_token = token,
        .fault_pc = entry->load.fault_pc,
        .effective_va = entry->load.effective_va,
        .value = value,
        .map_id = entry->load.map_id,
        .map_generation = entry->load.map_generation,
        .map_model_generation = entry->load.map_model_generation,
        .kind = kind,
        .status = status,
        .rt = entry->load.rt,
        .access_bytes = entry->load.access_bytes,
        .guest_cycle = guest_cycle,
    };
    async_load->event_count++;
    async_load->stats.event_high_water = MAX(async_load->stats.event_high_water,
                                      async_load->event_count);
    return true;
}

static void ub_async_load_recycle_plt(UbAsyncLoad *async_load, UbAsyncLoadPltEntry *entry)
{
    uint32_t generation = ub_async_load_next_generation(entry->generation);

    memset(entry, 0, sizeof(*entry));
    entry->generation = generation;
    async_load->pending_count--;
}

static uint64_t ub_async_load_load_value(const UbAsyncLoadPltEntry *entry)
{
    uint64_t value = 0;
    uint8_t index;

    if (entry->load.big_endian) {
        for (index = 0; index < entry->bytes_done; index++) {
            value = value << 8 | entry->payload[index];
        }
    } else {
        for (index = 0; index < entry->bytes_done; index++) {
            value |= (uint64_t)entry->payload[index] << (index * 8);
        }
    }
    return value;
}

static bool ub_async_load_load_valid(const UbAsyncLoadDesc *load)
{
    return load && load->fault_pc <= UINT64_MAX - 4 && load->rt <= 31 &&
        !load->sign_extend &&
        (load->access_bytes == 1 || load->access_bytes == 2 ||
         load->access_bytes == 4 || load->access_bytes == 8);
}

UbAsyncLoadPendingResult ub_async_load_load_pending(
    UbAsyncLoad *async_load, uint64_t context_id, const UbAsyncLoadDesc *load,
    UbAsyncLoadPltToken *plt_token)
{
    UbAsyncLoadPltEntry *entry = NULL;
    uint16_t slot;

    if (!async_load || !context_id || !plt_token ||
        !ub_async_load_load_valid(load) || async_load->fail_stop) {
        return UB_ASYNC_LOAD_PENDING_INVALID;
    }
    if (async_load->pending_count == async_load->config.pending_load_entries ||
        async_load->event_count == async_load->config.event_queue_depth) {
        async_load->stats.capacity_stalls++;
        return UB_ASYNC_LOAD_PENDING_SYNC_STALL;
    }
    for (slot = 0; slot < async_load->config.pending_load_entries; slot++) {
        if (async_load->plt[slot].state == UB_ASYNC_LOAD_PLT_FREE) {
            entry = &async_load->plt[slot];
            break;
        }
    }
    g_assert(entry);
    entry->state = UB_ASYNC_LOAD_PLT_PENDING;
    entry->context_id = context_id;
    entry->load = *load;
    *plt_token = ub_async_load_plt_token(async_load, slot);
    async_load->pending_count++;
    async_load->stats.pending_loads++;
    async_load->stats.pending_high_water = MAX(async_load->stats.pending_high_water,
                                        async_load->pending_count);
    if (!ub_async_load_event_push(async_load, UB_ASYNC_LOAD_EVENT_PENDING, entry,
                             *plt_token, UB_ASYNC_LOAD_STATUS_SUCCESS, 0,
                             load->submit_cycle)) {
        ub_async_load_recycle_plt(async_load, entry);
        return UB_ASYNC_LOAD_PENDING_INVALID;
    }
    return UB_ASYNC_LOAD_PENDING_ACCEPTED;
}

UbAsyncLoadCompletionResult ub_async_load_load_complete(
    UbAsyncLoad *async_load, UbAsyncLoadPltToken plt_token, UbAsyncLoadStatus status,
    const void *payload, uint8_t bytes_done, uint64_t complete_cycle)
{
    UbAsyncLoadPltEntry *entry = ub_async_load_find_plt(async_load, plt_token);
    uint64_t value = 0;

    if (!entry) {
        if (async_load) {
            async_load->stats.stale_completions++;
        }
        return UB_ASYNC_LOAD_COMPLETION_STALE;
    }
    if (entry->state != UB_ASYNC_LOAD_PLT_PENDING) {
        async_load->stats.duplicate_completions++;
        return UB_ASYNC_LOAD_COMPLETION_DUPLICATE;
    }
    entry->complete_cycle = complete_cycle;
    if (status == UB_ASYNC_LOAD_STATUS_SUCCESS && payload &&
        bytes_done == entry->load.access_bytes) {
        memcpy(entry->payload, payload, bytes_done);
        entry->bytes_done = bytes_done;
        entry->status = status;
        entry->state = UB_ASYNC_LOAD_PLT_COMPLETE;
        value = ub_async_load_load_value(entry);
        async_load->stats.completed_loads++;
        ub_async_load_event_push(async_load, UB_ASYNC_LOAD_EVENT_COMPLETE, entry,
                            plt_token, status, value, complete_cycle);
    } else {
        if (status == UB_ASYNC_LOAD_STATUS_SUCCESS) {
            status = UB_ASYNC_LOAD_STATUS_INTERNAL;
        }
        entry->status = status;
        entry->state = UB_ASYNC_LOAD_PLT_FAULTED;
        async_load->stats.faulted_loads++;
        ub_async_load_event_push(async_load, UB_ASYNC_LOAD_EVENT_FAULT, entry,
                            plt_token, status, 0, complete_cycle);
    }
    return UB_ASYNC_LOAD_COMPLETION_ACCEPTED;
}

bool ub_async_load_event_pop(UbAsyncLoad *async_load, UbAsyncLoadEvent *event,
                        bool replay_retire)
{
    UbAsyncLoadPltEntry *entry;

    if (!async_load || !event || !async_load->event_count) {
        return false;
    }
    *event = async_load->events[async_load->event_head];
    async_load->event_head = (async_load->event_head + 1) %
        async_load->config.event_queue_depth;
    async_load->event_count--;
    event->sequence = async_load->next_event_sequence++;
    if (!async_load->next_event_sequence) {
        async_load->next_event_sequence = 1;
    }
    if (event->kind == UB_ASYNC_LOAD_EVENT_COMPLETE ||
        event->kind == UB_ASYNC_LOAD_EVENT_FAULT) {
        entry = ub_async_load_find_plt(async_load, event->plt_token);
        if (!entry ||
            (entry->state != UB_ASYNC_LOAD_PLT_COMPLETE &&
             entry->state != UB_ASYNC_LOAD_PLT_FAULTED)) {
            async_load->fail_stop = true;
            return true;
        }
        if (event->kind == UB_ASYNC_LOAD_EVENT_COMPLETE) {
            async_load->stats.completion_events_delivered++;
            if (replay_retire) {
                uint16_t slot;
                uint16_t replay_ready = 0;

                entry->state = UB_ASYNC_LOAD_PLT_REPLAY_READY;
                event->flags |= UB_ASYNC_LOAD_EVENT_FLAG_REPLAY_RETIRE;
                for (slot = 0;
                     slot < async_load->config.pending_load_entries; slot++) {
                    replay_ready +=
                        async_load->plt[slot].state ==
                        UB_ASYNC_LOAD_PLT_REPLAY_READY;
                }
                async_load->stats.replay_ready_high_water = MAX(
                    async_load->stats.replay_ready_high_water, replay_ready);
                return true;
            }
        }
        ub_async_load_recycle_plt(async_load, entry);
    }
    return true;
}

bool ub_async_load_event_pending(const UbAsyncLoad *async_load)
{
    return async_load && async_load->event_count;
}

bool ub_async_load_replay_expected(const UbAsyncLoad *async_load,
                              uint64_t context_id)
{
    uint16_t slot;

    if (!async_load || !context_id) {
        return false;
    }
    for (slot = 0; slot < async_load->config.pending_load_entries; slot++) {
        const UbAsyncLoadPltEntry *entry = &async_load->plt[slot];

        if (entry->state == UB_ASYNC_LOAD_PLT_REPLAY_READY &&
            entry->context_id == context_id) {
            return true;
        }
    }
    return false;
}

static bool ub_async_load_replay_load_matches(
    const UbAsyncLoadDesc *expected, const UbAsyncLoadDesc *actual)
{
    return expected->fault_pc == actual->fault_pc &&
        expected->effective_va == actual->effective_va &&
        expected->map_id == actual->map_id &&
        expected->map_generation == actual->map_generation &&
        expected->map_model_generation == actual->map_model_generation &&
        expected->remote_offset == actual->remote_offset &&
        expected->rt == actual->rt &&
        expected->access_bytes == actual->access_bytes &&
        expected->mmu_index == actual->mmu_index &&
        expected->sign_extend == actual->sign_extend &&
        expected->big_endian == actual->big_endian;
}

UbAsyncLoadReplayResult ub_async_load_replay_consume(
    UbAsyncLoad *async_load, uint64_t context_id, const UbAsyncLoadDesc *load,
    uint64_t *value)
{
    UbAsyncLoadPltEntry *entry = NULL;
    uint16_t slot;

    if (!async_load || !context_id || !load || !value || async_load->fail_stop) {
        return UB_ASYNC_LOAD_REPLAY_NONE;
    }
    for (slot = 0; slot < async_load->config.pending_load_entries; slot++) {
        if (async_load->plt[slot].state == UB_ASYNC_LOAD_PLT_REPLAY_READY &&
            async_load->plt[slot].context_id == context_id) {
            if (entry) {
                async_load->stats.replay_mismatch++;
                async_load->fail_stop = true;
                return UB_ASYNC_LOAD_REPLAY_MISMATCH;
            }
            entry = &async_load->plt[slot];
        }
    }
    if (!entry) {
        return UB_ASYNC_LOAD_REPLAY_NONE;
    }
    if (!ub_async_load_replay_load_matches(&entry->load, load)) {
        async_load->stats.replay_mismatch++;
        async_load->fail_stop = true;
        return UB_ASYNC_LOAD_REPLAY_MISMATCH;
    }
    *value = ub_async_load_load_value(entry);
    async_load->stats.replay_consumed++;
    ub_async_load_recycle_plt(async_load, entry);
    return UB_ASYNC_LOAD_REPLAY_CONSUMED;
}

void ub_async_load_record_direct_upcall(UbAsyncLoad *async_load)
{
    if (async_load) {
        async_load->stats.direct_upcalls++;
    }
}

void ub_async_load_mark_fail_stop(UbAsyncLoad *async_load)
{
    if (async_load) {
        async_load->fail_stop = true;
    }
}

bool ub_async_load_fail_stop(const UbAsyncLoad *async_load)
{
    return !async_load || async_load->fail_stop;
}

uint16_t ub_async_load_pending_count(const UbAsyncLoad *async_load)
{
    return async_load ? async_load->pending_count : 0;
}

const UbAsyncLoadStats *ub_async_load_stats(const UbAsyncLoad *async_load)
{
    return async_load ? &async_load->stats : NULL;
}

bool ub_async_load_logical_remote_ordinal(uint16_t context_slot,
                                     uint16_t context_count,
                                     uint64_t remote_local_ordinal,
                                     uint64_t *operation_ordinal)
{
    if (!context_count || context_slot >= context_count ||
        !operation_ordinal ||
        remote_local_ordinal >
            (UINT64_MAX - context_slot) / context_count) {
        return false;
    }
    *operation_ordinal = remote_local_ordinal * context_count +
        context_slot;
    return true;
}

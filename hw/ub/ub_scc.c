/*
 * Provider-neutral pending-load and EL0-upcall event model.
 */

#include "qemu/osdep.h"
#include "hw/ub/ub_scc.h"

#define SCC_MAX_CLOCK_MHZ 100000U

typedef struct ObmmSccPltEntry {
    uint32_t generation;
    ObmmSccPltState state;
    uint64_t context_id;
    ObmmSccLoadDesc load;
    ObmmSccLoadStatus status;
    uint64_t complete_cycle;
    uint8_t bytes_done;
    uint8_t payload[8];
} ObmmSccPltEntry;

struct ObmmScc {
    uint16_t owner_id;
    uint32_t owner_generation;
    ObmmSccConfig config;
    ObmmSccPltEntry plt[OBMM_SCC_MAX_PENDING_LOADS];
    ObmmSccEvent events[OBMM_SCC_MAX_EVENTS];
    uint16_t event_head;
    uint16_t event_tail;
    uint16_t event_count;
    uint16_t pending_count;
    uint64_t next_event_sequence;
    bool fail_stop;
    ObmmSccStats stats;
};

bool obmm_scc_config_parse(const char *spec, bool *enabled,
                           ObmmSccConfig *config)
{
    unsigned int values[5];
    int consumed = 0;

    if (!enabled || !config) {
        return false;
    }
    *enabled = false;
    *config = (ObmmSccConfig) {
        .context_entries = 64,
        .pending_load_entries = 64,
        .event_queue_depth = 128,
        .clock_mhz = 2000,
    };
    if (!spec || !*spec) {
        return true;
    }
    if (sscanf(spec,
               "v2|enabled=%u|contexts=%u|pending=%u|events=%u|"
               "clock_mhz=%u%n",
               &values[0], &values[1], &values[2], &values[3],
               &values[4], &consumed) != 5 || spec[consumed] != '\0' ||
        values[0] > 1 || values[1] == 0 ||
        values[1] > OBMM_SCC_MAX_CONTEXTS || values[2] == 0 ||
        values[2] > OBMM_SCC_MAX_PENDING_LOADS || values[3] == 0 ||
        values[3] > OBMM_SCC_MAX_EVENTS || values[4] == 0 ||
        values[4] > SCC_MAX_CLOCK_MHZ) {
        return false;
    }
    *enabled = values[0];
    *config = (ObmmSccConfig) {
        .context_entries = values[1],
        .pending_load_entries = values[2],
        .event_queue_depth = values[3],
        .clock_mhz = values[4],
    };
    return true;
}

uint64_t obmm_scc_context_id_make(uint32_t generation,
                                  uint16_t home_core, uint16_t slot)
{
    return (uint64_t)generation << 32 | (uint64_t)home_core << 16 | slot;
}

uint32_t obmm_scc_context_id_generation(uint64_t context_id)
{
    return context_id >> 32;
}

uint16_t obmm_scc_context_id_home_core(uint64_t context_id)
{
    return context_id >> 16;
}

uint16_t obmm_scc_context_id_slot(uint64_t context_id)
{
    return context_id;
}

static uint32_t obmm_scc_next_generation(uint32_t generation)
{
    generation++;
    return generation ? generation : 1;
}

static bool obmm_scc_config_valid(const ObmmSccConfig *config)
{
    return config && config->context_entries > 0 &&
        config->context_entries <= OBMM_SCC_MAX_CONTEXTS &&
        config->pending_load_entries > 0 &&
        config->pending_load_entries <= OBMM_SCC_MAX_PENDING_LOADS &&
        config->event_queue_depth > 0 &&
        config->event_queue_depth <= OBMM_SCC_MAX_EVENTS &&
        config->clock_mhz > 0;
}

ObmmScc *obmm_scc_new(uint16_t owner_id, uint32_t owner_generation,
                      const ObmmSccConfig *config)
{
    ObmmScc *scc;
    uint16_t slot;

    if (!owner_id || !owner_generation || !obmm_scc_config_valid(config)) {
        return NULL;
    }
    scc = g_new0(ObmmScc, 1);
    scc->owner_id = owner_id;
    scc->owner_generation = owner_generation;
    scc->config = *config;
    scc->next_event_sequence = 1;
    for (slot = 0; slot < config->pending_load_entries; slot++) {
        scc->plt[slot].generation = 1;
    }
    return scc;
}

void obmm_scc_free(ObmmScc *scc)
{
    g_free(scc);
}

static ObmmSccPltToken obmm_scc_plt_token(const ObmmScc *scc,
                                          uint16_t slot)
{
    return (ObmmSccPltToken) {
        .generation = scc->plt[slot].generation,
        .owner_id = scc->owner_id,
        .slot = slot,
    };
}

static ObmmSccPltEntry *obmm_scc_find_plt(ObmmScc *scc,
                                          ObmmSccPltToken token)
{
    ObmmSccPltEntry *entry;

    if (!scc || token.owner_id != scc->owner_id ||
        token.slot >= scc->config.pending_load_entries) {
        return NULL;
    }
    entry = &scc->plt[token.slot];
    if (entry->state == OBMM_SCC_PLT_FREE ||
        entry->generation != token.generation) {
        return NULL;
    }
    return entry;
}

static bool obmm_scc_event_push(ObmmScc *scc, ObmmSccEventKind kind,
                                const ObmmSccPltEntry *entry,
                                ObmmSccPltToken token,
                                ObmmSccLoadStatus status,
                                uint64_t value, uint64_t guest_cycle)
{
    ObmmSccEvent *event;

    if (scc->event_count == scc->config.event_queue_depth) {
        scc->stats.event_overflow++;
        scc->fail_stop = true;
        return false;
    }
    if (kind == OBMM_SCC_EVENT_PENDING) {
        scc->event_head = (scc->event_head +
                           scc->config.event_queue_depth - 1) %
            scc->config.event_queue_depth;
        event = &scc->events[scc->event_head];
    } else {
        event = &scc->events[scc->event_tail];
        scc->event_tail = (scc->event_tail + 1) %
            scc->config.event_queue_depth;
    }
    *event = (ObmmSccEvent) {
        .context_id = entry->context_id,
        .plt_token = token,
        .fault_pc = entry->load.fault_pc,
        .effective_va = entry->load.effective_va,
        .value = value,
        .kind = kind,
        .status = status,
        .rt = entry->load.rt,
        .access_bytes = entry->load.access_bytes,
        .guest_cycle = guest_cycle,
    };
    scc->event_count++;
    scc->stats.event_high_water = MAX(scc->stats.event_high_water,
                                      scc->event_count);
    return true;
}

static void obmm_scc_recycle_plt(ObmmScc *scc, ObmmSccPltEntry *entry)
{
    uint32_t generation = obmm_scc_next_generation(entry->generation);

    memset(entry, 0, sizeof(*entry));
    entry->generation = generation;
    scc->pending_count--;
}

static uint64_t obmm_scc_load_value(const ObmmSccPltEntry *entry)
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

static bool obmm_scc_load_valid(const ObmmSccLoadDesc *load)
{
    return load && load->fault_pc <= UINT64_MAX - 4 && load->rt <= 31 &&
        !load->sign_extend &&
        (load->access_bytes == 1 || load->access_bytes == 2 ||
         load->access_bytes == 4 || load->access_bytes == 8);
}

ObmmSccPendingResult obmm_scc_load_pending(
    ObmmScc *scc, uint64_t context_id, const ObmmSccLoadDesc *load,
    ObmmSccPltToken *plt_token)
{
    ObmmSccPltEntry *entry = NULL;
    uint16_t slot;

    if (!scc || !context_id || !plt_token ||
        !obmm_scc_load_valid(load) || scc->fail_stop) {
        return OBMM_SCC_PENDING_INVALID;
    }
    if (scc->pending_count == scc->config.pending_load_entries ||
        scc->event_count == scc->config.event_queue_depth) {
        scc->stats.capacity_stalls++;
        return OBMM_SCC_PENDING_SYNC_STALL;
    }
    for (slot = 0; slot < scc->config.pending_load_entries; slot++) {
        if (scc->plt[slot].state == OBMM_SCC_PLT_FREE) {
            entry = &scc->plt[slot];
            break;
        }
    }
    g_assert(entry);
    entry->state = OBMM_SCC_PLT_PENDING;
    entry->context_id = context_id;
    entry->load = *load;
    *plt_token = obmm_scc_plt_token(scc, slot);
    scc->pending_count++;
    scc->stats.pending_loads++;
    scc->stats.pending_high_water = MAX(scc->stats.pending_high_water,
                                        scc->pending_count);
    if (!obmm_scc_event_push(scc, OBMM_SCC_EVENT_PENDING, entry,
                             *plt_token, OBMM_SCC_LOAD_SUCCESS, 0,
                             load->submit_cycle)) {
        obmm_scc_recycle_plt(scc, entry);
        return OBMM_SCC_PENDING_INVALID;
    }
    return OBMM_SCC_PENDING_ACCEPTED;
}

ObmmSccCompletionResult obmm_scc_load_complete(
    ObmmScc *scc, ObmmSccPltToken plt_token, ObmmSccLoadStatus status,
    const void *payload, uint8_t bytes_done, uint64_t complete_cycle)
{
    ObmmSccPltEntry *entry = obmm_scc_find_plt(scc, plt_token);
    uint64_t value = 0;

    if (!entry) {
        if (scc) {
            scc->stats.stale_completions++;
        }
        return OBMM_SCC_COMPLETION_STALE;
    }
    if (entry->state != OBMM_SCC_PLT_PENDING) {
        scc->stats.duplicate_completions++;
        return OBMM_SCC_COMPLETION_DUPLICATE;
    }
    entry->complete_cycle = complete_cycle;
    if (status == OBMM_SCC_LOAD_SUCCESS && payload &&
        bytes_done == entry->load.access_bytes) {
        memcpy(entry->payload, payload, bytes_done);
        entry->bytes_done = bytes_done;
        entry->status = status;
        entry->state = OBMM_SCC_PLT_COMPLETE;
        value = obmm_scc_load_value(entry);
        scc->stats.completed_loads++;
        obmm_scc_event_push(scc, OBMM_SCC_EVENT_COMPLETE, entry,
                            plt_token, status, value, complete_cycle);
    } else {
        if (status == OBMM_SCC_LOAD_SUCCESS) {
            status = OBMM_SCC_LOAD_INTERNAL;
        }
        entry->status = status;
        entry->state = OBMM_SCC_PLT_FAULTED;
        scc->stats.faulted_loads++;
        obmm_scc_event_push(scc, OBMM_SCC_EVENT_FAULT, entry,
                            plt_token, status, 0, complete_cycle);
    }
    return OBMM_SCC_COMPLETION_ACCEPTED;
}

bool obmm_scc_event_pop(ObmmScc *scc, ObmmSccEvent *event)
{
    ObmmSccPltEntry *entry;

    if (!scc || !event || !scc->event_count) {
        return false;
    }
    *event = scc->events[scc->event_head];
    scc->event_head = (scc->event_head + 1) %
        scc->config.event_queue_depth;
    scc->event_count--;
    event->sequence = scc->next_event_sequence++;
    if (!scc->next_event_sequence) {
        scc->next_event_sequence = 1;
    }
    if (event->kind == OBMM_SCC_EVENT_COMPLETE ||
        event->kind == OBMM_SCC_EVENT_FAULT) {
        entry = obmm_scc_find_plt(scc, event->plt_token);
        if (!entry ||
            (entry->state != OBMM_SCC_PLT_COMPLETE &&
             entry->state != OBMM_SCC_PLT_FAULTED)) {
            scc->fail_stop = true;
            return true;
        }
        if (event->kind == OBMM_SCC_EVENT_COMPLETE) {
            scc->stats.completion_events_delivered++;
        }
        obmm_scc_recycle_plt(scc, entry);
    }
    return true;
}

bool obmm_scc_event_pending(const ObmmScc *scc)
{
    return scc && scc->event_count;
}

void obmm_scc_record_direct_upcall(ObmmScc *scc)
{
    if (scc) {
        scc->stats.direct_upcalls++;
    }
}

void obmm_scc_mark_fail_stop(ObmmScc *scc)
{
    if (scc) {
        scc->fail_stop = true;
    }
}

bool obmm_scc_fail_stop(const ObmmScc *scc)
{
    return !scc || scc->fail_stop;
}

uint16_t obmm_scc_pending_count(const ObmmScc *scc)
{
    return scc ? scc->pending_count : 0;
}

const ObmmSccStats *obmm_scc_stats(const ObmmScc *scc)
{
    return scc ? &scc->stats : NULL;
}

bool obmm_scc_logical_remote_ordinal(uint16_t context_slot,
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

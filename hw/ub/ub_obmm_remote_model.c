/*
 * Deterministic OBMM remote-memory latency and failure model.
 */

#include "qemu/osdep.h"
#include "hw/ub/ub_obmm_remote_model.h"
#include "qapi/qmp/qbool.h"
#include "qapi/qmp/qdict.h"
#include "qapi/qmp/qjson.h"
#include "qapi/qmp/qnum.h"
#include "qapi/qmp/qstring.h"
#include "qemu/cutils.h"

#define FNV1A_OFFSET_BASIS 0xcbf29ce484222325ULL
#define FNV1A_PRIME 0x00000100000001b3ULL
#define OUTCOME_LANE 0x243f6a8885a308d3ULL
#define JITTER_LANE 0x13198a2e03707344ULL
#define TAIL_LANE 0xa4093822299f31d0ULL
#define DUPLICATE_LANE 0x082efa98ec4e6c89ULL
#define REORDER_LANE 0x452821e638d01377ULL
#define DUPLICATE_REORDER_XOR 0xd1310ba698dfb5acULL

struct UbObmmRemoteQueuedEvent {
    bool active;
    bool primary_published;
    uint64_t model_accept_ns;
    uint64_t model_due_ns;
    UbObmmRemoteDecision decision;
    UbObmmRemoteDueFn due;
    UbObmmRemotePublishFn publish;
    UbObmmRemoteDestroyFn destroy;
    void *opaque;
};

typedef struct UbObmmRemoteDueAction {
    uint32_t event_index;
    uint64_t due_ns;
    uint64_t reorder_key;
    bool duplicate;
} UbObmmRemoteDueAction;

static bool ub_obmm_model_get_u64(const QDict *dict, const char *key,
                                  uint64_t *value)
{
    QNum *number = qobject_to(QNum, qdict_get(dict, key));

    return number && qnum_get_try_uint(number, value);
}

static bool ub_obmm_model_get_u32(const QDict *dict, const char *key,
                                  uint32_t *value)
{
    uint64_t parsed;

    if (!ub_obmm_model_get_u64(dict, key, &parsed) || parsed > UINT32_MAX) {
        return false;
    }
    *value = parsed;
    return true;
}

static bool ub_obmm_model_get_bool(const QDict *dict, const char *key,
                                   bool *value)
{
    QBool *boolean = qobject_to(QBool, qdict_get(dict, key));

    if (!boolean) {
        return false;
    }
    *value = qbool_get_bool(boolean);
    return true;
}

static uint64_t ub_obmm_fnv1a(const uint8_t *data, size_t length)
{
    uint64_t hash = FNV1A_OFFSET_BASIS;
    size_t index;

    for (index = 0; index < length; index++) {
        hash ^= data[index];
        hash *= FNV1A_PRIME;
    }
    return hash;
}

static void ub_obmm_fnv1a_u64(uint64_t *hash, uint64_t value)
{
    unsigned int index;

    for (index = 0; index < sizeof(value); index++) {
        *hash ^= (value >> (index * 8)) & 0xff;
        *hash *= FNV1A_PRIME;
    }
}

static void ub_obmm_fnv1a_u32(uint64_t *hash, uint32_t value)
{
    unsigned int index;

    for (index = 0; index < sizeof(value); index++) {
        *hash ^= (value >> (index * 8)) & 0xff;
        *hash *= FNV1A_PRIME;
    }
}

static uint64_t ub_obmm_splitmix64(uint64_t value)
{
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

static uint64_t ub_obmm_lane_draw(uint64_t operation_key, uint64_t lane)
{
    return ub_obmm_splitmix64(operation_key ^ lane);
}

static bool ub_obmm_model_validate(const UbObmmRemoteModelConfig *config,
                                   Error **errp)
{
    if (config->fixed_latency_ns > UB_OBMM_REMOTE_MODEL_MAX_LATENCY_NS ||
        config->tail_extra_latency_ns > UB_OBMM_REMOTE_MODEL_MAX_LATENCY_NS ||
        config->duplicate_delay_ns > UB_OBMM_REMOTE_MODEL_MAX_LATENCY_NS) {
        error_setg(errp, "OBMM remote model latency exceeds maximum");
        return false;
    }
    if (config->jitter_max_abs_ns >
        config->fixed_latency_ns + UB_OBMM_REMOTE_MODEL_MAX_LATENCY_NS) {
        error_setg(errp, "OBMM remote model jitter exceeds maximum");
        return false;
    }
    if (config->jitter_mode == UB_OBMM_REMOTE_JITTER_NONE &&
        config->jitter_max_abs_ns != 0) {
        error_setg(errp, "OBMM remote model none jitter must be zero");
        return false;
    }
    if (config->queue_depth == 0 ||
        config->queue_depth > UB_OBMM_REMOTE_MODEL_MAX_QUEUE_DEPTH ||
        config->reorder_window == 0 ||
        config->reorder_window > config->queue_depth) {
        error_setg(errp, "OBMM remote model queue/reorder bounds are invalid");
        return false;
    }
    if (config->tail_probability_ppm > UB_OBMM_REMOTE_MODEL_PPM_SCALE ||
        config->drop_ppm > UB_OBMM_REMOTE_MODEL_PPM_SCALE ||
        config->error_ppm > UB_OBMM_REMOTE_MODEL_PPM_SCALE ||
        config->duplicate_ppm > UB_OBMM_REMOTE_MODEL_PPM_SCALE ||
        config->drop_ppm + config->error_ppm >
        UB_OBMM_REMOTE_MODEL_PPM_SCALE) {
        error_setg(errp, "OBMM remote model ppm values are invalid");
        return false;
    }
    return true;
}

static GString *ub_obmm_model_canonical_json(
    const char *scenario_name, uint64_t scenario_seed,
    const UbObmmRemoteModelConfig *config)
{
    g_autoptr(QString) name = qstring_from_str(scenario_name);
    g_autoptr(GString) name_json = qobject_to_json(QOBJECT(name));
    const char *jitter_mode = config->jitter_mode ==
        UB_OBMM_REMOTE_JITTER_UNIFORM ? "uniform" : "none";
    GString *json = g_string_new(NULL);

    g_string_append_printf(
        json,
        "{\"schema\":%u,\"scenario_name\":%s,\"scenario_seed\":%" PRIu64
        ",\"remote_memory_model\":{\"enabled\":%s,"
        "\"time_source\":\"qemu_virtual\",\"fixed_latency_ns\":%" PRIu64
        ",\"jitter\":{\"mode\":\"%s\",\"max_abs_ns\":%" PRIu64
        "},\"tail\":{\"probability_ppm\":%u,\"extra_latency_ns\":%" PRIu64
        "},\"queue_depth\":%u,\"reorder_window\":%u,\"drop_ppm\":%u,"
        "\"error_ppm\":%u,\"duplicate_ppm\":%u,"
        "\"duplicate_delay_ns\":%" PRIu64 ",\"seed\":%" PRIu64 "}}",
        UB_OBMM_REMOTE_MODEL_SCHEMA, name_json->str, scenario_seed,
        config->enabled ? "true" : "false", config->fixed_latency_ns,
        jitter_mode, config->jitter_max_abs_ns,
        config->tail_probability_ppm, config->tail_extra_latency_ns,
        config->queue_depth, config->reorder_window, config->drop_ppm,
        config->error_ppm, config->duplicate_ppm,
        config->duplicate_delay_ns, config->seed);
    return json;
}

static bool ub_obmm_model_parse_config(const QDict *root,
                                       UbObmmRemoteModelConfig *config,
                                       const char **scenario_name,
                                       uint64_t *scenario_seed,
                                       const char **manifest_hash,
                                       Error **errp)
{
    QDict *model;
    QDict *jitter;
    QDict *tail;
    const char *time_source;
    const char *jitter_mode;
    uint64_t schema;

    if (qdict_size(root) != 5 ||
        !ub_obmm_model_get_u64(root, "schema", &schema) ||
        schema != UB_OBMM_REMOTE_MODEL_SCHEMA ||
        !ub_obmm_model_get_u64(root, "scenario_seed", scenario_seed)) {
        error_setg(errp, "invalid OBMM remote model manifest header");
        return false;
    }
    *scenario_name = qdict_get_try_str(root, "scenario_name");
    *manifest_hash = qdict_get_try_str(root, "manifest_hash");
    model = qobject_to(QDict, qdict_get(root, "remote_memory_model"));
    if (!*scenario_name || !**scenario_name || !*manifest_hash || !model ||
        qdict_size(model) != 12) {
        error_setg(errp, "invalid OBMM remote model manifest fields");
        return false;
    }

    time_source = qdict_get_try_str(model, "time_source");
    jitter = qobject_to(QDict, qdict_get(model, "jitter"));
    tail = qobject_to(QDict, qdict_get(model, "tail"));
    if (!time_source || strcmp(time_source, "qemu_virtual") != 0 ||
        !jitter || qdict_size(jitter) != 2 ||
        !tail || qdict_size(tail) != 2 ||
        !ub_obmm_model_get_bool(model, "enabled", &config->enabled) ||
        !ub_obmm_model_get_u64(model, "fixed_latency_ns",
                               &config->fixed_latency_ns) ||
        !ub_obmm_model_get_u64(jitter, "max_abs_ns",
                               &config->jitter_max_abs_ns) ||
        !ub_obmm_model_get_u32(tail, "probability_ppm",
                               &config->tail_probability_ppm) ||
        !ub_obmm_model_get_u64(tail, "extra_latency_ns",
                               &config->tail_extra_latency_ns) ||
        !ub_obmm_model_get_u32(model, "queue_depth",
                               &config->queue_depth) ||
        !ub_obmm_model_get_u32(model, "reorder_window",
                               &config->reorder_window) ||
        !ub_obmm_model_get_u32(model, "drop_ppm", &config->drop_ppm) ||
        !ub_obmm_model_get_u32(model, "error_ppm", &config->error_ppm) ||
        !ub_obmm_model_get_u32(model, "duplicate_ppm",
                               &config->duplicate_ppm) ||
        !ub_obmm_model_get_u64(model, "duplicate_delay_ns",
                               &config->duplicate_delay_ns) ||
        !ub_obmm_model_get_u64(model, "seed", &config->seed)) {
        error_setg(errp, "invalid OBMM remote model configuration types");
        return false;
    }

    jitter_mode = qdict_get_try_str(jitter, "mode");
    if (jitter_mode && strcmp(jitter_mode, "none") == 0) {
        config->jitter_mode = UB_OBMM_REMOTE_JITTER_NONE;
    } else if (jitter_mode && strcmp(jitter_mode, "uniform") == 0) {
        config->jitter_mode = UB_OBMM_REMOTE_JITTER_UNIFORM;
    } else {
        error_setg(errp, "invalid OBMM remote model jitter mode");
        return false;
    }
    return ub_obmm_model_validate(config, errp);
}

void ub_obmm_remote_model_init(UbObmmRemoteModelState *model)
{
    memset(model, 0, sizeof(*model));
    model->config.queue_depth = 64;
    model->config.reorder_window = 1;
    model->config.duplicate_delay_ns = 1000;
    model->config.seed = 1;
}

bool ub_obmm_remote_model_load(UbObmmRemoteModelState *model,
                               const char *path, Error **errp)
{
    g_autofree char *contents = NULL;
    g_autoptr(GString) canonical = NULL;
    QObject *object = NULL;
    Error *local_error = NULL;
    QDict *root;
    const char *scenario_name;
    const char *manifest_hash;
    uint64_t scenario_seed;
    uint64_t expected_hash;
    uint64_t actual_hash;
    bool loaded = false;

    if (!path || !*path) {
        error_setg(errp, "OBMM remote model manifest path is empty");
        return false;
    }
    if (!g_file_get_contents(path, &contents, NULL, NULL)) {
        error_setg(errp, "cannot read OBMM remote model manifest '%s'", path);
        return false;
    }
    object = qobject_from_json(contents, &local_error);
    if (!object) {
        error_propagate_prepend(errp, local_error,
                                "decode OBMM remote model manifest: ");
        local_error = NULL;
        goto out;
    }
    root = qobject_to(QDict, object);
    if (!root || !ub_obmm_model_parse_config(root, &model->config,
                                              &scenario_name, &scenario_seed,
                                              &manifest_hash, errp)) {
        goto out;
    }
    if (sscanf(manifest_hash, "fnv1a64:%16" SCNx64, &expected_hash) != 1 ||
        strlen(manifest_hash) != strlen("fnv1a64:0000000000000000")) {
        error_setg(errp, "invalid OBMM remote model manifest hash '%s'",
                   manifest_hash);
        goto out;
    }
    canonical = ub_obmm_model_canonical_json(scenario_name, scenario_seed,
                                              &model->config);
    actual_hash = ub_obmm_fnv1a((const uint8_t *)canonical->str,
                                canonical->len);
    if (actual_hash != expected_hash) {
        error_setg(errp,
                   "OBMM remote model manifest hash mismatch: "
                   "expected=%016" PRIx64
                   " actual=%016" PRIx64,
                   expected_hash, actual_hash);
        goto out;
    }

    pstrcpy(model->manifest_hash, sizeof(model->manifest_hash), manifest_hash);
    model->loaded = true;
    loaded = true;

out:
    error_free(local_error);
    qobject_unref(object);
    return loaded;
}

void ub_obmm_remote_model_cleanup(UbObmmRemoteModelState *model)
{
    uint32_t index;

    if (!model) {
        return;
    }
    for (index = 0; index < model->event_capacity; index++) {
        UbObmmRemoteQueuedEvent *event = &model->events[index];

        if (event->active && event->destroy) {
            event->destroy(event->opaque);
        }
    }
    g_free(model->events);
    model->events = NULL;
    model->event_capacity = 0;
    model->pending = 0;
}

uint64_t ub_obmm_remote_operation_key(
    const UbObmmRemoteModelConfig *config,
    const UbObmmRemoteOperation *operation)
{
    uint64_t hash = FNV1A_OFFSET_BASIS;

    ub_obmm_fnv1a_u64(&hash, config->seed);
    ub_obmm_fnv1a_u64(&hash, operation->map_id);
    ub_obmm_fnv1a_u64(&hash, operation->map_generation);
    ub_obmm_fnv1a_u64(&hash, operation->remote_offset);
    ub_obmm_fnv1a_u32(&hash, operation->length);
    ub_obmm_fnv1a_u64(&hash, operation->per_range_ordinal);
    return hash;
}

UbObmmRemoteDecision ub_obmm_remote_model_decide(
    const UbObmmRemoteModelConfig *config,
    const UbObmmRemoteOperation *operation)
{
    UbObmmRemoteDecision decision = { 0 };
    uint64_t draw;
    __int128 service_ns;

    decision.operation_key = ub_obmm_remote_operation_key(config, operation);
    decision.reorder_key = ub_obmm_lane_draw(decision.operation_key,
                                              REORDER_LANE);
    if (!config->enabled) {
        decision.outcome = UB_OBMM_REMOTE_SUCCESS;
        return decision;
    }

    draw = ub_obmm_lane_draw(decision.operation_key, OUTCOME_LANE) %
        UB_OBMM_REMOTE_MODEL_PPM_SCALE;
    if (draw < config->drop_ppm) {
        decision.outcome = UB_OBMM_REMOTE_DROP;
    } else if (draw < config->drop_ppm + config->error_ppm) {
        decision.outcome = UB_OBMM_REMOTE_ERROR;
    } else {
        decision.outcome = UB_OBMM_REMOTE_SUCCESS;
    }

    if (config->jitter_max_abs_ns != 0) {
        uint64_t span = config->jitter_max_abs_ns * 2 + 1;

        draw = ub_obmm_lane_draw(decision.operation_key, JITTER_LANE);
        decision.jitter_ns = (int64_t)(draw % span) -
            (int64_t)config->jitter_max_abs_ns;
    }
    draw = ub_obmm_lane_draw(decision.operation_key, TAIL_LANE) %
        UB_OBMM_REMOTE_MODEL_PPM_SCALE;
    decision.tail_applied = draw < config->tail_probability_ppm;
    service_ns = (__int128)config->fixed_latency_ns + decision.jitter_ns;
    if (decision.tail_applied) {
        service_ns += config->tail_extra_latency_ns;
    }
    decision.service_ns = service_ns > 0 ? (uint64_t)service_ns : 0;

    draw = ub_obmm_lane_draw(decision.operation_key, DUPLICATE_LANE) %
        UB_OBMM_REMOTE_MODEL_PPM_SCALE;
    decision.duplicate = decision.outcome == UB_OBMM_REMOTE_SUCCESS &&
        draw < config->duplicate_ppm;
    if (decision.duplicate) {
        decision.duplicate_delay_ns = config->duplicate_delay_ns;
    }
    return decision;
}

bool ub_obmm_remote_model_try_accept(UbObmmRemoteModelState *model,
                                     const UbObmmRemoteOperation *operation,
                                     UbObmmRemoteDecision *decision)
{
    if (model->pending >= model->config.queue_depth) {
        model->capacity_rejected++;
        return false;
    }
    model->pending++;
    model->accepted++;
    *decision = ub_obmm_remote_model_decide(&model->config, operation);
    if (UINT64_MAX - model->total_service_ns < decision->service_ns) {
        model->total_service_ns = UINT64_MAX;
    } else {
        model->total_service_ns += decision->service_ns;
    }
    if (decision->outcome == UB_OBMM_REMOTE_DROP) {
        model->dropped++;
    } else if (decision->outcome == UB_OBMM_REMOTE_ERROR) {
        model->errored++;
    }
    if (decision->duplicate) {
        model->duplicated++;
    }
    return true;
}

bool ub_obmm_remote_model_release(UbObmmRemoteModelState *model)
{
    if (model->pending == 0) {
        return false;
    }
    model->pending--;
    model->completed++;
    return true;
}

static bool ub_obmm_remote_model_prepare_events(
    UbObmmRemoteModelState *model)
{
    if (model->event_capacity == model->config.queue_depth) {
        return true;
    }
    if (model->pending != 0) {
        return false;
    }
    g_free(model->events);
    model->events = g_new0(UbObmmRemoteQueuedEvent,
                           model->config.queue_depth);
    model->event_capacity = model->config.queue_depth;
    return true;
}

static uint64_t ub_obmm_remote_model_add_ns(uint64_t base, uint64_t delta)
{
    return delta > UINT64_MAX - base ? UINT64_MAX : base + delta;
}

bool ub_obmm_remote_model_enqueue(
    UbObmmRemoteModelState *model, const UbObmmRemoteOperation *operation,
    uint64_t model_accept_ns, UbObmmRemoteDueFn due,
    UbObmmRemotePublishFn publish,
    UbObmmRemoteDestroyFn destroy, void *opaque,
    UbObmmRemoteDecision *decision)
{
    UbObmmRemoteQueuedEvent *event = NULL;
    uint32_t index;

    if (!model || !operation || !publish || !decision ||
        !ub_obmm_remote_model_prepare_events(model) ||
        !ub_obmm_remote_model_try_accept(model, operation, decision)) {
        return false;
    }
    for (index = 0; index < model->event_capacity; index++) {
        if (!model->events[index].active) {
            event = &model->events[index];
            break;
        }
    }
    if (!event) {
        ub_obmm_remote_model_release(model);
        model->capacity_rejected++;
        return false;
    }

    *event = (UbObmmRemoteQueuedEvent) {
        .active = true,
        .model_accept_ns = model_accept_ns,
        .model_due_ns = ub_obmm_remote_model_add_ns(
            model_accept_ns, decision->service_ns),
        .decision = *decision,
        .due = due,
        .publish = publish,
        .destroy = destroy,
        .opaque = opaque,
    };
    return true;
}

uint64_t ub_obmm_remote_model_next_due_ns(
    const UbObmmRemoteModelState *model)
{
    uint64_t next_due = UINT64_MAX;
    uint32_t index;

    if (!model) {
        return next_due;
    }
    for (index = 0; index < model->event_capacity; index++) {
        const UbObmmRemoteQueuedEvent *event = &model->events[index];
        uint64_t due_ns;

        if (!event->active) {
            continue;
        }
        due_ns = event->primary_published ?
            ub_obmm_remote_model_add_ns(
                event->model_due_ns, event->decision.duplicate_delay_ns) :
            event->model_due_ns;
        next_due = MIN(next_due, due_ns);
    }
    return next_due;
}

static int ub_obmm_remote_due_compare(const void *left, const void *right)
{
    const UbObmmRemoteDueAction *a = left;
    const UbObmmRemoteDueAction *b = right;

    if (a->due_ns != b->due_ns) {
        return a->due_ns < b->due_ns ? -1 : 1;
    }
    if (a->reorder_key != b->reorder_key) {
        return a->reorder_key < b->reorder_key ? -1 : 1;
    }
    return a->event_index < b->event_index ? -1 :
        a->event_index != b->event_index;
}

static int ub_obmm_remote_reorder_compare(const void *left,
                                          const void *right)
{
    const UbObmmRemoteDueAction *a = left;
    const UbObmmRemoteDueAction *b = right;

    if (a->reorder_key != b->reorder_key) {
        return a->reorder_key < b->reorder_key ? -1 : 1;
    }
    return a->event_index < b->event_index ? -1 :
        a->event_index != b->event_index;
}

static void ub_obmm_remote_model_reorder_actions(
    UbObmmRemoteDueAction *actions, uint32_t count, uint32_t window)
{
    uint32_t start;

    qsort(actions, count, sizeof(*actions), ub_obmm_remote_due_compare);
    for (start = 0; start < count; start += window) {
        uint32_t batch = MIN(window, count - start);

        qsort(actions + start, batch, sizeof(*actions),
              ub_obmm_remote_reorder_compare);
    }
}

uint32_t ub_obmm_remote_model_run_due(UbObmmRemoteModelState *model,
                                      uint64_t model_now_ns)
{
    g_autofree UbObmmRemoteDueAction *actions = NULL;
    uint32_t action_count = 0;
    uint32_t index;

    if (!model || model->event_capacity == 0) {
        return 0;
    }
    actions = g_new0(UbObmmRemoteDueAction, model->event_capacity);
    for (index = 0; index < model->event_capacity; index++) {
        UbObmmRemoteQueuedEvent *event = &model->events[index];
        uint64_t due_ns;

        if (!event->active) {
            continue;
        }
        due_ns = event->primary_published ?
            ub_obmm_remote_model_add_ns(
                event->model_due_ns, event->decision.duplicate_delay_ns) :
            event->model_due_ns;
        if (due_ns > model_now_ns) {
            continue;
        }
        actions[action_count++] = (UbObmmRemoteDueAction) {
            .event_index = index,
            .due_ns = due_ns,
            .reorder_key = event->decision.reorder_key ^
                (event->primary_published ? DUPLICATE_REORDER_XOR : 0),
            .duplicate = event->primary_published,
        };
    }
    ub_obmm_remote_model_reorder_actions(actions, action_count,
                                          model->config.reorder_window);

    for (index = 0; index < action_count; index++) {
        UbObmmRemoteDueAction *action = &actions[index];
        UbObmmRemoteQueuedEvent *event =
            &model->events[action->event_index];
        bool keep_for_duplicate;

        if (!event->active || event->primary_published != action->duplicate) {
            continue;
        }
        if (!action->duplicate && event->due) {
            event->due(event->opaque, &event->decision,
                       event->model_accept_ns, action->due_ns);
        }
        if (event->decision.outcome != UB_OBMM_REMOTE_DROP) {
            event->publish(event->opaque, &event->decision,
                           action->duplicate, event->model_accept_ns,
                           action->due_ns, model_now_ns);
            model->published++;
            if (action->duplicate) {
                model->duplicate_published++;
            } else {
                uint64_t elapsed = model_now_ns >= event->model_accept_ns ?
                    model_now_ns - event->model_accept_ns : 0;

                if (UINT64_MAX - model->total_accept_to_publish_ns <
                    elapsed) {
                    model->total_accept_to_publish_ns = UINT64_MAX;
                } else {
                    model->total_accept_to_publish_ns += elapsed;
                }
            }
        }
        keep_for_duplicate = !action->duplicate &&
            event->decision.outcome == UB_OBMM_REMOTE_SUCCESS &&
            event->decision.duplicate;
        if (keep_for_duplicate) {
            event->primary_published = true;
            continue;
        }
        if (event->destroy) {
            event->destroy(event->opaque);
        }
        memset(event, 0, sizeof(*event));
        ub_obmm_remote_model_release(model);
    }
    return action_count;
}

bool ub_obmm_remote_model_reset_stats(UbObmmRemoteModelState *model)
{
    if (!model || model->pending != 0) {
        return false;
    }
    model->accepted = 0;
    model->capacity_rejected = 0;
    model->completed = 0;
    model->dropped = 0;
    model->errored = 0;
    model->duplicated = 0;
    model->published = 0;
    model->duplicate_published = 0;
    model->total_service_ns = 0;
    model->total_accept_to_publish_ns = 0;
    return true;
}

/*
 * GSVA route management.
 *
 * GSVA map/unmap/query route tables, VA range lookup,
 * token metadata, retired-object tombstones.
 */

#include "qemu/osdep.h"
#include "hw/ub/gsva_route.h"
#include "qemu/log.h"
#include "qemu/module.h"

static void gsva_route_finalize(Object *obj)
{
    GsvaRouteEntry *route = (GsvaRouteEntry *)obj;

    assert(!route->cpu_window_mapped);
    if (route->cpu_window_initialized) {
        object_unparent(OBJECT(&route->cpu_window));
    }
}

static const TypeInfo gsva_route_type = {
    .name = TYPE_GSVA_ROUTE,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(GsvaRouteEntry),
    .instance_finalize = gsva_route_finalize,
};

static void gsva_route_register_types(void)
{
    type_register_static(&gsva_route_type);
}

type_init(gsva_route_register_types)

void gsva_route_init_cpu_window(GsvaRouteEntry *route,
                                const MemoryRegionOps *ops)
{
    assert(!route->cpu_window_initialized);
    memory_region_init_io(&route->cpu_window, OBJECT(route), ops, route,
                          "gsva-cpu-window", route->key.size);
    route->cpu_window_initialized = true;
}

static void gsva_route_detach_cpu_window(GsvaRouteEntry *route)
{
    route->state = GSVA_ROUTE_RETIRED;
    if (route->cpu_window_mapped) {
        memory_region_del_subregion(route->cpu_window.container,
                                    &route->cpu_window);
        route->cpu_window_mapped = false;
    }
}

void gsva_route_table_init(GsvaRouteTable *tbl)
{
    QTAILQ_INIT(&tbl->routes);
    QTAILQ_INIT(&tbl->tombstones);
    tbl->next_map_id = 1;
    tbl->route_count = 0;
    tbl->tombstone_count = 0;
}

void gsva_route_table_destroy(GsvaRouteTable *tbl)
{
    GsvaRouteEntry *entry;
    while ((entry = QTAILQ_FIRST(&tbl->routes)) != NULL) {
        QTAILQ_REMOVE(&tbl->routes, entry, next);
        gsva_route_detach_cpu_window(entry);
        object_unref(OBJECT(entry));
        tbl->route_count--;
    }
    while ((entry = QTAILQ_FIRST(&tbl->tombstones)) != NULL) {
        QTAILQ_REMOVE(&tbl->tombstones, entry, next);
        object_unref(OBJECT(entry));
        tbl->tombstone_count--;
    }
}

int gsva_route_map(GsvaRouteTable *tbl, const GsvaKeyV1 *key,
                   uint64_t local_pa, uint64_t local_va, uint64_t remote_uba,
                   uint32_t source, uint32_t address_profile,
                   uint32_t home_cna,
                   uint32_t token_id, uint32_t token_value,
                   uint32_t access_flags,
                   uint64_t *map_id_out)
{
    GsvaRouteEntry *entry;
    GsvaRouteEntry *existing;

    if (!tbl || !key || !map_id_out) {
        return GSVA_ERR_BAD_VERSION;
    }

    /* Validate key */
    int kerr = gsva_key_validate(key);
    if (kerr != GSVA_OK) {
        return kerr;
    }

    /* Strict GSVA: local_va == home_va == remote_uba */
    if (address_profile == GSVA_ADDRESS_PROFILE_STRICT_GSVA) {
        if (local_va != key->home_va) {
            qemu_log("GSVA_ROUTE: strict address violation: "
                     "local_va=%" PRIx64 " != home_va=%" PRIx64 "\n",
                     local_va, key->home_va);
            return GSVA_ERR_STRICT_ADDRESS;
        }
        if (remote_uba != key->home_va) {
            qemu_log("GSVA_ROUTE: strict address violation: "
                     "remote_uba=%" PRIx64 " != home_va=%" PRIx64 "\n",
                     remote_uba, key->home_va);
            return GSVA_ERR_STRICT_ADDRESS;
        }
    }

    /* Check for overlapping existing route */
    QTAILQ_FOREACH(existing, &tbl->routes, next) {
        /* Quarantined routes still own their interval until unmap completes. */
        if (existing->state == GSVA_ROUTE_ACTIVE ||
            existing->state == GSVA_ROUTE_STALE) {
            uint64_t ex_start = existing->key.home_va;
            uint64_t ex_end = ex_start + existing->key.size;
            uint64_t new_start = key->home_va;
            uint64_t new_end = new_start + key->size;
            if (new_start < ex_end && new_end > ex_start) {
                qemu_log("GSVA_ROUTE: overlap detected: new [%#" PRIx64
                         ", %#" PRIx64 ") vs existing [%#" PRIx64
                         ", %#" PRIx64 ")\n",
                         new_start, new_end, ex_start, ex_end);
                return GSVA_ERR_KEY_MISMATCH;
            }
        }
    }

    /* Check tombstone for stale epoch reuse */
    QTAILQ_FOREACH(existing, &tbl->tombstones, next) {
        if (gsva_key_base_equal(&existing->key, key)) {
            if (key->epoch <= existing->key.epoch) {
                qemu_log("GSVA_ROUTE: stale epoch on tombstone: "
                         "new epoch=%" PRIu64 " <= old epoch=%" PRIu64 "\n",
                         key->epoch, existing->key.epoch);
                return GSVA_ERR_STALE_EPOCH;
            }
            /* New epoch is higher -- remove tombstone, allow reuse */
            QTAILQ_REMOVE(&tbl->tombstones, existing, next);
            object_unref(OBJECT(existing));
            tbl->tombstone_count--;
            break;
        }
    }

    entry = (GsvaRouteEntry *)object_new(TYPE_GSVA_ROUTE);
    entry->key = *key;
    entry->state = GSVA_ROUTE_ACTIVE;
    entry->local_pa = local_pa;
    entry->local_va = local_va;
    entry->remote_uba = remote_uba;
    entry->source = source;
    entry->address_profile = address_profile;
    entry->home_cna = home_cna;
    entry->owner_cna = home_cna;
    entry->map_id = tbl->next_map_id++;

    /* Token lease */
    entry->token.token_id = token_id;
    entry->token.token_value = token_value ? token_value : token_id;
    entry->token.access_flags = access_flags;
    entry->token.lease_epoch = 1;
    entry->token.allowed_cna_bitmap = 0;
    entry->token.state = (entry->token.token_id != 0 &&
                          entry->token.token_value != 0) ?
                         GSVA_TOKEN_ACTIVE : GSVA_TOKEN_INVALID;
    entry->token.active = (entry->token.state == GSVA_TOKEN_ACTIVE);

    QTAILQ_INSERT_TAIL(&tbl->routes, entry, next);
    tbl->route_count++;

    *map_id_out = entry->map_id;

    qemu_log("GSVA_MAP: map_id=%" PRIu64 " segment_id=%#" PRIx64
             " home_va=%#" PRIx64 " size=%#" PRIx64
             " epoch=%" PRIu64 " p_tag=%" PRIu32
             " cache_policy=%" PRIu32 " source=%" PRIu32
             " profile=%" PRIu32 "\n",
             entry->map_id, key->segment_id, key->home_va, key->size,
             key->epoch, key->p_tag, key->cache_policy, source,
             address_profile);

    return GSVA_OK;
}

int gsva_route_unmap(GsvaRouteTable *tbl, uint64_t map_id, bool keep_tombstone)
{
    GsvaRouteEntry *entry;

    if (!tbl) {
        return GSVA_ERR_BAD_VERSION;
    }

    QTAILQ_FOREACH(entry, &tbl->routes, next) {
        if (entry->map_id == map_id) {
            QTAILQ_REMOVE(&tbl->routes, entry, next);
            tbl->route_count--;
            gsva_route_detach_cpu_window(entry);

            qemu_log("GSVA_UNMAP: map_id=%" PRIu64 " segment_id=%#" PRIx64
                     " home_va=%#" PRIx64 " epoch=%" PRIu64
                     " tombstone=%s\n",
                     map_id, entry->key.segment_id, entry->key.home_va,
                     entry->key.epoch, keep_tombstone ? "yes" : "no");

            if (keep_tombstone) {
                entry->state = GSVA_ROUTE_RETIRED;
                QTAILQ_INSERT_TAIL(&tbl->tombstones, entry, next);
                tbl->tombstone_count++;
            } else {
                object_unref(OBJECT(entry));
            }
            return GSVA_OK;
        }
    }

    QTAILQ_FOREACH(entry, &tbl->tombstones, next) {
        if (entry->map_id == map_id) {
            qemu_log("GSVA_UNMAP: map_id=%" PRIu64
                     " already tombstoned segment_id=%#" PRIx64
                     " home_va=%#" PRIx64 " epoch=%" PRIu64 "\n",
                     map_id, entry->key.segment_id, entry->key.home_va,
                     entry->key.epoch);
            return GSVA_OK;
        }
    }

    qemu_log("GSVA_UNMAP: map_id=%" PRIu64 " not found\n", map_id);
    return GSVA_ERR_ROUTE_MISSING;
}

GsvaRouteEntry *gsva_route_lookup_va(GsvaRouteTable *tbl,
                                     uint64_t vmid, uint64_t asid,
                                     uint64_t va)
{
    GsvaRouteEntry *entry;

    if (!tbl) {
        return NULL;
    }

    QTAILQ_FOREACH(entry, &tbl->routes, next) {
        if (entry->state != GSVA_ROUTE_ACTIVE) {
            continue;
        }
        if (entry->key.vmid != vmid || entry->key.asid != asid) {
            continue;
        }
        if (va >= entry->key.home_va &&
            va < entry->key.home_va + entry->key.size) {
            return entry;
        }
    }
    return NULL;
}

int gsva_route_resolve_pto(GsvaRouteTable *tbl, uint64_t local_pa,
                           uint64_t length, uint32_t requester_cna,
                           GsvaRouteAccess *access)
{
    GsvaRouteEntry *entry, *route = NULL;
    uint64_t offset;
    bool window_backing;

    if (!tbl || !access || !length || local_pa > UINT64_MAX - length) {
        return GSVA_ERR_KEY_MISMATCH;
    }
    QTAILQ_FOREACH(entry, &tbl->routes, next) {
        if (local_pa >= entry->local_pa &&
            local_pa - entry->local_pa < entry->key.size) {
            if (route) {
                return GSVA_ERR_KEY_MISMATCH;
            }
            route = entry;
        }
    }
    if (!route) {
        return GSVA_ERR_ROUTE_MISSING;
    }
    offset = local_pa - route->local_pa;
    if (gsva_key_validate(&route->key) != GSVA_OK || !route->key.epoch ||
        route->local_pa > UINT64_MAX - route->key.size ||
        route->key.home_va > UINT64_MAX - route->key.size ||
        length > route->key.size - offset) {
        return GSVA_ERR_KEY_MISMATCH;
    }
    window_backing = route->cpu_window_initialized &&
                     route->cpu_window_mapped;
    if (route->address_profile != GSVA_ADDRESS_PROFILE_STRICT_GSVA ||
        route->local_va != route->key.home_va ||
        route->remote_uba != route->key.home_va ||
        !route->backing_token_id || !route->map_id ||
        route->direct_home_backing == window_backing) {
        return GSVA_ERR_FEATURE_MISSING;
    }
    if (route->state != GSVA_ROUTE_ACTIVE ||
        route->token.state != GSVA_TOKEN_ACTIVE || !route->token.active ||
        route->token.pending_token_value || !route->token.lease_epoch ||
        !route->home_cna || !route->owner_cna || !requester_cna ||
        !route->token.access_flags || (route->token.access_flags & ~3u) ||
        (route->token.allowed_cna_bitmap && requester_cna >= 64) ||
        gsva_route_validate_token(route, requester_cna,
                                  route->token.token_id,
                                  route->token.token_value, 0) != GSVA_OK) {
        return GSVA_ERR_TOKEN_DENIED;
    }
    *access = (GsvaRouteAccess) {
        .key = route->key,
        .map_id = route->map_id,
        .local_pa = route->local_pa,
        .lease_epoch = route->token.lease_epoch,
        .allowed_cna_bitmap = route->token.allowed_cna_bitmap,
        .home_cna = route->home_cna,
        .owner_cna = route->owner_cna,
        .source = route->source,
        .token_id = route->token.token_id,
        .token_value = route->token.token_value,
        .access_flags = route->token.access_flags,
        .token_flags = route->token.flags,
        .backing_token_id = route->backing_token_id,
        .direct_home_backing = route->direct_home_backing,
    };
    return GSVA_OK;
}

bool gsva_route_access_equal(const GsvaRouteAccess *a,
                              const GsvaRouteAccess *b)
{
    return a && b && gsva_key_base_equal(&a->key, &b->key) &&
           a->key.version == b->key.version && a->key.flags == b->key.flags &&
           a->key.size == b->key.size && a->key.epoch == b->key.epoch &&
           a->map_id == b->map_id && a->local_pa == b->local_pa &&
           a->lease_epoch == b->lease_epoch &&
           a->allowed_cna_bitmap == b->allowed_cna_bitmap &&
           a->home_cna == b->home_cna && a->owner_cna == b->owner_cna &&
           a->source == b->source && a->token_id == b->token_id &&
           a->token_value == b->token_value &&
           a->access_flags == b->access_flags &&
           a->token_flags == b->token_flags &&
           a->backing_token_id == b->backing_token_id &&
           a->direct_home_backing == b->direct_home_backing;
}

GsvaRouteEntry *gsva_route_lookup_base(GsvaRouteTable *tbl,
                                       const GsvaKeyV1 *key)
{
    GsvaRouteEntry *entry;

    if (!tbl || !key) {
        return NULL;
    }

    QTAILQ_FOREACH(entry, &tbl->routes, next) {
        if (gsva_key_base_equal(&entry->key, key)) {
            return entry;
        }
    }
    return NULL;
}

GsvaRouteEntry *gsva_route_lookup_tombstone(GsvaRouteTable *tbl,
                                            const GsvaKeyV1 *key)
{
    GsvaRouteEntry *entry;

    if (!tbl || !key) {
        return NULL;
    }

    QTAILQ_FOREACH(entry, &tbl->tombstones, next) {
        if (gsva_key_base_equal(&entry->key, key)) {
            return entry;
        }
    }
    return NULL;
}

GsvaRouteEntry *gsva_route_lookup_home_va(GsvaRouteTable *tbl,
                                          uint64_t va, uint64_t len)
{
    GsvaRouteEntry *entry;

    if (!tbl) {
        return NULL;
    }

    QTAILQ_FOREACH(entry, &tbl->routes, next) {
        if (entry->state != GSVA_ROUTE_ACTIVE) {
            continue;
        }
        if (va >= entry->key.home_va &&
            va + len <= entry->key.home_va + entry->key.size) {
            return entry;
        }
    }
    return NULL;
}

int gsva_route_validate_token(const GsvaRouteEntry *route,
                              uint32_t requester_cna,
                              uint32_t token_id, uint32_t token_value,
                              uint32_t access_type)
{
    if (!route) {
        return GSVA_ERR_ROUTE_MISSING;
    }
    if (route->state != GSVA_ROUTE_ACTIVE) {
        return GSVA_ERR_ROUTE_MISSING;
    }

    /* No token required for this route (INVALID state = no token enforced). */
    if (route->token.state == GSVA_TOKEN_INVALID) {
        return GSVA_OK;
    }

    /* Token must be ACTIVE */
    if (route->token.state != GSVA_TOKEN_ACTIVE) {
        return GSVA_ERR_TOKEN_DENIED;
    }

    /* token_id/value must be non-zero for a protected route */
    if (route->token.token_id == 0 || route->token.token_value == 0 ||
        token_id == 0 || token_value == 0) {
        return GSVA_ERR_TOKEN_DENIED;
    }

    /* Exact token match */
    if (route->token.token_id != token_id) {
        return GSVA_ERR_TOKEN_DENIED;
    }
    if (route->token.token_value != token_value) {
        return GSVA_ERR_TOKEN_DENIED;
    }

    /* allowed_cna_bitmap: 0 = any CNA allowed */
    if (route->token.allowed_cna_bitmap != 0) {
        if (!(route->token.allowed_cna_bitmap & (1ULL << requester_cna))) {
            return GSVA_ERR_TOKEN_DENIED;
        }
    }

    /* access_flags: bit 0 = read, bit 1 = write. 0 = full access (default open) */
    if (route->token.access_flags != 0) {
        if ((access_type & 1) && !(route->token.access_flags & 1)) {
            return GSVA_ERR_TOKEN_DENIED;
        }
        if ((access_type & 2) && !(route->token.access_flags & 2)) {
            return GSVA_ERR_TOKEN_DENIED;
        }
    }

    return GSVA_OK;
}

int gsva_route_rotate_token(GsvaRouteTable *tbl, const GsvaKeyV1 *key,
                            uint32_t token_id, uint32_t new_token_value)
{
    GsvaRouteEntry *entry;

    if (!tbl || !key) {
        return GSVA_ERR_BAD_VERSION;
    }
    if (token_id == 0 || new_token_value == 0) {
        return GSVA_ERR_TOKEN_DENIED;
    }

    QTAILQ_FOREACH(entry, &tbl->routes, next) {
        if (entry->state != GSVA_ROUTE_ACTIVE) {
            continue;
        }
        if (gsva_key_base_equal(&entry->key, key)) {
            if (memcmp(&entry->key, key, sizeof(*key)) != 0) {
                return GSVA_ERR_STALE_EPOCH;
            }
            if (!entry->token.active ||
                entry->token.state != GSVA_TOKEN_ACTIVE ||
                entry->token.token_id != token_id) {
                return GSVA_ERR_TOKEN_DENIED;
            }
            entry->token.state = GSVA_TOKEN_REVOKING;
            entry->token.lease_epoch++;
            entry->token.pending_token_value = new_token_value;
            entry->token.active = false;
            qemu_log("GSVA_ROUTE: token revoke pending segment_id=%#" PRIx64
                     " token_id=%" PRIu32 " lease_epoch=%" PRIu64 "\n",
                     key->segment_id, token_id, entry->token.lease_epoch);
            return GSVA_OK;
        }
    }
    return GSVA_ERR_ROUTE_MISSING;
}

int gsva_route_ack_token_revoke(GsvaRouteTable *tbl, const GsvaKeyV1 *key,
                                uint32_t token_id,
                                uint32_t new_token_value,
                                uint32_t requester_cna)
{
    GsvaRouteEntry *entry;

    if (!tbl || !key) {
        return GSVA_ERR_BAD_VERSION;
    }
    if (token_id == 0 || new_token_value == 0) {
        return GSVA_ERR_TOKEN_DENIED;
    }

    QTAILQ_FOREACH(entry, &tbl->routes, next) {
        if (entry->state != GSVA_ROUTE_ACTIVE) {
            continue;
        }
        if (gsva_key_base_equal(&entry->key, key)) {
            if (memcmp(&entry->key, key, sizeof(*key)) != 0) {
                return GSVA_ERR_STALE_EPOCH;
            }
            if (entry->token.active &&
                entry->token.state == GSVA_TOKEN_ACTIVE &&
                entry->token.token_id == token_id &&
                entry->token.token_value == new_token_value &&
                entry->token.pending_token_value == 0) {
                return GSVA_OK;
            }
            if (entry->token.state != GSVA_TOKEN_REVOKING ||
                entry->token.token_id != token_id ||
                entry->token.pending_token_value != new_token_value) {
                return GSVA_ERR_TOKEN_DENIED;
            }
            entry->token.token_value = new_token_value;
            entry->token.pending_token_value = 0;
            entry->token.state = GSVA_TOKEN_ACTIVE;
            entry->token.active = true;
            qemu_log("GSVA_ROUTE: token revoke ack segment_id=%#" PRIx64
                     " token_id=%" PRIu32 " cna=%" PRIu32
                     " lease_epoch=%" PRIu64 "\n",
                     key->segment_id, token_id, requester_cna,
                     entry->token.lease_epoch);
            return GSVA_OK;
        }
    }

    return GSVA_ERR_ROUTE_MISSING;
}

int gsva_route_abort_token_revoke(GsvaRouteTable *tbl,
                                  const GsvaKeyV1 *key,
                                  uint32_t token_id,
                                  uint32_t pending_token_value)
{
    GsvaRouteEntry *entry;

    if (!tbl || !key || !token_id || !pending_token_value) {
        return GSVA_ERR_BAD_VERSION;
    }
    QTAILQ_FOREACH(entry, &tbl->routes, next) {
        if (entry->state != GSVA_ROUTE_ACTIVE ||
            !gsva_key_base_equal(&entry->key, key)) {
            continue;
        }
        if (memcmp(&entry->key, key, sizeof(*key)) != 0) {
            return GSVA_ERR_STALE_EPOCH;
        }
        if (entry->token.state != GSVA_TOKEN_REVOKING ||
            entry->token.token_id != token_id ||
            entry->token.pending_token_value != pending_token_value) {
            return GSVA_ERR_TOKEN_DENIED;
        }
        entry->token.pending_token_value = 0;
        entry->token.state = GSVA_TOKEN_ACTIVE;
        entry->token.active = true;
        return GSVA_OK;
    }
    return GSVA_ERR_ROUTE_MISSING;
}

void gsva_route_get_stats(GsvaRouteTable *tbl,
                          uint64_t *map_total, uint64_t *unmap_total,
                          uint64_t *lookup_total, uint64_t *miss_total)
{
    if (!tbl) {
        return;
    }
    if (map_total) {
        *map_total = tbl->next_map_id - 1;
    }
    if (unmap_total) {
        *unmap_total = tbl->tombstone_count;
    }
    if (lookup_total) {
        *lookup_total = 0;
    }
    if (miss_total) {
        *miss_total = 0;
    }
}

/*
 * GSVA route management.
 *
 * GSVA map/unmap/query route tables, VA range lookup,
 * token metadata, retired-object tombstones.
 */

#include "qemu/osdep.h"
#include "hw/ub/gsva_route.h"
#include "qemu/log.h"

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
        g_free(entry);
        tbl->route_count--;
    }
    while ((entry = QTAILQ_FIRST(&tbl->tombstones)) != NULL) {
        QTAILQ_REMOVE(&tbl->tombstones, entry, next);
        g_free(entry);
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
    if (address_profile == 1 /* GSVA_ADDRESS_PROFILE_STRICT_GSVA */) {
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
        if (existing->state == GSVA_ROUTE_ACTIVE) {
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
            g_free(existing);
            tbl->tombstone_count--;
            break;
        }
    }

    entry = g_new0(GsvaRouteEntry, 1);
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
                g_free(entry);
            }
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

    /* No token required for this route (INVALID state = no token enforced) */
    if (route->token.state == GSVA_TOKEN_INVALID || !route->token.active) {
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
            if (!entry->token.active ||
                entry->token.state != GSVA_TOKEN_ACTIVE ||
                entry->token.token_id != token_id) {
                return GSVA_ERR_TOKEN_DENIED;
            }
            entry->token.state = GSVA_TOKEN_REVOKING;
            entry->token.lease_epoch++;
            entry->token.token_value = new_token_value;
            entry->token.state = GSVA_TOKEN_ACTIVE;
            qemu_log("GSVA_ROUTE: token rotated segment_id=%#" PRIx64
                     " token_id=%" PRIu32 " lease_epoch=%" PRIu64 "\n",
                     key->segment_id, token_id, entry->token.lease_epoch);
            return GSVA_OK;
        }
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

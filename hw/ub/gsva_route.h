/*
 * GSVA route management -- header.
 *
 * Owns: GSVA map/unmap/query route tables, VA range lookup,
 *       token metadata, retired-object tombstones.
 */

#ifndef GSVA_ROUTE_H
#define GSVA_ROUTE_H

#include "qemu/osdep.h"
#include "qemu/queue.h"
#include "hw/ub/gsva_key.h"
#include <stdint.h>
#include <stdbool.h>

typedef enum GsvaRouteState {
    GSVA_ROUTE_CREATING = 0,
    GSVA_ROUTE_ACTIVE = 1,
    GSVA_ROUTE_STALE = 2,
    GSVA_ROUTE_RETIRED = 3,
} GsvaRouteState;

/* Token lease state in route */
typedef struct GsvaTokenLease {
    uint32_t token_id;
    uint32_t token_value;
    uint32_t access_flags;
    uint32_t flags;
    uint64_t lease_epoch;
    bool active;
} GsvaTokenLease;

/* GSVA route entry */
typedef struct GsvaRouteEntry {
    GsvaKeyV1 key;
    GsvaRouteState state;
    uint64_t local_pa;
    uint64_t local_va;
    uint64_t remote_uba;
    uint32_t source;
    uint32_t address_profile;
    uint32_t home_cna;
    uint32_t owner_cna;
    GsvaTokenLease token;
    uint64_t map_id;
    QTAILQ_ENTRY(GsvaRouteEntry) next;
} GsvaRouteEntry;

/* GSVA route table */
typedef struct GsvaRouteTable {
    QTAILQ_HEAD(, GsvaRouteEntry) routes;
    QTAILQ_HEAD(, GsvaRouteEntry) tombstones;
    uint64_t next_map_id;
    int route_count;
    int tombstone_count;
} GsvaRouteTable;

/* Initialize route table */
void gsva_route_table_init(GsvaRouteTable *tbl);

/* Destroy route table and free all entries */
void gsva_route_table_destroy(GsvaRouteTable *tbl);

/* Map a new GSVA route. Returns 0 on success, negative error on failure. */
int gsva_route_map(GsvaRouteTable *tbl, const GsvaKeyV1 *key,
                   uint64_t local_pa, uint64_t local_va, uint64_t remote_uba,
                   uint32_t source, uint32_t address_profile,
                   uint32_t home_cna,
                   uint32_t token_id, uint32_t token_value,
                   uint32_t access_flags,
                   uint64_t *map_id_out);

/* Unmap a GSVA route by map_id. If keep_tombstone is true, retain tombstone. */
int gsva_route_unmap(GsvaRouteTable *tbl, uint64_t map_id, bool keep_tombstone);

/* Lookup route by VA range. Returns entry or NULL. */
GsvaRouteEntry *gsva_route_lookup_va(GsvaRouteTable *tbl,
                                     uint64_t vmid, uint64_t asid,
                                     uint64_t va);

/* Lookup route by base identity. Returns entry or NULL. */
GsvaRouteEntry *gsva_route_lookup_base(GsvaRouteTable *tbl,
                                       const GsvaKeyV1 *key);

/* Lookup tombstone by base identity. Returns entry or NULL. */
GsvaRouteEntry *gsva_route_lookup_tombstone(GsvaRouteTable *tbl,
                                            const GsvaKeyV1 *key);

/* Validate token for a route. Returns GSVA_OK or GSVA_ERR_TOKEN_DENIED. */
int gsva_route_validate_token(const GsvaRouteEntry *route,
                              uint32_t requester_cna,
                              uint32_t token_id, uint32_t token_value,
                              uint32_t access_type);

/* Get GSVA route stats */
void gsva_route_get_stats(GsvaRouteTable *tbl,
                          uint64_t *map_total, uint64_t *unmap_total,
                          uint64_t *lookup_total, uint64_t *miss_total);

#endif /* GSVA_ROUTE_H */

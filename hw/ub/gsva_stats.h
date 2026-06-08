/*
 * GSVA statistics -- header.
 *
 * Owns: counters for GSVA map/unmap/lookup/coherence operations.
 */

#ifndef GSVA_STATS_H
#define GSVA_STATS_H

#include "qemu/osdep.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct GsvaStats {
    uint64_t map_total;
    uint64_t map_fail;
    uint64_t unmap_total;
    uint64_t unmap_fail;
    uint64_t lookup_total;
    uint64_t lookup_miss;
    uint64_t read_acquire_total;
    uint64_t read_acquire_fail;
    uint64_t write_acquire_total;
    uint64_t write_acquire_fail;
    uint64_t retire_total;
    uint64_t retire_fail;
    uint64_t coh_objects;
    uint64_t route_objects;
} GsvaStats;

void gsva_stats_init(GsvaStats *st);
void gsva_stats_map(GsvaStats *st, bool ok);
void gsva_stats_unmap(GsvaStats *st, bool ok);
void gsva_stats_lookup(GsvaStats *st, bool hit);
void gsva_stats_read_acquire(GsvaStats *st, bool ok);
void gsva_stats_write_acquire(GsvaStats *st, bool ok);
void gsva_stats_retire(GsvaStats *st, bool ok);
void gsva_stats_set_objects(GsvaStats *st, uint64_t routes, uint64_t coh);

#endif /* GSVA_STATS_H */

/*
 * GSVA statistics.
 */

#include "qemu/osdep.h"
#include "hw/ub/gsva_stats.h"

void gsva_stats_init(GsvaStats *st)
{
    memset(st, 0, sizeof(*st));
}

void gsva_stats_map(GsvaStats *st, bool ok)
{
    st->map_total++;
    if (!ok) {
        st->map_fail++;
    }
}

void gsva_stats_unmap(GsvaStats *st, bool ok)
{
    st->unmap_total++;
    if (!ok) {
        st->unmap_fail++;
    }
}

void gsva_stats_lookup(GsvaStats *st, bool hit)
{
    st->lookup_total++;
    if (!hit) {
        st->lookup_miss++;
    }
}

void gsva_stats_read_acquire(GsvaStats *st, bool ok)
{
    st->read_acquire_total++;
    if (!ok) {
        st->read_acquire_fail++;
    }
}

void gsva_stats_write_acquire(GsvaStats *st, bool ok)
{
    st->write_acquire_total++;
    if (!ok) {
        st->write_acquire_fail++;
    }
}

void gsva_stats_retire(GsvaStats *st, bool ok)
{
    st->retire_total++;
    if (!ok) {
        st->retire_fail++;
    }
}

void gsva_stats_set_objects(GsvaStats *st, uint64_t routes, uint64_t coh)
{
    st->route_objects = routes;
    st->coh_objects = coh;
}

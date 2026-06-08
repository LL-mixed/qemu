/*
 * GSVA coherence -- header.
 *
 * Owns: GSVA object state, ReadAcquire/WriteAcquire,
 *       revoke/invalidate/downgrade/writeback/fence orchestration,
 *       pending sequence handling, timeout terminal state.
 */

#ifndef GSVA_COHERENCE_H
#define GSVA_COHERENCE_H

#include "qemu/osdep.h"
#include "qemu/queue.h"
#include "hw/ub/gsva_key.h"
#include "hw/ub/gsva_route.h"
#include <stdint.h>
#include <stdbool.h>

typedef enum GsvaCohState {
    GSVA_COH_I = 0,
    GSVA_COH_S = 1,
    GSVA_COH_E = 2,
    GSVA_COH_M = 3,
    GSVA_COH_RETIRED = 4,
} GsvaCohState;

/* GSVA coherence object state */
typedef struct GsvaCohObject {
    GsvaKeyV1 key;
    GsvaCohState state;
    uint32_t home_cna;
    uint32_t owner_cna;
    uint64_t sharer_bitmap;
    uint64_t epoch;
    bool pending;
    uint64_t pending_seq;
    uint32_t pending_op;
    uint32_t pending_target;
    uint64_t pending_ack_bitmap;
    uint64_t map_id;
    QTAILQ_ENTRY(GsvaCohObject) next;
} GsvaCohObject;

/* GSVA coherence table */
typedef struct GsvaCohTable {
    QTAILQ_HEAD(, GsvaCohObject) objects;
    uint64_t next_seq;
    int object_count;
} GsvaCohTable;

/* Initialize coherence table */
void gsva_coh_table_init(GsvaCohTable *tbl);

/* Destroy coherence table */
void gsva_coh_table_destroy(GsvaCohTable *tbl);

/* Create coherence object for a route. Returns 0 on success. */
int gsva_coh_object_create(GsvaCohTable *tbl, const GsvaKeyV1 *key,
                           uint32_t home_cna, uint64_t map_id);

/* Remove coherence object. Returns 0 on success. */
int gsva_coh_object_remove(GsvaCohTable *tbl, const GsvaKeyV1 *key);

/* Lookup coherence object by key. Returns object or NULL. */
GsvaCohObject *gsva_coh_lookup(GsvaCohTable *tbl, const GsvaKeyV1 *key);

/* ReadAcquire: acquire shared access. Returns GSVA_OK or error. */
int gsva_coh_read_acquire(GsvaCohTable *tbl, const GsvaKeyV1 *key,
                          uint32_t requester_cna);

/* WriteAcquire: acquire exclusive/modified access. Returns GSVA_OK or error. */
int gsva_coh_write_acquire(GsvaCohTable *tbl, const GsvaKeyV1 *key,
                           uint32_t requester_cna);

/* Retire: start retire transaction. Returns GSVA_OK or error. */
int gsva_coh_retire(GsvaCohTable *tbl, const GsvaKeyV1 *key,
                    uint32_t requester_cna);

/* Get object state as string */
const char *gsva_coh_state_name(GsvaCohState state);

#endif /* GSVA_COHERENCE_H */

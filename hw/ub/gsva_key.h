/*
 * GSVA key validation module -- header.
 *
 * Owns: ABI validation, key/base-identity comparison,
 *       containment checks, stable error-name helpers.
 */

#ifndef GSVA_KEY_H
#define GSVA_KEY_H

#include "qemu/osdep.h"
#include <stdint.h>
#include <stdbool.h>

/* GSVA error codes */
#define GSVA_OK                     0
#define GSVA_ERR_BAD_VERSION        (-1)
#define GSVA_ERR_KEY_MISMATCH       (-2)
#define GSVA_ERR_STALE_EPOCH        (-3)
#define GSVA_ERR_TOKEN_DENIED       (-4)
#define GSVA_ERR_ROUTE_MISSING      (-5)
#define GSVA_ERR_COH_PENDING        (-6)
#define GSVA_ERR_COH_TIMEOUT        (-7)
#define GSVA_ERR_TLB_STALE          (-8)
#define GSVA_ERR_SEGMENT_RETIRED    (-9)
#define GSVA_ERR_UNSUPPORTED_POLICY (-10)
#define GSVA_ERR_STRICT_ADDRESS     (-11)
#define GSVA_ERR_FEATURE_MISSING    (-12)

/* GSVA key version 1 (wire format, matches UAPI gsva.h) */
typedef struct QEMU_PACKED GsvaKeyV1 {
    uint32_t version;
    uint32_t flags;
    uint64_t segment_id;
    uint64_t home_va;
    uint64_t size;
    uint64_t vmid;
    uint64_t asid;
    uint64_t pte_offset;
    uint32_t p_tag;
    uint32_t cache_policy;
    uint64_t epoch;
} GsvaKeyV1;

/* Validate gsva_key_v1 fields. Returns GSVA_OK or negative error. */
int gsva_key_validate(const GsvaKeyV1 *key);

/* Check containment: access_va + access_len within [home_va, home_va + size). */
bool gsva_key_contains(const GsvaKeyV1 *key, uint64_t access_va, uint64_t access_len);

/* Compare two keys for base-identity equality (ignoring epoch). */
bool gsva_key_base_equal(const GsvaKeyV1 *a, const GsvaKeyV1 *b);

/* Return stable error name string for a GSVA error code. */
const char *gsva_error_name(int error);

#endif /* GSVA_KEY_H */

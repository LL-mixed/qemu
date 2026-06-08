/*
 * GSVA key validation module.
 *
 * ABI validation, key comparison, containment checks,
 * and stable error-name helpers.
 */

#include "qemu/osdep.h"
#include "hw/ub/gsva_key.h"
#include "qemu/log.h"

/* ---- error names ---- */

static const char * const gsva_error_names[] = {
    "GSVA_OK",
    "GSVA_ERR_BAD_VERSION",
    "GSVA_ERR_KEY_MISMATCH",
    "GSVA_ERR_STALE_EPOCH",
    "GSVA_ERR_TOKEN_DENIED",
    "GSVA_ERR_ROUTE_MISSING",
    "GSVA_ERR_COH_PENDING",
    "GSVA_ERR_COH_TIMEOUT",
    "GSVA_ERR_TLB_STALE",
    "GSVA_ERR_SEGMENT_RETIRED",
    "GSVA_ERR_UNSUPPORTED_POLICY",
    "GSVA_ERR_STRICT_ADDRESS",
    "GSVA_ERR_FEATURE_MISSING",
};

#define GSVA_ERROR_COUNT ((int)(sizeof(gsva_error_names) / sizeof(gsva_error_names[0])))

const char *gsva_error_name(int error)
{
    if (error == 0) {
        return gsva_error_names[0];
    }
    if (error < 0 && -error < GSVA_ERROR_COUNT) {
        return gsva_error_names[-error];
    }
    return "GSVA_ERR_UNKNOWN";
}

/* ---- key validation ---- */

int gsva_key_validate(const GsvaKeyV1 *key)
{
    if (!key) {
        return GSVA_ERR_BAD_VERSION;
    }
    if (key->version != 1) {
        qemu_log("GSVA_KEY: bad version %" PRIu32 "\n", key->version);
        return GSVA_ERR_BAD_VERSION;
    }
    if (key->flags != 0) {
        qemu_log("GSVA_KEY: unsupported flags %" PRIu32 "\n", key->flags);
        return GSVA_ERR_BAD_VERSION;
    }
    if (key->segment_id == 0) {
        qemu_log("GSVA_KEY: segment_id must be non-zero\n");
        return GSVA_ERR_KEY_MISMATCH;
    }
    if (key->size == 0) {
        qemu_log("GSVA_KEY: size must be non-zero\n");
        return GSVA_ERR_KEY_MISMATCH;
    }
    return GSVA_OK;
}

/* ---- containment ---- */

bool gsva_key_contains(const GsvaKeyV1 *key, uint64_t access_va, uint64_t access_len)
{
    if (!key || access_len == 0) {
        return false;
    }
    if (access_va < key->home_va) {
        return false;
    }
    if (access_va + access_len > key->home_va + key->size) {
        return false;
    }
    return true;
}

/* ---- base identity comparison ---- */

bool gsva_key_base_equal(const GsvaKeyV1 *a, const GsvaKeyV1 *b)
{
    if (!a || !b) {
        return false;
    }
    return a->segment_id == b->segment_id
        && a->home_va == b->home_va
        && a->vmid == b->vmid
        && a->asid == b->asid
        && a->pte_offset == b->pte_offset
        && a->p_tag == b->p_tag
        && a->cache_policy == b->cache_policy;
}

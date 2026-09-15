#include "qemu/osdep.h"
#include "hw/ub/gsva_home.h"

struct GsvaHomeBinding {
    GsvaHomeRequest registration;
    uint32_t users;
    bool active;
    GsvaHomeBinding *next;
};

static bool valid_range(uint64_t address, uint64_t length)
{
    return length && length <= UINT64_MAX - address;
}

static bool overlaps(const GsvaKeyV1 *key, uint64_t address, uint64_t length)
{
    return address < key->home_va + key->size &&
        key->home_va < address + length;
}

static bool identity_equal(const GsvaHomeIdentity *a, const GsvaHomeIdentity *b)
{
    /* Packed, versioned value types have no implicit padding. */
    return !memcmp(a, b, sizeof(*a));
}

static int validate_identity(const GsvaHomeIdentity *identity, uint32_t cna)
{
    if (!identity || gsva_key_validate(&identity->key) != GSVA_OK ||
        !valid_range(identity->key.home_va, identity->key.size) ||
        !identity->key.epoch) {
        return GSVA_ERR_KEY_MISMATCH;
    }
    if (!cna || identity->home_cna != cna || !identity->token_id ||
        !identity->token_value || !identity->backing_token_id ||
        identity->backing_token_id == UINT32_MAX) {
        return GSVA_ERR_TOKEN_DENIED;
    }
    return GSVA_OK;
}

int gsva_home_update(GsvaHomeTable *table, uint32_t local_cna,
                     const GsvaHomeRequest *request)
{
    GsvaHomeBinding *binding, *exact = NULL;
    int error;

    if (!table || !request || request->version != 1 || request->reserved ||
        !request->export_mem_id ||
        !request->access_flags ||
        (request->access_flags & ~(GSVA_HOME_ACCESS_READ | GSVA_HOME_ACCESS_WRITE)) ||
        (request->operation != GSVA_HOME_BIND &&
         request->operation != GSVA_HOME_REVOKE)) {
        return GSVA_ERR_BAD_VERSION;
    }
    error = validate_identity(&request->identity, local_cna);
    if (error != GSVA_OK) {
        return error;
    }
    for (binding = table->bindings; binding; binding = binding->next) {
        if (identity_equal(&binding->registration.identity, &request->identity)) {
            exact = binding;
            break;
        }
    }
    if (exact) {
        if (exact->registration.export_mem_id != request->export_mem_id ||
            exact->registration.access_flags != request->access_flags) {
            return GSVA_ERR_KEY_MISMATCH;
        }
        if (request->operation == GSVA_HOME_REVOKE) {
            exact->active = false;
            return exact->users ? GSVA_ERR_COH_PENDING : GSVA_OK;
        }
        return exact->active ? GSVA_OK : GSVA_ERR_SEGMENT_RETIRED;
    }
    if (request->operation == GSVA_HOME_REVOKE) {
        return GSVA_ERR_ROUTE_MISSING;
    }
    for (binding = table->bindings; binding; binding = binding->next) {
        const GsvaKeyV1 *key = &binding->registration.identity.key;

        /* A segment identity has one export lifetime. Same-address reuse
         * requires a fresh segment, regardless of recycled backing tokens. */
        if (key->segment_id == request->identity.key.segment_id) {
            return GSVA_ERR_KEY_MISMATCH;
        }
        if ((binding->active || binding->users) &&
            overlaps(key, request->identity.key.home_va,
                     request->identity.key.size)) {
            return GSVA_ERR_COH_PENDING;
        }
    }
    binding = g_try_new0(GsvaHomeBinding, 1);
    if (!binding) {
        return GSVA_ERR_COH_PENDING;
    }
    binding->registration = *request;
    binding->active = true;
    binding->next = table->bindings;
    table->bindings = binding;
    return GSVA_OK;
}

int gsva_home_acquire(GsvaHomeTable *table, uint32_t local_cna,
                      const GsvaHomeIdentity *identity, uint64_t address,
                      uint64_t length, bool write, GsvaHomeBinding **pin)
{
    GsvaHomeBinding *binding;
    int error;

    if (!pin) {
        return GSVA_ERR_KEY_MISMATCH;
    }
    *pin = NULL;
    error = validate_identity(identity, local_cna);
    if (error != GSVA_OK) {
        return error;
    }
    if (!table || !valid_range(address, length) ||
        !gsva_key_contains(&identity->key, address, length)) {
        return GSVA_ERR_STRICT_ADDRESS;
    }
    for (binding = table->bindings; binding; binding = binding->next) {
        if (!identity_equal(&binding->registration.identity, identity)) {
            continue;
        }
        if (!binding->active) {
            return GSVA_ERR_SEGMENT_RETIRED;
        }
        if (!(binding->registration.access_flags &
              (write ? GSVA_HOME_ACCESS_WRITE : GSVA_HOME_ACCESS_READ))) {
            return GSVA_ERR_TOKEN_DENIED;
        }
        if (binding->users == UINT32_MAX) {
            return GSVA_ERR_COH_PENDING;
        }
        binding->users++;
        *pin = binding;
        return GSVA_OK;
    }
    return GSVA_ERR_ROUTE_MISSING;
}

void gsva_home_release(GsvaHomeBinding *pin)
{
    assert(pin && pin->users);
    pin->users--;
}

bool gsva_home_overlaps(const GsvaHomeTable *table, uint64_t address,
                        uint64_t length)
{
    const GsvaHomeBinding *binding;

    if (!table || !valid_range(address, length)) {
        return true;
    }
    for (binding = table->bindings; binding; binding = binding->next) {
        if (overlaps(&binding->registration.identity.key, address, length)) {
            return true;
        }
    }
    return false;
}

uint64_t gsva_home_reset(GsvaHomeTable *table)
{
    GsvaHomeBinding *binding;
    uint64_t closed = 0;

    for (binding = table->bindings; binding; binding = binding->next) {
        closed += binding->active;
        binding->active = false;
    }
    return closed;
}

void gsva_home_destroy(GsvaHomeTable *table)
{
    while (table->bindings) {
        GsvaHomeBinding *binding = table->bindings;

        assert(!binding->users);
        table->bindings = binding->next;
        g_free(binding);
    }
}

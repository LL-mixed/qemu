/* Home-authoritative managed export identities. All calls require BQL. */
#ifndef GSVA_HOME_H
#define GSVA_HOME_H

#include "hw/ub/gsva_key.h"

typedef struct QEMU_PACKED GsvaHomeIdentity {
    GsvaKeyV1 key;
    uint32_t home_cna;
    uint32_t token_id;
    uint32_t token_value;
    uint32_t backing_token_id;
} GsvaHomeIdentity;

#define GSVA_HOME_BIND 1
#define GSVA_HOME_REVOKE 2
#define GSVA_HOME_ACCESS_READ 1
#define GSVA_HOME_ACCESS_WRITE 2

/* Registered only by the local kernel, never by an importing peer. */
typedef struct QEMU_PACKED GsvaHomeRequest {
    uint32_t version;
    uint32_t operation;
    GsvaHomeIdentity identity;
    uint64_t export_mem_id;
    uint32_t access_flags;
    uint32_t reserved;
} GsvaHomeRequest;

typedef struct GsvaHomeBinding GsvaHomeBinding;
typedef struct GsvaHomeTable {
    GsvaHomeBinding *bindings;
} GsvaHomeTable;

/* Zero initialization is sufficient. Retired identities cannot be reactivated.
 * Failed revoke leaves the binding closed while outstanding users drain. */
int gsva_home_update(GsvaHomeTable *table, uint32_t local_cna,
                     const GsvaHomeRequest *request);
int gsva_home_acquire(GsvaHomeTable *table, uint32_t local_cna,
                      const GsvaHomeIdentity *identity, uint64_t address,
                      uint64_t length, bool write, GsvaHomeBinding **pin);
/* Resolve an active local export to its authoritative managed identity.
 * Exact export, backing token, address and size must all match. */
int gsva_home_resolve_export(const GsvaHomeTable *table, uint32_t local_cna,
                             uint64_t export_mem_id,
                             uint32_t backing_token_id,
                             uint64_t address, uint64_t length,
                             GsvaHomeRequest *registration);
void gsva_home_release(GsvaHomeBinding *pin);
/* Close admission without dropping identity history or outstanding pins. */
uint64_t gsva_home_reset(GsvaHomeTable *table);
/* Legacy traffic must not access even a retired managed address interval. */
bool gsva_home_overlaps(const GsvaHomeTable *table, uint64_t address,
                        uint64_t length);
void gsva_home_destroy(GsvaHomeTable *table);

#endif

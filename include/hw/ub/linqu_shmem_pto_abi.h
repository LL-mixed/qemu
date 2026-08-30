/*
 * Lingqu shared-memory PTO UB_GM simulator ABI.
 *
 * This is the QEMU-side mirror of the public bridge header owned by ub_sim.
 * Wire objects are decoded from little-endian guest memory before they cross
 * the Rust FFI boundary.
 */
#ifndef HW_UB_LINQU_SHMEM_PTO_ABI_H
#define HW_UB_LINQU_SHMEM_PTO_ABI_H

#include <stddef.h>
#include <stdint.h>

#define LINGQU_PTO_DISPATCH_ABI_V2 2u
#define LINGQU_SHMEM_MEMREF_ABI_V1 1u
#define LINGQU_PTO_SCALAR_ABI_V1 1u
#define PTO_SIM_UB_GM_ACCESS_ABI_V1 1u

#define LINGQU_PTO_DISPATCH_SLOT_TAG_V2 10u
#define LINGQU_PTO_DISPATCH_SLOT_OP_ID_OFFSET 1u
#define LINGQU_PTO_DISPATCH_SLOT_CONTROL_IOVA_OFFSET 9u
#define LINGQU_PTO_DISPATCH_SLOT_RESERVED_OFFSET 17u

#define LINGQU_PTO_MAX_MEMREFS 256u
#define LINGQU_PTO_MAX_SCALARS 128u
#define LINGQU_PTO_MAX_RANK 5u
#define LINGQU_PTO_DTYPE_MAX 14u
#define LINGQU_PTO_CNA_MAX 0x00ffffffu

/*
 * The simulator adaptor turns an active OBMM map registration into one opaque
 * 64-bit reference for the dispatch wire ABI.  The low byte identifies one of
 * the 64 endpoint map slots and the remaining 56 bits preserve its generation.
 * Applications receive the encoded value from the Lingqu shmem adaptor and
 * must not interpret either component.
 */
#define LINGQU_PTO_OBMM_MAP_ID_BITS 8u
#define LINGQU_PTO_OBMM_MAP_ID_MASK UINT64_C(0xff)
#define LINGQU_PTO_OBMM_MAP_GENERATION_MAX \
    (UINT64_MAX >> LINGQU_PTO_OBMM_MAP_ID_BITS)

static inline uint64_t lingqu_pto_obmm_mapping_ref_encode(
    uint64_t map_id, uint64_t map_generation)
{
    if (map_id == 0 || map_id > LINGQU_PTO_OBMM_MAP_ID_MASK ||
        map_generation == 0 ||
        map_generation > LINGQU_PTO_OBMM_MAP_GENERATION_MAX) {
        return 0;
    }
    return (map_generation << LINGQU_PTO_OBMM_MAP_ID_BITS) | map_id;
}

static inline uint64_t lingqu_pto_obmm_mapping_ref_map_id(
    uint64_t mapping_ref)
{
    return mapping_ref & LINGQU_PTO_OBMM_MAP_ID_MASK;
}

static inline uint64_t lingqu_pto_obmm_mapping_ref_generation(
    uint64_t mapping_ref)
{
    return mapping_ref >> LINGQU_PTO_OBMM_MAP_ID_BITS;
}

#define LINGQU_PTO_MEMREF_INPUT 1u
#define LINGQU_PTO_MEMREF_OUTPUT 2u
#define LINGQU_PTO_MEMREF_INOUT 3u

#define LINGQU_PTO_UB_GM_READ (1u << 0)
#define LINGQU_PTO_UB_GM_WRITE (1u << 1)
#define LINGQU_PTO_UB_GM_READ_WRITE \
    (LINGQU_PTO_UB_GM_READ | LINGQU_PTO_UB_GM_WRITE)

enum LingquPtoUbGmError {
    LINGQU_PTO_UB_GM_OK = 0,
    LINGQU_PTO_UB_GM_UNSUPPORTED_CALLABLE = 1,
    LINGQU_PTO_UB_GM_BAD_CONTROL_TABLE = 2,
    LINGQU_PTO_UB_GM_BAD_MEMREF = 3,
    LINGQU_PTO_UB_GM_UNBOUND = 4,
    LINGQU_PTO_UB_GM_ACCESS_DENIED = 5,
    LINGQU_PTO_UB_GM_AUTHORIZATION_TIMEOUT = 6,
    LINGQU_PTO_UB_GM_CALLBACK_FAILED = 7,
    LINGQU_PTO_UB_GM_EXECUTION_FAILED = 8,
    LINGQU_PTO_UB_GM_AUTHORIZATION_CANCELLED = 9,
};

typedef struct LingquPtoDispatchControlV2 {
    uint32_t abi_version;
    uint32_t struct_bytes;
    uint64_t request_id;
    uint64_t callable_id;
    uint32_t memref_count;
    uint32_t scalar_count;
    uint64_t memref_table_iova;
    uint64_t scalar_table_iova;
    uint64_t artifact_fingerprint;
    uint32_t metadata_crc32;
    uint32_t requester_cna;
} LingquPtoDispatchControlV2;

typedef struct LingquShmemMemrefV1 {
    uint32_t abi_version;
    uint32_t struct_bytes;
    uint64_t opaque_mapping_ref;
    uint64_t ub_gm_addr;
    uint64_t byte_offset;
    uint64_t byte_length;
    uint64_t shape_table_iova;
    uint64_t stride_table_iova;
    uint32_t arg_index;
    uint32_t rank;
    uint16_t dtype;
    uint8_t role;
    uint8_t access;
    uint32_t flags;
    uint32_t reserved0;
    uint32_t reserved1;
} LingquShmemMemrefV1;

typedef struct LingquPtoScalarV1 {
    uint32_t abi_version;
    uint32_t struct_bytes;
    uint32_t arg_index;
    uint16_t dtype;
    uint16_t flags;
    uint64_t value;
} LingquPtoScalarV1;

typedef struct PtoSimUbGmBindingV1 {
    uint64_t request_id;
    uint64_t binding_id;
    uint64_t aperture_base;
    uint64_t aperture_length;
    uint64_t ub_gm_base;
    uint64_t mapped_length;
    uint32_t access;
    uint32_t flags;
    uint64_t backend_cookie;
} PtoSimUbGmBindingV1;

typedef struct PtoSimUbGmAuthorizedMemrefV1 {
    LingquShmemMemrefV1 memref;
    PtoSimUbGmBindingV1 binding;
    uint32_t shape[LINGQU_PTO_MAX_RANK];
    uint32_t strides[LINGQU_PTO_MAX_RANK];
    uint64_t reserved;
} PtoSimUbGmAuthorizedMemrefV1;

typedef int (*PtoSimUbGmReadV1)(void *qemu_context,
                                uint64_t request_id,
                                uint64_t binding_id,
                                uint64_t ub_gm_addr,
                                void *dst,
                                uint64_t length);
typedef int (*PtoSimUbGmWriteV1)(void *qemu_context,
                                 uint64_t request_id,
                                 uint64_t binding_id,
                                 uint64_t ub_gm_addr,
                                 const void *src,
                                 uint64_t length);
typedef int (*PtoSimUbGmFenceV1)(void *qemu_context,
                                 uint64_t request_id,
                                 uint64_t binding_id,
                                 uint64_t ub_gm_addr,
                                 uint64_t length,
                                 uint32_t flags);

typedef struct PtoSimUbGmAccessOpsV1 {
    uint32_t abi_version;
    uint32_t struct_bytes;
    PtoSimUbGmReadV1 read;
    PtoSimUbGmWriteV1 write;
    PtoSimUbGmFenceV1 fence;
} PtoSimUbGmAccessOpsV1;

typedef struct LinquUbBridge LinquUbBridge;

LinquUbBridge *linqu_ub_bridge_new_from_yaml(const char *path);
void linqu_ub_bridge_free(LinquUbBridge *bridge);
int linqu_ub_bridge_register_endpoint(LinquUbBridge *bridge,
                                      uint16_t endpoint_id,
                                      uint32_t entity_id);
int linqu_ub_bridge_register_ub_gm_access_v1(
    LinquUbBridge *bridge,
    const PtoSimUbGmAccessOpsV1 *ops,
    void *qemu_context,
    uint32_t pto_device_cna);
int linqu_ub_bridge_query_ub_gm_callable_v1(LinquUbBridge *bridge,
                                             uint64_t callable_id,
                                             uint64_t *fingerprint_out);
int linqu_ub_bridge_submit_ub_gm_v2(
    LinquUbBridge *bridge,
    uint16_t endpoint_id,
    uint64_t op_id,
    const LingquPtoDispatchControlV2 *control,
    const PtoSimUbGmAuthorizedMemrefV1 *memrefs,
    uint32_t memref_count,
    const LingquPtoScalarV1 *scalars,
    uint32_t scalar_count);

_Static_assert(sizeof(LingquPtoDispatchControlV2) == 64,
               "LingquPtoDispatchControlV2 size changed");
_Static_assert(sizeof(LingquShmemMemrefV1) == 80,
               "LingquShmemMemrefV1 size changed");
_Static_assert(sizeof(LingquPtoScalarV1) == 24,
               "LingquPtoScalarV1 size changed");
_Static_assert(sizeof(PtoSimUbGmBindingV1) == 64,
               "PtoSimUbGmBindingV1 size changed");
_Static_assert(sizeof(PtoSimUbGmAuthorizedMemrefV1) == 192,
               "PtoSimUbGmAuthorizedMemrefV1 size changed");
_Static_assert(sizeof(PtoSimUbGmAccessOpsV1) == 32,
               "PtoSimUbGmAccessOpsV1 size changed");

#endif

/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2023-2024. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */
#include <sys/file.h>
#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "qapi/error.h"
#include "qemu/log.h"
#include "exec/address-spaces.h"
#include "hw/irq.h"
#include "qemu/module.h"
#include "qemu/main-loop.h"
#include "qemu/units.h"
#include "hw/arm/virt.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "hw/ub/ub.h"
#include "hw/ub/ub_bus.h"
#include "hw/ub/ub_ubc.h"
#include "hw/ub/gsva_key.h"
#include "hw/ub/gsva_route.h"
#include "hw/ub/gsva_coherence.h"
#include "hw/ub/gsva_stats.h"
#include "hw/ub/obmm_coherence.h"
#include "hw/ub/ub_obmm_async.h"
#include "hw/ub/ub_async_load_device.h"
#include "hw/ub/ub_ummu.h"
#include "hw/ub/ub_config.h"
#include "hw/ub/linqu_shmem_pto_abi.h"
#include "hw/ub/ub_usi.h"
#include "hw/ub/hisi/ubc.h"
#include "hw/ub/hisi/ub_mem.h"
#include "hw/ub/hisi/ub_fm.h"
#include "hw/ub/ub_link.h"
#include "sysemu/dma.h"
#include "sysemu/cpus.h"
#include "hw/core/cpu.h"
#include "migration/vmstate.h"
#include "hw/ub/ub_common.h"
#include "hw/ub/ubus_instance.h"
#include "hw/ub/ub_pool_msg.h"
#include "trace.h"

void tlb_flush_all_cpus_synced(CPUState *src_cpu);

static bool ubc_trace_link_lookup_enabled(void)
{
    const char *val = g_getenv("UBC_TRACE_LINK_LOOKUP");

    return val && val[0] && strcmp(val, "0") != 0;
}

static bool ubc_trace_data_path_enabled(void)
{
    const char *val = g_getenv("UBC_TRACE_DATA_PATH");

    return val && val[0] && strcmp(val, "0") != 0;
}

int linqu_ub_bridge_get_default_segment(LinquUbBridge *bridge,
                                        uint16_t endpoint_id,
                                        uint64_t *segment_out);
int linqu_ub_bridge_submit_slot(LinquUbBridge *bridge,
                                uint16_t endpoint_id,
                                const uint8_t *slot,
                                size_t slot_len);
int linqu_ub_bridge_write_segment_payload(LinquUbBridge *bridge,
                                          uint64_t segment,
                                          size_t offset,
                                          const uint8_t *data,
                                          size_t data_len);
int linqu_ub_bridge_read_segment_payload(LinquUbBridge *bridge,
                                         uint64_t segment,
                                         size_t offset,
                                         uint8_t *out,
                                         size_t out_len);
int linqu_ub_bridge_register_model_runtime_object_payload(LinquUbBridge *bridge,
                                                          const uint8_t *object_ref,
                                                          size_t object_ref_len,
                                                          const uint8_t *payload,
                                                          size_t payload_len);
int linqu_ub_bridge_ring_doorbell(LinquUbBridge *bridge,
                                  uint16_t endpoint_id,
                                  uint32_t max_batch,
                                  uint32_t *submitted_out,
                                  uint32_t *pending_out);
int linqu_ub_bridge_poll_completion(LinquUbBridge *bridge,
                                    uint16_t endpoint_id,
                                    uint8_t *slot_out,
                                    size_t slot_len);

#define LINQU_UAPI_ENDPOINT_ID 1
#define LINQU_UAPI_ENTITY_ID 0
#define LINQU_UAPI_VERSION 0x0000000400030000ULL
#define LINQU_UAPI_ENDPOINT_BASE 0x1000
#define LINQU_UAPI_REG_VERSION 0x000
#define LINQU_UAPI_REG_CMDQ_BASE_LO 0x010
#define LINQU_UAPI_REG_CMDQ_BASE_HI 0x018
#define LINQU_UAPI_REG_CMDQ_SIZE 0x020
#define LINQU_UAPI_REG_CMDQ_HEAD 0x028
#define LINQU_UAPI_REG_CMDQ_TAIL 0x030
#define LINQU_UAPI_REG_CQ_BASE_LO 0x038
#define LINQU_UAPI_REG_CQ_BASE_HI 0x040
#define LINQU_UAPI_REG_CQ_SIZE 0x048
#define LINQU_UAPI_REG_CQ_HEAD 0x050
#define LINQU_UAPI_REG_CQ_TAIL 0x058
#define LINQU_UAPI_REG_STATUS 0x060
#define LINQU_UAPI_REG_DOORBELL 0x068
#define LINQU_UAPI_REG_LAST_ERROR 0x070
#define LINQU_UAPI_REG_IRQ_STATUS 0x078
#define LINQU_UAPI_REG_IRQ_ACK 0x080
#define LINQU_UAPI_REG_DEFAULT_SEGMENT 0x088
#define LINQU_UAPI_REG_SEG_DATA_OFFSET 0x090
#define LINQU_UAPI_REG_SEG_DATA_VALUE 0x098
#define LINQU_UAPI_REG_CANCEL_OP_ID 0x0a0
#define LINQU_UAPI_REG_CANCEL_DOORBELL 0x0a8
#define LINQU_UAPI_DESC_BYTES 64
#define LINQU_UAPI_DEFAULT_CMDQ_DEPTH 32
#define LINQU_UAPI_DEFAULT_CQ_DEPTH 64
#define LINQU_UAPI_IRQ_COMPLETION 1
#define LINQU_UAPI_IRQ_ERROR 2
#define LINQU_UAPI_IRQ_CQ_OVERFLOW 4

#define LINQU_PTO_CONTROL_WIRE_BYTES 64u
#define LINQU_PTO_MEMREF_WIRE_BYTES 80u
#define LINQU_PTO_SCALAR_WIRE_BYTES 24u
#define LINQU_PTO_APERTURE_BASE 0x700000000000ULL
#define LINQU_PTO_APERTURE_LENGTH 0x010000000000ULL
#define LINQU_PTO_APERTURE_ALIGN 0x10000ULL
#define LINQU_PTO_SHAPE_STRIDE_SNAPSHOT_BYTES \
    (2u * LINGQU_PTO_MAX_RANK * sizeof(uint32_t))
#define LINQU_PTO_AUTHORIZATION_TIMEOUT_NS_DEFAULT 1000000000ULL
#define LINQU_PTO_UB_GM_INTERNAL_PENDING (-EINPROGRESS)

typedef struct LinquUbGmBindingState {
    uint64_t request_id;
    uint64_t binding_id;
    uint64_t mapping_ref;
    uint64_t local_base;
    uint64_t length;
    uint64_t map_id;
    uint64_t map_generation;
    uint64_t remote_base;
    uint64_t read_bytes;
    uint64_t write_bytes;
    uint64_t fence_count;
    uint32_t token_id;
    uint32_t peer_cna;
    uint32_t access;
    bool dirty;
    bool active;
} LinquUbGmBindingState;

typedef struct LinquUbGmDispatchState {
    uint64_t op_id;
    uint64_t request_id;
    uint32_t binding_count;
    LinquUbGmBindingState *bindings;
    QTAILQ_ENTRY(LinquUbGmDispatchState) next;
} LinquUbGmDispatchState;

struct LinquUbGmRegistry {
    QemuMutex lock;
    QTAILQ_HEAD(, LinquUbGmDispatchState) dispatches;
    uint64_t qemu_load_bytes;
    uint64_t qemu_store_bytes;
    uint64_t qemu_fence_count;
};

typedef struct LinquPtoAuthorizationState {
    uint8_t slot[LINQU_UAPI_DESC_BYTES];
    uint8_t control_wire[LINQU_PTO_CONTROL_WIRE_BYTES];
    uint8_t *memref_wire;
    uint8_t *scalar_wire;
    uint8_t *shape_stride_wire;
    UbcObmmResolvedMap *resolved;
    bool *range_authorized;
    uint64_t op_id;
    uint64_t request_id;
    uint64_t sequence;
    uint64_t started_ns;
    uint64_t ready_ns;
    uint64_t deadline_ns;
    uint32_t cmdq_slot;
    uint32_t memref_count;
    uint32_t scalar_count;
    uint32_t memref_cursor;
    uint32_t resume_batch;
    int completion_status;
    bool waiting;
    bool completion_ready;
} LinquPtoAuthorizationState;

/*
 * ============================================================================
 * SIM_DEC: Simulation Decoder Protocol for Cross-Node Memory Access
 * ============================================================================
 */

/* SIM_DEC protocol version */
#define SIM_DEC_PROTO_VERSION       1

/* SIM_DEC opcodes */
#define SIM_DEC_OP_MAP              0x01
#define SIM_DEC_OP_UNMAP            0x02
#define SIM_DEC_OP_SYNC             0x03
#define SIM_DEC_OP_QUERY            0x04
#define SIM_DEC_OP_OBMM_BOOTSTRAP_PUBLISH 0x05
#define SIM_DEC_OP_OBMM_BOOTSTRAP_LOOKUP  0x06
#define SIM_DEC_OP_GVA_MAP          0x07
#define SIM_DEC_OP_COH_FENCE        0x08
#define SIM_DEC_OP_GSVA_MAP_V1      0x09
#define SIM_DEC_OP_GSVA_UNMAP_V1    0x0a
#define SIM_DEC_OP_GSVA_EVENT_V1    0x0b
#define SIM_DEC_OP_GSVA_QUERY_V1    0x0c

/* SIM_DEC status codes */
#define SIM_DEC_STATUS_SUCCESS          0x00
#define SIM_DEC_STATUS_INVALID_PARAM    0x01
#define SIM_DEC_STATUS_RESOURCE_BUSY    0x02
#define SIM_DEC_STATUS_BACKEND_ERROR    0x03
#define SIM_DEC_STATUS_TIMEOUT          0x04
#define SIM_DEC_STATUS_NOT_SUPPORTED    0x05

/* SIM_DEC message header */
typedef struct QEMU_PACKED SimDecMsgHdr {
    uint8_t  version;
    uint8_t  opcode;
    uint16_t seq;
    uint16_t status;
    uint16_t payload_len;
} SimDecMsgHdr;

/* SIM_DEC MAP request */
typedef struct QEMU_PACKED SimDecMapReq {
    uint64_t local_pa;
    uint64_t size;
    uint64_t remote_uba;
    uint32_t token_id;
    uint32_t token_value;
    uint32_t scna;
    uint32_t dcna;
    uint8_t  seid[16];
    uint8_t  deid[16];
    uint32_t upi;
    uint32_t src_eid;
} SimDecMapReq;

/* SIM_DEC GVA map request (Phase A metadata plane) */
typedef struct QEMU_PACKED SimDecGvaMapReq {
    SimDecMapReq map_req;
    uint64_t local_va;
    uint64_t home_va;
    uint64_t pte_offset;
    uint32_t vmid;
    uint32_t asid;
    uint32_t tid;
    uint32_t p_tag;
    uint32_t cache_policy;
    uint32_t map_source;
    uint32_t address_profile;
    uint32_t access_flags;
    uint64_t gva_id;
} SimDecGvaMapReq;

/* SIM_DEC MAP response */
typedef struct QEMU_PACKED SimDecMapResp {
    uint64_t map_id;
    uint32_t status;
    uint32_t rsvd;
    uint32_t p_tag;
    uint32_t mp_ubc_port;
    uint32_t mp_lane;
    uint32_t mp_link_id;
} SimDecMapResp;

/* SIM_DEC UNMAP request */
typedef struct QEMU_PACKED SimDecUnmapReq {
    uint64_t map_id;
} SimDecUnmapReq;

/* SIM_DEC SYNC request */
typedef struct QEMU_PACKED SimDecSyncReq {
    uint64_t map_id;
    uint64_t offset;
    uint64_t len;
} SimDecSyncReq;

/* SIM_DEC QUERY request/response */
typedef struct QEMU_PACKED SimDecQueryReq {
    uint64_t map_id;
} SimDecQueryReq;

typedef struct QEMU_PACKED SimDecQueryResp {
    uint64_t local_pa;
    uint64_t size;
    uint32_t status;
    uint32_t ref_count;
} SimDecQueryResp;

/* map_source values for SIM_DEC_GVA_MAP */
#define SIM_DEC_MAP_SOURCE_LEGACY_OBMM 1
#define SIM_DEC_MAP_SOURCE_GVA_MANAGER 2

/* address_profile values for SIM_DEC_GVA_MAP */
#define SIM_DEC_ADDRESS_PROFILE_GENERIC_GVA 1
#define SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY 2

/* cache_policy values for SIM_DEC_GVA_MAP */
#define SIM_DEC_CACHE_POLICY_NC 0
#define SIM_DEC_CACHE_POLICY_WRITE_THROUGH 1
#define SIM_DEC_CACHE_POLICY_READ_CACHE 2
#define SIM_DEC_CACHE_POLICY_WRITE_BACK 3
#define SIM_DEC_CACHE_POLICY_DIRECTORY_MESI 4
#define SIM_DEC_GVA_FAULT_P_TAG_ROUTE_MISS UINT32_MAX
#define SIM_DEC_GVA_ACCESS_READ_ONLY BIT(0)
#define SIM_DEC_GVA_ACCESS_EXPLICIT_SYNC BIT(1)
#define SIM_DEC_GVA_ACCESS_FAULT_UPI_MISMATCH BIT(31)
#define SIM_DEC_GVA_MP_UNRESOLVED UINT32_MAX

/* GSVA query types */
#define GSVA_QUERY_CAPS       1
#define GSVA_QUERY_ROUTE      2
#define GSVA_QUERY_COHERENCE  3
#define GSVA_QUERY_SEGMENT    4

/* GSVA capability flags */
#define GSVA_CAP_STRICT_ADDRESS_IDENTITY  (1u << 0)
#define GSVA_CAP_ROUTE_LAYER              (1u << 1)
#define GSVA_CAP_COHERENCE_LAYER          (1u << 2)
#define GSVA_CAP_ARM_MMU_MODE             (1u << 3)
#define GSVA_CAP_RETIRE_REUSE_TXN         (1u << 4)

/* GSVA query request */
typedef struct QEMU_PACKED SimDecGsvaQueryReq {
    uint32_t version;
    uint32_t query_type;
    GsvaKeyV1 key;
} SimDecGsvaQueryReq;

/* GSVA map request */
typedef struct QEMU_PACKED SimDecGsvaMapReq {
    uint32_t version;
    uint32_t flags;
    GsvaKeyV1 key;
    uint64_t local_pa;
    uint64_t local_va;
    uint64_t remote_uba;
    uint64_t token_id;
    uint64_t token_value;
    uint32_t source;
    uint32_t address_profile;
    uint32_t access_flags;
    uint32_t scna;
    uint32_t dcna;
} SimDecGsvaMapReq;

/* GSVA map response */
typedef struct QEMU_PACKED SimDecGsvaMapResp {
    uint64_t map_id;
    int32_t  error;
    uint32_t reserved;
} SimDecGsvaMapResp;

/* GSVA unmap request */
typedef struct QEMU_PACKED SimDecGsvaUnmapReq {
    uint32_t version;
    uint32_t flags;
    GsvaKeyV1 key;
    uint64_t map_id;
} SimDecGsvaUnmapReq;

/* GSVA unmap response */
typedef struct QEMU_PACKED SimDecGsvaUnmapResp {
    int32_t  error;
    uint32_t reserved;
} SimDecGsvaUnmapResp;

/* GSVA capability response */
typedef struct QEMU_PACKED SimDecGsvaCapsResp {
    uint32_t version;
    uint32_t flags;
    uint32_t max_nodes;
    uint32_t supported_cache_policies;
    uint32_t supported_modes;
    uint32_t reserved;
} SimDecGsvaCapsResp;

/* GSVA generic query response (route/coherence/segment) */
typedef struct QEMU_PACKED SimDecGsvaQueryResp {
    uint32_t version;
    int32_t  error;
    uint8_t  data[240];
} SimDecGsvaQueryResp;

#define SIM_DEC_OBMM_BOOTSTRAP_MAX_NODES 8

typedef struct QEMU_PACKED SimDecObmmBootstrapRecord {
    uint64_t export_mem_id;
    uint64_t remote_uba;
    uint64_t backing_uba;
    uint64_t size;
    uint64_t generation;
    uint64_t flags;
    uint32_t node_id;
    uint32_t node_count;
    uint32_t export_cna;
    uint32_t token_id;
} SimDecObmmBootstrapRecord;

typedef struct QEMU_PACKED SimDecObmmBootstrapPublishReq {
    SimDecObmmBootstrapRecord record;
} SimDecObmmBootstrapPublishReq;

typedef struct QEMU_PACKED SimDecObmmBootstrapLookupReq {
    uint64_t generation;
    uint32_t node_count;
    uint32_t rsvd;
} SimDecObmmBootstrapLookupReq;

typedef struct QEMU_PACKED SimDecObmmBootstrapLookupResp {
    uint32_t count;
    uint32_t rsvd;
    SimDecObmmBootstrapRecord records[SIM_DEC_OBMM_BOOTSTRAP_MAX_NODES];
} SimDecObmmBootstrapLookupResp;

#define LINGQU_OBMM_OBJECT_REF_MAGIC 0x514f424d4d524546ULL
#define LINGQU_OBJECT_STATE_COMMITTED_WIRE 2
#define MODEL_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT 5
#define MODEL_OBMM_KIND_TOKEN_RESULT 6
#define MODEL_OBMM_KIND_KV_STATE 7
#define OBMM_POOL_HEADER_BYTES 64
#define OBMM_REGION_DIRENT_BYTES 32
#define OBMM_REGION_W4_PAYLOAD 5

typedef struct QEMU_PACKED LinquObmmObjectRefWire {
    uint64_t magic;
    uint16_t layout_version;
    uint16_t object_kind;
    uint16_t state;
    uint16_t flags;
    uint32_t owner_entity;
    uint32_t producer_entity;
    uint64_t object_version;
    uint64_t key_hash;
    uint64_t payload_offset;
    uint64_t payload_bytes;
    uint64_t payload_checksum;
} LinquObmmObjectRefWire;

typedef struct QEMU_PACKED ObmmPoolHeaderWire {
    uint64_t magic;
    uint32_t layout_version;
    uint16_t node_id;
    uint16_t node_count;
    uint32_t state;
    uint32_t generation;
    uint64_t region_size;
    uint64_t directory_offset;
    uint32_t directory_count;
    uint32_t default_queue_depth;
    uint32_t flags;
    uint32_t reserved[3];
} ObmmPoolHeaderWire;

typedef struct QEMU_PACKED ObmmRegionDirentWire {
    uint32_t region_id;
    uint16_t kind;
    uint16_t peer_node_id;
    uint64_t offset;
    uint64_t size;
    uint32_t flags;
    uint32_t reserved;
} ObmmRegionDirentWire;

/* Decoder map entry for simulation backend */
/* Page cache for SIM_DEC imported-PA CPU window reads */
#define SIM_DEC_PAGE_SIZE           4096
#define SIM_DEC_CACHE_MAX_PER_MAP   0
#define SIM_DEC_CACHE_MAX_GLOBAL    8192

typedef struct SimDecPageCacheEntry {
    uint64_t page_index;
    uint8_t *page_buf;
    uint64_t last_used;
    bool dirty;
    uint64_t dirty_off;
    uint64_t dirty_len;
    QTAILQ_ENTRY(SimDecPageCacheEntry) lru_next;
} SimDecPageCacheEntry;

typedef struct SimDecPageCache {
    GHashTable *pages;                 /* key: page_index (uint64_t), value: SimDecPageCacheEntry* */
    QTAILQ_HEAD(, SimDecPageCacheEntry) lru_list;
    uint64_t max_pages;
    uint64_t cur_pages;
    QemuMutex lock;
} SimDecPageCache;

typedef enum SimDecRouteState {
    SIM_DEC_ROUTE_CREATING = 0,
    SIM_DEC_ROUTE_ACTIVE = 1,
    SIM_DEC_ROUTE_STALE = 2,
    SIM_DEC_ROUTE_ERROR = 3,
    SIM_DEC_ROUTE_RETIRED = 4,
} SimDecRouteState;

typedef struct SimDecMapEntry {
    uint64_t map_id;
    uint64_t local_pa;
    uint64_t size;
    uint64_t remote_uba;
    uint32_t token_id;
    uint32_t token_value;
    uint32_t scna;
    uint32_t dcna;
    uint8_t  seid[16];
    uint8_t  deid[16];
    uint32_t upi;
    uint32_t src_eid;
    uint32_t map_source;
    uint32_t address_profile;
    uint32_t cache_policy;
    uint32_t access_flags;
    uint32_t vmid;
    uint32_t asid;
    uint32_t tid;
    uint32_t p_tag;
    uint32_t mp_ubc_port;
    uint32_t mp_lane;
    uint32_t mp_link_id;
    uint64_t local_va;
    uint64_t home_va;
    uint64_t pte_offset;
    uint64_t gva_id;
    uint64_t remote_read_ordinal;
    SimDecRouteState state;
    int last_error;
    bool     gva_ownership_registered;
    bool     active;
    bool     mapped;
    uint8_t *sync_shadow;
    uint64_t sync_valid_off;
    uint64_t sync_valid_len;
    MemoryRegion cpu_window;
    SimDecPageCache *page_cache;
    uint32_t next_batch_seqno;
    QTAILQ_ENTRY(SimDecMapEntry) next;
} SimDecMapEntry;

typedef struct SimDecLookupResult {
    uint64_t map_id;
    uint64_t gva_id;
    uint64_t local_pa;
    uint64_t local_va;
    uint64_t remote_uba;
    uint32_t token_id;
    uint32_t token_value;
    uint32_t src_eid;
    uint32_t address_profile;
    uint32_t access_flags;
    uint32_t vmid;
    uint32_t asid;
    uint32_t dcna;
    uint32_t tid;
    uint32_t upi;
    uint32_t p_tag;
    uint32_t mp_ubc_port;
    uint32_t mp_lane;
    uint32_t mp_link_id;
    uint32_t cache_policy;
} SimDecLookupResult;

/* Decoder simulation context */
typedef enum SimDecWriteMode {
    SIM_DEC_WRITE_THROUGH,
    SIM_DEC_WRITE_BACK,
} SimDecWriteMode;

typedef struct SimDecoderState {
    BusControllerState *bcs;
    uint64_t next_map_id;
    QTAILQ_HEAD(, SimDecMapEntry) map_list;
    QTAILQ_HEAD(, SimDecMapEntry) retired_map_list;
    QemuMutex lock;
    bool enabled;
    SimDecStats stats;
    uint64_t page_cache_max_per_map;
    uint64_t page_cache_max_global;
    uint64_t page_cache_global_pages;
    bool page_cache_prefetch;
    SimDecWriteMode write_mode;
    bool experimental_gsva_enabled;
} SimDecoderState;

static SimDecoderState *g_sim_decoder;
static bool sim_dec_experimental_feature_enabled(const char *feature)
{
    const char *features = g_getenv("UB_SIM_EXPERIMENTAL_FEATURES");
    g_auto(GStrv) entries = NULL;

    if (!features || !features[0]) {
        return false;
    }

    entries = g_strsplit_set(features, ",:; ", -1);
    return g_strv_contains((const gchar *const *)entries, feature);
}

static bool sim_dec_is_gva_entry(const SimDecMapEntry *entry);
static bool sim_dec_entry_write_back(const SimDecMapEntry *entry);
static void sim_dec_log_gva_path(const SimDecMapEntry *entry, const char *op,
                                 hwaddr addr, unsigned size, uint64_t count);
static void sim_dec_log_gva_dma_path(const SimDecLookupResult *result,
                                     const char *op, uint64_t iova,
                                     size_t len, uint64_t count);
static bool sim_dec_gva_access_fault(const SimDecMapEntry *entry,
                                     const char *op, hwaddr addr,
                                     unsigned size);
static bool sim_dec_gva_dma_access_fault(const SimDecLookupResult *result,
                                         const char *op, uint64_t iova,
                                         size_t len);
static void sim_dec_log_gva_route_dump(const SimDecMapEntry *entry,
                                       const char *state);
static int sim_dec_lookup_result_by_pa(uint64_t pa,
                                       SimDecLookupResult *result);
static bool sim_dec_gva_tcg_enabled(void);
static void sim_dec_flush_gva_tlbs(const char *reason);
static void gsva_tlb_stable_flush_all(const char *reason);
static void gsva_tlb_stable_flush_key(const GsvaKeyV1 *key,
                                      const char *reason);
static int sim_dec_gva_ownership_register_req(const SimDecGvaMapReq *req,
                                              uint64_t map_id);
static void sim_dec_gva_ownership_unregister_entry(
    const SimDecMapEntry *entry);

static void sim_dec_flush_gva_tlbs(const char *reason)
{
    CPUState *src = current_cpu ? current_cpu : first_cpu;
    bool arm_mmu = gsva_arm_mmu_enabled();

    if (!sim_dec_gva_tcg_enabled() && !arm_mmu) {
        return;
    }

    if (!src) {
        qemu_log("%s skipped reason=%s no_cpu=1\n",
                 arm_mmu ? "GSVA_TLB: flush" : "GVA_TCG_TLB_FLUSH",
                 reason ? reason : "unspecified");
        return;
    }

    tlb_flush_all_cpus_synced(src);
    if (arm_mmu) {
        gsva_tlb_stable_flush_all(reason);
    } else {
        qemu_log("GVA_TCG_TLB_FLUSH reason=%s\n",
                 reason ? reason : "unspecified");
    }
}

/* SIM_DEC stats helpers */
void sim_dec_stats_accumulate(SimDecStats *dst, const SimDecStats *src)
{
    int i;
    if (!dst || !src) {
        return;
    }
    dst->gva_cpu_reads += src->gva_cpu_reads;
    dst->gva_cpu_writes += src->gva_cpu_writes;
    dst->gva_cpu_read_bytes += src->gva_cpu_read_bytes;
    dst->gva_cpu_write_bytes += src->gva_cpu_write_bytes;
    dst->gva_dma_reads += src->gva_dma_reads;
    dst->gva_dma_writes += src->gva_dma_writes;
    dst->gva_dma_read_bytes += src->gva_dma_read_bytes;
    dst->gva_dma_write_bytes += src->gva_dma_write_bytes;
    dst->cpu_window_reads += src->cpu_window_reads;
    dst->cpu_window_writes += src->cpu_window_writes;
    for (i = 0; i < 4; i++) {
        dst->cpu_window_read_bytes[i] += src->cpu_window_read_bytes[i];
        dst->cpu_window_write_bytes[i] += src->cpu_window_write_bytes[i];
    }
    dst->shadow_hits += src->shadow_hits;
    dst->shadow_misses += src->shadow_misses;
    dst->page_cache_hits += src->page_cache_hits;
    dst->page_cache_misses += src->page_cache_misses;
    dst->page_cache_prefetches += src->page_cache_prefetches;
    dst->page_cache_prefetch_skips += src->page_cache_prefetch_skips;
    dst->remote_reads += src->remote_reads;
    dst->remote_writes += src->remote_writes;
    dst->remote_read_bytes += src->remote_read_bytes;
    dst->remote_write_bytes += src->remote_write_bytes;
    dst->dma_path_reads += src->dma_path_reads;
    dst->dma_path_writes += src->dma_path_writes;
    dst->dma_path_read_bytes += src->dma_path_read_bytes;
    dst->dma_path_write_bytes += src->dma_path_write_bytes;
    dst->read_timeouts += src->read_timeouts;
    dst->read_errors += src->read_errors;
    dst->write_errors += src->write_errors;
    dst->batch_frames += src->batch_frames;
    dst->batch_ops += src->batch_ops;
    dst->batch_bytes += src->batch_bytes;
}

void sim_dec_print_stats(const SimDecStats *stats, const char *prefix)
{
    uint64_t cpu_rbytes = 0, cpu_wbytes = 0;
    int i;
    if (!stats) {
        return;
    }
    for (i = 0; i < 4; i++) {
        cpu_rbytes += stats->cpu_window_read_bytes[i];
        cpu_wbytes += stats->cpu_window_write_bytes[i];
    }
    qemu_log("%sSIM_DEC_STATS gva_cpu_reads=%" PRIu64 " gva_cpu_writes=%" PRIu64
             " gva_cpu_rbytes=%" PRIu64 " gva_cpu_wbytes=%" PRIu64
             " gva_dma_reads=%" PRIu64 " gva_dma_writes=%" PRIu64
             " gva_dma_rbytes=%" PRIu64 " gva_dma_wbytes=%" PRIu64
             " cpu_reads=%" PRIu64 " cpu_writes=%" PRIu64
             " cpu_rbytes=%" PRIu64 " cpu_wbytes=%" PRIu64
             " shadow_hits=%" PRIu64 " shadow_misses=%" PRIu64
             " page_cache_hits=%" PRIu64 " page_cache_misses=%" PRIu64
             " page_cache_prefetches=%" PRIu64 " page_cache_prefetch_skips=%" PRIu64
             " remote_reads=%" PRIu64 " remote_writes=%" PRIu64
             " remote_rbytes=%" PRIu64 " remote_wbytes=%" PRIu64
             " dma_reads=%" PRIu64 " dma_writes=%" PRIu64
             " dma_rbytes=%" PRIu64 " dma_wbytes=%" PRIu64
             " read_timeouts=%" PRIu64 " read_errors=%" PRIu64
             " write_errors=%" PRIu64
             " batch_frames=%" PRIu64 " batch_ops=%" PRIu64
             " batch_bytes=%" PRIu64 "\n",
             prefix ? prefix : "",
             stats->gva_cpu_reads, stats->gva_cpu_writes,
             stats->gva_cpu_read_bytes, stats->gva_cpu_write_bytes,
             stats->gva_dma_reads, stats->gva_dma_writes,
             stats->gva_dma_read_bytes, stats->gva_dma_write_bytes,
             stats->cpu_window_reads, stats->cpu_window_writes,
             cpu_rbytes, cpu_wbytes,
             stats->shadow_hits, stats->shadow_misses,
             stats->page_cache_hits, stats->page_cache_misses,
             stats->page_cache_prefetches, stats->page_cache_prefetch_skips,
             stats->remote_reads, stats->remote_writes,
             stats->remote_read_bytes, stats->remote_write_bytes,
             stats->dma_path_reads, stats->dma_path_writes,
             stats->dma_path_read_bytes, stats->dma_path_write_bytes,
             stats->read_timeouts, stats->read_errors, stats->write_errors,
             stats->batch_frames, stats->batch_ops, stats->batch_bytes);
    qemu_log("%sGVA_STATS cpu_reads=%" PRIu64 " cpu_writes=%" PRIu64
             " cpu_rbytes=%" PRIu64 " cpu_wbytes=%" PRIu64
             " dma_reads=%" PRIu64 " dma_writes=%" PRIu64
             " dma_rbytes=%" PRIu64 " dma_wbytes=%" PRIu64
             " remote_reads=%" PRIu64 " remote_writes=%" PRIu64
             " remote_rbytes=%" PRIu64 " remote_wbytes=%" PRIu64
             " read_timeouts=%" PRIu64 " read_errors=%" PRIu64
             " write_errors=%" PRIu64 "\n",
             prefix ? prefix : "",
             stats->gva_cpu_reads, stats->gva_cpu_writes,
             stats->gva_cpu_read_bytes, stats->gva_cpu_write_bytes,
             stats->gva_dma_reads, stats->gva_dma_writes,
             stats->gva_dma_read_bytes, stats->gva_dma_write_bytes,
             stats->remote_reads, stats->remote_writes,
             stats->remote_read_bytes, stats->remote_write_bytes,
             stats->read_timeouts, stats->read_errors, stats->write_errors);
}

static void sim_dec_print_global_stats(void)
{
    if (!g_sim_decoder) {
        return;
    }
    qemu_mutex_lock(&g_sim_decoder->lock);
    sim_dec_print_stats(&g_sim_decoder->stats, "");
    qemu_mutex_unlock(&g_sim_decoder->lock);
}

/* Forward declarations for SIM decoder */
static void sim_dec_init(BusControllerState *bcs);
static void sim_dec_cleanup(void);
MemTxResult ubc_sim_dec_remote_write(BusControllerDev *ubc_dev,
                                     uint64_t remote_uba,
                                     uint32_t token_id,
                                     uint32_t dcna,
                                     const uint8_t *buf,
                                     uint32_t len);
MemTxResult ubc_sim_dec_remote_read(BusControllerDev *ubc_dev,
                                    uint64_t remote_uba,
                                           uint32_t token_id,
                                           uint32_t dcna,
                                           uint8_t *buf,
                                           uint32_t len);
static void linqu_uapi_maybe_register_model_runtime_object_payload(
    BusControllerDev *ubc_dev, uint64_t segment, uint64_t write_offset);
static uint8_t ubc_node_ip_suffix_from_id(const char *node_id);
static void ubc_fill_link_local_eid_hw(uint8_t eid_hw[16], uint8_t suffix);

/* Page cache helpers */
static SimDecPageCache *sim_dec_page_cache_new(uint64_t max_pages)
{
    SimDecPageCache *cache = g_malloc0(sizeof(*cache));
    cache->pages = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                          g_free, NULL);
    QTAILQ_INIT(&cache->lru_list);
    cache->max_pages = max_pages;
    cache->cur_pages = 0;
    qemu_mutex_init(&cache->lock);
    return cache;
}

static void sim_dec_page_cache_free(SimDecPageCache *cache)
{
    SimDecPageCacheEntry *ce, *tmp;
    if (!cache) {
        return;
    }
    qemu_mutex_lock(&cache->lock);
    QTAILQ_FOREACH_SAFE(ce, &cache->lru_list, lru_next, tmp) {
        QTAILQ_REMOVE(&cache->lru_list, ce, lru_next);
        g_free(ce->page_buf);
        g_free(ce);
    }
    if (cache->pages) {
        g_hash_table_destroy(cache->pages);
    }
    qemu_mutex_unlock(&cache->lock);
    qemu_mutex_destroy(&cache->lock);
    g_free(cache);
}

static void sim_dec_page_cache_evict_lru(SimDecPageCache *cache)
{
    SimDecPageCacheEntry *victim;
    victim = QTAILQ_LAST(&cache->lru_list);
    if (!victim) {
        return;
    }
    QTAILQ_REMOVE(&cache->lru_list, victim, lru_next);
    g_hash_table_remove(cache->pages, &victim->page_index);
    g_free(victim->page_buf);
    g_free(victim);
    cache->cur_pages--;
    if (g_sim_decoder) {
        g_sim_decoder->page_cache_global_pages--;
    }
}

static void sim_dec_page_cache_insert(SimDecPageCache *cache,
                                      uint64_t page_index,
                                      const uint8_t *data)
{
    SimDecPageCacheEntry *ce;
    uint64_t *key;

    if (!cache || cache->max_pages == 0) {
        return;
    }

    /* Evict if at per-map limit or global limit */
    while (cache->cur_pages >= cache->max_pages ||
           (g_sim_decoder && g_sim_decoder->page_cache_global_pages >=
            g_sim_decoder->page_cache_max_global)) {
        sim_dec_page_cache_evict_lru(cache);
        if (cache->cur_pages == 0) {
            break; /* cannot evict further */
        }
    }

    ce = g_malloc0(sizeof(*ce));
    ce->page_index = page_index;
    ce->page_buf = g_malloc(SIM_DEC_PAGE_SIZE);
    memcpy(ce->page_buf, data, SIM_DEC_PAGE_SIZE);
    ce->last_used = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    key = g_malloc(sizeof(*key));
    *key = page_index;
    g_hash_table_insert(cache->pages, key, ce);
    QTAILQ_INSERT_HEAD(&cache->lru_list, ce, lru_next);
    cache->cur_pages++;
    if (g_sim_decoder) {
        g_sim_decoder->page_cache_global_pages++;
    }
}

static SimDecPageCacheEntry *sim_dec_page_cache_lookup(SimDecPageCache *cache,
                                                        uint64_t page_index)
{
    SimDecPageCacheEntry *ce;
    if (!cache) {
        return NULL;
    }
    ce = g_hash_table_lookup(cache->pages, &page_index);
    if (ce) {
        ce->last_used = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        QTAILQ_REMOVE(&cache->lru_list, ce, lru_next);
        QTAILQ_INSERT_HEAD(&cache->lru_list, ce, lru_next);
    }
    return ce;
}

static void sim_dec_page_cache_invalidate_all(SimDecPageCache *cache)
{
    SimDecPageCacheEntry *ce, *tmp;
    if (!cache) {
        return;
    }
    QTAILQ_FOREACH_SAFE(ce, &cache->lru_list, lru_next, tmp) {
        QTAILQ_REMOVE(&cache->lru_list, ce, lru_next);
        g_free(ce->page_buf);
        g_free(ce);
    }
    g_hash_table_remove_all(cache->pages);
    if (g_sim_decoder) {
        g_sim_decoder->page_cache_global_pages -= cache->cur_pages;
    }
    cache->cur_pages = 0;
}

static void sim_dec_page_cache_invalidate_range(SimDecPageCache *cache,
                                                uint64_t start_page,
                                                uint64_t end_page)
{
    uint64_t idx;
    SimDecPageCacheEntry *ce;
    if (!cache) {
        return;
    }
    for (idx = start_page; idx < end_page; idx++) {
        ce = g_hash_table_lookup(cache->pages, &idx);
        if (ce) {
            QTAILQ_REMOVE(&cache->lru_list, ce, lru_next);
            g_hash_table_remove(cache->pages, &idx);
            g_free(ce->page_buf);
            g_free(ce);
            cache->cur_pages--;
            if (g_sim_decoder) {
                g_sim_decoder->page_cache_global_pages--;
            }
        }
    }
}

static int sim_dec_send_batch_writes(BusControllerDev *ubc_dev, uint32_t dcna,
                                       uint32_t token_id,
                                       SimDecBatchWriteOp *ops,
                                       uint8_t **datas,
                                       uint32_t op_count,
                                       uint32_t barrier_epoch);

static int sim_dec_page_cache_flush_dirty(SimDecMapEntry *entry)
{
    SimDecPageCache *cache = entry->page_cache;
    SimDecPageCacheEntry *ce;
    MemTxResult ret;
    int rc = 0;
    bool use_batch = false;

    if (!cache || cache->max_pages == 0) {
        return 0;
    }

    /* Batch dirty writes when write-back mode is enabled */
    if (g_sim_decoder && g_sim_decoder->write_mode == SIM_DEC_WRITE_BACK) {
        use_batch = true;
    }

    qemu_mutex_lock(&cache->lock);

    if (use_batch) {
        SimDecBatchWriteOp batch_ops[SIM_DEC_BATCH_MAX_OPS];
        uint8_t *batch_datas[SIM_DEC_BATCH_MAX_OPS];
        uint32_t batch_count = 0;
        uint32_t batch_data = 0;

        QTAILQ_FOREACH(ce, &cache->lru_list, lru_next) {
            if (!ce->dirty) {
                continue;
            }
            batch_ops[batch_count].remote_uba = entry->remote_uba +
                                                ce->page_index * SIM_DEC_PAGE_SIZE +
                                                ce->dirty_off;
            batch_ops[batch_count].data_len = (uint32_t)ce->dirty_len;
            batch_datas[batch_count] = ce->page_buf + ce->dirty_off;
            batch_count++;
            batch_data += (uint32_t)ce->dirty_len;

            if (batch_count >= SIM_DEC_BATCH_MAX_OPS ||
                batch_data >= SIM_DEC_BATCH_MAX_DATA) {
                if (sim_dec_send_batch_writes(g_sim_decoder->bcs->ubc_dev,
                                               entry->dcna, entry->token_id,
                                               batch_ops, batch_datas,
                                               batch_count,
                                               entry->next_batch_seqno++) != 0) {
                    rc = -1;
                }
                batch_count = 0;
                batch_data = 0;
            }
        }

        if (batch_count > 0) {
            if (sim_dec_send_batch_writes(g_sim_decoder->bcs->ubc_dev,
                                           entry->dcna, entry->token_id,
                                           batch_ops, batch_datas,
                                           batch_count,
                                           entry->next_batch_seqno++) != 0) {
                rc = -1;
            }
        }

        if (rc == 0) {
            QTAILQ_FOREACH(ce, &cache->lru_list, lru_next) {
                if (ce->dirty) {
                    ce->dirty = false;
                    ce->dirty_off = 0;
                    ce->dirty_len = 0;
                }
            }
        }
    } else {
        QTAILQ_FOREACH(ce, &cache->lru_list, lru_next) {
            if (!ce->dirty) {
                continue;
            }
            ret = ubc_sim_dec_remote_write(g_sim_decoder->bcs->ubc_dev,
                                           entry->remote_uba + ce->page_index * SIM_DEC_PAGE_SIZE + ce->dirty_off,
                                           entry->token_id, entry->dcna,
                                           ce->page_buf + ce->dirty_off,
                                           (uint32_t)ce->dirty_len);
            if (ret != MEMTX_OK) {
                qemu_log("SIM_DEC: flush dirty failed map=%" PRIx64 " page=%" PRIx64
                         " off=%" PRIx64 " len=%" PRIx64 " ret=%d\n",
                         entry->map_id, ce->page_index, ce->dirty_off, ce->dirty_len, ret);
                rc = -1;
            } else {
                ce->dirty = false;
                ce->dirty_off = 0;
                ce->dirty_len = 0;
            }
        }
    }

    qemu_mutex_unlock(&cache->lock);
    return rc;
}

static MemTxResult sim_dec_page_cache_fetch(SimDecMapEntry *entry,
                                            uint64_t page_index,
                                            uint8_t *buf)
{
    uint64_t remote_uba = entry->remote_uba + page_index * SIM_DEC_PAGE_SIZE;
    uint32_t fetch_len = SIM_DEC_PAGE_SIZE;
    MemTxResult ret;

    /* Clamp to map boundary */
    if (remote_uba + fetch_len > entry->remote_uba + entry->size) {
        fetch_len = (uint32_t)(entry->remote_uba + entry->size - remote_uba);
    }

    ret = ubc_sim_dec_remote_read(g_sim_decoder->bcs->ubc_dev,
                                  remote_uba, entry->token_id, entry->dcna,
                                  buf, fetch_len);
    if (ret != MEMTX_OK) {
        return ret;
    }
    /* Zero-fill partial page past end of map */
    if (fetch_len < SIM_DEC_PAGE_SIZE) {
        memset(buf + fetch_len, 0, SIM_DEC_PAGE_SIZE - fetch_len);
    }
    return MEMTX_OK;
}

static void sim_dec_page_cache_prefetch_next(SimDecMapEntry *entry,
                                             uint64_t page_index)
{
    uint64_t next_page = page_index + 1;
    uint64_t next_off = next_page * SIM_DEC_PAGE_SIZE;
    uint8_t page_buf[SIM_DEC_PAGE_SIZE];
    SimDecPageCacheEntry *ce;
    MemTxResult ret;

    if (!g_sim_decoder || !g_sim_decoder->page_cache_prefetch ||
        !entry || !entry->page_cache || entry->page_cache->max_pages == 0 ||
        next_off >= entry->size) {
        return;
    }

    qemu_mutex_lock(&entry->page_cache->lock);
    ce = sim_dec_page_cache_lookup(entry->page_cache, next_page);
    if (ce) {
        qemu_mutex_unlock(&entry->page_cache->lock);
        g_sim_decoder->stats.page_cache_prefetch_skips++;
        return;
    }
    qemu_mutex_unlock(&entry->page_cache->lock);

    ret = sim_dec_page_cache_fetch(entry, next_page, page_buf);
    if (ret != MEMTX_OK) {
        return;
    }

    qemu_mutex_lock(&entry->page_cache->lock);
    ce = sim_dec_page_cache_lookup(entry->page_cache, next_page);
    if (!ce) {
        sim_dec_page_cache_insert(entry->page_cache, next_page, page_buf);
        g_sim_decoder->stats.page_cache_prefetches++;
    } else {
        g_sim_decoder->stats.page_cache_prefetch_skips++;
    }
    qemu_mutex_unlock(&entry->page_cache->lock);
}

static uint64_t sim_dec_cpu_window_read(void *opaque, hwaddr addr,
                                        unsigned size)
{
    SimDecMapEntry *entry = opaque;
    uint8_t buf[8] = { 0 };
    uint64_t remote_uba;
    MemTxResult ret;
    int size_idx;
    uint64_t page_index;
    uint64_t page_off;
    SimDecPageCacheEntry *ce;

    if (!entry || !entry->active || size > sizeof(buf) || addr + size > entry->size) {
        qemu_log("SIM_DEC: cpu read invalid addr=%#" PRIx64 " size=%u map=%" PRIx64 "\n",
                 (uint64_t)addr, size, entry ? entry->map_id : 0);
        return 0;
    }

    if (sim_dec_gva_access_fault(entry, "read", addr, size)) {
        g_sim_decoder->stats.read_errors++;
        return 0;
    }

    size_idx = (size == 1) ? 0 : (size == 2) ? 1 : (size == 4) ? 2 : 3;
    g_sim_decoder->stats.cpu_window_reads++;
    g_sim_decoder->stats.cpu_window_read_bytes[size_idx] += size;
    if (sim_dec_is_gva_entry(entry)) {
        g_sim_decoder->stats.gva_cpu_reads++;
        g_sim_decoder->stats.gva_cpu_read_bytes += size;
        sim_dec_log_gva_path(entry, "read", addr, size,
                             g_sim_decoder->stats.gva_cpu_reads);
    }

    /* Directory MESI coherence path */
    if (entry->cache_policy == SIM_DEC_CACHE_POLICY_DIRECTORY_MESI) {
        remote_uba = entry->remote_uba + addr;
        ret = obmm_coh_read(g_sim_decoder->bcs->ubc_dev,
                            remote_uba, entry->token_id, entry->dcna,
                            buf, (uint32_t)size);
        if (ret != MEMTX_OK) {
            g_sim_decoder->stats.read_errors++;
        }
        goto done;
    }

    remote_uba = entry->remote_uba + addr;
    if (entry->sync_shadow &&
        addr >= entry->sync_valid_off &&
        addr + size <= entry->sync_valid_off + entry->sync_valid_len) {
        memcpy(buf, entry->sync_shadow + addr, size);
        g_sim_decoder->stats.shadow_hits++;
        goto done;
    }

    /* Page cache lookup */
    if (entry->page_cache && entry->page_cache->max_pages > 0) {
        page_index = addr / SIM_DEC_PAGE_SIZE;
        page_off = addr % SIM_DEC_PAGE_SIZE;
        qemu_mutex_lock(&entry->page_cache->lock);
        ce = sim_dec_page_cache_lookup(entry->page_cache, page_index);
        if (ce) {
            memcpy(buf, ce->page_buf + page_off, size);
            qemu_mutex_unlock(&entry->page_cache->lock);
            g_sim_decoder->stats.page_cache_hits++;
            sim_dec_page_cache_prefetch_next(entry, page_index);
            goto done;
        }
        qemu_mutex_unlock(&entry->page_cache->lock);
        g_sim_decoder->stats.page_cache_misses++;
        /*
         * Fill the cache page and satisfy this read from that page.  This
         * avoids the old miss path's scalar remote read followed by a second
         * full-page fetch for the same page.
         */
        uint8_t page_buf[SIM_DEC_PAGE_SIZE];
        ret = sim_dec_page_cache_fetch(entry, page_index, page_buf);
        if (ret == MEMTX_OK) {
            memcpy(buf, page_buf + page_off, size);
            qemu_mutex_lock(&entry->page_cache->lock);
            sim_dec_page_cache_insert(entry->page_cache, page_index, page_buf);
            qemu_mutex_unlock(&entry->page_cache->lock);
            sim_dec_page_cache_prefetch_next(entry, page_index);
            goto done;
        }
    }

    g_sim_decoder->stats.shadow_misses++;
    ret = ubc_sim_dec_remote_read(g_sim_decoder->bcs->ubc_dev, remote_uba,
                                  entry->token_id, entry->dcna, buf, size);
    if (ret != MEMTX_OK) {
        if (ret == MEMTX_DECODE_ERROR) {
            qemu_log("SIM_DEC: cpu read retryable map=%" PRIx64
                     " remote_uba=%#" PRIx64 " size=%u ret=%d\n",
                     entry->map_id, remote_uba, size, ret);
        } else {
            qemu_log("SIM_DEC: cpu read failed map=%" PRIx64
                     " remote_uba=%#" PRIx64 " size=%u ret=%d\n",
                     entry->map_id, remote_uba, size, ret);
        }
        g_sim_decoder->stats.read_errors++;
        return 0;
    }

done:
    switch (size) {
    case 1:
        return buf[0];
    case 2:
        return lduw_le_p(buf);
    case 4:
        return ldl_le_p(buf);
    case 8:
        return ldq_le_p(buf);
    default:
        return 0;
    }
}

static void sim_dec_cpu_window_write(void *opaque, hwaddr addr,
                                     uint64_t value, unsigned size)
{
    SimDecMapEntry *entry = opaque;
    uint8_t buf[8] = { 0 };
    uint64_t remote_uba;
    MemTxResult ret;
    int size_idx;
    bool write_through = true;

    if (!entry || !entry->active || size > sizeof(buf) || addr + size > entry->size) {
        qemu_log("SIM_DEC: cpu write invalid addr=%#" PRIx64 " size=%u map=%" PRIx64 "\n",
                 (uint64_t)addr, size, entry ? entry->map_id : 0);
        return;
    }

    if (sim_dec_gva_access_fault(entry, "write", addr, size)) {
        g_sim_decoder->stats.write_errors++;
        return;
    }

    size_idx = (size == 1) ? 0 : (size == 2) ? 1 : (size == 4) ? 2 : 3;
    g_sim_decoder->stats.cpu_window_writes++;
    g_sim_decoder->stats.cpu_window_write_bytes[size_idx] += size;
    if (sim_dec_is_gva_entry(entry)) {
        g_sim_decoder->stats.gva_cpu_writes++;
        g_sim_decoder->stats.gva_cpu_write_bytes += size;
        sim_dec_log_gva_path(entry, "write", addr, size,
                             g_sim_decoder->stats.gva_cpu_writes);
    }

    /* Directory MESI coherence path */
    if (entry->cache_policy == SIM_DEC_CACHE_POLICY_DIRECTORY_MESI) {
        switch (size) {
        case 1: buf[0] = (uint8_t)value; break;
        case 2: stw_le_p(buf, (uint16_t)value); break;
        case 4: stl_le_p(buf, (uint32_t)value); break;
        case 8: stq_le_p(buf, value); break;
        default: return;
        }
        remote_uba = entry->remote_uba + addr;
        ret = obmm_coh_write(g_sim_decoder->bcs->ubc_dev,
                             remote_uba, entry->token_id, entry->dcna,
                             buf, (uint32_t)size);
        if (ret != MEMTX_OK) {
            g_sim_decoder->stats.write_errors++;
        }
        return;
    }

    switch (size) {
    case 1:
        buf[0] = (uint8_t)value;
        break;
    case 2:
        stw_le_p(buf, (uint16_t)value);
        break;
    case 4:
        stl_le_p(buf, (uint32_t)value);
        break;
    case 8:
        stq_le_p(buf, value);
        break;
    default:
        return;
    }

    /* Write-back path: update local page cache and mark dirty */
    if (sim_dec_entry_write_back(entry) &&
        entry->page_cache && entry->page_cache->max_pages > 0) {
        uint64_t page_index = addr / SIM_DEC_PAGE_SIZE;
        uint64_t page_off = addr % SIM_DEC_PAGE_SIZE;
        SimDecPageCacheEntry *ce;

        qemu_mutex_lock(&entry->page_cache->lock);
        ce = sim_dec_page_cache_lookup(entry->page_cache, page_index);
        if (!ce) {
            uint8_t page_buf[SIM_DEC_PAGE_SIZE];
            qemu_mutex_unlock(&entry->page_cache->lock);
            /* Fetch page for write-back cache (read-before-write for partial page) */
            ret = sim_dec_page_cache_fetch(entry, page_index, page_buf);
            if (ret == MEMTX_OK) {
                qemu_mutex_lock(&entry->page_cache->lock);
                sim_dec_page_cache_insert(entry->page_cache, page_index, page_buf);
                ce = sim_dec_page_cache_lookup(entry->page_cache, page_index);
            }
        }
        if (ce) {
            memcpy(ce->page_buf + page_off, buf, size);
            if (!ce->dirty) {
                ce->dirty = true;
                ce->dirty_off = page_off;
                ce->dirty_len = size;
            } else {
                uint64_t dstart = MIN(ce->dirty_off, page_off);
                uint64_t dend = MAX(ce->dirty_off + ce->dirty_len, page_off + size);
                ce->dirty_off = dstart;
                ce->dirty_len = dend - dstart;
            }
            qemu_mutex_unlock(&entry->page_cache->lock);
            write_through = false;
            if (sim_dec_is_gva_entry(entry)) {
                qemu_log("GVA_WRITE_BACK map_id=%" PRIx64
                         " gva_id=%" PRIx64 " offset=%#" PRIx64
                         " size=%u cache_policy=%" PRIu32 "\n",
                         entry->map_id, entry->gva_id, (uint64_t)addr,
                         size, entry->cache_policy);
            }
        } else if (entry->page_cache) {
            qemu_mutex_unlock(&entry->page_cache->lock);
        }
    }

    if (write_through) {
        remote_uba = entry->remote_uba + addr;
        ret = ubc_sim_dec_remote_write(g_sim_decoder->bcs->ubc_dev, remote_uba,
                                       entry->token_id, entry->dcna, buf, size);
        if (ret != MEMTX_OK) {
            qemu_log("SIM_DEC: cpu write failed map=%" PRIx64 " remote_uba=%#" PRIx64
                     " size=%u ret=%d\n",
                     entry->map_id, remote_uba, size, ret);
            g_sim_decoder->stats.write_errors++;
        }
    }
}

static const MemoryRegionOps sim_dec_cpu_window_ops = {
    .read = sim_dec_cpu_window_read,
    .write = sim_dec_cpu_window_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

/*
 * GSVA-specific CPU window IO ops.
 * opaque is GsvaRouteEntry*, which has a different layout than SimDecMapEntry.
 * These ops perform remote read/write through the PA-MESI coherence layer.
 */
static uint64_t sim_dec_gsva_cpu_window_read(void *opaque, hwaddr addr,
                                              unsigned size)
{
    GsvaRouteEntry *route = opaque;
    uint8_t buf[8] = { 0 };
    uint64_t remote_uba;
    MemTxResult ret;
    int tok_rc;

    if (!route || size > sizeof(buf) || addr + size > route->key.size) {
        qemu_log("GSVA_CPU: read invalid addr=%#" PRIx64 " size=%u map_id=%"
                 PRIx64 "\n", (uint64_t)addr, size, route ? route->map_id : 0);
        return 0;
    }

    /* Token validation before PA-MESI access */
    tok_rc = gsva_route_validate_token(route, route->home_cna,
                                       route->token.token_id,
                                       route->token.token_value, 1);
    if (tok_rc != GSVA_OK) {
        qemu_log("GSVA_CPU: read token denied map_id=%" PRIx64
                 " addr=%#" PRIx64 " rc=%d\n",
                 route->map_id, (uint64_t)addr, tok_rc);
        return 0;
    }

    remote_uba = route->remote_uba + addr;
    ret = ubc_sim_dec_remote_read(g_sim_decoder->bcs->ubc_dev, remote_uba,
                                  route->token.token_id, route->home_cna,
                                  buf, size);
    if (ret != MEMTX_OK) {
        qemu_log("GSVA_CPU: read failed map_id=%" PRIx64
                 " remote_uba=%#" PRIx64 " size=%u ret=%d\n",
                 route->map_id, remote_uba, size, ret);
        return 0;
    }

    switch (size) {
    case 1: return buf[0];
    case 2: return lduw_le_p(buf);
    case 4: return ldl_le_p(buf);
    case 8: return ldq_le_p(buf);
    default: return 0;
    }
}

static void sim_dec_gsva_cpu_window_write(void *opaque, hwaddr addr,
                                          uint64_t value, unsigned size)
{
    GsvaRouteEntry *route = opaque;
    uint8_t buf[8] = { 0 };
    uint64_t remote_uba;
    MemTxResult ret;
    int tok_rc;

    if (!route || size > sizeof(buf) || addr + size > route->key.size) {
        qemu_log("GSVA_CPU: write invalid addr=%#" PRIx64 " size=%u map_id=%"
                 PRIx64 "\n", (uint64_t)addr, size, route ? route->map_id : 0);
        return;
    }

    /* Token validation before PA-MESI access */
    tok_rc = gsva_route_validate_token(route, route->home_cna,
                                       route->token.token_id,
                                       route->token.token_value, 2);
    if (tok_rc != GSVA_OK) {
        qemu_log("GSVA_CPU: write token denied map_id=%" PRIx64
                 " addr=%#" PRIx64 " rc=%d\n",
                 route->map_id, (uint64_t)addr, tok_rc);
        return;
    }

    switch (size) {
    case 1: buf[0] = (uint8_t)value; break;
    case 2: stw_le_p(buf, (uint16_t)value); break;
    case 4: stl_le_p(buf, (uint32_t)value); break;
    case 8: stq_le_p(buf, value); break;
    default: return;
    }

    remote_uba = route->remote_uba + addr;
    ret = ubc_sim_dec_remote_write(g_sim_decoder->bcs->ubc_dev, remote_uba,
                                   route->token.token_id, route->home_cna,
                                   buf, size);
    if (ret != MEMTX_OK) {
        qemu_log("GSVA_CPU: write failed map_id=%" PRIx64
                 " remote_uba=%#" PRIx64 " size=%u ret=%d\n",
                 route->map_id, remote_uba, size, ret);
    }
}

static const MemoryRegionOps sim_dec_gsva_cpu_window_ops = {
    .read = sim_dec_gsva_cpu_window_read,
    .write = sim_dec_gsva_cpu_window_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

#define UBC_ERS_PAGE_SIZE (4 * KiB)
#define UBC_ERS2_MMIO_SIZE (UBC_ERS2_SPACE_SIZE * UBC_ERS_PAGE_SIZE)

/*
 * The ERS ubba values must map to addresses visible inside the
 * "ub-idev-ers-as" MMIO alias window (VIRT_UB_IDEV_ERS range).
 * The static UBC_ERSx_SPACE_ADDR constants (0x18000000 etc.) are only
 * valid when that window happens to start at 0x18000000, which it never
 * does on a machine with 8 GiB RAM — the alias base is typically at
 * 0x180_0000_0000 (512 GiB aligned).  This helper resolves the dynamic
 * base so that both the controller config-space fields and entity
 * injection messages carry addresses that the guest can actually ioremap.
 */
uint64_t ub_ers_phys_base(void)
{
    VirtMachineState *vms = VIRT_MACHINE(qdev_get_machine());
    return vms->memmap[VIRT_UB_IDEV_ERS].base;
}

/* ubase cmdq registers in ERS2 */
#define UBASE_CSQ_BASEADDR_L_REG 0x18400
#define UBASE_CSQ_BASEADDR_H_REG 0x18404
#define UBASE_CSQ_DEPTH_REG 0x18408
#define UBASE_CSQ_TAIL_REG 0x18410
#define UBASE_CSQ_HEAD_REG 0x18414
#define UBASE_CRQ_BASEADDR_L_REG 0x18418
#define UBASE_CRQ_BASEADDR_H_REG 0x1841c
#define UBASE_CRQ_DEPTH_REG 0x18420
#define UBASE_CRQ_TAIL_REG 0x18424
#define UBASE_CRQ_HEAD_REG 0x18428

/* ubase ctrlq registers in ERS2 (separate from cmdq, offset by 0x400) */
#define UBASE_CTRLQ_CSQ_BASEADDR_L_REG 0x18800
#define UBASE_CTRLQ_CSQ_BASEADDR_H_REG 0x18804
#define UBASE_CTRLQ_CSQ_DEPTH_REG 0x18808
#define UBASE_CTRLQ_CSQ_TAIL_REG 0x18810
#define UBASE_CTRLQ_CSQ_HEAD_REG 0x18814
#define UBASE_CTRLQ_CRQ_BASEADDR_L_REG 0x18818
#define UBASE_CTRLQ_CRQ_BASEADDR_H_REG 0x1881c
#define UBASE_CTRLQ_CRQ_DEPTH_REG 0x18820
#define UBASE_CTRLQ_CRQ_TAIL_REG 0x18824
#define UBASE_CTRLQ_CRQ_HEAD_REG 0x18828

#define UBASE_CTRLQ_BB_LEN 32
#define UBASE_CTRLQ_HDR_LEN 12
#define UBASE_CTRLQ_DATA_NUM 5
#define UBASE_CTRLQ_DATA_LEN (UBASE_CTRLQ_BB_LEN - UBASE_CTRLQ_HDR_LEN)

typedef struct QEMU_PACKED UBCCtrlqBaseBlock {
    uint8_t  service_ver;
    uint8_t  service_type;
    uint8_t  bb_num;
    uint8_t  opcode;
    uint8_t  ret;
    uint16_t seq;
    uint8_t  mbx_ue_id;
    uint16_t bus_ue_id;
    uint16_t rsv;
    uint32_t data[UBASE_CTRLQ_DATA_NUM];
} UBCCtrlqBaseBlock;

/* ctrlq service types */
#define UBASE_CTRLQ_SER_TYPE_QOS 0x04
#define UBASE_CTRLQ_SER_TYPE_DEV_REGISTER 0x02
#define UBASE_CTRLQ_SER_TYPE_TP_ACL 0x01

/* ctrlq DEV_REGISTER opcodes */
#define UBASE_CTRLQ_OPC_GET_SEID_INFO 0x01

/* ctrlq TP_ACL opcodes */
#define UBASE_CTRLQ_OPC_GET_TP_LIST 0x21

/* ctrlq QOS opcodes */
#define UBASE_CTRLQ_OPC_QUERY_VL 0x01
#define UBASE_CTRLQ_OPC_QUERY_SL 0x02

#define UBASE_VECTOR0_CMDQ_SRC_REG 0x18004
#define UBASE_VECTOR0_RX_CMDQ_INT_B 1
#define UBASE_VECTOR0_CTRLQ_SRC_REG 0x18014
#define UBASE_VECTOR0_RX_CTRLQ_INT_B 0

#define UBASE_OPC_QUERY_FW_VER 0x0001
#define UBASE_OPC_QUERY_UE_RES 0x0002
#define UBASE_OPC_QUERY_CTL_INFO 0x0003
#define UBASE_OPC_QUERY_COMM_RSRC_PARAM 0x0030
#define UBASE_OPC_QUERY_PORT_INFO 0x6200
#define UBASE_OPC_QUERY_CHIP_INFO 0x6201
#define UBASE_OPC_QUERY_OOR_CAPS 0x4200
#define UBASE_OPC_QUERY_UB_PORT_BITMAP 0x5105
#define UBASE_OPC_QUERY_PORT_BITMAP 0xA017
#define UBASE_OPC_QUERY_TA_SL_VL_MAP 0x4201
#define UBASE_OPC_QUERY_TM_Q_INFO 0x4205
#define UBASE_OPC_QUERY_TM_QS_INFO 0x4206
#define UBASE_OPC_POST_MB 0x7000
#define UBASE_OPC_QUERY_MB_ST 0x7001
#define UBASE_OPC_UE2UE_UBASE 0xF00E
#define UBASE_OPC_MUE_TO_UE 0xF001
#define UBASE_OPC_UE_TO_MUE 0xF002

#define UDMA_CMD_NOTIFY_MUE_SAVE_TP 0x2

#define UBASE_CMD_FLAG_IN BIT(0)
#define UBASE_CMD_FLAG_OUT BIT(1)
#define UBASE_CMD_FLAG_NEXT BIT(2)
#define UBASE_CMD_FLAG_WR BIT(3)
#define UBASE_CMD_FLAG_NO_INTR BIT(4)
#define UBASE_CMD_FLAG_GET_BD_NUM BIT(6)

#define UBASE_SUPPORT_TA_EXTDB_BUF_B 2
#define UBASE_SUPPORT_TA_TIMER_BUF_B 3
#define UBASE_SUPPORT_IP_OVER_URMA_B 17
#define UBASE_SUPPORT_UBL_B 0
#define UBASE_MAX_VL_NUM 16
#define UBASE_MAX_SL_NUM 16

/*
 * Mailbox sub-opcodes (cmd field in UBCMbox.cmd_tag).
 * Two parallel control channels exist:
 *   1. Mailbox: used for JFS/JFC/JFR context lifecycle (CREATE/MODIFY/QUERY/DESTROY)
 *   2. CtrlQ:   used for service-type operations (TP_ACL, DEV_REGISTER, QOS)
 * CtrlQ messages may also be carried via CMDQ fallback (UBASE_OPC_UE2UE_UBASE)
 * when the native CtrlQ path is unavailable.
 *
 * JFS sub-opcodes (high nibble = 0x0):
 */
#define UBC_MB_CREATE_JFS_CONTEXT   0x04
#define UBC_MB_MODIFY_JFS_CONTEXT   0x05
#define UBC_MB_QUERY_JFS_CONTEXT    0x06
#define UBC_MB_DESTROY_JFS_CONTEXT  0x07
/* JFC sub-opcodes (high nibble = 0x2): */
#define UBC_MB_CREATE_JFC_CONTEXT   0x24
#define UBC_MB_MODIFY_JFC_CONTEXT   0x25
#define UBC_MB_QUERY_JFC_CONTEXT    0x26
#define UBC_MB_DESTROY_JFC_CONTEXT  0x27
/* AEQ sub-opcodes (high nibble = 0x3): */
#define UBC_MB_CREATE_AEQ_CONTEXT   0x34
#define UBC_MB_QUERY_AEQ_CONTEXT    0x36
#define UBC_MB_DESTROY_AEQ_CONTEXT  0x37
/* CEQ sub-opcodes (high nibble = 0x4): */
#define UBC_MB_CREATE_CEQ_CONTEXT   0x44
#define UBC_MB_QUERY_CEQ_CONTEXT    0x46
#define UBC_MB_DESTROY_CEQ_CONTEXT  0x47
/* JFR sub-opcodes (high nibble = 0x5): */
#define UBC_MB_CREATE_JFR_CONTEXT   0x54
#define UBC_MB_MODIFY_JFR_CONTEXT   0x55
#define UBC_MB_QUERY_JFR_CONTEXT    0x56
#define UBC_MB_DESTROY_JFR_CONTEXT  0x57

/* udma_jetty_ctx size and field extraction constants */
#define UDMA_JETTY_CTX_SIZE  256
#define UDMA_JFC_CTX_SIZE    256
#define UDMA_JFR_CTX_SIZE    256
#define UBASE_EQ_CTX_SIZE    64

/* WQE / SQE constants for DMA transport */
#define UDMA_SQE_SIZE          64   /* one WQEBB = 64 bytes */
#define UDMA_SQE_CTL_LEN_SEND  48   /* sizeof(udma_sqe_ctl) for SEND opcode */
#define UDMA_SQE_INLINE_EN_BIT 6  /* bit position in DW1 */
#define UDMA_JFS_SGE_SIZE      16   /* sizeof(udma_normal_sge) */

/* WQE opcodes (DW1[15:8]) — matches kernel enum udma_sq_opcode */
#define UDMA_SQE_OPCODE_SEND               0x00  /* UDMA_OPC_SEND */
#define UDMA_SQE_OPCODE_SEND_WITH_IMM      0x01  /* UDMA_OPC_SEND_WITH_IMM */
#define UDMA_SQE_OPCODE_SEND_WITH_INVALID  0x02  /* UDMA_OPC_SEND_WITH_INVALID */
#define UDMA_SQE_OPCODE_WRITE              0x03  /* UDMA_OPC_WRITE */
#define UDMA_SQE_OPCODE_WRITE_WITH_IMM     0x04  /* UDMA_OPC_WRITE_WITH_IMM */
#define UDMA_SQE_OPCODE_READ               0x06  /* UDMA_OPC_READ */
#define UDMA_SQE_OPCODE_CAS                0x07  /* UDMA_OPC_CAS */
#define UDMA_SQE_OPCODE_FAA                0x0b  /* UDMA_OPC_FAA */

/* Jetty state machine values (matches guest enum jetty_state) */
#define JETTY_STATE_RESET   0
#define JETTY_STATE_READY   1
#define JETTY_STATE_ERROR   2
#define JETTY_STATE_SUSPEND 3

/* CQE status codes */
#define CQE_STATUS_SUCCESS        0
#define CQE_STATUS_LOCAL_LEN_ERR  1
#define CQE_STATUS_LOCAL_OP_ERR   2
#define CQE_STATUS_REMOTE_ERR     3
#define CQE_STATUS_RQ_EMPTY_ERR   4
#define CQE_STATUS_DMA_ERR        5

/* Extended msg_codes for RDMA READ/WRITE (stored in msgetah) */
#define UB_MSG_CODE_URMA_READ_REQ  6
#define UB_MSG_CODE_URMA_READ_RESP 5
#define UB_MSG_CODE_URMA_WRITE     4

#define UBC_SIM_DEC_MAX_MSG_PAYLOAD 4095
#define UBC_SIM_DEC_WRITE_CHUNK_MAX \
    (UBC_SIM_DEC_MAX_MSG_PAYLOAD - (uint32_t)sizeof(UBCSimDecWritePldHdr))
#define UBC_SIM_DEC_READ_CHUNK_MAX \
    (UBC_SIM_DEC_MAX_MSG_PAYLOAD - (uint32_t)sizeof(UBCSimDecReadRespPldHdr))
#define UBC_SIM_DEC_READ_WAIT_USEC  1000
#define UBC_SIM_DEC_SHM_READ_WAIT_USEC 50
#define UBC_SIM_DEC_READ_WAIT_LOOPS 30000
#define UBC_OBMM_ASYNC_CHILD_CAPACITY \
    (OBMM_REMOTE_PARENT_CAPACITY * OBMM_REMOTE_MAX_CHILDREN)

typedef struct UbcObmmAsyncChild {
    bool active;
    bool model_queued;
    uint32_t req_id;
    uint32_t peer_cna;
    uint32_t expected_len;
    ObmmRemoteToken token;
    uint16_t child_index;
    UbObmmRemoteOperation operation;
    UbcObmmAsyncReadCompleteFn complete;
    void *opaque;
} UbcObmmAsyncChild;

/* Doorbell/MMIO region constants (matches UAPI) */
#define UDMA_JETTY_DSQE_OFFSET   0x1000
#define UDMA_DOORBELL_OFFSET     0x80
#define UDMA_HW_PAGE_SIZE        0x1000
#define UBASE_EQE_SIZE           64

/* RDMA WRITE payload header (prepended before actual data) */
typedef struct {
    uint64_t remote_addr;   /* Remote memory address to write to */
} UBCWritePayloadHdr;

/* --- URMA RX buffering for early-arriving packets --- */
#define UBC_URMA_RX_BUF_MAX 64
#define UBC_URMA_RX_BUF_DATA_MAX 4096

typedef struct UBCUrxBufEntry {
    uint32_t dst_jetty;
    uint32_t src_jetty;
    uint32_t src_scna;
    uint32_t data_len;
    uint8_t  src_eid[16];
    uint8_t  data[4096];
} UBCUrxBufEntry;

typedef struct QEMU_PACKED UBCCmdqDesc {
    uint16_t opcode;
    uint8_t flag;
    uint8_t bd_num;
    uint16_t ret;
    uint16_t rsv;
    uint32_t data[6];
} UBCCmdqDesc;

typedef struct QEMU_PACKED UBCEntityBuf {
    uint32_t len;
    uint32_t seq_num;
    uint8_t data[0];
} UBCEntityBuf;

typedef struct QEMU_PACKED UBCEntityMsg {
    uint8_t dst_ue_idx;
    uint8_t opcode;
    uint16_t rsv;
    UBCEntityBuf buf;
} UBCEntityMsg;

typedef struct QEMU_PACKED UBCQueryVersionResp {
    uint32_t fw_version;
    uint8_t rsv[20];
} UBCQueryVersionResp;

typedef struct QEMU_PACKED UBCResCmdResp {
    uint32_t cap_bits[3];
    uint32_t rsvd0[3];

    uint8_t node_type;
    uint8_t rsvd1;
    uint16_t ceq_vector_num;
    uint16_t aeq_vector_num;
    uint16_t misc_vector_num;
    uint16_t aeqe_size;
    uint16_t ceqe_size;
    uint16_t udma_cqe_size;
    uint16_t nic_cqe_size;
    uint32_t aeqe_depth;
    uint32_t ceqe_depth;
    uint32_t udma_jfs_max_cnt;
    uint8_t rsvd2[4];

    uint32_t udma_jfs_depth;
    uint32_t udma_jfr_max_cnt;
    uint8_t rsvd3[4];
    uint32_t udma_jfr_depth;
    uint8_t rsvd4[12];
    uint32_t udma_jfc_max_cnt;

    uint8_t rsvd5[4];
    uint32_t udma_jfc_depth;
    uint8_t rsvd6[24];

    uint32_t nic_jfs_max_cnt;
    uint8_t rsvd7[4];
    uint32_t nic_jfs_depth;
    uint32_t nic_jfr_max_cnt;
    uint8_t rsvd8[4];
    uint32_t nic_jfr_depth;
    uint32_t rsvd9[2];

    uint32_t rsvd10;
    uint32_t nic_jfc_max_cnt;
    uint8_t rsvd11[4];
    uint32_t nic_jfc_depth;
    uint8_t rsvd12[16];

    uint8_t rsvd13[8];
    uint32_t total_ue_num;
    uint8_t rsvd14[16];
    uint16_t rsvd_jetty_cnt;
    uint16_t mac_stats_num;

    uint32_t ta_extdb_buf_size;
    uint32_t ta_timer_buf_size;
    uint32_t public_jetty_cnt;
    uint8_t rsvd15[10];
    uint8_t udma_tp_resp_vl_offset;
    uint8_t ue_num;
    uint8_t rsvd16[8];

    uint8_t rsvd17[8];
    uint32_t udma_rc_depth;
    uint8_t rsvd18[4];
    uint32_t jtg_max_cnt;
    uint32_t rc_max_cnt_per_vl;
    uint8_t rsvd19[8];

    uint8_t rsvd20[32];
} UBCResCmdResp;

typedef struct QEMU_PACKED UBCCtrlInfoResp {
    uint32_t rsvd0[2];
    uint8_t flags;
    uint8_t rsvd1[15];
} UBCCtrlInfoResp;

typedef struct QEMU_PACKED UBCPortBitmapResp {
    uint32_t logic_port_bitmap;
    uint32_t chip_id;
    uint32_t die_id;
    uint32_t resv[3];
} UBCPortBitmapResp;

typedef struct QEMU_PACKED UBCChipInfoResp {
    uint16_t nl_port_id;
    uint16_t chip_id;
    uint16_t die_id;
    uint16_t io_port_id;
    uint16_t ue_id;
    uint16_t ub_port_logic_id;
    uint16_t nl_id;
    uint16_t io_port_logic_id;
} UBCChipInfoResp;

typedef struct QEMU_PACKED UBCPortInfoResp {
    uint32_t speed;
    uint8_t rsv[10];
    uint8_t lanes;
    uint8_t rsv2[9];
} UBCPortInfoResp;

typedef struct QEMU_PACKED UBCOorResp {
    uint8_t oor_en;
    uint8_t reorder_cq_buffer_en;
    uint8_t reorder_cap;
    uint8_t reorder_cq_shift;
    uint32_t on_flight_size;
    uint8_t dynamic_ack_timeout;
    uint8_t rsvd0[15];
} UBCOorResp;

/*
 * Response for UDMA_CMD_QUERY_UE_RES (0x0002).
 * Must match guest struct udma_cmd_ue_resource in udma_cmd.h exactly.
 * The struct is split into BD0..BD3 blocks; total 128 bytes.
 */
typedef struct QEMU_PACKED UBCUeResourceResp {
    /* BD0 */
    uint16_t jfs_num_shift : 4;
    uint16_t jfr_num_shift : 4;
    uint16_t jfc_num_shift : 4;
    uint16_t jetty_num_shift : 4;

    uint16_t jetty_grp_num;

    uint16_t jfs_depth_shift : 4;
    uint16_t jfr_depth_shift : 4;
    uint16_t jfc_depth_shift : 4;
    uint16_t cqe_size_shift : 4;

    uint16_t jfs_sge : 5;
    uint16_t jfr_sge : 5;
    uint16_t jfs_rsge : 6;

    uint16_t max_jfs_inline_sz;
    uint16_t max_jfc_inline_sz;
    uint32_t cap_info;

    uint16_t trans_mode : 5;
    uint16_t ue_num : 8;
    uint16_t virtualization : 1;
    uint16_t dcqcn_sw_en : 1;
    uint16_t rsvd0 : 1;

    uint16_t ue_cnt;
    uint8_t  ue_id;
    uint8_t  default_cong_alg;
    uint8_t  cons_ctrl_alg;
    uint8_t  cc_priority_cnt;

    /* BD1 */
    uint16_t src_addr_tbl_sz;
    uint16_t src_addr_tbl_num;
    uint16_t dest_addr_tbl_sz;
    uint16_t dest_addr_tbl_num;
    uint16_t seid_upi_tbl_sz;
    uint16_t seid_upi_tbl_num;
    uint16_t tpm_tbl_sz;
    uint16_t tpm_tbl_num;
    uint32_t tp_range;
    uint8_t  port_num;
    uint8_t  port_id;
    uint8_t  rsvd1[2];
    uint16_t rc_queue_num;
    uint16_t rc_depth;
    uint8_t  rc_entry;
    uint8_t  rsvd2[3];

    /* BD2 */
    uint16_t well_known_jetty_start;
    uint16_t well_known_jetty_num;
    uint16_t ccu_jetty_start;
    uint16_t ccu_jetty_num;
    uint16_t drv_jetty_start;
    uint16_t drv_jetty_num;
    uint16_t cache_lock_jetty_start;
    uint16_t cache_lock_jetty_num;
    uint16_t normal_jetty_start;
    uint16_t normal_jetty_num;
    uint16_t standard_jetty_start;
    uint16_t standard_jetty_num;
    uint32_t rsvd3[2];

    /* BD3 */
    uint32_t max_write_size;
    uint32_t max_read_size;
    uint32_t max_cas_size;
    uint32_t max_fetch_and_add_size;
    uint32_t atomic_feat;
    uint32_t rsvd4[3];
} UBCUeResourceResp;

typedef struct QEMU_PACKED UBCSlVlResp {
    uint8_t sl_num;
    uint8_t sl_vl[23];
} UBCSlVlResp;

typedef struct QEMU_PACKED UBCTmQueueResp {
    uint16_t bus_ue_id;
    uint8_t queue_num;
    uint8_t resv0;
    uint8_t queue_vl[UBASE_MAX_VL_NUM];
    uint8_t queue_id[UBASE_MAX_VL_NUM];
    uint8_t qset_id[UBASE_MAX_VL_NUM];
    uint16_t link_vld_bitmap;
    uint8_t resv1[2];
} UBCTmQueueResp;

typedef struct QEMU_PACKED UBCTmQsetResp {
    uint16_t bus_ue_id;
    uint8_t qset_num;
    uint8_t rate_limit_bypass;
    uint8_t ir_b[UBASE_MAX_VL_NUM];
    uint8_t ir_u[UBASE_MAX_VL_NUM];
    uint8_t ir_s[UBASE_MAX_VL_NUM];
    uint8_t bs_b[UBASE_MAX_VL_NUM];
    uint8_t bs_s[UBASE_MAX_VL_NUM];
    uint32_t rate[UBASE_MAX_VL_NUM];
    uint8_t qset_id[UBASE_MAX_VL_NUM];
    uint8_t pri_id[UBASE_MAX_VL_NUM];
    uint16_t qset_pri_link_vld;
    uint16_t qset_sch_mode;
    uint8_t qset_weight[UBASE_MAX_VL_NUM];
    uint8_t resv1[16];
} UBCTmQsetResp;

typedef struct QEMU_PACKED UBCMbox {
    uint32_t in_param_l;
    uint32_t in_param_h;
    uint32_t cmd_tag;
    uint32_t ctrl;
    uint32_t status;
} UBCMbox;

typedef struct QEMU_PACKED UBCUe2UeCommonHead {
    uint16_t bus_ue_id;
    uint16_t mbx_ue_id;
    uint16_t sub_cmd;
    uint16_t status;
} UBCUe2UeCommonHead;

typedef struct QEMU_PACKED UBCUe2UeCtrlqHead {
    UBCUe2UeCommonHead head;
    uint16_t seq;
    uint16_t in_size;
    uint16_t out_size;
    uint8_t flags;
    uint8_t rsv;
} UBCUe2UeCtrlqHead;

typedef struct QEMU_PACKED UBCCtrlqMsgHdr {
    uint8_t service_ver;
    uint8_t service_type;
    uint8_t bb_num;
    uint8_t opcode;
    uint8_t ret;
    uint16_t seq;
    uint8_t mbx_ue_id;
    uint16_t bus_ue_id;
    uint16_t rsv;
} UBCCtrlqMsgHdr;

typedef struct QEMU_PACKED UBCCtrlqQuerySlResp {
    uint16_t unic_sl_bitmap;
    uint16_t rc_max_cnt;
    uint16_t udma_tp_sl_bitmap;
    uint16_t udma_ctp_sl_bitmap;
    uint8_t rsv[12];
} UBCCtrlqQuerySlResp;

typedef struct QEMU_PACKED UBCCtrlqQueryVlResp {
    uint16_t vl_bitmap;
    uint8_t rsv[18];
} UBCCtrlqQueryVlResp;

static void ubc_raise_cmdq_event(BusControllerDev *ubc_dev);
static void ubc_raise_ctrlq_event(BusControllerDev *ubc_dev);
static bool ubc_notify_vector(BusControllerDev *ubc_dev, uint16_t usi_vector,
                              uint16_t msi_vector, const char *name);

static uint32_t ub_msgq_cq_int_ro(BusControllerState *s)
{
    uint32_t status = ub_get_long(s->msgq_reg + CQ_INT_STATUS) & 0x1;
    uint32_t mask = ub_get_long(s->msgq_reg + CQ_INT_MASK) & 0x1;

    return status & ~mask;
}

static void ub_msgq_update_irq(BusControllerState *s)
{
    uint32_t ro = ub_msgq_cq_int_ro(s);
    qemu_log("ub_msgq_update_irq %s ro=%u mask=%u status=%u\n",
             s->ubc_dev ? s->ubc_dev->parent.qdev.id : "<unknown>",
             ro,
             ub_get_long(s->msgq_reg + CQ_INT_MASK) & 0x1,
             ub_get_long(s->msgq_reg + CQ_INT_STATUS) & 0x1);
    qemu_set_irq(s->irq, ro ? 1 : 0);
}

static uint64_t ub_msgq_reg_read(void *opaque, hwaddr addr, unsigned len)
{
    BusControllerState *s = opaque;
    uint64_t val;

    if (addr == CQ_INT_RO) {
        return ub_msgq_cq_int_ro(s);
    }

    switch (len) {
    case BYTE_SIZE:
        val = ub_get_byte(s->msgq_reg + addr);
        break;
    case WORD_SIZE:
        val = ub_get_word(s->msgq_reg + addr);
        break;
    case DWORD_SIZE:
        val = ub_get_long(s->msgq_reg + addr);
        break;
    default:
        qemu_log("invalid argument len 0x%x\n", len);
        val = ~0x0;
        break;
    }

    return val;
}

static void ub_msgq_reg_write(void *opaque, hwaddr addr, uint64_t val, unsigned len)
{
    BusControllerState *s = opaque;

    if (len != DWORD_SIZE) {
        switch (len) {
        case BYTE_SIZE:
            ub_set_byte(s->msgq_reg + addr, val);
            break;
        case WORD_SIZE:
            ub_set_word(s->msgq_reg + addr, val);
            break;
        default:
            qemu_log("invalid argument len 0x%x val 0x%" PRIx64 "\n", len, val);
            return;
        }
        return;
    }

    switch (addr) {
    case CQ_INT_MASK:
        ub_set_long(s->msgq_reg + addr, val & 0x1);
        ub_msgq_update_irq(s);
        return;
    case CQ_INT_STATUS:
        if (val & 0x1) {
            ub_set_long(s->msgq_reg + addr, 0);
        }
        ub_msgq_update_irq(s);
        return;
    case CQ_INT_SET:
        if (val & 0x1) {
            ub_set_long(s->msgq_reg + CQ_INT_STATUS, 0x1);
        }
        ub_msgq_update_irq(s);
        return;
    default:
        break;
    }

    switch (len) {
    case DWORD_SIZE:
        ub_set_long(s->msgq_reg + addr, val);
        break;
    default:
        /* As length is under guest control, handle illegal values. */
        qemu_log("invalid argument len 0x%x val 0x%" PRIx64 "\n", len, val);
        return;
    }

    /* only support 1 queue */
    switch (addr) {
    case SQ_PI:
        if (!s->msgq.sq_inited || !s->msgq.rq_inited || !s->msgq.cq_inited) {
            qemu_log("skip SQ_PI processing before msgq fully initialized: sq=%d rq=%d cq=%d pi=%" PRIu64 "\n",
                     s->msgq.sq_inited, s->msgq.rq_inited, s->msgq.cq_inited, val);
            break;
        }
        if (msgq_process_task(s, val)) {
            ub_try_inject_remote_cfg_notifies(s);
        }
        break;
    case SQ_ADDR_H:
        msgq_sq_init(s);
        break;
    case CQ_ADDR_H:
        msgq_cq_init(s);
        ub_fm_try_inject_pending_entities();
        break;
    case RQ_ADDR_H:
        msgq_rq_init(s);
        ub_fm_try_inject_pending_entities();
        break;
    case MSGQ_RST:
        msgq_handle_rst(s);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps ub_msgq_reg_ops = {
    .read = ub_msgq_reg_read,
    .write = ub_msgq_reg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static const MemoryRegionOps ub_fm_msgq_reg_ops = {
    .read = ub_fm_msgq_reg_read,
    .write = ub_fm_msgq_reg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static inline uint32_t ubc_ers2_read32(BusControllerDev *ubc_dev, hwaddr off)
{
    return ldl_le_p(ubc_dev->ers[2].storage + off);
}

static inline void ubc_ers2_write32(BusControllerDev *ubc_dev, hwaddr off,
                                    uint32_t val)
{
    stl_le_p(ubc_dev->ers[2].storage + off, val);
}

static inline uint64_t ubc_read_q_base(BusControllerDev *ubc_dev, hwaddr lo_reg)
{
    uint64_t lo = ubc_ers2_read32(ubc_dev, lo_reg);
    uint64_t hi = ubc_ers2_read32(ubc_dev, lo_reg + 4);

    return (hi << 32) | lo;
}

static inline uint16_t ubc_read_q_depth(BusControllerDev *ubc_dev, hwaddr depth_reg)
{
    uint32_t raw = ubc_ers2_read32(ubc_dev, depth_reg);

    return raw ? (uint16_t)(raw << 3) : 0;
}

static inline AddressSpace *ubc_dma_as(BusControllerDev *ubc_dev)
{
    UBDevice *ub_dev = &ubc_dev->parent;
    AddressSpace *as = ub_device_iommu_address_space(ub_dev);

    return as ? as : &address_space_memory;
}

typedef enum UBCDmaAccessPath {
    UBC_DMA_ACCESS_CTRL = 0,
    UBC_DMA_ACCESS_DATA = 1,
} UBCDmaAccessPath;

#define UBC_DMA_TID_AUTO UINT32_MAX

/*
 * Kernel-side UDMA paths program queue/context addresses as virtual addresses.
 * In simulation, if UMMU translation misses, recover by aliasing the low 32 bits
 * into guest RAM window [0x40000000, ...], consistent with UMMU early alias.
 */
static bool ubc_is_kernel_linear_va(dma_addr_t iova)
{
    return iova >= 0xFFFF800000000000ULL && iova < 0xFFFFC00000000000ULL;
}

static MemTxResult ubc_dma_read_via_cpu_ptw(dma_addr_t va, void *buf, size_t len)
{
    CPUState *cs = first_cpu;
    uint8_t *dst = buf;
    size_t done = 0;

    if (!cs) {
        return MEMTX_DECODE_ERROR;
    }

    while (done < len) {
        vaddr cur_va = (vaddr)(va + done);
        hwaddr page_base_va = (hwaddr)cur_va & ~(UDMA_HW_PAGE_SIZE - 1);
        hwaddr page_base_pa = cpu_get_phys_page_debug(cs, page_base_va);
        size_t page_off = (size_t)cur_va & (UDMA_HW_PAGE_SIZE - 1);
        size_t chunk = MIN(len - done, UDMA_HW_PAGE_SIZE - page_off);
        MemTxResult ret;

        if (page_base_pa == (hwaddr)-1) {
            return MEMTX_DECODE_ERROR;
        }

        ret = address_space_read(&address_space_memory,
                                 page_base_pa + page_off,
                                 MEMTXATTRS_UNSPECIFIED,
                                 dst + done, chunk);
        if (ret != MEMTX_OK) {
            return ret;
        }
        done += chunk;
    }

    return MEMTX_OK;
}

static MemTxResult ubc_dma_write_via_cpu_ptw(dma_addr_t va, const void *buf, size_t len)
{
    CPUState *cs = first_cpu;
    const uint8_t *src = buf;
    size_t done = 0;

    if (!cs) {
        return MEMTX_DECODE_ERROR;
    }

    while (done < len) {
        vaddr cur_va = (vaddr)(va + done);
        hwaddr page_base_va = (hwaddr)cur_va & ~(UDMA_HW_PAGE_SIZE - 1);
        hwaddr page_base_pa = cpu_get_phys_page_debug(cs, page_base_va);
        size_t page_off = (size_t)cur_va & (UDMA_HW_PAGE_SIZE - 1);
        size_t chunk = MIN(len - done, UDMA_HW_PAGE_SIZE - page_off);
        MemTxResult ret;

        if (page_base_pa == (hwaddr)-1) {
            return MEMTX_DECODE_ERROR;
        }

        ret = address_space_write(&address_space_memory,
                                  page_base_pa + page_off,
                                  MEMTXATTRS_UNSPECIFIED,
                                  src + done, chunk);
        if (ret != MEMTX_OK) {
            return ret;
        }
        done += chunk;
    }

    return MEMTX_OK;
}

static bool ubc_dma_alias_addr(dma_addr_t iova, dma_addr_t *alias)
{
    if (ubc_is_kernel_linear_va(iova)) {
        /*
         * Guest SQ/RQ/CQ buffers can be programmed as kernel linear VAs
         * (phys_to_virt(pa)). Convert back with the arm64 linear-map base.
         */
        *alias = iova - 0xFFFF800000000000ULL;
        return true;
    }

    if (iova <= 0x200000000ULL) {
        return false;
    }

    *alias = 0x40000000ULL + (iova & 0xFFFFFFFFULL);
    return true;
}

static inline bool ubc_dma_fallback_allowed(BusControllerDev *ubc_dev,
                                            UBCDmaAccessPath path)
{
    return path == UBC_DMA_ACCESS_CTRL &&
           ubc_dev && ubc_dev->dma_fallback_init_window;
}

static inline void ubc_close_dma_fallback_window(BusControllerDev *ubc_dev,
                                                 const char *reason)
{
    if (!ubc_dev || !ubc_dev->dma_fallback_init_window) {
        return;
    }
    ubc_dev->dma_fallback_init_window = false;
    qemu_log("ubc dma fallback: init window closed (%s)\n",
             reason ? reason : "unspecified");
}

MemTxResult ubc_sim_dec_remote_write(BusControllerDev *ubc_dev,
                                     uint64_t remote_uba,
                                     uint32_t token_id,
                                     uint32_t dcna,
                                     const uint8_t *data,
                                     uint32_t len);
MemTxResult ubc_sim_dec_remote_read(BusControllerDev *ubc_dev,
                                    uint64_t remote_uba,
                                    uint32_t token_id,
                                    uint32_t dcna,
                                           uint8_t *data,
                                           uint32_t len);

/*
 * General-purpose DMA read from an IOVA address.
 * Tries IOMMU address space first, then CSQ learned bias,
 * then linear IOVA mapping and low-bits mapping.
 */
static MemTxResult ubc_dma_read_ex(BusControllerDev *ubc_dev, dma_addr_t iova,
                                   void *buf, size_t len, UBCDmaAccessPath path,
                                   uint32_t tid)
{
    AddressSpace *as = ubc_dma_as(ubc_dev);
    dma_addr_t alias;
    bool is_kva = ubc_is_kernel_linear_va(iova);
    bool try_ptw = is_kva || (iova >= 0xFFFF000000000000ULL);
    bool has_alias = ubc_dma_alias_addr(iova, &alias);
    bool allow_fallback = ubc_dma_fallback_allowed(ubc_dev, path);
    MemTxResult ret;
    bool trace_iova = ((iova & 0xFFFFFFFFFFFFF000ULL) == 0xFFFF8000805E6000ULL);
    bool tid_override_active = false;
    UMMUTidOverrideScope tid_scope = { 0 };
    SimDecLookupResult lookup = { 0 };

    if (path == UBC_DMA_ACCESS_DATA && ubc_dev &&
        len > 0 &&
        sim_dec_lookup_result_by_pa((uint64_t)iova, &lookup) == 0) {
        uint32_t eff_token_id = lookup.token_id ? lookup.token_id : tid;
        if (sim_dec_gva_dma_access_fault(&lookup, "read",
                                         (uint64_t)iova, len)) {
            if (g_sim_decoder) {
                g_sim_decoder->stats.read_errors++;
            }
            return MEMTX_DECODE_ERROR;
        }
        if (g_sim_decoder) {
            g_sim_decoder->stats.dma_path_reads++;
            g_sim_decoder->stats.dma_path_read_bytes += len;
            if (lookup.address_profile != 0) {
                g_sim_decoder->stats.gva_dma_reads++;
                g_sim_decoder->stats.gva_dma_read_bytes += len;
                sim_dec_log_gva_dma_path(&lookup, "read", (uint64_t)iova,
                                         len,
                                         g_sim_decoder->stats.gva_dma_reads);
            }
        }
        if (lookup.cache_policy == SIM_DEC_CACHE_POLICY_DIRECTORY_MESI) {
            if (len > UINT32_MAX) {
                return MEMTX_DECODE_ERROR;
            }
            return obmm_coh_read(ubc_dev, lookup.remote_uba,
                                 eff_token_id, lookup.dcna,
                                 (uint8_t *)buf, (uint32_t)len);
        }
        return ubc_sim_dec_remote_read(ubc_dev, lookup.remote_uba,
                                       eff_token_id, lookup.dcna,
                                       (uint8_t *)buf, (uint32_t)len);
    }

    if (ubc_dev && ubc_dev->ummu && tid != UBC_DMA_TID_AUTO) {
        tid_scope = ummu_dma_tid_override_enter(ubc_dev->ummu, tid);
        tid_override_active = true;
    }

    ret = address_space_read(as, iova, MEMTXATTRS_UNSPECIFIED, buf, len);
    if (trace_iova) {
        qemu_log("ubc dma read trace: iova=%#" PRIx64 " len=%zu as=%s ret=%d path=%s is_kva=%d try_ptw=%d has_alias=%d alias=%#" PRIx64 "\n",
                 (uint64_t)iova, len,
                 (as == &address_space_memory) ? "memory" : "iommu", ret,
                 path == UBC_DMA_ACCESS_DATA ? "data" : "ctrl",
                 is_kva, try_ptw, has_alias, (uint64_t)alias);
    }

    if (ret != MEMTX_OK && allow_fallback && try_ptw) {
        ret = ubc_dma_read_via_cpu_ptw(iova, buf, len);
        if (trace_iova) {
            qemu_log("ubc dma read trace: cpu_ptw iova=%#" PRIx64 " ret=%d\n",
                     (uint64_t)iova, ret);
        }
    }

    if (ret != MEMTX_OK && allow_fallback && as != &address_space_memory) {
        /* IOMMU translation failed — fall back to direct physical access.
         * This handles the early-boot case where the guest kernel's DMA layer
         * allocates IOVAs from the IOMMU aperture but the UMMU has not yet
         * been programmed with TECTE/page tables.  The controller device
         * should be able to access guest memory directly in this case. */
        ret = address_space_read(&address_space_memory, iova,
                                 MEMTXATTRS_UNSPECIFIED, buf, len);
        if (trace_iova) {
            qemu_log("ubc dma read trace: fallback memory iova=%#" PRIx64 " ret=%d\n",
                     (uint64_t)iova, ret);
        }
    }
    if (ret != MEMTX_OK && allow_fallback && has_alias) {
        ret = address_space_read(&address_space_memory, alias,
                                 MEMTXATTRS_UNSPECIFIED, buf, len);
        if (ret == MEMTX_OK) {
            qemu_log("ubc dma read alias: iova=%#" PRIx64 " -> %#" PRIx64
                     " len=%zu\n", (uint64_t)iova, (uint64_t)alias, len);
        }
    }
    if (ret != MEMTX_OK && path == UBC_DMA_ACCESS_DATA) {
        qemu_log("ubc dma strict read failed: iova=%#" PRIx64
                 " len=%zu ret=%d tid=%u auto=%u has_ummu=%u\n",
                 (uint64_t)iova, len, ret, tid,
                 tid == UBC_DMA_TID_AUTO, ubc_dev && ubc_dev->ummu);
    }
    if (tid_override_active) {
        ummu_dma_tid_override_leave(ubc_dev->ummu, tid_scope);
    }
    return ret;
}

static MemTxResult ubc_dma_write_ex(BusControllerDev *ubc_dev, dma_addr_t iova,
                                    const void *buf, size_t len,
                                    UBCDmaAccessPath path, uint32_t tid)
{
    AddressSpace *as = ubc_dma_as(ubc_dev);
    dma_addr_t alias;
    bool is_kva = ubc_is_kernel_linear_va(iova);
    bool try_ptw = is_kva || (iova >= 0xFFFF000000000000ULL);
    bool has_alias = ubc_dma_alias_addr(iova, &alias);
    bool allow_fallback = ubc_dma_fallback_allowed(ubc_dev, path);
    MemTxResult ret;
    bool tid_override_active = false;
    UMMUTidOverrideScope tid_scope = { 0 };
    SimDecLookupResult lookup = { 0 };

    if (path == UBC_DMA_ACCESS_DATA && ubc_dev &&
        len > 0 &&
        sim_dec_lookup_result_by_pa((uint64_t)iova, &lookup) == 0) {
        uint32_t eff_token_id = lookup.token_id ? lookup.token_id : tid;
        if (sim_dec_gva_dma_access_fault(&lookup, "write",
                                         (uint64_t)iova, len)) {
            if (g_sim_decoder) {
                g_sim_decoder->stats.write_errors++;
            }
            return MEMTX_DECODE_ERROR;
        }
        if (g_sim_decoder) {
            g_sim_decoder->stats.dma_path_writes++;
            g_sim_decoder->stats.dma_path_write_bytes += len;
            if (lookup.address_profile != 0) {
                g_sim_decoder->stats.gva_dma_writes++;
                g_sim_decoder->stats.gva_dma_write_bytes += len;
                sim_dec_log_gva_dma_path(&lookup, "write", (uint64_t)iova,
                                         len,
                                         g_sim_decoder->stats.gva_dma_writes);
            }
        }
        if (lookup.cache_policy == SIM_DEC_CACHE_POLICY_DIRECTORY_MESI) {
            if (len > UINT32_MAX) {
                return MEMTX_DECODE_ERROR;
            }
            return obmm_coh_write(ubc_dev, lookup.remote_uba,
                                  eff_token_id, lookup.dcna,
                                  (const uint8_t *)buf, (uint32_t)len);
        }
        return ubc_sim_dec_remote_write(ubc_dev, lookup.remote_uba,
                                        eff_token_id, lookup.dcna,
                                        (const uint8_t *)buf,
                                        (uint32_t)len);
    }

    if (ubc_dev && ubc_dev->ummu && tid != UBC_DMA_TID_AUTO) {
        tid_scope = ummu_dma_tid_override_enter(ubc_dev->ummu, tid);
        tid_override_active = true;
    }

    ret = address_space_write(as, iova, MEMTXATTRS_UNSPECIFIED, buf, len);

    if (ret != MEMTX_OK && allow_fallback && try_ptw) {
        ret = ubc_dma_write_via_cpu_ptw(iova, buf, len);
    }
    if (ret != MEMTX_OK && allow_fallback && as != &address_space_memory) {
        ret = address_space_write(&address_space_memory, iova,
                                  MEMTXATTRS_UNSPECIFIED, buf, len);
    }
    if (ret != MEMTX_OK && allow_fallback && has_alias) {
        ret = address_space_write(&address_space_memory, alias,
                                  MEMTXATTRS_UNSPECIFIED, buf, len);
        if (ret == MEMTX_OK) {
            qemu_log("ubc dma write alias: iova=%#" PRIx64 " -> %#" PRIx64
                     " len=%zu\n", (uint64_t)iova, (uint64_t)alias, len);
        }
    }
    if (ret != MEMTX_OK && path == UBC_DMA_ACCESS_DATA) {
        qemu_log("ubc dma strict write failed: iova=%#" PRIx64
                 " len=%zu ret=%d tid=%u auto=%u has_ummu=%u\n",
                 (uint64_t)iova, len, ret, tid,
                 tid == UBC_DMA_TID_AUTO, ubc_dev && ubc_dev->ummu);
    }
    if (tid_override_active) {
        ummu_dma_tid_override_leave(ubc_dev->ummu, tid_scope);
    }
    return ret;
}

static inline MemTxResult ubc_dma_read_ctrl(BusControllerDev *ubc_dev,
                                            dma_addr_t iova, void *buf,
                                            size_t len)
{
    return ubc_dma_read_ex(ubc_dev, iova, buf, len, UBC_DMA_ACCESS_CTRL,
                           UBC_DMA_TID_AUTO);
}

static inline MemTxResult ubc_dma_read_data_tid(BusControllerDev *ubc_dev,
                                                dma_addr_t iova, void *buf,
                                                size_t len, uint32_t tid)
{
    return ubc_dma_read_ex(ubc_dev, iova, buf, len, UBC_DMA_ACCESS_DATA, tid);
}

static inline uint32_t ubc_tid_or_auto(uint32_t tid)
{
    return tid ? tid : UBC_DMA_TID_AUTO;
}

static inline MemTxResult ubc_dma_write_ctrl(BusControllerDev *ubc_dev,
                                             dma_addr_t iova, const void *buf,
                                             size_t len)
{
    return ubc_dma_write_ex(ubc_dev, iova, buf, len, UBC_DMA_ACCESS_CTRL,
                            UBC_DMA_TID_AUTO);
}

static inline MemTxResult ubc_dma_write_data(BusControllerDev *ubc_dev,
                                             dma_addr_t iova, const void *buf,
                                             size_t len)
{
    return ubc_dma_write_ex(ubc_dev, iova, buf, len, UBC_DMA_ACCESS_DATA,
                            UBC_DMA_TID_AUTO);
}

static inline MemTxResult ubc_dma_write_data_tid(BusControllerDev *ubc_dev,
                                                 dma_addr_t iova,
                                                 const void *buf, size_t len,
                                                 uint32_t tid)
{
    return ubc_dma_write_ex(ubc_dev, iova, buf, len, UBC_DMA_ACCESS_DATA, tid);
}

/*
 * Keep existing helper names as ctrl-path default to avoid touching all
 * mailbox/cmdq call sites. Data-path call sites use *_data explicitly.
 */
static inline MemTxResult ubc_dma_read(BusControllerDev *ubc_dev,
                                       dma_addr_t iova, void *buf, size_t len)
{
    return ubc_dma_read_ctrl(ubc_dev, iova, buf, len);
}

static inline MemTxResult ubc_dma_write(BusControllerDev *ubc_dev,
                                        dma_addr_t iova, const void *buf,
                                        size_t len)
{
    return ubc_dma_write_ctrl(ubc_dev, iova, buf, len);
}

static MemTxResult ubc_read_desc(const UBCmdQueueState *q,
                                 BusControllerDev *ubc_dev,
                                 uint16_t idx,
                                 UBCCmdqDesc *desc)
{
    AddressSpace *as = ubc_dma_as(ubc_dev);
    dma_addr_t addr = q->base + (dma_addr_t)idx * sizeof(*desc);
    MemTxResult ret = dma_memory_read(as, addr, desc, sizeof(*desc),
                                      MEMTXATTRS_MEMORY);
    if (ret != MEMTX_OK && as != &address_space_memory) {
        ret = dma_memory_read(&address_space_memory, addr, desc,
                              sizeof(*desc), MEMTXATTRS_MEMORY);
    }
    return ret;
}

static MemTxResult ubc_write_desc(const UBCmdQueueState *q,
                                  BusControllerDev *ubc_dev,
                                  uint16_t idx,
                                  const UBCCmdqDesc *desc)
{
    AddressSpace *as = ubc_dma_as(ubc_dev);
    dma_addr_t addr = q->base + (dma_addr_t)idx * sizeof(*desc);
    MemTxResult ret = dma_memory_write(as, addr, desc, sizeof(*desc),
                                       MEMTXATTRS_MEMORY);
    if (ret != MEMTX_OK && as != &address_space_memory) {
        ret = dma_memory_write(&address_space_memory, addr, desc,
                               sizeof(*desc), MEMTXATTRS_MEMORY);
    }
    return ret;
}

static size_t ubc_cmdq_capacity(uint8_t bd_num)
{
    if (!bd_num) {
        return 0;
    }
    return (size_t)bd_num * sizeof(UBCCmdqDesc) - 8;
}

static inline uint16_t ubc_ring_free(uint16_t head, uint16_t tail,
                                     uint16_t depth)
{
    uint16_t used;

    if (!depth) {
        return 0;
    }
    used = (tail + depth - head) % depth;
    return depth - used - 1;
}

static uint8_t ubc_cmdq_bd_num_from_len(size_t len)
{
    if (len <= sizeof(((UBCCmdqDesc *)0)->data)) {
        return 1;
    }

    return (uint8_t)(1 + DIV_ROUND_UP(len - sizeof(((UBCCmdqDesc *)0)->data),
                                       sizeof(UBCCmdqDesc)));
}

static uint8_t ubc_ctrlq_bb_num_from_data_len(size_t data_len)
{
    if (data_len <= UBASE_CTRLQ_DATA_LEN) {
        return 1;
    }

    return (uint8_t)(1 + DIV_ROUND_UP(data_len - UBASE_CTRLQ_DATA_LEN,
                                       UBASE_CTRLQ_BB_LEN));
}

static int ubc_cmdq_push_crq_msg(BusControllerDev *ubc_dev, uint16_t opcode,
                                 const uint8_t *payload, size_t payload_len)
{
    UBCmdQueueState *crq = &ubc_dev->cmd_crq;
    UBCCmdqDesc *descs;
    uint8_t bd_num;
    uint16_t free_num;
    uint16_t idx;
    size_t off = 0;
    int i;
    int ret = 0;

    if (!crq->base || !crq->depth) {
        return -ENXIO;
    }

    bd_num = ubc_cmdq_bd_num_from_len(payload_len);
    free_num = ubc_ring_free(crq->head, crq->tail, crq->depth);
    if (bd_num > free_num) {
        return -ENOSPC;
    }

    descs = g_new0(UBCCmdqDesc, bd_num);
    descs[0].opcode = cpu_to_le16(opcode);
    descs[0].flag = UBASE_CMD_FLAG_IN | UBASE_CMD_FLAG_OUT;
    descs[0].bd_num = bd_num;

    if (payload_len) {
        size_t copy_len = MIN(sizeof(descs[0].data), payload_len);

        memcpy(descs[0].data, payload, copy_len);
        off = copy_len;
    }
    for (i = 1; i < bd_num && off < payload_len; i++) {
        size_t copy_len = MIN(sizeof(UBCCmdqDesc), payload_len - off);

        memcpy(&descs[i], payload + off, copy_len);
        off += copy_len;
    }

    idx = crq->tail;
    for (i = 0; i < bd_num; i++) {
        MemTxResult wret;
        qemu_log("ubc push_crq idx=%u/%u crq_base=%#" PRIx64 " depth=%u"
                 " head=%u tail=%u bd_num=%u opcode=0x%x\n",
                 idx, crq->depth, (uint64_t)crq->base, crq->depth,
                 crq->head, crq->tail, bd_num, opcode);
        wret = ubc_write_desc(crq, ubc_dev, idx, &descs[i]);
        if (wret != MEMTX_OK) {
            qemu_log("ubc push_crq write failed idx=%u wret=%d\n", idx, wret);
            ret = -EIO;
            break;
        }
        idx = (idx + 1) % crq->depth;
    }

    if (!ret) {
        UBCCmdqDesc verify;
        uint16_t first = (crq->tail - bd_num + crq->depth) % crq->depth;
        crq->tail = idx;
        ubc_ers2_write32(ubc_dev, UBASE_CRQ_TAIL_REG, crq->tail);
        ubc_raise_cmdq_event(ubc_dev);
        /* verify first desc was written to guest RAM */
        if (ubc_read_desc(crq, ubc_dev, first, &verify) == MEMTX_OK) {
            qemu_log("ubc push_crq verify[%u] opcode=0x%x flag=0x%x bd=%u"
                     " data[0]=0x%08x\n",
                     first, le16_to_cpu(verify.opcode), verify.flag,
                     verify.bd_num, le32_to_cpu(verify.data[0]));
        } else {
            qemu_log("ubc push_crq verify readback failed\n");
        }
    }
    g_free(descs);

    return ret;
}

static int ubc_handle_ue_to_mue(BusControllerDev *ubc_dev,
                                const uint8_t *req, size_t req_len)
{
    uint8_t resp[sizeof(UBCEntityMsg) + sizeof(int32_t)] = { 0 };
    const UBCEntityMsg *req_msg = (const UBCEntityMsg *)req;
    UBCEntityMsg *resp_msg = (UBCEntityMsg *)resp;
    int32_t ret_code = 0;

    if (req_len < sizeof(*req_msg)) {
        qemu_log("ubc UE_TO_MUE invalid len=%zu\n", req_len);
        return -EINVAL;
    }

    switch (req_msg->opcode) {
    case UDMA_CMD_NOTIFY_MUE_SAVE_TP:
        resp_msg->dst_ue_idx = req_msg->dst_ue_idx;
        resp_msg->opcode = req_msg->opcode;
        resp_msg->buf.len = cpu_to_le32(sizeof(ret_code));
        resp_msg->buf.seq_num = req_msg->buf.seq_num;
        memcpy(resp_msg->buf.data, &ret_code, sizeof(ret_code));

        qemu_log("ubc UE_TO_MUE save_tp ack: dst_ue=%u seq=%u req_len=%zu\n",
                 req_msg->dst_ue_idx, le32_to_cpu(req_msg->buf.seq_num), req_len);
        return ubc_cmdq_push_crq_msg(ubc_dev, UBASE_OPC_MUE_TO_UE,
                                     resp, sizeof(resp));
    default:
        qemu_log("ubc UE_TO_MUE unsupported opcode=0x%x len=%zu\n",
                 req_msg->opcode, req_len);
        return -EOPNOTSUPP;
    }
}

static int ubc_handle_ue2ue_ctrlq(BusControllerDev *ubc_dev,
                                  const uint8_t *req, size_t req_len,
                                  uint8_t *resp_flat, size_t resp_cap)
{
    const UBCUe2UeCtrlqHead *req_head;
    const UBCCtrlqMsgHdr *req_hdr;
    UBCUe2UeCtrlqHead *resp_head;
    UBCCtrlqMsgHdr *resp_hdr;
    uint8_t *resp_data;
    size_t resp_data_len;
    size_t total_len;

    if (req_len < sizeof(*req_head) + UBASE_CTRLQ_HDR_LEN) {
        return -EINVAL;
    }

    req_head = (const UBCUe2UeCtrlqHead *)req;
    req_hdr = (const UBCCtrlqMsgHdr *)(req + sizeof(*req_head));
    if (ubc_trace_data_path_enabled()) {
        qemu_log("ubc cmdq ue2ue req seq=%u service_type=0x%x opcode=0x%x "
                 "in_size=%u out_size=%u flags=0x%x\n",
                 le16_to_cpu(req_head->seq), req_hdr->service_type, req_hdr->opcode,
                 le16_to_cpu(req_head->in_size), le16_to_cpu(req_head->out_size),
                 req_head->flags);
    }
    resp_data_len = le16_to_cpu(req_head->out_size);
    if (req_hdr->service_type == UBASE_CTRLQ_SER_TYPE_QOS &&
        req_hdr->opcode == UBASE_CTRLQ_OPC_QUERY_SL) {
        resp_data_len = MAX(resp_data_len, sizeof(UBCCtrlqQuerySlResp));
    } else if (req_hdr->service_type == UBASE_CTRLQ_SER_TYPE_QOS &&
               req_hdr->opcode == UBASE_CTRLQ_OPC_QUERY_VL) {
        resp_data_len = MAX(resp_data_len, sizeof(UBCCtrlqQueryVlResp));
    }

    total_len = sizeof(*resp_head) + UBASE_CTRLQ_HDR_LEN + resp_data_len;
    if (total_len > resp_cap) {
        total_len = resp_cap;
        resp_data_len = total_len - sizeof(*resp_head) - UBASE_CTRLQ_HDR_LEN;
    }

    /* Build response in resp_flat (scattered back to CSQ descriptors) */
    memset(resp_flat, 0, total_len);
    memcpy(resp_flat, req, MIN(sizeof(*resp_head) + UBASE_CTRLQ_HDR_LEN, total_len));

    resp_head = (UBCUe2UeCtrlqHead *)resp_flat;
    resp_hdr = (UBCCtrlqMsgHdr *)(resp_flat + sizeof(*resp_head));
    resp_data = resp_flat + sizeof(*resp_head) + UBASE_CTRLQ_HDR_LEN;
    resp_hdr->ret = 0;
    resp_hdr->bb_num = ubc_ctrlq_bb_num_from_data_len(resp_data_len);
    resp_head->flags |= BIT(1); /* is_resp */

    if (req_hdr->service_type == UBASE_CTRLQ_SER_TYPE_QOS &&
        req_hdr->opcode == UBASE_CTRLQ_OPC_QUERY_SL &&
        resp_data_len >= sizeof(UBCCtrlqQuerySlResp)) {
        UBCCtrlqQuerySlResp *sl = (UBCCtrlqQuerySlResp *)resp_data;

        sl->unic_sl_bitmap = cpu_to_le16(0x1);
        sl->rc_max_cnt = cpu_to_le16(64);
        sl->udma_tp_sl_bitmap = cpu_to_le16(0x1);
        sl->udma_ctp_sl_bitmap = cpu_to_le16(0x1);
    } else if (req_hdr->service_type == UBASE_CTRLQ_SER_TYPE_QOS &&
               req_hdr->opcode == UBASE_CTRLQ_OPC_QUERY_VL &&
               resp_data_len >= sizeof(UBCCtrlqQueryVlResp)) {
        UBCCtrlqQueryVlResp *vl = (UBCCtrlqQueryVlResp *)resp_data;

        vl->vl_bitmap = cpu_to_le16(0x1);
    } else if (req_hdr->service_type == UBASE_CTRLQ_SER_TYPE_TP_ACL &&
               req_hdr->opcode == UBASE_CTRLQ_OPC_GET_TP_LIST &&
               resp_data_len >= 8) {
        uint32_t tpid = ubc_dev->next_tp_id;

        if (tpid == 0 || tpid > 0x00FFFFFFU) {
            tpid = 1;
        }
        ubc_dev->next_tp_id = tpid + 1;
        if (ubc_dev->next_tp_id == 0 || ubc_dev->next_tp_id > 0x00FFFFFFU) {
            ubc_dev->next_tp_id = 1;
        }

        qemu_log("ubc ue2ue ctrlq: GET_TP_LIST returning 1 TP entry (tpid=%u)\n",
                 tpid);
        *(uint32_t *)resp_data = cpu_to_le32(1);
        *(uint32_t *)(resp_data + 4) = cpu_to_le32((tpid & 0x00FFFFFFU) | (1U << 24));
        *(uint32_t *)(resp_data + 8) = 0;
    } else if (req_hdr->service_type == UBASE_CTRLQ_SER_TYPE_DEV_REGISTER &&
               req_hdr->opcode == UBASE_CTRLQ_OPC_GET_SEID_INFO &&
               resp_data_len >= 28) {
        const char *fm_node_id = g_getenv("UB_FM_NODE_ID");
        uint8_t eid_suffix = ubc_node_ip_suffix_from_id(fm_node_id);
        uint8_t eid_hw[16];

        if (eid_suffix == 0) {
            qemu_log("ubc ue2ue seid resp: unresolved UB_FM_NODE_ID=%s\n",
                     fm_node_id ? fm_node_id : "(null)");
            return -EINVAL;
        }
        ubc_fill_link_local_eid_hw(eid_hw, eid_suffix);
        *(uint32_t *)resp_data = cpu_to_le32(1);
        *(uint32_t *)(resp_data + 4) = cpu_to_le32(0);
        memcpy(resp_data + 8, eid_hw, 16);
        qemu_log("ubc ue2ue seid resp: 1 eid fe80::%u\n", eid_suffix);
    }

    return 0;
}

static void ubc_fill_res_caps(UBCResCmdResp *resp, uint32_t entity_count)
{
    memset(resp, 0, sizeof(*resp));

    resp->cap_bits[0] = cpu_to_le32(BIT(UBASE_SUPPORT_UBL_B) |
                                    BIT(UBASE_SUPPORT_TA_EXTDB_BUF_B) |
                                    BIT(UBASE_SUPPORT_TA_TIMER_BUF_B) |
                                    BIT(UBASE_SUPPORT_IP_OVER_URMA_B));
    resp->node_type = 1;
    resp->ceq_vector_num = cpu_to_le16(16);
    resp->aeq_vector_num = cpu_to_le16(16);
    resp->misc_vector_num = cpu_to_le16(16);
    resp->aeqe_size = cpu_to_le16(64);
    resp->ceqe_size = cpu_to_le16(64);
    resp->udma_cqe_size = cpu_to_le16(64);
    resp->nic_cqe_size = cpu_to_le16(64);
    resp->aeqe_depth = cpu_to_le32(512);
    resp->ceqe_depth = cpu_to_le32(1024);

    resp->udma_jfs_max_cnt = cpu_to_le32(64);
    resp->udma_jfs_depth = cpu_to_le32(4096);
    resp->udma_jfr_max_cnt = cpu_to_le32(64);
    resp->udma_jfr_depth = cpu_to_le32(4096);
    resp->udma_jfc_max_cnt = cpu_to_le32(64);
    resp->udma_jfc_depth = cpu_to_le32(4096);

    resp->nic_jfs_max_cnt = cpu_to_le32(64);
    resp->nic_jfs_depth = cpu_to_le32(128);
    resp->nic_jfr_max_cnt = cpu_to_le32(64);
    resp->nic_jfr_depth = cpu_to_le32(128);
    resp->nic_jfc_max_cnt = cpu_to_le32(64);
    resp->nic_jfc_depth = cpu_to_le32(128);

    resp->total_ue_num = cpu_to_le32(entity_count);
    resp->rsvd_jetty_cnt = cpu_to_le16(0);
    resp->mac_stats_num = cpu_to_le16(0);
    resp->ta_extdb_buf_size = cpu_to_le32(0x1000);
    resp->ta_timer_buf_size = cpu_to_le32(0x1000);
    resp->public_jetty_cnt = cpu_to_le32(64);
    resp->udma_tp_resp_vl_offset = 0;
    resp->ue_num = entity_count;

    resp->udma_rc_depth = cpu_to_le32(1024);
    resp->jtg_max_cnt = cpu_to_le32(64);
    resp->rc_max_cnt_per_vl = cpu_to_le32(64);
}

/*
 * Parse udma_jetty_ctx raw DW words to extract SQ buffer address.
 * Layout (little-endian):
 *   DW0[11:8]  = sqe_bb_shift
 *   DW0[18:16] = state
 *   DW0[19]    = jfs_mode
 *   DW1[31:12] = sqe_base_addr_l (20 bits)
 *   DW2         = sqe_base_addr_h (32 bits)
 *   DW4[19:0]  = tx_jfcn
 *   DW4[31:20] = jfrn_l (12 bits)
 *   DW5[7:0]   = jfrn_h (8 bits)
 *   DW5[31:12] = rx_jfcn (20 bits)
 *   DW6[9:0]   = seid_idx
 */
static void ubc_parse_jetty_ctx(const uint32_t *dw, UBCJettyState *js)
{
    uint32_t sqe_base_addr_l = (dw[1] >> 12) & 0xFFFFF;
    uint32_t sqe_base_addr_h = dw[2];
    uint32_t sqe_tid_l = (dw[0] >> 20) & 0xFFF;
    uint32_t sqe_tid_h = dw[1] & 0xFF;

    js->sq_buf_addr = ((uint64_t)sqe_base_addr_h << 32) |
                      ((uint64_t)sqe_base_addr_l << 12);
    js->sqe_bb_shift = (dw[0] >> 8) & 0xF;
    js->sq_depth = 1u << js->sqe_bb_shift;
    js->sqe_token_id = sqe_tid_l | (sqe_tid_h << 12);
    js->jfs_mode = (dw[0] >> 19) & 0x1;
    js->tx_jfcn = dw[4] & 0xFFFFF;
    js->jfrn = ((dw[4] >> 20) & 0xFFF) | ((dw[5] & 0xFF) << 12);
    js->rx_jfcn = (dw[5] >> 12) & 0xFFFFF;
    js->seid_idx = dw[6] & 0x3FF;
    /* DW7-DW8: user_data (jfs kernel address, copied to CQE) */
    js->user_data_l = dw[7];
    js->user_data_h = dw[8];
}

static void ubc_parse_eq_ctx(const uint32_t *dw, UBCEqState *eq)
{
    uint32_t shift = dw[1] & 0x1F;
    uint32_t eqe_base_l = (dw[2] >> 12) & 0xFFFFF;
    uint32_t eqe_base_h = dw[3];

    eq->eq_buf_addr = ((uint64_t)eqe_base_h << 32) |
                      ((uint64_t)eqe_base_l << 12);
    if (shift <= 20) {
        eq->eq_depth = 1u << (shift + 6);
    } else {
        eq->eq_depth = 0;
    }
    eq->irq_num = dw[6] & 0xFFFF;
}

static bool ubc_push_ceq_event(BusControllerDev *ubc_dev, uint32_t ceqn,
                               uint32_t jfcn)
{
    UBCEqState *ceq;
    uint8_t ceqe[UBASE_EQE_SIZE];
    uint32_t owner;
    uint32_t comp;
    dma_addr_t ceqe_addr;
    MemTxResult ret;

    if (ceqn >= UBC_MAX_CEQS) {
        qemu_log("ubc CEQE: ceqn=%u out of range\n", ceqn);
        return false;
    }

    ceq = &ubc_dev->ceqs[ceqn];
    if (!ceq->active || ceq->eq_depth == 0) {
        qemu_log("ubc CEQE: ceq %u inactive or depth=0 (active=%d depth=%u)\n",
                 ceqn, ceq->active, ceq->eq_depth);
        return false;
    }

    owner = ceq->eq_owner_phase & 0x1;
    comp = (jfcn & 0xFFFFF) | (owner << 31);
    memset(ceqe, 0, sizeof(ceqe));
    memcpy(ceqe, &comp, sizeof(comp));

    ceqe_addr = ceq->eq_buf_addr + (dma_addr_t)ceq->eq_pi * UBASE_EQE_SIZE;
    ret = ubc_dma_write_data(ubc_dev, ceqe_addr, ceqe, sizeof(ceqe));
    if (ret != MEMTX_OK) {
        qemu_log("ubc CEQE: DMA write failed ceqn=%u addr=%#" PRIx64 "\n",
                 ceqn, (uint64_t)ceqe_addr);
        return false;
    }

    ceq->eq_pi = (ceq->eq_pi + 1) % ceq->eq_depth;
    if (ceq->eq_pi == 0) {
        ceq->eq_owner_phase ^= 1;
    }

    if (ubc_trace_data_path_enabled()) {
        qemu_log("ubc CEQE: ceqn=%u jfcn=%u owner=%u eq_pi=%u irq_num=%u\n",
                 ceqn, jfcn, owner, ceq->eq_pi, ceq->irq_num);
    }

    /*
     * CEQ notify vector mapping:
     * - USI path uses eq_ctx.irq_num from mailbox context.
     * - INT_TYPE1 path uses MSI vector index order: misc(0), aeq(1), ceq(2+ceqn).
     */
    if (ubc_notify_vector(ubc_dev, (uint16_t)ceq->irq_num,
                          (uint16_t)(2 + ceqn), "ceq")) {
        return true;
    }

    qemu_log("ubc CEQE: failed to notify vector=%u\n", ceq->irq_num);
    return false;
}

/* Forward declarations for URMA RX buffering helpers (defined later) */
static void ubc_flush_urma_rx_buffer(BusControllerDev *ubc_dev, uint32_t jetty_id);
static bool ubc_try_handle_urma_rx_data(BusControllerDev *ubc_dev, uint32_t dst_jetty,
                                        uint32_t src_jetty, uint32_t src_scna,
                                        const uint8_t src_eid[16],
                                        const uint8_t *data, uint32_t data_len,
                                        bool allow_buffer);

/*
 * Mailbox command handler.
 *
 * Protocol boundary:
 *   - Mailbox path: CMDQ opcode POST_MB(0x7000) → ubc_handle_post_mb()
 *     Handles JFS/JFC/JFR context lifecycle (CREATE/MODIFY/QUERY/DESTROY).
 *   - CtrlQ path: CMDQ opcode UE2UE_UBASE(0xF00E) → ubc_handle_ue2ue_ctrlq()
 *     Handles service-type operations (TP_ACL, DEV_REGISTER, QOS).
 *     Also available via native CtrlQ registers (separate CSQ/CRQ pair).
 *   - Query path: CMDQ opcodes like QUERY_FW_VER, QUERY_UE_RES, etc.
 *     Directly handled by ubc_cmd_fill_resp().
 */
static int ubc_handle_post_mb(BusControllerDev *ubc_dev,
                               const uint8_t *req, size_t req_len,
                               uint8_t *resp, size_t resp_cap)
{
    UBCMbox mb = { 0 };
    uint32_t cmd_tag, sub_op, tag;
    uint64_t dma_addr;
    uint8_t ctx_buf[UDMA_JETTY_CTX_SIZE];
    MemTxResult dma_ret;
    size_t resp_len;

    if (req_len < sizeof(mb) || resp_cap < sizeof(mb)) {
        return -1;
    }

    memcpy(&mb, req, sizeof(mb));

    /* Extract sub-opcode and tag from cmd_tag word */
    cmd_tag = le32_to_cpu(mb.cmd_tag);
    sub_op = cmd_tag & 0xFF;
    tag = (cmd_tag >> 8) & 0xFFFFFF;

    /* Extract DMA address of mailbox input buffer */
    dma_addr = ((uint64_t)le32_to_cpu(mb.in_param_h) << 32) |
               le32_to_cpu(mb.in_param_l);

    qemu_log("ubc POST_MB raw cmd_tag=0x%08x sub_op=0x%02x tag=%u dma_addr=%#" PRIx64 "\n",
             cmd_tag, sub_op, tag, (uint64_t)dma_addr);

    switch (sub_op) {
    /* ---- JFS (Send Queue) lifecycle ---- */
    case UBC_MB_CREATE_JFS_CONTEXT: {
        UBCJettyState *js;

        if (tag >= UBC_MAX_JETTIES) {
            qemu_log("ubc POST_MB CREATE_JFS: tag %u out of range\n", tag);
            mb.status = cpu_to_le32(1);
            break;
        }

        /* DMA-read the udma_jetty_ctx from guest memory */
        dma_ret = ubc_dma_read(ubc_dev, dma_addr, ctx_buf, sizeof(ctx_buf));
        if (dma_ret != MEMTX_OK) {
            qemu_log("ubc POST_MB CREATE_JFS: DMA read failed addr=%#" PRIx64 "\n",
                     (uint64_t)dma_addr);
            mb.status = cpu_to_le32(1);
            break;
        }
        {
            const uint32_t *dw = (const uint32_t *)ctx_buf;
            qemu_log("ubc CREATE_JFS raw short: tag=%u dma=%#" PRIx64
                     " dw0=%08x dw1=%08x dw2=%08x dw9=%08x\n",
                     tag, (uint64_t)dma_addr, dw[0], dw[1], dw[2], dw[9]);
        }

        js = &ubc_dev->jetties[tag];
        memset(js, 0, sizeof(*js));
        js->active = true;
        js->jetty_id = tag;
        js->jetty_state = JETTY_STATE_READY;
        ubc_parse_jetty_ctx((const uint32_t *)ctx_buf, js);

        qemu_log("ubc POST_MB CREATE_JFS: jetty_id=%u sq_buf=%#" PRIx64
                 " sq_depth=%u sq_tid=%u tx_jfcn=%u jfs_mode=%d seid_idx=%u state=READY\n",
                 js->jetty_id, js->sq_buf_addr, js->sq_depth,
                 js->sqe_token_id, js->tx_jfcn, js->jfs_mode, js->seid_idx);

        /* Flush any buffered URMA RX packets for this jetty now active */
        ubc_flush_urma_rx_buffer(ubc_dev, tag);

        mb.status = cpu_to_le32(0);
        break;
    }
    case UBC_MB_MODIFY_JFS_CONTEXT: {
        UBCJettyState *js;

        if (tag >= UBC_MAX_JETTIES || !ubc_dev->jetties[tag].active) {
            qemu_log("ubc POST_MB MODIFY_JFS: tag=%u invalid or inactive\n", tag);
            mb.status = cpu_to_le32(0);
            break;
        }

        /* DMA-read updated context and re-parse mutable fields */
        dma_ret = ubc_dma_read(ubc_dev, dma_addr, ctx_buf, sizeof(ctx_buf));
        js = &ubc_dev->jetties[tag];

        if (dma_ret == MEMTX_OK) {
            /* Re-parse to pick up any changed fields (depth, SGE, etc.) */
            ubc_parse_jetty_ctx((const uint32_t *)ctx_buf, js);
            js->jetty_state = JETTY_STATE_READY;
        }

        qemu_log("ubc POST_MB MODIFY_JFS: jetty_id=%u sq_tid=%u state=READY\n",
                 tag, js->sqe_token_id);
        mb.status = cpu_to_le32(0);
        break;
    }
    case UBC_MB_QUERY_JFS_CONTEXT: {
        /*
         * Driver queries jetty state to check PI/CI and state.
         * udma_jetty_ctx layout (little-endian bitfields in DW0):
         *   ta_timeout[1:0] | rnr_retry_num[4:2] | type[7:5] | sqe_bb_shift[11:8]
         *   | sl[15:12] | state[18:16] | jfs_mode[19] | sqe_token_id_l[31:20]
         * enum jetty_state: RESET=0, READY=1, ERROR=2, SUSPEND=3
         * DW18[31:16] = CI, DW20[15:0] = PI
         * DW26[24]    = flush_ssn_vld, DW26[26] = flush_cqe_done
         *
         * On jetty teardown path, guest driver waits these flush/ack bits
         * before proceeding to destroy/unimport. Keep them asserted in
         * simulation to model an always-acked flush state.
         */
        if (tag < UBC_MAX_JETTIES && ubc_dev->jetties[tag].active) {
            UBCJettyState *js = &ubc_dev->jetties[tag];
            uint32_t *dw = (uint32_t *)ctx_buf;

            /* DMA-read the existing context */
            dma_ret = ubc_dma_read(ubc_dev, dma_addr, ctx_buf, sizeof(ctx_buf));
            if (dma_ret == MEMTX_OK) {
                /* Update state in DW0 bits [18:16] */
                dw[0] = (dw[0] & ~(0x7u << 16)) |
                         ((uint32_t)js->jetty_state << 16);
                /* Update CI = sq_ci in DW18 bits [31:16] */
                dw[18] = (dw[18] & 0xFFFFu) | ((uint32_t)js->sq_ci << 16);
                /* Update PI = sq_pi in DW20 bits [15:0] */
                dw[20] = (dw[20] & ~0xFFFFu) | (js->sq_pi & 0xFFFFu);
                /* Report flush ack done to satisfy destroy precondition. */
                dw[26] |= (1u << 24); /* flush_ssn_vld */
                dw[26] |= (1u << 26); /* flush_cqe_done */

                /* DMA-write back the modified context */
                ubc_dma_write(ubc_dev, dma_addr, ctx_buf, sizeof(ctx_buf));
            }
            qemu_log("ubc POST_MB QUERY_JFS: jetty=%u state=%u pi=%u ci=%u\n",
                     tag, js->jetty_state, js->sq_pi, js->sq_ci);
        }
        mb.status = cpu_to_le32(0);
        break;
    }
    case UBC_MB_DESTROY_JFS_CONTEXT: {
        UBCJettyState *js;

        if (tag >= UBC_MAX_JETTIES) {
            mb.status = cpu_to_le32(0);
            break;
        }
        js = &ubc_dev->jetties[tag];
        qemu_log("ubc POST_MB DESTROY_JFS: jetty_id=%u was_active=%d\n",
                 tag, js->active);
        memset(js, 0, sizeof(*js));
        mb.status = cpu_to_le32(0);
        break;
    }

    /* ---- JFC (Completion Queue) lifecycle ---- */
    case UBC_MB_CREATE_JFC_CONTEXT: {
        if (tag >= UBC_MAX_JFCS) {
            qemu_log("ubc POST_MB CREATE_JFC: tag %u out of range\n", tag);
            mb.status = cpu_to_le32(1);
            break;
        }
        if (ubc_dev->jfcs[tag].active) {
            qemu_log("ubc POST_MB CREATE_JFC: tag %u already active\n", tag);
            mb.status = cpu_to_le32(0);
            break;
        }

        UBCJfcState *jfc = &ubc_dev->jfcs[tag];
        memset(jfc, 0, sizeof(*jfc));
        jfc->active = true;
        jfc->jfc_id = tag;
        jfc->ceqn = 0;
        /*
         * CQE owner semantics in guest driver:
         *   valid_owner = (ci >> log2(depth)) & 1
         *   new CQE when cqe->owner != valid_owner
         * With initial ci=0, the first generated CQE must carry owner=1.
         */
        jfc->cq_owner_phase = 1;

        dma_ret = ubc_dma_read(ubc_dev, dma_addr, ctx_buf, UDMA_JFC_CTX_SIZE);
        if (dma_ret == MEMTX_OK) {
            /*
             * Extract CQ buffer address/depth from JFC context.
             * Guest layout (struct udma_jfc_ctx):
             *   DW0[31:12] cqe_va_l
             *   DW1        cqe_va_h
             *   DW0[7:4]   shift, where depth = 1 << (shift + 6)
             *   DW2[31:24] ceqn
             */
            const uint32_t *dw = (const uint32_t *)ctx_buf;
            uint32_t cqe_base_l = (dw[0] >> 12) & 0xFFFFF;
            uint32_t cqe_base_h = dw[1];
            uint32_t cqe_shift = (dw[0] >> 4) & 0xF;
            uint32_t ceqn = (dw[2] >> 24) & 0xFF;
            uint32_t cqe_tid = dw[2] & 0xFFFFF;
            jfc->cq_buf_addr = ((uint64_t)cqe_base_h << 32) |
                               ((uint64_t)cqe_base_l << 12);
            jfc->cq_depth = 1u << (cqe_shift + 6);
            jfc->cqe_token_id = cqe_tid;
            if (ceqn < UBC_MAX_CEQS) {
                jfc->ceqn = ceqn;
            }
        }

        qemu_log("ubc POST_MB CREATE_JFC: jfc_id=%u cq_buf=%#" PRIx64
                 " cq_depth=%u cq_tid=%u ceqn=%u owner_phase=%u\n",
                 jfc->jfc_id, jfc->cq_buf_addr, jfc->cq_depth,
                 jfc->cqe_token_id, jfc->ceqn, jfc->cq_owner_phase);
        mb.status = cpu_to_le32(0);
        break;
    }
    case UBC_MB_MODIFY_JFC_CONTEXT: {
        if (tag >= UBC_MAX_JFCS || !ubc_dev->jfcs[tag].active) {
            qemu_log("ubc POST_MB MODIFY_JFC: tag=%u invalid or inactive\n", tag);
            mb.status = cpu_to_le32(0);
            break;
        }

        UBCJfcState *jfc = &ubc_dev->jfcs[tag];
        dma_ret = ubc_dma_read(ubc_dev, dma_addr, ctx_buf, UDMA_JFC_CTX_SIZE);
        if (dma_ret == MEMTX_OK) {
            const uint32_t *dw = (const uint32_t *)ctx_buf;
            uint32_t cqe_base_l = (dw[0] >> 12) & 0xFFFFF;
            uint32_t cqe_base_h = dw[1];
            uint32_t cqe_shift = (dw[0] >> 4) & 0xF;
            uint32_t ceqn = (dw[2] >> 24) & 0xFF;
            uint32_t cqe_tid = dw[2] & 0xFFFFF;
            jfc->cq_buf_addr = ((uint64_t)cqe_base_h << 32) |
                               ((uint64_t)cqe_base_l << 12);
            jfc->cq_depth = 1u << (cqe_shift + 6);
            jfc->cqe_token_id = cqe_tid;
            if (ceqn < UBC_MAX_CEQS) {
                jfc->ceqn = ceqn;
            }
        }

        qemu_log("ubc POST_MB MODIFY_JFC: jfc_id=%u cq_buf=%#" PRIx64
                 " cq_depth=%u cq_tid=%u ceqn=%u\n", jfc->jfc_id,
                 jfc->cq_buf_addr, jfc->cq_depth, jfc->cqe_token_id,
                 jfc->ceqn);
        mb.status = cpu_to_le32(0);
        break;
    }
    case UBC_MB_QUERY_JFC_CONTEXT: {
        if (tag < UBC_MAX_JFCS && ubc_dev->jfcs[tag].active) {
            UBCJfcState *jfc = &ubc_dev->jfcs[tag];
            uint32_t *dw = (uint32_t *)ctx_buf;

            dma_ret = ubc_dma_read(ubc_dev, dma_addr, ctx_buf, UDMA_JFC_CTX_SIZE);
            if (dma_ret == MEMTX_OK) {
                /* Update state in DW0 bits [18:16] = READY */
                dw[0] = (dw[0] & ~(0x7u << 16)) | (1u << 16);
                /* Update CQ PI in DW18 bits [31:16] */
                dw[18] = (dw[18] & 0xFFFFu) | ((uint32_t)jfc->cq_pi << 16);
                /* Update CQ CI in DW20 bits [15:0] */
                dw[20] = (dw[20] & ~0xFFFFu) | (jfc->cq_ci & 0xFFFFu);
                ubc_dma_write(ubc_dev, dma_addr, ctx_buf, UDMA_JFC_CTX_SIZE);
            }
            qemu_log("ubc POST_MB QUERY_JFC: jfc_id=%u cq_pi=%u cq_ci=%u\n",
                     tag, jfc->cq_pi, jfc->cq_ci);
        }
        mb.status = cpu_to_le32(0);
        break;
    }
    case UBC_MB_DESTROY_JFC_CONTEXT: {
        if (tag < UBC_MAX_JFCS) {
            qemu_log("ubc POST_MB DESTROY_JFC: jfc_id=%u was_active=%d\n",
                     tag, ubc_dev->jfcs[tag].active);
            memset(&ubc_dev->jfcs[tag], 0, sizeof(UBCJfcState));
        }
        mb.status = cpu_to_le32(0);
        break;
    }

    /* ---- AEQ lifecycle ---- */
    case UBC_MB_CREATE_AEQ_CONTEXT: {
        UBCEqState *aeq;

        if (tag >= UBC_MAX_AEQS) {
            qemu_log("ubc POST_MB CREATE_AEQ: tag %u out of range\n", tag);
            mb.status = cpu_to_le32(1);
            break;
        }

        aeq = &ubc_dev->aeqs[tag];
        memset(aeq, 0, sizeof(*aeq));
        aeq->active = true;
        aeq->eq_id = tag;
        aeq->eq_owner_phase = 1;

        dma_ret = ubc_dma_read(ubc_dev, dma_addr, ctx_buf, UBASE_EQ_CTX_SIZE);
        if (dma_ret == MEMTX_OK) {
            ubc_parse_eq_ctx((const uint32_t *)ctx_buf, aeq);
        }

        qemu_log("ubc POST_MB CREATE_AEQ: eqn=%u buf=%#" PRIx64
                 " depth=%u irq=%u owner_phase=%u\n",
                 aeq->eq_id, aeq->eq_buf_addr, aeq->eq_depth,
                 aeq->irq_num, aeq->eq_owner_phase);
        mb.status = cpu_to_le32(0);
        break;
    }
    case UBC_MB_QUERY_AEQ_CONTEXT: {
        if (tag < UBC_MAX_AEQS && ubc_dev->aeqs[tag].active) {
            UBCEqState *aeq = &ubc_dev->aeqs[tag];
            uint32_t *dw = (uint32_t *)ctx_buf;

            dma_ret = ubc_dma_read(ubc_dev, dma_addr, ctx_buf, UBASE_EQ_CTX_SIZE);
            if (dma_ret == MEMTX_OK) {
                dw[0] = (dw[0] & 0xFFu) | ((aeq->eq_pi & 0xFFFFFFu) << 8);
                dw[0] = (dw[0] & ~0x3u) | 0x1u;
                dw[1] = (dw[1] & 0xFFu) | ((aeq->eq_ci & 0xFFFFFFu) << 8);
                dw[11] = (dw[11] & ~0x3u) | 0x1u;
                ubc_dma_write(ubc_dev, dma_addr, ctx_buf, UBASE_EQ_CTX_SIZE);
            }
            qemu_log("ubc POST_MB QUERY_AEQ: eqn=%u pi=%u ci=%u\n",
                     tag, aeq->eq_pi, aeq->eq_ci);
        }
        mb.status = cpu_to_le32(0);
        break;
    }
    case UBC_MB_DESTROY_AEQ_CONTEXT: {
        if (tag < UBC_MAX_AEQS) {
            qemu_log("ubc POST_MB DESTROY_AEQ: eqn=%u was_active=%d\n",
                     tag, ubc_dev->aeqs[tag].active);
            memset(&ubc_dev->aeqs[tag], 0, sizeof(UBCEqState));
        }
        mb.status = cpu_to_le32(0);
        break;
    }

    /* ---- CEQ lifecycle ---- */
    case UBC_MB_CREATE_CEQ_CONTEXT: {
        UBCEqState *ceq;

        if (tag >= UBC_MAX_CEQS) {
            qemu_log("ubc POST_MB CREATE_CEQ: tag %u out of range\n", tag);
            mb.status = cpu_to_le32(1);
            break;
        }

        ceq = &ubc_dev->ceqs[tag];
        memset(ceq, 0, sizeof(*ceq));
        ceq->active = true;
        ceq->eq_id = tag;
        ceq->eq_owner_phase = 1;

        dma_ret = ubc_dma_read(ubc_dev, dma_addr, ctx_buf, UBASE_EQ_CTX_SIZE);
        if (dma_ret == MEMTX_OK) {
            ubc_parse_eq_ctx((const uint32_t *)ctx_buf, ceq);
        }

        qemu_log("ubc POST_MB CREATE_CEQ: eqn=%u buf=%#" PRIx64
                 " depth=%u irq=%u owner_phase=%u\n",
                 ceq->eq_id, ceq->eq_buf_addr, ceq->eq_depth,
                 ceq->irq_num, ceq->eq_owner_phase);
        mb.status = cpu_to_le32(0);
        break;
    }
    case UBC_MB_QUERY_CEQ_CONTEXT: {
        if (tag < UBC_MAX_CEQS && ubc_dev->ceqs[tag].active) {
            UBCEqState *ceq = &ubc_dev->ceqs[tag];
            uint32_t *dw = (uint32_t *)ctx_buf;

            dma_ret = ubc_dma_read(ubc_dev, dma_addr, ctx_buf, UBASE_EQ_CTX_SIZE);
            if (dma_ret == MEMTX_OK) {
                dw[0] = (dw[0] & 0xFFu) | ((ceq->eq_pi & 0xFFFFFFu) << 8);
                dw[0] = (dw[0] & ~0x3u) | 0x1u;
                dw[1] = (dw[1] & 0xFFu) | ((ceq->eq_ci & 0xFFFFFFu) << 8);
                dw[11] = (dw[11] & ~0x3u) | 0x1u;
                ubc_dma_write(ubc_dev, dma_addr, ctx_buf, UBASE_EQ_CTX_SIZE);
            }
            qemu_log("ubc POST_MB QUERY_CEQ: eqn=%u pi=%u ci=%u\n",
                     tag, ceq->eq_pi, ceq->eq_ci);
        }
        mb.status = cpu_to_le32(0);
        break;
    }
    case UBC_MB_DESTROY_CEQ_CONTEXT: {
        if (tag < UBC_MAX_CEQS) {
            qemu_log("ubc POST_MB DESTROY_CEQ: eqn=%u was_active=%d\n",
                     tag, ubc_dev->ceqs[tag].active);
            memset(&ubc_dev->ceqs[tag], 0, sizeof(UBCEqState));
        }
        mb.status = cpu_to_le32(0);
        break;
    }

    /* ---- JFR (Receive Queue) lifecycle ---- */
    case UBC_MB_CREATE_JFR_CONTEXT: {
        if (tag >= UBC_MAX_JFRS) {
            qemu_log("ubc POST_MB CREATE_JFR: tag %u out of range\n", tag);
            mb.status = cpu_to_le32(1);
            break;
        }

        UBCJfrState *jfr = &ubc_dev->jfrs[tag];
        memset(jfr, 0, sizeof(*jfr));
        jfr->active = true;
        jfr->jfr_id = tag;

        dma_ret = ubc_dma_read(ubc_dev, dma_addr, ctx_buf, UDMA_JFR_CTX_SIZE);
        if (dma_ret == MEMTX_OK) {
            const uint32_t *dw = (const uint32_t *)ctx_buf;
            uint32_t rqe_base_l = (dw[1] >> 12) & 0xFFFFF;
            uint32_t rqe_base_h = dw[2];
            uint32_t rqe_bb_shift = (dw[0] >> 8) & 0xF;
            uint32_t rqe_tid = ((dw[0] >> 18) & 0x3FFF) | ((dw[1] & 0x3F) << 14);
            jfr->rq_buf_addr = ((uint64_t)rqe_base_h << 32) |
                               ((uint64_t)rqe_base_l << 12);
            jfr->rq_depth = 1u << rqe_bb_shift;
            jfr->rqe_token_id = rqe_tid;
        }

        qemu_log("ubc POST_MB CREATE_JFR: jfr_id=%u rq_buf=%#" PRIx64
                 " rq_depth=%u rq_tid=%u\n", jfr->jfr_id, jfr->rq_buf_addr,
                 jfr->rq_depth, jfr->rqe_token_id);

        /* Flush any URMA RX data buffered for jetties using this JFR */
        for (uint32_t ji = 0; ji < UBC_MAX_JETTIES; ji++) {
            UBCJettyState *js = &ubc_dev->jetties[ji];
            if (js->active && js->jfrn == tag) {
                ubc_flush_urma_rx_buffer(ubc_dev, ji);
            }
        }
        mb.status = cpu_to_le32(0);
        break;
    }
    case UBC_MB_MODIFY_JFR_CONTEXT: {
        if (tag >= UBC_MAX_JFRS || !ubc_dev->jfrs[tag].active) {
            qemu_log("ubc POST_MB MODIFY_JFR: tag=%u invalid or inactive\n", tag);
            mb.status = cpu_to_le32(0);
            break;
        }

        UBCJfrState *jfr = &ubc_dev->jfrs[tag];
        dma_ret = ubc_dma_read(ubc_dev, dma_addr, ctx_buf, UDMA_JFR_CTX_SIZE);
        if (dma_ret == MEMTX_OK) {
            const uint32_t *dw = (const uint32_t *)ctx_buf;
            uint32_t rqe_base_l = (dw[1] >> 12) & 0xFFFFF;
            uint32_t rqe_base_h = dw[2];
            uint32_t rqe_bb_shift = (dw[0] >> 8) & 0xF;
            uint32_t rqe_tid = ((dw[0] >> 18) & 0x3FFF) | ((dw[1] & 0x3F) << 14);
            jfr->rq_buf_addr = ((uint64_t)rqe_base_h << 32) |
                               ((uint64_t)rqe_base_l << 12);
            jfr->rq_depth = 1u << rqe_bb_shift;
            jfr->rqe_token_id = rqe_tid;
        }

        qemu_log("ubc POST_MB MODIFY_JFR: jfr_id=%u rq_buf=%#" PRIx64
                 " rq_depth=%u rq_tid=%u\n", jfr->jfr_id, jfr->rq_buf_addr,
                 jfr->rq_depth, jfr->rqe_token_id);
        mb.status = cpu_to_le32(0);
        break;
    }
    case UBC_MB_QUERY_JFR_CONTEXT: {
        if (tag < UBC_MAX_JFRS && ubc_dev->jfrs[tag].active) {
            UBCJfrState *jfr = &ubc_dev->jfrs[tag];
            uint32_t *dw = (uint32_t *)ctx_buf;

            dma_ret = ubc_dma_read(ubc_dev, dma_addr, ctx_buf, UDMA_JFR_CTX_SIZE);
            if (dma_ret == MEMTX_OK) {
                /* Update state in DW0 bits [18:16] = READY */
                dw[0] = (dw[0] & ~(0x7u << 16)) | (1u << 16);
                /* Update RQ CI in DW18 bits [31:16] */
                dw[18] = (dw[18] & 0xFFFFu) | ((uint32_t)jfr->rq_ci << 16);
                /* Update RQ PI in DW20 bits [15:0] */
                dw[20] = (dw[20] & ~0xFFFFu) | (jfr->rq_pi & 0xFFFFu);
                ubc_dma_write(ubc_dev, dma_addr, ctx_buf, UDMA_JFR_CTX_SIZE);
            }
            qemu_log("ubc POST_MB QUERY_JFR: jfr_id=%u rq_pi=%u rq_ci=%u\n",
                     tag, jfr->rq_pi, jfr->rq_ci);
        }
        mb.status = cpu_to_le32(0);
        break;
    }
    case UBC_MB_DESTROY_JFR_CONTEXT: {
        if (tag < UBC_MAX_JFRS) {
            UBCJfrState *jfr = &ubc_dev->jfrs[tag];
            qemu_log("ubc POST_MB DESTROY_JFR: jfr_id=%u was_active=%d\n",
                     tag, jfr->active);
            /* Clear any jetty references to this JFR */
            for (uint32_t ji = 0; ji < UBC_MAX_JETTIES; ji++) {
                UBCJettyState *js = &ubc_dev->jetties[ji];
                if (js->active && js->jfrn == tag) {
                    js->jfrn = 0;
                }
            }
            memset(jfr, 0, sizeof(*jfr));
        }
        mb.status = cpu_to_le32(0);
        break;
    }

    /* ---- Other known sub-opcodes (passthrough) ---- */
    case 0x00:  /* NOP / generic */
        qemu_log("ubc POST_MB NOP: tag=%u\n", tag);
        mb.status = cpu_to_le32(0);
        break;
    case 0x14:  /* page table / DMA region config */
        dma_ret = ubc_dma_read(ubc_dev, dma_addr, ctx_buf, sizeof(ctx_buf));
        if (dma_ret == MEMTX_OK) {
            const uint32_t *dw = (const uint32_t *)ctx_buf;
            qemu_log("ubc POST_MB sub_op=0x14: tag=%u dma_addr=%#" PRIx64
                     " dw[0..7]=%08x %08x %08x %08x %08x %08x %08x %08x\n",
                     tag, (uint64_t)dma_addr,
                     dw[0], dw[1], dw[2], dw[3], dw[4], dw[5], dw[6], dw[7]);
        }
        mb.status = cpu_to_le32(0);
        break;

    default:
        qemu_log("ubc POST_MB: unhandled sub_op=0x%02x tag=%u\n", sub_op, tag);
        mb.status = cpu_to_le32(0);
        break;
    }

    /* Echo back the mailbox with updated status */
    resp_len = sizeof(mb);
    memcpy(resp, &mb, resp_len);
    (void)resp_len;
    return 0;
}

static size_t ubc_cmd_fill_resp(uint16_t opcode, const uint8_t *req, size_t req_len,
                                uint8_t *resp, size_t resp_cap, uint32_t entity_count)
{
    size_t resp_len = 0;

    switch (opcode) {
    case UBASE_OPC_QUERY_FW_VER: {
        UBCQueryVersionResp fw = { 0 };
        fw.fw_version = cpu_to_le32(0x01000001);
        resp_len = MIN(sizeof(fw), resp_cap);
        memcpy(resp, &fw, resp_len);
        break;
    }
    case UBASE_OPC_QUERY_UE_RES: {
        UBCUeResourceResp ue = { 0 };

        /* BD0: basic capability fields */
        ue.jetty_grp_num = cpu_to_le16(4);
        ue.jfs_sge = 13;
        ue.jfr_sge = 4;
        ue.jfs_rsge = 4;
        ue.max_jfs_inline_sz = cpu_to_le16(64);
        ue.max_jfc_inline_sz = cpu_to_le16(32);
        ue.trans_mode = 1;  /* UD mode */
        ue.ue_cnt = cpu_to_le16(entity_count); /* dynamic entity count from device config */
        ue.ue_id = 1;

        /* BD1: address tables and SEID — critical for ubcore registration */
        ue.seid_upi_tbl_sz = cpu_to_le16(64);
        ue.seid_upi_tbl_num = cpu_to_le16(64);
        ue.src_addr_tbl_sz = cpu_to_le16(64);
        ue.src_addr_tbl_num = cpu_to_le16(64);
        ue.dest_addr_tbl_sz = cpu_to_le16(64);
        ue.dest_addr_tbl_num = cpu_to_le16(64);
        ue.port_num = 1;
        ue.port_id = 0;
        ue.rc_queue_num = cpu_to_le16(64);
        ue.rc_depth = cpu_to_le16(1024);

        /* BD2: jetty ranges — well-known jetties 0-1023 to match ubcore UBCORE_RESERVED_JETTY_ID_MAX */
        ue.well_known_jetty_start = cpu_to_le16(0);
        ue.well_known_jetty_num = cpu_to_le16(1024);
        ue.ccu_jetty_start = cpu_to_le16(0);
        ue.ccu_jetty_num = cpu_to_le16(0);
        ue.drv_jetty_start = cpu_to_le16(0);
        ue.drv_jetty_num = cpu_to_le16(0);
        ue.cache_lock_jetty_start = cpu_to_le16(0);
        ue.cache_lock_jetty_num = cpu_to_le16(0);
        ue.normal_jetty_start = cpu_to_le16(0);
        ue.normal_jetty_num = cpu_to_le16(0);
        ue.standard_jetty_start = cpu_to_le16(1024);
        ue.standard_jetty_num = cpu_to_le16(0);

        /* BD3: RDMA sizes */
        ue.max_write_size = cpu_to_le32(0x100000);
        ue.max_read_size = cpu_to_le32(0x100000);
        ue.max_cas_size = cpu_to_le32(8);
        ue.max_fetch_and_add_size = cpu_to_le32(8);

        resp_len = MIN(sizeof(ue), resp_cap);
        memcpy(resp, &ue, resp_len);
        qemu_log("ubc UE_RES resp: sizeof=%zu resp_len=%zu "
                 "standard_start=%u standard_num=%u "
                 "bytes@76=[%02x %02x] bytes@78=[%02x %02x]\n",
                 sizeof(ue), resp_len,
                 le16_to_cpu(ue.standard_jetty_start),
                 le16_to_cpu(ue.standard_jetty_num),
                 resp[76], resp[77], resp[78], resp[79]);
        break;
    }
    case UBASE_OPC_QUERY_COMM_RSRC_PARAM: {
        UBCResCmdResp res;
        ubc_fill_res_caps(&res, entity_count);
        resp_len = MIN(sizeof(res), resp_cap);
        memcpy(resp, &res, resp_len);
        break;
    }
    case UBASE_OPC_QUERY_CTL_INFO: {
        UBCCtrlInfoResp ctl = { 0 };
        resp_len = MIN(sizeof(ctl), resp_cap);
        memcpy(resp, &ctl, resp_len);
        break;
    }
    case UBASE_OPC_QUERY_CHIP_INFO: {
        UBCChipInfoResp chip = { 0 };
        chip.chip_id = cpu_to_le16(1);
        resp_len = MIN(sizeof(chip), resp_cap);
        memcpy(resp, &chip, resp_len);
        break;
    }
    case UBASE_OPC_QUERY_PORT_INFO: {
        UBCPortInfoResp port = { 0 };

        /*
         * Guest UDMA query_device_status() accepts only 200G/400G on the
         * UBL-capable path. Return a stable simulated active link so
         * query_dev_attr can complete instead of consuming an all-zero
         * response and failing with "invalid port speed = 0".
         */
        port.speed = cpu_to_le32(400000);
        port.lanes = 16;
        resp_len = MIN(sizeof(port), resp_cap);
        memcpy(resp, &port, resp_len);
        qemu_log("ubc QUERY_PORT_INFO resp: speed=%u lanes=%u len=%zu\n",
                 le32_to_cpu(port.speed), port.lanes, resp_len);
        break;
    }
    case UBASE_OPC_QUERY_OOR_CAPS: {
        UBCOorResp oor = { 0 };
        resp_len = MIN(sizeof(oor), resp_cap);
        memcpy(resp, &oor, resp_len);
        break;
    }
    case UBASE_OPC_QUERY_UB_PORT_BITMAP:
    case UBASE_OPC_QUERY_PORT_BITMAP: {
        UBCPortBitmapResp bm = { 0 };
        bm.logic_port_bitmap = cpu_to_le32(0x1);
        bm.chip_id = cpu_to_le32(1);
        resp_len = MIN(sizeof(bm), resp_cap);
        memcpy(resp, &bm, resp_len);
        break;
    }
    case UBASE_OPC_QUERY_TA_SL_VL_MAP: {
        UBCSlVlResp sl = { 0 };
        int i;

        sl.sl_num = UBASE_MAX_SL_NUM;
        for (i = 0; i < UBASE_MAX_SL_NUM && i < ARRAY_SIZE(sl.sl_vl); i++) {
            sl.sl_vl[i] = i;
        }
        resp_len = MIN(sizeof(sl), resp_cap);
        memcpy(resp, &sl, resp_len);
        break;
    }
    case UBASE_OPC_QUERY_TM_Q_INFO: {
        UBCTmQueueResp q = { 0 };
        q.queue_num = 1;
        q.queue_vl[0] = 0;
        q.queue_id[0] = 0;
        q.qset_id[0] = 0;
        q.link_vld_bitmap = cpu_to_le16(1);
        resp_len = MIN(sizeof(q), resp_cap);
        memcpy(resp, &q, resp_len);
        break;
    }
    case UBASE_OPC_QUERY_TM_QS_INFO: {
        UBCTmQsetResp q = { 0 };
        q.qset_num = 1;
        q.qset_id[0] = 0;
        q.pri_id[0] = 0;
        q.qset_weight[0] = 100;
        q.rate[0] = cpu_to_le32(100000);
        q.qset_pri_link_vld = cpu_to_le16(1);
        resp_len = MIN(sizeof(q), resp_cap);
        memcpy(resp, &q, resp_len);
        break;
    }
    case UBASE_OPC_QUERY_MB_ST: {
        UBCMbox mb = { 0 };
        mb.in_param_l = cpu_to_le32(1);
        mb.status = cpu_to_le32(1); /* query_status bit 0 = 1 → mailbox done */
        resp_len = MIN(sizeof(mb), resp_cap);
        memcpy(resp, &mb, resp_len);
        break;
    }
    default:
        resp_len = 0;
        break;
    }

    return resp_len;
}

static bool ubc_notify_vector(BusControllerDev *ubc_dev, uint16_t usi_vector,
                              uint16_t msi_vector, const char *name)
{
    UBDevice *ub_dev = &ubc_dev->parent;

    if (ub_dev->usi_entries_nr > usi_vector) {
        usi_notify(ub_dev, usi_vector);
        return true;
    }

    /*
     * INT_TYPE1 fallback: msi_data is vector-indexed (event id).
     * Vector0 keeps behavior compatible with existing cmdq/ctrlq flow.
     */
    {
        uint64_t cap_off =
            ub_cfg_offset_to_emulated_offset(UB_CFG1_CAP3_INT_TYPE1, true);
        UbCfg1IntType1Cap *int1 =
            (UbCfg1IntType1Cap *)(ub_dev->config + cap_off);
        uint8_t int_en = int1->interrupt_enable & 0x1;
        uint32_t rid = int1->interrupt_id;
        uint32_t data = int1->interrupt_data + msi_vector;
        uint32_t addr_l = (uint32_t)(int1->interrupt_address & UINT32_MAX);
        uint32_t addr_h = (uint32_t)(int1->interrupt_address >> 32);
        USIMessage msg = {
            .address = ((uint64_t)addr_h << 32) | addr_l,
            .data = data,
        };

        if (int_en && msg.address) {
            USIMessage kick = {
                .address = 0x8090040ULL,
                .data = data,
            };
            usi_send_message(&msg, rid, NULL);
            /* Compatibility kick: some environments only route the low ITS doorbell. */
            usi_send_message(&kick, rid, NULL);
            if (ubc_trace_data_path_enabled()) {
                qemu_log("ubc %s notify vector=%u rid=0x%x data=0x%x addr=%#" PRIx64 "\n",
                         name, msi_vector, rid, data, (uint64_t)msg.address);
            }
            return true;
        }

        msg.address = 0x8090040ULL;
        msg.data = msi_vector;
        usi_send_message(&msg, UBC_INTERRUPT_ID_START + msi_vector, NULL);
        if (ubc_trace_data_path_enabled()) {
            qemu_log("ubc %s msi notify fallback vector=%u rid=0x%x\n",
                     name, msi_vector, UBC_INTERRUPT_ID_START + msi_vector);
        }
    }
    return true;
}

static void ubc_raise_queue_event(BusControllerDev *ubc_dev, hwaddr src_reg,
                                  uint32_t src_bit, const char *name)
{
    UBDevice *ub_dev = &ubc_dev->parent;
    BusControllerState *s = container_of_ubbus(ub_get_bus(ub_dev));
    uint32_t src, new_src;

    src = ubc_ers2_read32(ubc_dev, src_reg);
    new_src = src | BIT(src_bit);
    ubc_ers2_write32(ubc_dev, src_reg, new_src);
    fprintf(stderr, "ubc raise %s event: src old=%#x new=%#x\n",
            name, src, new_src);
    ubc_notify_vector(ubc_dev, 0, 0, name);

    /* Fallback edge on the wired IRQ line to avoid losing CRQ service kicks
     * when MSI routing is not effective in the current simulation topology. */
    if (s) {
        qemu_set_irq(s->irq, 1);
        qemu_set_irq(s->irq, 0);
    }
}

static void ubc_raise_cmdq_event(BusControllerDev *ubc_dev)
{
    ubc_raise_queue_event(ubc_dev, UBASE_VECTOR0_CMDQ_SRC_REG,
                          UBASE_VECTOR0_RX_CMDQ_INT_B, "cmdq");
}

static void ubc_raise_ctrlq_event(BusControllerDev *ubc_dev)
{
    ubc_raise_queue_event(ubc_dev, UBASE_VECTOR0_CTRLQ_SRC_REG,
                          UBASE_VECTOR0_RX_CTRLQ_INT_B, "ctrlq");
}

static void ubc_sync_cmdq_regs(BusControllerDev *ubc_dev)
{
    dma_addr_t old_csq_base = ubc_dev->cmd_csq.base;

    ubc_dev->cmd_csq.base = ubc_read_q_base(ubc_dev, UBASE_CSQ_BASEADDR_L_REG);
    ubc_dev->cmd_csq.depth = ubc_read_q_depth(ubc_dev, UBASE_CSQ_DEPTH_REG);
    ubc_dev->cmd_csq.head = (uint16_t)ubc_ers2_read32(ubc_dev, UBASE_CSQ_HEAD_REG);
    ubc_dev->cmd_csq.tail = (uint16_t)ubc_ers2_read32(ubc_dev, UBASE_CSQ_TAIL_REG);
    if (old_csq_base != ubc_dev->cmd_csq.base) {
        /* Reset CMDQ base tracking when CSQ is cleared (guest resets queue) */
        if (ubc_dev->cmd_csq.base == 0) {
            ubc_dev->cmdq_last_csq_base = 0;
        }
    }

    ubc_dev->cmd_crq.base = ubc_read_q_base(ubc_dev, UBASE_CRQ_BASEADDR_L_REG);
    ubc_dev->cmd_crq.depth = ubc_read_q_depth(ubc_dev, UBASE_CRQ_DEPTH_REG);
    ubc_dev->cmd_crq.head = (uint16_t)ubc_ers2_read32(ubc_dev, UBASE_CRQ_HEAD_REG);
    ubc_dev->cmd_crq.tail = (uint16_t)ubc_ers2_read32(ubc_dev, UBASE_CRQ_TAIL_REG);
    if (ubc_trace_data_path_enabled()) {
        qemu_log("ubc cmdq sync base=%#" PRIx64 " depth=%u head=%u tail=%u crq_base=%#" PRIx64
                 " crq_depth=%u crq_head=%u crq_tail=%u\n",
                 (uint64_t)ubc_dev->cmd_csq.base, ubc_dev->cmd_csq.depth,
                 ubc_dev->cmd_csq.head, ubc_dev->cmd_csq.tail,
                 (uint64_t)ubc_dev->cmd_crq.base, ubc_dev->cmd_crq.depth,
                 ubc_dev->cmd_crq.head, ubc_dev->cmd_crq.tail);
    }
}

static void ubc_process_cmdq(BusControllerDev *ubc_dev)
{
    UBCmdQueueState *csq = &ubc_dev->cmd_csq;
    uint16_t guard = 0;

    ub_fm_poll_rx_links_now();

    if (!csq->base || !csq->depth) {
        return;
    }

    if (ubc_trace_data_path_enabled()) {
        fprintf(stderr, "ubc_process_cmdq: ENTER base=%#lx depth=%u head=%u tail=%u\n",
                (unsigned long)csq->base, csq->depth, csq->head, csq->tail);
    }

    while (csq->head != csq->tail && guard++ < csq->depth) {
        UBCCmdqDesc *descs;
        UBCCmdqDesc head_desc;
        uint8_t *resp_flat;
        uint8_t *req_flat;
        uint16_t idx;
        uint16_t pending;
        uint8_t bd_num;
        uint16_t opcode;
        size_t cap;
        size_t req_len;
        size_t copy_len;
        int ret = 0;
        int i;
        MemTxResult rdret;

        dma_addr_t desc_addr = csq->base + (dma_addr_t)csq->head * sizeof(head_desc);
        rdret = ubc_read_desc(csq, ubc_dev, csq->head, &head_desc);
        if (rdret != MEMTX_OK) {
            fprintf(stderr, "ubc_process_cmdq: DMA read FAILED addr=%#llx"
                    " base=%#llx idx=%u ret=%d as=%p\n",
                    (unsigned long long)desc_addr,
                    (unsigned long long)csq->base, csq->head, (int)rdret,
                    ubc_dma_as(ubc_dev));
            break;
        }
        opcode = le16_to_cpu(head_desc.opcode);
        if (ubc_trace_data_path_enabled()) {
            qemu_log("ubc cmdq process opcode=0x%x bd=%u head=%u tail=%u depth=%u\n",
                     opcode, head_desc.bd_num, csq->head, csq->tail, csq->depth);
        }
        ub_fm_poll_rx_links_now();
        bd_num = head_desc.bd_num ? head_desc.bd_num : 1;
        if (bd_num > csq->depth) {
            bd_num = 1;
        }
        pending = (csq->tail + csq->depth - csq->head) % csq->depth;
        if (!pending) {
            break;
        }
        if (!(head_desc.flag & UBASE_CMD_FLAG_IN) ||
            (head_desc.flag & UBASE_CMD_FLAG_OUT) ||
            head_desc.bd_num == 0) {
            break;
        }
        if (bd_num > pending) {
            bd_num = pending;
        }

        cap = ubc_cmdq_capacity(bd_num);
        req_len = cap;
        req_flat = g_malloc0(MAX(cap, (size_t)1));
        resp_flat = g_malloc0(MAX(cap, (size_t)1));
        descs = g_new0(UBCCmdqDesc, bd_num);

        idx = csq->head;
        for (i = 0; i < bd_num; i++) {
            if (ubc_read_desc(csq, ubc_dev, idx, &descs[i]) != MEMTX_OK) {
                break;
            }
            idx = (idx + 1) % csq->depth;
        }
        if (i != bd_num) {
            g_free(req_flat);
            g_free(resp_flat);
            g_free(descs);
            break;
        }

        if (cap) {
            copy_len = MIN(sizeof(descs[0].data), cap);
            memcpy(req_flat, descs[0].data, copy_len);
            for (i = 1; i < bd_num; i++) {
                size_t off = sizeof(descs[0].data) +
                             (size_t)(i - 1) * sizeof(UBCCmdqDesc);
                if (off >= cap) {
                    break;
                }
                copy_len = MIN(sizeof(UBCCmdqDesc), cap - off);
                memcpy(req_flat + off, &descs[i], copy_len);
            }
        }

        if (descs[0].flag & UBASE_CMD_FLAG_GET_BD_NUM) {
            resp_flat[0] = 1;
        } else if (opcode == UBASE_OPC_UE2UE_UBASE) {
            ret = ubc_handle_ue2ue_ctrlq(ubc_dev, req_flat, req_len,
                                                   resp_flat, cap);
        } else if (opcode == UBASE_OPC_UE_TO_MUE) {
            ret = ubc_handle_ue_to_mue(ubc_dev, req_flat, req_len);
        } else if (opcode == UBASE_OPC_POST_MB) {
            ret = ubc_handle_post_mb(ubc_dev, req_flat, req_len, resp_flat, cap);
        } else {
            (void)ubc_cmd_fill_resp(opcode, req_flat, req_len, resp_flat, cap, ubc_dev->entity_count);
        }

        if (cap) {
            copy_len = MIN(sizeof(descs[0].data), cap);
            memcpy(descs[0].data, resp_flat, copy_len);
            for (i = 1; i < bd_num; i++) {
                size_t off = sizeof(descs[0].data) +
                             (size_t)(i - 1) * sizeof(UBCCmdqDesc);
                if (off >= cap) {
                    break;
                }
                copy_len = MIN(sizeof(UBCCmdqDesc), cap - off);
                memcpy(&descs[i], resp_flat + off, copy_len);
            }
            if (opcode == UBASE_OPC_QUERY_UE_RES) {
                qemu_log("ubc UE_RES scatter: bd_num=%u cap=%zu "
                         "desc0.data[12..15]=[%02x %02x %02x %02x] "
                         "desc1.data[8..15]=[%02x %02x %02x %02x %02x %02x %02x %02x] "
                         "desc2.bytes[16..23]=[%02x %02x %02x %02x %02x %02x %02x %02x]\n",
                         bd_num, cap,
                         ((uint8_t *)descs[0].data)[12], ((uint8_t *)descs[0].data)[13],
                         ((uint8_t *)descs[0].data)[14], ((uint8_t *)descs[0].data)[15],
                         ((uint8_t *)&descs[1])[8], ((uint8_t *)&descs[1])[9],
                         ((uint8_t *)&descs[1])[10], ((uint8_t *)&descs[1])[11],
                         ((uint8_t *)&descs[1])[12], ((uint8_t *)&descs[1])[13],
                         ((uint8_t *)&descs[1])[14], ((uint8_t *)&descs[1])[15],
                         ((uint8_t *)&descs[2])[16], ((uint8_t *)&descs[2])[17],
                         ((uint8_t *)&descs[2])[18], ((uint8_t *)&descs[2])[19],
                         ((uint8_t *)&descs[2])[20], ((uint8_t *)&descs[2])[21],
                         ((uint8_t *)&descs[2])[22], ((uint8_t *)&descs[2])[23]);
            }
        }

        descs[0].flag |= UBASE_CMD_FLAG_OUT;
        descs[0].ret = cpu_to_le16((ret < 0) ? (uint16_t)(-ret) : 0);

        idx = csq->head;
        for (i = 0; i < bd_num; i++) {
            if (opcode == UBASE_OPC_QUERY_UE_RES) {
                qemu_log("ubc UE_RES write_desc[%d] addr=%#" PRIx64
                         " bytes[0..7]=[%02x %02x %02x %02x %02x %02x %02x %02x]"
                         " data[0..7]=[%02x %02x %02x %02x %02x %02x %02x %02x]\n",
                         i, (uint64_t)(csq->base + (dma_addr_t)idx * sizeof(descs[0])),
                         ((uint8_t *)&descs[i])[0], ((uint8_t *)&descs[i])[1],
                         ((uint8_t *)&descs[i])[2], ((uint8_t *)&descs[i])[3],
                         ((uint8_t *)&descs[i])[4], ((uint8_t *)&descs[i])[5],
                         ((uint8_t *)&descs[i])[6], ((uint8_t *)&descs[i])[7],
                         ((uint8_t *)descs[i].data)[0], ((uint8_t *)descs[i].data)[1],
                         ((uint8_t *)descs[i].data)[2], ((uint8_t *)descs[i].data)[3],
                         ((uint8_t *)descs[i].data)[4], ((uint8_t *)descs[i].data)[5],
                         ((uint8_t *)descs[i].data)[6], ((uint8_t *)descs[i].data)[7]);
            }
            (void)ubc_write_desc(csq, ubc_dev, idx, &descs[i]);
            idx = (idx + 1) % csq->depth;
        }

        csq->head = idx;
        ubc_ers2_write32(ubc_dev, UBASE_CSQ_HEAD_REG, csq->head);
        ubc_raise_cmdq_event(ubc_dev);

        /* Record the CSQ base used for successful CMDQ processing */
        ubc_dev->cmdq_last_csq_base = csq->base;

        /* Diagnostic: verify ue2ue ctrlq response reached guest RAM */
        if (opcode == UBASE_OPC_UE2UE_UBASE && bd_num > 0) {
            UBCCmdqDesc verify;
            uint16_t first = (idx + csq->depth - bd_num) % csq->depth;
            fprintf(stderr, "ubc ue2ue resp write: bd0 op=0x%x flag=0x%x ret=%u"
                    " data[0..3]=[%08x %08x %08x %08x] data[4..7]=[%08x %08x %08x %08x]\n",
                    le16_to_cpu(descs[0].opcode), descs[0].flag,
                    le16_to_cpu(descs[0].ret),
                    le32_to_cpu(descs[0].data[0]), le32_to_cpu(descs[0].data[1]),
                    le32_to_cpu(descs[0].data[2]), le32_to_cpu(descs[0].data[3]),
                    le32_to_cpu(descs[0].data[4]), le32_to_cpu(descs[0].data[5]),
                    le32_to_cpu(descs[0].data[6]), le32_to_cpu(descs[0].data[7]));
            if (ubc_read_desc(csq, ubc_dev, first, &verify) == MEMTX_OK) {
                fprintf(stderr, "ubc ue2ue resp readback[%u]: op=0x%x flag=0x%x ret=%u"
                        " data[0..3]=[%08x %08x %08x %08x] data[4..7]=[%08x %08x %08x %08x]\n",
                        first, le16_to_cpu(verify.opcode), verify.flag,
                        le16_to_cpu(verify.ret),
                        le32_to_cpu(verify.data[0]), le32_to_cpu(verify.data[1]),
                        le32_to_cpu(verify.data[2]), le32_to_cpu(verify.data[3]),
                        le32_to_cpu(verify.data[4]), le32_to_cpu(verify.data[5]),
                        le32_to_cpu(verify.data[6]), le32_to_cpu(verify.data[7]));
            } else {
                fprintf(stderr, "ubc ue2ue resp readback FAILED for idx=%u\n", first);
            }
        }

        /* Push ue2ue ctrlq response to CMDQ CRQ for event dispatch.
         * The guest's ctrlq CMDQ-fallback path expects the ue2ue response
         * on CMDQ CRQ so ubase_cmd_crq_handler can dispatch it to the
         * registered ue2ue event handler, which then calls
         * ubase_ctrlq_notify_completed → complete(&ctx->done) to wake
         * the ctrlq waiter. */
        if (opcode == UBASE_OPC_UE2UE_UBASE && ret == 0) {
            UBCmdQueueState *cmd_crq = &ubc_dev->cmd_crq;
            const UBCCtrlqMsgHdr *crq_msg_hdr;
            UBCCmdqDesc crq_bd;
            size_t actual_len;
            uint8_t crq_bd_num, j;
            uint16_t crq_next;

            /* Use cached CRQ state (already synced by ubc_sync_cmdq_regs
             * at start of ubc_process_cmdq).  Do NOT re-read from storage
             * here — the values are stable for the duration of this call. */
            if (ubc_trace_data_path_enabled()) {
                qemu_log("ubc cmdq crq push ue2ue: cached base=%#lx"
                         " depth=%u head=%u tail=%u\n",
                         (unsigned long)cmd_crq->base, cmd_crq->depth,
                         cmd_crq->head, cmd_crq->tail);
            }

            if (cmd_crq->base && cmd_crq->depth) {
                /* Determine actual response length from ctrlq bb_num field */
                if (cap >= 28) {
                    crq_msg_hdr = (const UBCCtrlqMsgHdr *)(resp_flat + 16);
                    actual_len = 28 + (size_t)crq_msg_hdr->bb_num * 20;
                    if (actual_len > cap) {
                        actual_len = cap;
                    }
                } else {
                    actual_len = cap;
                }

                /* Calculate CRQ BDs: msg_data_len = bd_num * 32 - 8 */
                crq_bd_num = (uint8_t)((actual_len + 8 + 31) / 32);
                if (crq_bd_num < 1) {
                    crq_bd_num = 1;
                }

                if (ubc_trace_data_path_enabled()) {
                    qemu_log("ubc cmdq crq push ue2ue: actual_len=%zu"
                             " crq_bd_num=%u head=%u tail=%u depth=%u\n",
                             actual_len, crq_bd_num, cmd_crq->head,
                             cmd_crq->tail, cmd_crq->depth);
                }

                /* Write BD0: header + first 24 bytes of resp_flat */
                memset(&crq_bd, 0, sizeof(crq_bd));
                crq_bd.opcode = cpu_to_le16(UBASE_OPC_UE2UE_UBASE);
                crq_bd.flag = UBASE_CMD_FLAG_IN | UBASE_CMD_FLAG_OUT;
                crq_bd.bd_num = crq_bd_num;
                crq_bd.ret = 0;
                memcpy(crq_bd.data, resp_flat, MIN(24, actual_len));

                crq_next = (cmd_crq->tail + 1) % cmd_crq->depth;
                if (crq_next == cmd_crq->head) {
                    qemu_log("ubc cmdq crq full, cannot push ue2ue\n");
                } else if (ubc_write_desc(cmd_crq, ubc_dev,
                                           cmd_crq->tail,
                                           &crq_bd) != MEMTX_OK) {
                    qemu_log("ubc cmdq crq write bd0 failed\n");
                } else {
                    cmd_crq->tail = crq_next;

                    /* Write continuation BDs */
                    for (j = 1; j < crq_bd_num; j++) {
                        size_t off = 24 + (size_t)(j - 1) * 32;

                        crq_next = (cmd_crq->tail + 1) % cmd_crq->depth;
                        if (crq_next == cmd_crq->head) {
                            qemu_log("ubc cmdq crq full at cont bd%u\n", j);
                            break;
                        }
                        memset(&crq_bd, 0, sizeof(crq_bd));
                        if (off < actual_len) {
                            memcpy(&crq_bd, resp_flat + off,
                                   MIN(32, actual_len - off));
                        }
                        if (ubc_write_desc(cmd_crq, ubc_dev,
                                           cmd_crq->tail,
                                           &crq_bd) != MEMTX_OK) {
                            qemu_log("ubc cmdq crq write cont bd%u"
                                     " failed\n", j);
                            break;
                        }
                        cmd_crq->tail = crq_next;
                    }

                    /* Update CRQ tail register and raise interrupt */
                    ubc_ers2_write32(ubc_dev, UBASE_CRQ_TAIL_REG,
                                     cmd_crq->tail);
                    if (ubc_trace_data_path_enabled()) {
                        qemu_log("ubc cmdq crq pushed: new_tail=%u\n",
                                 cmd_crq->tail);
                    }
                    ubc_raise_cmdq_event(ubc_dev);
                }
            } else {
                fprintf(stderr, "ubc cmdq crq not initialized"
                        " (base=%#lx depth=%u)\n",
                        (unsigned long)cmd_crq->base, cmd_crq->depth);
            }
        }

        g_free(req_flat);
        g_free(resp_flat);
        g_free(descs);
    }
}

static void ubc_sync_ctrlq_regs(BusControllerDev *ubc_dev)
{
    ubc_dev->ctrl_csq.base = ubc_read_q_base(ubc_dev, UBASE_CTRLQ_CSQ_BASEADDR_L_REG);
    ubc_dev->ctrl_csq.depth = ubc_read_q_depth(ubc_dev, UBASE_CTRLQ_CSQ_DEPTH_REG);
    ubc_dev->ctrl_csq.head = (uint16_t)ubc_ers2_read32(ubc_dev, UBASE_CTRLQ_CSQ_HEAD_REG);
    ubc_dev->ctrl_csq.tail = (uint16_t)ubc_ers2_read32(ubc_dev, UBASE_CTRLQ_CSQ_TAIL_REG);

    ubc_dev->ctrl_crq.base = ubc_read_q_base(ubc_dev, UBASE_CTRLQ_CRQ_BASEADDR_L_REG);
    ubc_dev->ctrl_crq.depth = ubc_read_q_depth(ubc_dev, UBASE_CTRLQ_CRQ_DEPTH_REG);
    ubc_dev->ctrl_crq.head = (uint16_t)ubc_ers2_read32(ubc_dev, UBASE_CTRLQ_CRQ_HEAD_REG);
    ubc_dev->ctrl_crq.tail = (uint16_t)ubc_ers2_read32(ubc_dev, UBASE_CTRLQ_CRQ_TAIL_REG);

    qemu_log("ubc ctrlq sync csq_base=%#" PRIx64 " csq_depth=%u csq_head=%u csq_tail=%u"
             " crq_base=%#" PRIx64 " crq_depth=%u crq_head=%u crq_tail=%u\n",
             (uint64_t)ubc_dev->ctrl_csq.base, ubc_dev->ctrl_csq.depth,
             ubc_dev->ctrl_csq.head, ubc_dev->ctrl_csq.tail,
             (uint64_t)ubc_dev->ctrl_crq.base, ubc_dev->ctrl_crq.depth,
             ubc_dev->ctrl_crq.head, ubc_dev->ctrl_crq.tail);
}

static void ubc_process_ctrlq(BusControllerDev *ubc_dev)
{
    UBCmdQueueState *csq = &ubc_dev->ctrl_csq;
    UBCmdQueueState *crq = &ubc_dev->ctrl_crq;
    uint16_t guard = 0;

    if (!csq->base || !csq->depth) {
        fprintf(stderr, "ubc ctrlq SKIP: csq_base=%#lx csq_depth=%u\n",
                (unsigned long)csq->base, csq->depth);
        return;
    }

    fprintf(stderr, "ubc ctrlq ENTER: csq_head=%u csq_tail=%u csq_base=%#lx"
            " crq_head=%u crq_tail=%u crq_base=%#lx crq_depth=%u\n",
            csq->head, csq->tail, (unsigned long)csq->base,
            crq->head, crq->tail, (unsigned long)crq->base, crq->depth);

    while (csq->head != csq->tail && guard++ < csq->depth) {
        UBCCtrlqBaseBlock req_bb;
        UBCCtrlqBaseBlock resp_bb;
        uint16_t crq_next;

        if (ubc_read_desc(csq, ubc_dev, csq->head,
                          (UBCCmdqDesc *)&req_bb) != MEMTX_OK) {
            qemu_log("ubc ctrlq read csq[%u] failed\n", csq->head);
            break;
        }

        qemu_log("ubc ctrlq process service_type=0x%x opcode=0x%x bb_num=%u"
                 " seq=%u head=%u tail=%u\n",
                 req_bb.service_type, req_bb.opcode, req_bb.bb_num,
                 le16_to_cpu(req_bb.seq), csq->head, csq->tail);

        csq->head = (csq->head + 1) % csq->depth;
        ubc_ers2_write32(ubc_dev, UBASE_CTRLQ_CSQ_HEAD_REG, csq->head);
        ubc_raise_cmdq_event(ubc_dev);

        memset(&resp_bb, 0, sizeof(resp_bb));
        resp_bb.service_ver = req_bb.service_ver;
        resp_bb.service_type = req_bb.service_type;
        resp_bb.opcode = req_bb.opcode;
        resp_bb.seq = req_bb.seq;
        resp_bb.mbx_ue_id = req_bb.mbx_ue_id;
        resp_bb.bus_ue_id = req_bb.bus_ue_id;
        resp_bb.bb_num = 1;

        if (req_bb.service_type == UBASE_CTRLQ_SER_TYPE_QOS) {
            switch (req_bb.opcode) {
            case UBASE_CTRLQ_OPC_QUERY_VL:
                resp_bb.data[0] = cpu_to_le32(0x0001);
                resp_bb.ret = 0;
                break;
            case UBASE_CTRLQ_OPC_QUERY_SL: {
                uint16_t sl_bitmap = 0xFFFF;
                uint16_t rc_max = 64;
                uint8_t *dp = (uint8_t *)resp_bb.data;
                memcpy(dp, &sl_bitmap, sizeof(sl_bitmap));
                memcpy(dp + 2, &rc_max, sizeof(rc_max));
                memcpy(dp + 4, &sl_bitmap, sizeof(sl_bitmap));
                resp_bb.ret = 0;
                break;
            }
            default:
                resp_bb.ret = 0;
                break;
            }
        } else if (req_bb.service_type == UBASE_CTRLQ_SER_TYPE_DEV_REGISTER &&
                   req_bb.opcode == UBASE_CTRLQ_OPC_GET_SEID_INFO) {
            /*
             * Return 1 SEID entry with a link-local EID (fe80::1).
             * The ctrlq EID bytes are in hw order; udma_swap_endian()
             * in the guest driver will reverse them to produce the
             * kernel-native EID, which ipourma then uses as IPv6 addr.
             *
             * EID response layout (after 12-byte ctrlq header):
             *   [0:3]  seid_num=1 | rsv=0       (4 bytes)
             *   [4:7]  eid_idx=0                 (4 bytes)
             *   [8:23] eid[16] byte-reversed     (16 bytes)
             *   [24:27] upi=0                    (4 bytes)
             *
             * Total data = 28 bytes → needs 2 BDs (20 + 8).
             * fe80::1 in big-endian = fe80...01, reversed = 01...80fe
             * fe80::2 in big-endian = fe80...02, reversed = 02...80fe
             * Use UB_FM_NODE_ID to differentiate: nodeA → ::1, nodeB → ::2
             */
            const char *fm_node_id = g_getenv("UB_FM_NODE_ID");
            uint8_t eid_suffix = ubc_node_ip_suffix_from_id(fm_node_id);
            uint8_t eid_hw[16];
            UBCCmdqDesc cont;
            uint16_t crq_next2;

            if (eid_suffix == 0) {
                qemu_log("ubc ctrlq GET_SEID_INFO: unresolved UB_FM_NODE_ID=%s\n",
                         fm_node_id ? fm_node_id : "(null)");
                return;
            }
            ubc_fill_link_local_eid_hw(eid_hw, eid_suffix);

            resp_bb.bb_num = 2;
            resp_bb.ret = 0;
            /* data[0] = seid_num(1) | rsv(0) */
            resp_bb.data[0] = cpu_to_le32(1);
            /* data[1] = eid_idx(0) */
            resp_bb.data[1] = cpu_to_le32(0);
            /* data[2..4] = eid_hw[0..11] */
            memcpy(&resp_bb.data[2], eid_hw, 12);

            /* BD1: continuation — remaining eid bytes + upi */
            memset(&cont, 0, sizeof(cont));
            memcpy(cont.data, eid_hw + 12, 4);
            /* cont.data[1] = upi = 0 (already zeroed) */

            /* write BD0 (header) */
            crq_next = (crq->tail + 1) % crq->depth;
            if (crq_next == crq->head) {
                qemu_log("ubc ctrlq crq full (seid resp)\n");
                break;
            }
            if (ubc_write_desc(crq, ubc_dev, crq->tail,
                               (const UBCCmdqDesc *)&resp_bb) != MEMTX_OK) {
                qemu_log("ubc ctrlq write crq[seid bd0] failed\n");
                break;
            }
            crq->tail = crq_next;

            /* write BD1 (continuation) */
            crq_next2 = (crq->tail + 1) % crq->depth;
            if (crq_next2 == crq->head) {
                qemu_log("ubc ctrlq crq full (seid cont)\n");
                break;
            }
            if (ubc_write_desc(crq, ubc_dev, crq->tail,
                               &cont) != MEMTX_OK) {
                qemu_log("ubc ctrlq write crq[seid bd1] failed\n");
                break;
            }
            crq->tail = crq_next2;

            ubc_ers2_write32(ubc_dev, UBASE_CTRLQ_CRQ_TAIL_REG, crq->tail);
            ubc_raise_ctrlq_event(ubc_dev);
            qemu_log("ubc ctrlq seid resp: 1 eid fe80::%u (2 BDs)\n", eid_suffix);
            continue;
        } else if (req_bb.service_type == UBASE_CTRLQ_SER_TYPE_TP_ACL &&
                   req_bb.opcode == UBASE_CTRLQ_OPC_GET_TP_LIST) {
            /*
             * TP ACL GET_TP_LIST response (single BD, 12 bytes data):
             *   [0:3]  tp_list_cnt(1) | rsv(0)
             *   [4:7]  tpid=1 | tpn_cnt=1
             *   [8:11] tpn_start=0 | migr=0 | rsv=0
             */
            uint32_t tpid = ubc_dev->next_tp_id;

            if (tpid == 0 || tpid > 0x00FFFFFFU) {
                tpid = 1;
            }
            ubc_dev->next_tp_id = tpid + 1;
            if (ubc_dev->next_tp_id == 0 || ubc_dev->next_tp_id > 0x00FFFFFFU) {
                ubc_dev->next_tp_id = 1;
            }

            resp_bb.ret = 0;
            resp_bb.data[0] = cpu_to_le32(1); /* tp_list_cnt=1 */
            resp_bb.data[1] = cpu_to_le32((tpid & 0x00FFFFFFU) | (1U << 24));
            resp_bb.data[2] = 0; /* tpn_start=0 */
            qemu_log("ubc ctrlq tp_acl get_tp_list resp: 1 TP entry (tpid=%u)\n",
                     tpid);
        } else {
            resp_bb.ret = 0;
        }

        if (!crq->base || !crq->depth) {
            fprintf(stderr, "ubc ctrlq crq not initialized: crq_base=%#lx"
                    " crq_depth=%u, skipping response\n",
                    (unsigned long)crq->base, crq->depth);
            continue;
        }

        crq_next = (crq->tail + 1) % crq->depth;
        if (crq_next == crq->head) {
            qemu_log("ubc ctrlq crq full, cannot push response\n");
            break;
        }

        fprintf(stderr, "ubc ctrlq write resp: crq_base=%#lx crq_tail=%u"
                " crq_head=%u crq_depth=%u\n",
                (unsigned long)crq->base, crq->tail, crq->head, crq->depth);

        if (ubc_write_desc(crq, ubc_dev, crq->tail,
                           (const UBCCmdqDesc *)&resp_bb) != MEMTX_OK) {
            qemu_log("ubc ctrlq write crq[%u] failed\n", crq->tail);
            break;
        }
        crq->tail = crq_next;
        ubc_ers2_write32(ubc_dev, UBASE_CTRLQ_CRQ_TAIL_REG, crq->tail);
        fprintf(stderr, "ubc ctrlq resp done: crq_tail=%u\n", crq->tail);
        ubc_raise_ctrlq_event(ubc_dev);
    }
}

static void ubc_ers2_mmio_write(BusControllerDev *ubc_dev, hwaddr addr,
                                uint64_t val, unsigned len)
{
    uint8_t *storage = ubc_dev->ers[2].storage;
    uint32_t cur;

    if (addr + len > ubc_dev->ers[2].storage_size) {
        fprintf(stderr, "ubc ers2 mmio write OUT OF BOUNDS addr=%#lx val=%#lx"
               " storage_size=%#lx\n", (unsigned long)addr,
               (unsigned long)val,
               (unsigned long)ubc_dev->ers[2].storage_size);
        return;
    }

    /* Diagnostic: log all ctrlq-range writes */
    if (addr >= 0x18800 && addr < 0x18830) {
        fprintf(stderr, "ubc ers2 CTRLQ WRITE addr=%#lx val=%#lx len=%u\n",
                (unsigned long)addr, (unsigned long)val, len);
    }

    if (len == DWORD_SIZE) {
        switch (addr) {
        case UBASE_VECTOR0_CMDQ_SRC_REG:
            cur = ubc_ers2_read32(ubc_dev, addr);
            fprintf(stderr,
                    "ubc ers2 WRITE CMDQ_SRC clear_mask=%#x old=%#x new=%#x\n",
                    (uint32_t)val, cur, cur & ~(uint32_t)val);
            cur &= ~(uint32_t)val;
            ubc_ers2_write32(ubc_dev, addr, cur);
            return;
        case UBASE_VECTOR0_CTRLQ_SRC_REG:
            cur = ubc_ers2_read32(ubc_dev, addr);
            fprintf(stderr,
                    "ubc ers2 WRITE CTRLQ_SRC clear_mask=%#x old=%#x new=%#x\n",
                    (uint32_t)val, cur, cur & ~(uint32_t)val);
            cur &= ~(uint32_t)val;
            ubc_ers2_write32(ubc_dev, addr, cur);
            return;
        /* CMDQ CSQ registers */
        case UBASE_CSQ_BASEADDR_L_REG:
        case UBASE_CSQ_BASEADDR_H_REG:
        case UBASE_CSQ_DEPTH_REG:
        case UBASE_CSQ_HEAD_REG:
            ubc_ers2_write32(ubc_dev, addr, (uint32_t)val);
            ubc_sync_cmdq_regs(ubc_dev);
            return;
        case UBASE_CSQ_TAIL_REG:
            ubc_ers2_write32(ubc_dev, addr, (uint32_t)val);
            ubc_sync_cmdq_regs(ubc_dev);
            ubc_process_cmdq(ubc_dev);
            return;
        /* CMDQ CRQ registers */
        case UBASE_CRQ_BASEADDR_L_REG:
        case UBASE_CRQ_BASEADDR_H_REG:
        case UBASE_CRQ_DEPTH_REG:
        case UBASE_CRQ_HEAD_REG:
            ubc_ers2_write32(ubc_dev, addr, (uint32_t)val);
            ubc_sync_cmdq_regs(ubc_dev);
            return;
        case UBASE_CRQ_TAIL_REG:
            return;
        /* CtrlQ CSQ registers (separate register set at 0x18800) */
        case UBASE_CTRLQ_CSQ_BASEADDR_L_REG:
        case UBASE_CTRLQ_CSQ_BASEADDR_H_REG:
        case UBASE_CTRLQ_CSQ_DEPTH_REG:
        case UBASE_CTRLQ_CSQ_HEAD_REG:
            ubc_ers2_write32(ubc_dev, addr, (uint32_t)val);
            ubc_sync_ctrlq_regs(ubc_dev);
            return;
        case UBASE_CTRLQ_CSQ_TAIL_REG:
            ubc_ers2_write32(ubc_dev, addr, (uint32_t)val);
            ubc_sync_ctrlq_regs(ubc_dev);
            ubc_process_ctrlq(ubc_dev);
            return;
        /* CtrlQ CRQ registers (separate register set at 0x18800) */
        case UBASE_CTRLQ_CRQ_BASEADDR_L_REG:
        case UBASE_CTRLQ_CRQ_BASEADDR_H_REG:
        case UBASE_CTRLQ_CRQ_DEPTH_REG:
        case UBASE_CTRLQ_CRQ_HEAD_REG:
            ubc_ers2_write32(ubc_dev, addr, (uint32_t)val);
            ubc_sync_ctrlq_regs(ubc_dev);
            return;
        case UBASE_CTRLQ_CRQ_TAIL_REG:
            return;
        default:
            break;
        }
    }

    switch (len) {
    case BYTE_SIZE:
        storage[addr] = (uint8_t)val;
        break;
    case WORD_SIZE:
        stw_le_p(storage + addr, (uint16_t)val);
        break;
    case DWORD_SIZE:
        stl_le_p(storage + addr, (uint32_t)val);
        break;
    case sizeof(uint64_t):
        stq_le_p(storage + addr, val);
        break;
    default:
        break;
    }
}

static bool linqu_uapi_decode_endpoint(hwaddr addr, hwaddr *reg)
{
    if (addr < LINQU_UAPI_ENDPOINT_BASE ||
        addr >= (LINQU_UAPI_ENDPOINT_BASE + 0x1000)) {
        return false;
    }
    *reg = (addr - LINQU_UAPI_ENDPOINT_BASE) & 0xfffULL;
    return true;
}

static uint64_t linqu_uapi_status(BusControllerDev *ubc_dev)
{
    uint32_t cmdq_pending;
    uint32_t cq_pending;

    if (ubc_dev->linqu_uapi_cmdq_tail >= ubc_dev->linqu_uapi_cmdq_head) {
        cmdq_pending = ubc_dev->linqu_uapi_cmdq_tail - ubc_dev->linqu_uapi_cmdq_head;
    } else {
        cmdq_pending = ubc_dev->linqu_uapi_cmdq_depth -
                       ubc_dev->linqu_uapi_cmdq_head +
                       ubc_dev->linqu_uapi_cmdq_tail;
    }
    if (ubc_dev->linqu_uapi_cq_tail >= ubc_dev->linqu_uapi_cq_head) {
        cq_pending = ubc_dev->linqu_uapi_cq_tail - ubc_dev->linqu_uapi_cq_head;
    } else {
        cq_pending = ubc_dev->linqu_uapi_cq_depth -
                     ubc_dev->linqu_uapi_cq_head +
                     ubc_dev->linqu_uapi_cq_tail;
    }

    return ((uint64_t)cmdq_pending << 0) |
           ((uint64_t)cq_pending << 16) |
           ((uint64_t)ubc_dev->linqu_uapi_cmdq_head << 32) |
           ((uint64_t)ubc_dev->linqu_uapi_cmdq_tail << 40) |
           ((uint64_t)ubc_dev->linqu_uapi_cq_head << 48) |
           ((uint64_t)ubc_dev->linqu_uapi_cq_tail << 56);
}

static struct LinquUbGmRegistry *linqu_ub_gm_registry_new(void)
{
    struct LinquUbGmRegistry *registry = g_new0(struct LinquUbGmRegistry, 1);

    qemu_mutex_init(&registry->lock);
    QTAILQ_INIT(&registry->dispatches);
    return registry;
}

static void linqu_ub_gm_dispatch_free(LinquUbGmDispatchState *dispatch)
{
    if (!dispatch) {
        return;
    }
    g_free(dispatch->bindings);
    g_free(dispatch);
}

static void linqu_ub_gm_registry_free(struct LinquUbGmRegistry *registry)
{
    LinquUbGmDispatchState *dispatch;

    if (!registry) {
        return;
    }
    qemu_mutex_lock(&registry->lock);
    while ((dispatch = QTAILQ_FIRST(&registry->dispatches)) != NULL) {
        QTAILQ_REMOVE(&registry->dispatches, dispatch, next);
        linqu_ub_gm_dispatch_free(dispatch);
    }
    qemu_mutex_unlock(&registry->lock);
    qemu_mutex_destroy(&registry->lock);
    g_free(registry);
}

static void linqu_uapi_schedule_kick(BusControllerDev *ubc_dev,
                                     uint32_t batch);

static void linqu_uapi_authorization_free(
    LinquPtoAuthorizationState *authorization)
{
    if (!authorization) {
        return;
    }
    g_free(authorization->range_authorized);
    g_free(authorization->resolved);
    g_free(authorization->shape_stride_wire);
    g_free(authorization->scalar_wire);
    g_free(authorization->memref_wire);
    g_free(authorization);
}

static void linqu_uapi_authorization_discard(BusControllerDev *ubc_dev)
{
    if (!ubc_dev) {
        return;
    }
    if (ubc_dev->linqu_uapi_authorization_timer) {
        timer_del(ubc_dev->linqu_uapi_authorization_timer);
    }
    linqu_uapi_authorization_free(ubc_dev->linqu_uapi_authorization);
    ubc_dev->linqu_uapi_authorization = NULL;
}

static uint64_t linqu_uapi_ns_add_saturating(uint64_t base, uint64_t delta)
{
    return delta > UINT64_MAX - base ? UINT64_MAX : base + delta;
}

static bool linqu_uapi_authorization_complete(
    BusControllerDev *ubc_dev, uint64_t op_id, uint64_t request_id,
    uint64_t sequence, int completion_status, const char *source)
{
    LinquPtoAuthorizationState *authorization;
    const char *status;

    if (!ubc_dev || !ubc_dev->linqu_uapi_authorization) {
        qemu_log("QEMU_UB_GM_AUTHORIZATION_COMPLETION_IGNORED"
                 " op=%" PRIu64 " request=%" PRIu64
                 " sequence=%" PRIu64 " source=%s reason=no_pending\n",
                 op_id, request_id, sequence,
                 source ? source : "unknown");
        return false;
    }
    authorization = ubc_dev->linqu_uapi_authorization;
    if (authorization->op_id != op_id ||
        authorization->request_id != request_id ||
        authorization->sequence != sequence) {
        qemu_log("QEMU_UB_GM_AUTHORIZATION_COMPLETION_IGNORED"
                 " op=%" PRIu64 " request=%" PRIu64
                 " sequence=%" PRIu64 " source=%s"
                 " reason=identity_mismatch active_op=%" PRIu64
                 " active_request=%" PRIu64
                 " active_sequence=%" PRIu64 "\n",
                 op_id, request_id, sequence,
                 source ? source : "unknown", authorization->op_id,
                 authorization->request_id, authorization->sequence);
        return false;
    }
    if (!authorization->waiting) {
        qemu_log("QEMU_UB_GM_AUTHORIZATION_COMPLETION_IGNORED"
                 " op=%" PRIu64 " request=%" PRIu64
                 " sequence=%" PRIu64 " source=%s"
                 " reason=already_completed\n",
                 op_id, request_id, sequence,
                 source ? source : "unknown");
        return false;
    }

    authorization->waiting = false;
    authorization->completion_ready = true;
    authorization->completion_status = completion_status;
    status = completion_status == LINGQU_PTO_UB_GM_OK ? "ready" :
             completion_status == LINGQU_PTO_UB_GM_AUTHORIZATION_TIMEOUT ?
             "timeout" : "failed";
    qemu_log("QEMU_UB_GM_AUTHORIZATION_RESUME op=%" PRIu64
             " request=%" PRIu64 " slot=%u cursor=%u sequence=%" PRIu64
             " status=%s source=%s\n",
             authorization->op_id, authorization->request_id,
             authorization->cmdq_slot, authorization->memref_cursor,
             authorization->sequence, status,
             source ? source : "unknown");
    linqu_uapi_schedule_kick(
        ubc_dev, MAX(authorization->resume_batch, 1u));
    return true;
}

static void linqu_uapi_authorization_timer(void *opaque)
{
    BusControllerDev *ubc_dev = opaque;
    LinquPtoAuthorizationState *authorization;
    uint64_t op_id;
    uint64_t request_id;
    uint64_t sequence;
    int completion_status;

    if (!ubc_dev || !ubc_dev->linqu_uapi_authorization) {
        return;
    }
    authorization = ubc_dev->linqu_uapi_authorization;
    if (!authorization->waiting) {
        return;
    }

    op_id = authorization->op_id;
    request_id = authorization->request_id;
    sequence = authorization->sequence;
    if (authorization->ready_ns > authorization->deadline_ns) {
        completion_status = LINGQU_PTO_UB_GM_AUTHORIZATION_TIMEOUT;
    } else {
        completion_status = LINGQU_PTO_UB_GM_OK;
    }
    if (linqu_uapi_authorization_complete(
            ubc_dev, op_id, request_id, sequence, completion_status,
            "timer") &&
        ubc_dev->pto_authorization_inject_duplicate_completion) {
        linqu_uapi_authorization_complete(
            ubc_dev, op_id, request_id, sequence, completion_status,
            "duplicate-injection");
    }
}

static bool linqu_ub_gm_binding_snapshot(BusControllerDev *ubc_dev,
                                         uint64_t request_id,
                                         uint64_t binding_id,
                                         uint64_t ub_gm_addr,
                                         uint64_t length,
                                         uint32_t required_access,
                                         LinquUbGmBindingState *snapshot)
{
    struct LinquUbGmRegistry *registry;
    LinquUbGmDispatchState *dispatch;
    bool found = false;

    if (!ubc_dev || !snapshot || request_id == 0 || binding_id == 0 ||
        length == 0 || ub_gm_addr > UINT64_MAX - length) {
        return false;
    }
    registry = ubc_dev->linqu_uapi_ub_gm_registry;
    if (!registry) {
        return false;
    }

    qemu_mutex_lock(&registry->lock);
    QTAILQ_FOREACH(dispatch, &registry->dispatches, next) {
        uint32_t index;

        if (dispatch->request_id != request_id) {
            continue;
        }
        for (index = 0; index < dispatch->binding_count; index++) {
            LinquUbGmBindingState *binding = &dispatch->bindings[index];
            uint64_t offset;

            if (!binding->active || binding->binding_id != binding_id ||
                (binding->access & required_access) != required_access ||
                ub_gm_addr < binding->local_base) {
                continue;
            }
            offset = ub_gm_addr - binding->local_base;
            if (offset > binding->length ||
                length > binding->length - offset) {
                continue;
            }
            *snapshot = *binding;
            found = true;
            break;
        }
        break;
    }
    qemu_mutex_unlock(&registry->lock);
    return found;
}

static bool linqu_ub_gm_mapping_still_authorized(
    BusControllerDev *ubc_dev,
    const LinquUbGmBindingState *binding,
    uint64_t ub_gm_addr,
    uint64_t length,
    UbcObmmResolvedMap *resolved)
{
    uint64_t offset;

    if (!binding || !resolved || ub_gm_addr < binding->local_base ||
        length == 0 || length > UINT32_MAX) {
        return false;
    }
    offset = ub_gm_addr - binding->local_base;
    if (offset > binding->length || length > binding->length - offset ||
        binding->remote_base > UINT64_MAX - offset ||
        !ub_obmm_async_resolve_mapping_ref(
            ubc_dev->obmm_async, binding->mapping_ref, ub_gm_addr, length,
            resolved)) {
        return false;
    }
    return resolved->map_id == binding->map_id &&
           resolved->map_generation == binding->map_generation &&
           resolved->remote_uba == binding->remote_base + offset &&
           resolved->token_id == binding->token_id &&
           resolved->peer_cna == binding->peer_cna;
}

static void linqu_ub_gm_account_access(BusControllerDev *ubc_dev,
                                       uint64_t request_id,
                                       uint64_t binding_id,
                                       uint64_t read_bytes,
                                       uint64_t write_bytes,
                                       bool fenced)
{
    struct LinquUbGmRegistry *registry;
    LinquUbGmDispatchState *dispatch;

    if (!ubc_dev || !ubc_dev->linqu_uapi_ub_gm_registry) {
        return;
    }
    registry = ubc_dev->linqu_uapi_ub_gm_registry;
    qemu_mutex_lock(&registry->lock);
    QTAILQ_FOREACH(dispatch, &registry->dispatches, next) {
        uint32_t index;

        if (dispatch->request_id != request_id) {
            continue;
        }
        for (index = 0; index < dispatch->binding_count; index++) {
            LinquUbGmBindingState *binding = &dispatch->bindings[index];

            if (binding->binding_id != binding_id) {
                continue;
            }
            binding->read_bytes += read_bytes;
            binding->write_bytes += write_bytes;
            binding->dirty |= write_bytes != 0;
            binding->fence_count += fenced ? 1 : 0;
            break;
        }
        break;
    }
    registry->qemu_load_bytes += read_bytes;
    registry->qemu_store_bytes += write_bytes;
    registry->qemu_fence_count += fenced ? 1 : 0;
    qemu_mutex_unlock(&registry->lock);
}

static int linqu_ub_gm_read(void *opaque, uint64_t request_id,
                            uint64_t binding_id, uint64_t ub_gm_addr,
                            void *dst, uint64_t length)
{
    BusControllerDev *ubc_dev = opaque;
    LinquUbGmBindingState binding;
    UbcObmmResolvedMap resolved;

    /* Simpler invokes this callback from its PTO worker thread. */
    QEMU_IOTHREAD_LOCK_GUARD();

    if (!dst || !linqu_ub_gm_binding_snapshot(
                    ubc_dev, request_id, binding_id, ub_gm_addr, length,
                    LINGQU_PTO_UB_GM_READ, &binding) ||
        !linqu_ub_gm_mapping_still_authorized(
            ubc_dev, &binding, ub_gm_addr, length, &resolved)) {
        qemu_log("QEMU_UB_GM_LOAD denied request=%" PRIu64
                 " binding=%" PRIu64 " addr=0x%" PRIx64
                 " length=%" PRIu64 "\n",
                 request_id, binding_id, ub_gm_addr, length);
        return -LINGQU_PTO_UB_GM_UNBOUND;
    }
    if (ubc_sim_dec_remote_read(ubc_dev, resolved.remote_uba,
                                resolved.token_id, resolved.peer_cna,
                                dst, (uint32_t)length) != MEMTX_OK) {
        return -LINGQU_PTO_UB_GM_CALLBACK_FAILED;
    }
    linqu_ub_gm_account_access(ubc_dev, request_id, binding_id,
                               length, 0, false);
    qemu_log("QEMU_UB_GM_LOAD request=%" PRIu64 " binding=%" PRIu64
             " addr=0x%" PRIx64 " length=%" PRIu64
             " map=%" PRIu64 " generation=%" PRIu64 "\n",
             request_id, binding_id, ub_gm_addr, length,
             resolved.map_id, resolved.map_generation);
    return 0;
}

static int linqu_ub_gm_write(void *opaque, uint64_t request_id,
                             uint64_t binding_id, uint64_t ub_gm_addr,
                             const void *src, uint64_t length)
{
    BusControllerDev *ubc_dev = opaque;
    LinquUbGmBindingState binding;
    UbcObmmResolvedMap resolved;

    /* Simpler invokes this callback from its PTO worker thread. */
    QEMU_IOTHREAD_LOCK_GUARD();

    if (!src || !linqu_ub_gm_binding_snapshot(
                    ubc_dev, request_id, binding_id, ub_gm_addr, length,
                    LINGQU_PTO_UB_GM_WRITE, &binding) ||
        !linqu_ub_gm_mapping_still_authorized(
            ubc_dev, &binding, ub_gm_addr, length, &resolved) ||
        (resolved.access_flags & SIM_DEC_GVA_ACCESS_READ_ONLY)) {
        qemu_log("QEMU_UB_GM_STORE denied request=%" PRIu64
                 " binding=%" PRIu64 " addr=0x%" PRIx64
                 " length=%" PRIu64 "\n",
                 request_id, binding_id, ub_gm_addr, length);
        return -LINGQU_PTO_UB_GM_ACCESS_DENIED;
    }
    if (ubc_sim_dec_remote_write(ubc_dev, resolved.remote_uba,
                                 resolved.token_id, resolved.peer_cna,
                                 src, (uint32_t)length) != MEMTX_OK) {
        return -LINGQU_PTO_UB_GM_CALLBACK_FAILED;
    }
    linqu_ub_gm_account_access(ubc_dev, request_id, binding_id,
                               0, length, false);
    qemu_log("QEMU_UB_GM_STORE request=%" PRIu64 " binding=%" PRIu64
             " addr=0x%" PRIx64 " length=%" PRIu64
             " map=%" PRIu64 " generation=%" PRIu64 "\n",
             request_id, binding_id, ub_gm_addr, length,
             resolved.map_id, resolved.map_generation);
    return 0;
}

static int linqu_ub_gm_fence(void *opaque, uint64_t request_id,
                             uint64_t binding_id, uint64_t ub_gm_addr,
                             uint64_t length, uint32_t flags)
{
    BusControllerDev *ubc_dev = opaque;
    LinquUbGmBindingState binding;
    UbcObmmResolvedMap resolved;

    /* Simpler invokes this callback from its PTO worker thread. */
    QEMU_IOTHREAD_LOCK_GUARD();

    if (flags != 0 || !linqu_ub_gm_binding_snapshot(
                          ubc_dev, request_id, binding_id, ub_gm_addr,
                          length, LINGQU_PTO_UB_GM_WRITE, &binding) ||
        !binding.dirty || !linqu_ub_gm_mapping_still_authorized(
                              ubc_dev, &binding, ub_gm_addr, length,
                              &resolved)) {
        return -LINGQU_PTO_UB_GM_CALLBACK_FAILED;
    }
    linqu_ub_gm_account_access(ubc_dev, request_id, binding_id,
                               0, 0, true);
    qemu_log("QEMU_UB_GM_FENCE request=%" PRIu64 " binding=%" PRIu64
             " addr=0x%" PRIx64 " length=%" PRIu64 "\n",
             request_id, binding_id, ub_gm_addr, length);
    return 0;
}

static bool linqu_ub_gm_register_dispatch(
    BusControllerDev *ubc_dev,
    uint64_t op_id,
    uint64_t request_id,
    const LinquUbGmBindingState *bindings,
    uint32_t binding_count)
{
    struct LinquUbGmRegistry *registry;
    LinquUbGmDispatchState *existing;
    LinquUbGmDispatchState *dispatch;

    if (!ubc_dev || !bindings || binding_count == 0 ||
        !ubc_dev->linqu_uapi_ub_gm_registry) {
        return false;
    }
    registry = ubc_dev->linqu_uapi_ub_gm_registry;
    dispatch = g_new0(LinquUbGmDispatchState, 1);
    dispatch->op_id = op_id;
    dispatch->request_id = request_id;
    dispatch->binding_count = binding_count;
    dispatch->bindings = g_memdup2(
        bindings, sizeof(*bindings) * (size_t)binding_count);

    qemu_mutex_lock(&registry->lock);
    QTAILQ_FOREACH(existing, &registry->dispatches, next) {
        if (existing->op_id == op_id || existing->request_id == request_id) {
            qemu_mutex_unlock(&registry->lock);
            linqu_ub_gm_dispatch_free(dispatch);
            return false;
        }
    }
    QTAILQ_INSERT_TAIL(&registry->dispatches, dispatch, next);
    qemu_mutex_unlock(&registry->lock);
    return true;
}

static void linqu_ub_gm_remove_dispatch(BusControllerDev *ubc_dev,
                                        uint64_t op_id,
                                        const char *reason)
{
    struct LinquUbGmRegistry *registry;
    LinquUbGmDispatchState *dispatch;
    LinquUbGmDispatchState *dispatch_next;

    if (!ubc_dev || !ubc_dev->linqu_uapi_ub_gm_registry) {
        return;
    }
    registry = ubc_dev->linqu_uapi_ub_gm_registry;
    qemu_mutex_lock(&registry->lock);
    QTAILQ_FOREACH_SAFE(dispatch, &registry->dispatches, next,
                        dispatch_next) {
        uint64_t read_bytes = 0;
        uint64_t write_bytes = 0;
        uint64_t fence_count = 0;
        uint32_t index;

        if (dispatch->op_id != op_id) {
            continue;
        }
        for (index = 0; index < dispatch->binding_count; index++) {
            LinquUbGmBindingState *binding = &dispatch->bindings[index];

            binding->active = false;
            read_bytes += binding->read_bytes;
            write_bytes += binding->write_bytes;
            fence_count += binding->fence_count;
        }
        QTAILQ_REMOVE(&registry->dispatches, dispatch, next);
        qemu_mutex_unlock(&registry->lock);
        qemu_log("QEMU_UB_GM_UNBIND op=%" PRIu64 " request=%" PRIu64
                 " reason=%s bindings=%u load_bytes=%" PRIu64
                 " store_bytes=%" PRIu64 " fences=%" PRIu64
                 " segment_payload_staging_bytes=0\n",
                 dispatch->op_id, dispatch->request_id,
                 reason ? reason : "unknown", dispatch->binding_count,
                 read_bytes, write_bytes, fence_count);
        linqu_ub_gm_dispatch_free(dispatch);
        return;
    }
    qemu_mutex_unlock(&registry->lock);
}

static bool linqu_uapi_init_bridge(BusControllerDev *ubc_dev)
{
    const char *scenario_path;
    PtoSimUbGmAccessOpsV1 ub_gm_ops = {
        .abi_version = PTO_SIM_UB_GM_ACCESS_ABI_V1,
        .struct_bytes = sizeof(PtoSimUbGmAccessOpsV1),
        .read = linqu_ub_gm_read,
        .write = linqu_ub_gm_write,
        .fence = linqu_ub_gm_fence,
    };
    uint64_t segment = 0;

    if (!ubc_dev) {
        return false;
    }
    if (ubc_dev->linqu_uapi_bridge_ready) {
        return ubc_dev->linqu_uapi_bridge != NULL;
    }

    ubc_dev->linqu_uapi_bridge_ready = true;
    ubc_dev->linqu_uapi_cmdq_depth = LINQU_UAPI_DEFAULT_CMDQ_DEPTH;
    ubc_dev->linqu_uapi_cq_depth = LINQU_UAPI_DEFAULT_CQ_DEPTH;

    scenario_path = g_getenv("SIM_UAPI_SCENARIO_CONFIG");
    if (!scenario_path || scenario_path[0] == '\0') {
        ubc_dev->linqu_uapi_last_error = 1;
        return false;
    }

    ubc_dev->linqu_uapi_bridge = linqu_ub_bridge_new_from_yaml(scenario_path);
    if (!ubc_dev->linqu_uapi_bridge) {
        ubc_dev->linqu_uapi_last_error = 2;
        return false;
    }
    if (linqu_ub_bridge_register_endpoint(ubc_dev->linqu_uapi_bridge,
                                          LINQU_UAPI_ENDPOINT_ID,
                                          LINQU_UAPI_ENTITY_ID) != 0) {
        ubc_dev->linqu_uapi_last_error = 3;
        linqu_ub_bridge_free(ubc_dev->linqu_uapi_bridge);
        ubc_dev->linqu_uapi_bridge = NULL;
        return false;
    }
    if (ubc_dev->pto_device_cna != 0) {
        ubc_dev->linqu_uapi_ub_gm_registry = linqu_ub_gm_registry_new();
        if (linqu_ub_bridge_register_ub_gm_access_v1(
                ubc_dev->linqu_uapi_bridge, &ub_gm_ops, ubc_dev,
                ubc_dev->pto_device_cna) != 0) {
            ubc_dev->linqu_uapi_last_error = 15;
            linqu_ub_gm_registry_free(
                ubc_dev->linqu_uapi_ub_gm_registry);
            ubc_dev->linqu_uapi_ub_gm_registry = NULL;
            linqu_ub_bridge_free(ubc_dev->linqu_uapi_bridge);
            ubc_dev->linqu_uapi_bridge = NULL;
            return false;
        }
        qemu_log("QEMU_UB_GM_ACCESS_REGISTER pto_device_cna=0x%x\n",
                 ubc_dev->pto_device_cna);
    } else {
        qemu_log("QEMU_UB_GM_ACCESS_DISABLED pto_device_cna=0\n");
    }
    if (linqu_ub_bridge_get_default_segment(ubc_dev->linqu_uapi_bridge,
                                            LINQU_UAPI_ENDPOINT_ID,
                                            &segment) != 0 ||
        segment == 0) {
        ubc_dev->linqu_uapi_last_error = 4;
        linqu_ub_gm_registry_free(ubc_dev->linqu_uapi_ub_gm_registry);
        ubc_dev->linqu_uapi_ub_gm_registry = NULL;
        linqu_ub_bridge_free(ubc_dev->linqu_uapi_bridge);
        ubc_dev->linqu_uapi_bridge = NULL;
        return false;
    }

    ubc_dev->linqu_uapi_default_segment = segment;
    qemu_log("linqu-uapi bridge ready scenario=%s default_segment=%" PRIu64 "\n",
             scenario_path, segment);
    return true;
}

static MemTxResult linqu_uapi_read_slot(uint64_t base, uint32_t slot,
                                        uint8_t *buf)
{
    return dma_memory_read(&address_space_memory,
                           base + ((hwaddr)slot * LINQU_UAPI_DESC_BYTES),
                           buf,
                           LINQU_UAPI_DESC_BYTES,
                           MEMTXATTRS_UNSPECIFIED);
}

static MemTxResult linqu_uapi_write_slot(uint64_t base, uint32_t slot,
                                         const uint8_t *buf)
{
    return dma_memory_write(&address_space_memory,
                            base + ((hwaddr)slot * LINQU_UAPI_DESC_BYTES),
                            buf,
                            LINQU_UAPI_DESC_BYTES,
                            MEMTXATTRS_UNSPECIFIED);
}

static bool linqu_uapi_wire_range_valid(uint64_t iova, uint64_t length)
{
    return iova != 0 && length != 0 && iova <= UINT64_MAX - length;
}

static bool linqu_uapi_wire_is_zero(const uint8_t *bytes, size_t length)
{
    size_t index;

    for (index = 0; index < length; index++) {
        if (bytes[index] != 0) {
            return false;
        }
    }
    return true;
}

static uint32_t linqu_uapi_crc32_ieee_update(uint32_t crc,
                                              const uint8_t *bytes,
                                              size_t length)
{
    size_t index;

    for (index = 0; index < length; index++) {
        uint32_t value = crc ^ bytes[index];
        uint32_t bit;

        for (bit = 0; bit < 8; bit++) {
            value = (value >> 1) ^
                    (0xedb88320u & (0u - (value & 1u)));
        }
        crc = value;
    }
    return crc;
}

static void linqu_uapi_decode_control(
    const uint8_t wire[LINQU_PTO_CONTROL_WIRE_BYTES],
    LingquPtoDispatchControlV2 *control)
{
    *control = (LingquPtoDispatchControlV2) {
        .abi_version = ldl_le_p(wire + 0),
        .struct_bytes = ldl_le_p(wire + 4),
        .request_id = ldq_le_p(wire + 8),
        .callable_id = ldq_le_p(wire + 16),
        .memref_count = ldl_le_p(wire + 24),
        .scalar_count = ldl_le_p(wire + 28),
        .memref_table_iova = ldq_le_p(wire + 32),
        .scalar_table_iova = ldq_le_p(wire + 40),
        .artifact_fingerprint = ldq_le_p(wire + 48),
        .metadata_crc32 = ldl_le_p(wire + 56),
        .requester_cna = ldl_le_p(wire + 60),
    };
}

static void linqu_uapi_decode_memref(
    const uint8_t wire[LINQU_PTO_MEMREF_WIRE_BYTES],
    LingquShmemMemrefV1 *memref)
{
    *memref = (LingquShmemMemrefV1) {
        .abi_version = ldl_le_p(wire + 0),
        .struct_bytes = ldl_le_p(wire + 4),
        .opaque_mapping_ref = ldq_le_p(wire + 8),
        .ub_gm_addr = ldq_le_p(wire + 16),
        .byte_offset = ldq_le_p(wire + 24),
        .byte_length = ldq_le_p(wire + 32),
        .shape_table_iova = ldq_le_p(wire + 40),
        .stride_table_iova = ldq_le_p(wire + 48),
        .arg_index = ldl_le_p(wire + 56),
        .rank = ldl_le_p(wire + 60),
        .dtype = lduw_le_p(wire + 64),
        .role = wire[66],
        .access = wire[67],
        .flags = ldl_le_p(wire + 68),
        .reserved0 = ldl_le_p(wire + 72),
        .reserved1 = ldl_le_p(wire + 76),
    };
}

static void linqu_uapi_decode_scalar(
    const uint8_t wire[LINQU_PTO_SCALAR_WIRE_BYTES],
    LingquPtoScalarV1 *scalar)
{
    *scalar = (LingquPtoScalarV1) {
        .abi_version = ldl_le_p(wire + 0),
        .struct_bytes = ldl_le_p(wire + 4),
        .arg_index = ldl_le_p(wire + 8),
        .dtype = lduw_le_p(wire + 12),
        .flags = lduw_le_p(wire + 14),
        .value = ldq_le_p(wire + 16),
    };
}

static LinquPtoAuthorizationState *linqu_uapi_authorization_snapshot_new(
    BusControllerDev *ubc_dev, uint32_t cmdq_slot, const uint8_t *slot,
    const uint8_t control_wire[LINQU_PTO_CONTROL_WIRE_BYTES],
    const LingquPtoDispatchControlV2 *control, const uint8_t *memref_wire,
    const uint8_t *scalar_wire)
{
    LinquPtoAuthorizationState *authorization;
    uint64_t timeout_ns;

    if (!ubc_dev || !slot || !control_wire || !control || !memref_wire ||
        control->memref_count == 0) {
        return NULL;
    }
    authorization = g_new0(LinquPtoAuthorizationState, 1);
    memcpy(authorization->slot, slot, sizeof(authorization->slot));
    memcpy(authorization->control_wire, control_wire,
           sizeof(authorization->control_wire));
    authorization->memref_wire = g_memdup2(
        memref_wire,
        (size_t)control->memref_count * LINQU_PTO_MEMREF_WIRE_BYTES);
    if (control->scalar_count != 0) {
        authorization->scalar_wire = g_memdup2(
            scalar_wire,
            (size_t)control->scalar_count * LINQU_PTO_SCALAR_WIRE_BYTES);
    }
    authorization->shape_stride_wire = g_malloc0(
        (size_t)control->memref_count *
        LINQU_PTO_SHAPE_STRIDE_SNAPSHOT_BYTES);
    authorization->resolved = g_new0(UbcObmmResolvedMap,
                                      control->memref_count);
    authorization->range_authorized = g_new0(bool,
                                              control->memref_count);
    authorization->op_id = ldq_le_p(
        slot + LINGQU_PTO_DISPATCH_SLOT_OP_ID_OFFSET);
    authorization->request_id = control->request_id;
    authorization->cmdq_slot = cmdq_slot;
    authorization->memref_count = control->memref_count;
    authorization->scalar_count = control->scalar_count;
    authorization->resume_batch = 1;
    authorization->started_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    timeout_ns = ubc_dev->pto_authorization_timeout_ns != 0 ?
                 ubc_dev->pto_authorization_timeout_ns :
                 LINQU_PTO_AUTHORIZATION_TIMEOUT_NS_DEFAULT;
    authorization->deadline_ns = linqu_uapi_ns_add_saturating(
        authorization->started_ns, timeout_ns);
    return authorization;
}

static uint8_t *linqu_uapi_authorization_shape_wire(
    LinquPtoAuthorizationState *authorization, uint32_t index)
{
    return authorization->shape_stride_wire +
           (size_t)index * LINQU_PTO_SHAPE_STRIDE_SNAPSHOT_BYTES;
}

static uint8_t *linqu_uapi_authorization_stride_wire(
    LinquPtoAuthorizationState *authorization, uint32_t index)
{
    return linqu_uapi_authorization_shape_wire(authorization, index) +
           LINGQU_PTO_MAX_RANK * sizeof(uint32_t);
}

static bool linqu_uapi_authorization_start_range(
    BusControllerDev *ubc_dev, LinquPtoAuthorizationState *authorization,
    uint32_t index)
{
    uint64_t due_ns;
    uint64_t now_ns;

    if (!ubc_dev || !authorization ||
        !ubc_dev->linqu_uapi_authorization_timer ||
        index >= authorization->memref_count ||
        authorization->waiting || authorization->completion_ready) {
        return false;
    }
    now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    authorization->ready_ns = linqu_uapi_ns_add_saturating(
        now_ns, ubc_dev->pto_authorization_delay_ns);
    authorization->sequence =
        ++ubc_dev->linqu_uapi_next_authorization_sequence;
    if (authorization->sequence == 0) {
        authorization->sequence =
            ++ubc_dev->linqu_uapi_next_authorization_sequence;
    }
    authorization->waiting = true;
    authorization->completion_status = LINGQU_PTO_UB_GM_OK;
    due_ns = MIN(authorization->ready_ns, authorization->deadline_ns);
    qemu_log("QEMU_UB_GM_AUTHORIZATION_PENDING op=%" PRIu64
             " request=%" PRIu64 " slot=%u cursor=%u sequence=%" PRIu64
             " delay_ns=%" PRIu64 " deadline_ns=%" PRIu64 "\n",
             authorization->op_id, authorization->request_id,
             authorization->cmdq_slot, index, authorization->sequence,
             ubc_dev->pto_authorization_delay_ns,
             authorization->deadline_ns);
    timer_mod_ns(ubc_dev->linqu_uapi_authorization_timer,
                 MIN(due_ns, (uint64_t)INT64_MAX));
    return true;
}

static uint64_t linqu_uapi_dtype_bytes(uint16_t dtype)
{
    switch (dtype) {
    case 0:
    case 2:
    case 10:
        return 4;
    case 1:
    case 3:
    case 6:
    case 9:
        return 2;
    case 4:
    case 5:
    case 11:
    case 12:
    case 13:
    case 14:
        return 1;
    case 7:
    case 8:
        return 8;
    default:
        return 0;
    }
}

static bool linqu_uapi_validate_contiguous_memref(
    const LingquShmemMemrefV1 *memref,
    const uint32_t shape[LINGQU_PTO_MAX_RANK],
    const uint32_t strides[LINGQU_PTO_MAX_RANK])
{
    uint64_t elements = 1;
    uint64_t expected_stride = 1;
    uint64_t element_bytes;
    uint32_t index;

    if (!memref || memref->rank == 0 ||
        memref->rank > LINGQU_PTO_MAX_RANK) {
        return false;
    }
    element_bytes = linqu_uapi_dtype_bytes(memref->dtype);
    if (element_bytes == 0) {
        return false;
    }
    for (index = memref->rank; index > 0; index--) {
        uint32_t dim = shape[index - 1];

        if (dim == 0 || strides[index - 1] != expected_stride ||
            elements > UINT64_MAX / dim) {
            return false;
        }
        elements *= dim;
        expected_stride = elements;
    }
    if (elements > UINT64_MAX / element_bytes ||
        elements * element_bytes != memref->byte_length) {
        return false;
    }
    return true;
}

static bool linqu_uapi_role_access_valid(const LingquShmemMemrefV1 *memref)
{
    return (memref->role == LINGQU_PTO_MEMREF_INPUT &&
            memref->access == LINGQU_PTO_UB_GM_READ) ||
           (memref->role == LINGQU_PTO_MEMREF_OUTPUT &&
            memref->access == LINGQU_PTO_UB_GM_WRITE) ||
           (memref->role == LINGQU_PTO_MEMREF_INOUT &&
            memref->access == LINGQU_PTO_UB_GM_READ_WRITE);
}

static bool linqu_uapi_aperture_advance(uint64_t current,
                                        uint64_t length,
                                        uint64_t *next)
{
    uint64_t rounded;
    uint64_t limit = LINQU_PTO_APERTURE_BASE + LINQU_PTO_APERTURE_LENGTH;

    if (length == 0 || length > UINT64_MAX - (LINQU_PTO_APERTURE_ALIGN - 1)) {
        return false;
    }
    rounded = (length + LINQU_PTO_APERTURE_ALIGN - 1) &
              ~(LINQU_PTO_APERTURE_ALIGN - 1);
    if (current < LINQU_PTO_APERTURE_BASE || current > limit ||
        rounded > limit - current ||
        LINQU_PTO_APERTURE_ALIGN > limit - current - rounded) {
        return false;
    }
    *next = current + rounded + LINQU_PTO_APERTURE_ALIGN;
    return true;
}

static int linqu_uapi_submit_ub_gm_v2(BusControllerDev *ubc_dev,
                                      const uint8_t *slot,
                                      uint32_t cmdq_slot,
                                      uint64_t *op_id_out)
{
    uint8_t control_wire[LINQU_PTO_CONTROL_WIRE_BYTES];
    uint8_t control_crc_wire[LINQU_PTO_CONTROL_WIRE_BYTES];
    LingquPtoDispatchControlV2 control;
    PtoSimUbGmAuthorizedMemrefV1 *authorized = NULL;
    LingquPtoScalarV1 *scalars = NULL;
    LinquUbGmBindingState *bindings = NULL;
    LinquPtoAuthorizationState *authorization = NULL;
    uint8_t *memref_wire = NULL;
    uint8_t *scalar_wire = NULL;
    bool arg_seen[LINGQU_PTO_MAX_MEMREFS + LINGQU_PTO_MAX_SCALARS] = { 0 };
    uint64_t op_id;
    uint64_t control_iova;
    uint64_t expected_fingerprint = 0;
    uint64_t aperture = LINQU_PTO_APERTURE_BASE;
    uint32_t crc = 0xffffffffu;
    uint32_t total_args;
    uint32_t index;
    bool new_authorization = false;
    int error = LINGQU_PTO_UB_GM_BAD_CONTROL_TABLE;
    int rc;

    if (!ubc_dev || !slot || !op_id_out ||
        !ubc_dev->linqu_uapi_ub_gm_registry ||
        slot[0] != LINGQU_PTO_DISPATCH_SLOT_TAG_V2 ||
        !linqu_uapi_wire_is_zero(
            slot + LINGQU_PTO_DISPATCH_SLOT_RESERVED_OFFSET,
            LINQU_UAPI_DESC_BYTES - LINGQU_PTO_DISPATCH_SLOT_RESERVED_OFFSET)) {
        return LINGQU_PTO_UB_GM_BAD_CONTROL_TABLE;
    }
    authorization = ubc_dev->linqu_uapi_authorization;
    if (authorization &&
        (authorization->cmdq_slot != cmdq_slot ||
         memcmp(authorization->slot, slot, LINQU_UAPI_DESC_BYTES) != 0)) {
        linqu_uapi_authorization_discard(ubc_dev);
        return LINGQU_PTO_UB_GM_UNBOUND;
    }
    op_id = ldq_le_p(slot + LINGQU_PTO_DISPATCH_SLOT_OP_ID_OFFSET);
    control_iova = ldq_le_p(
        slot + LINGQU_PTO_DISPATCH_SLOT_CONTROL_IOVA_OFFSET);
    *op_id_out = op_id;
    if (op_id == 0 ||
        !linqu_uapi_wire_range_valid(control_iova, sizeof(control_wire))) {
        error = LINGQU_PTO_UB_GM_BAD_CONTROL_TABLE;
        goto out;
    }
    if (authorization) {
        memcpy(control_wire, authorization->control_wire,
               sizeof(control_wire));
    } else if (dma_memory_read(&address_space_memory, control_iova,
                               control_wire, sizeof(control_wire),
                               MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        error = LINGQU_PTO_UB_GM_BAD_CONTROL_TABLE;
        goto out;
    }
    linqu_uapi_decode_control(control_wire, &control);
    if (control.abi_version != LINGQU_PTO_DISPATCH_ABI_V2 ||
        control.struct_bytes < sizeof(control) || control.request_id == 0 ||
        control.callable_id == 0 || control.artifact_fingerprint == 0 ||
        control.requester_cna == 0 ||
        control.requester_cna > LINGQU_PTO_CNA_MAX ||
        control.requester_cna != ubc_dev->pto_device_cna ||
        control.memref_count == 0 ||
        control.memref_count > LINGQU_PTO_MAX_MEMREFS ||
        control.scalar_count > LINGQU_PTO_MAX_SCALARS ||
        (control.memref_count != 0 &&
         !linqu_uapi_wire_range_valid(
             control.memref_table_iova,
             (uint64_t)control.memref_count * LINQU_PTO_MEMREF_WIRE_BYTES)) ||
        (control.scalar_count != 0 &&
         !linqu_uapi_wire_range_valid(
             control.scalar_table_iova,
             (uint64_t)control.scalar_count * LINQU_PTO_SCALAR_WIRE_BYTES))) {
        error = control.requester_cna != ubc_dev->pto_device_cna ?
                LINGQU_PTO_UB_GM_ACCESS_DENIED :
                LINGQU_PTO_UB_GM_BAD_CONTROL_TABLE;
        goto out;
    }
    if (linqu_ub_bridge_query_ub_gm_callable_v1(
            ubc_dev->linqu_uapi_bridge, control.callable_id,
            &expected_fingerprint) != 0 ||
        expected_fingerprint == 0 ||
        expected_fingerprint != control.artifact_fingerprint) {
        error = LINGQU_PTO_UB_GM_UNSUPPORTED_CALLABLE;
        goto out;
    }

    total_args = control.memref_count + control.scalar_count;
    authorized = g_new0(PtoSimUbGmAuthorizedMemrefV1,
                        control.memref_count);
    bindings = g_new0(LinquUbGmBindingState, control.memref_count);
    if (authorization) {
        memref_wire = g_memdup2(
            authorization->memref_wire,
            (size_t)control.memref_count * LINQU_PTO_MEMREF_WIRE_BYTES);
    } else {
        memref_wire = g_malloc((size_t)control.memref_count *
                               LINQU_PTO_MEMREF_WIRE_BYTES);
        if (dma_memory_read(
                &address_space_memory, control.memref_table_iova, memref_wire,
                (size_t)control.memref_count * LINQU_PTO_MEMREF_WIRE_BYTES,
                MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            goto out;
        }
    }
    if (control.scalar_count != 0) {
        scalars = g_new0(LingquPtoScalarV1, control.scalar_count);
        if (authorization) {
            scalar_wire = g_memdup2(
                authorization->scalar_wire,
                (size_t)control.scalar_count * LINQU_PTO_SCALAR_WIRE_BYTES);
        } else {
            scalar_wire = g_malloc((size_t)control.scalar_count *
                                   LINQU_PTO_SCALAR_WIRE_BYTES);
            if (dma_memory_read(
                    &address_space_memory, control.scalar_table_iova,
                    scalar_wire,
                    (size_t)control.scalar_count *
                    LINQU_PTO_SCALAR_WIRE_BYTES,
                    MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
                goto out;
            }
        }
    }
    if (!authorization && ubc_dev->pto_authorization_delay_ns != 0) {
        authorization = linqu_uapi_authorization_snapshot_new(
            ubc_dev, cmdq_slot, slot, control_wire, &control,
            memref_wire, scalar_wire);
        if (!authorization) {
            goto out;
        }
        new_authorization = true;
    }

    memcpy(control_crc_wire, control_wire, sizeof(control_crc_wire));
    memset(control_crc_wire + 56, 0, sizeof(uint32_t));
    crc = linqu_uapi_crc32_ieee_update(
        crc, control_crc_wire, sizeof(control_crc_wire));
    crc = linqu_uapi_crc32_ieee_update(
        crc, memref_wire,
        (size_t)control.memref_count * LINQU_PTO_MEMREF_WIRE_BYTES);
    if (control.scalar_count != 0) {
        crc = linqu_uapi_crc32_ieee_update(
            crc, scalar_wire,
            (size_t)control.scalar_count * LINQU_PTO_SCALAR_WIRE_BYTES);
    }

    for (index = 0; index < control.memref_count; index++) {
        PtoSimUbGmAuthorizedMemrefV1 *item = &authorized[index];
        LingquShmemMemrefV1 *memref = &item->memref;
        const uint8_t *entry_wire =
            memref_wire + ((size_t)index * LINQU_PTO_MEMREF_WIRE_BYTES);
        uint8_t shape_wire[LINGQU_PTO_MAX_RANK * sizeof(uint32_t)] = { 0 };
        uint8_t stride_wire[LINGQU_PTO_MAX_RANK * sizeof(uint32_t)] = { 0 };
        uint64_t shape_bytes;
        uint64_t view_start;
        uint32_t dim;

        linqu_uapi_decode_memref(entry_wire, memref);
        if (memref->abi_version != LINGQU_SHMEM_MEMREF_ABI_V1 ||
            memref->struct_bytes < sizeof(*memref) ||
            memref->opaque_mapping_ref == 0 || memref->ub_gm_addr == 0 ||
            memref->byte_length == 0 ||
            memref->arg_index >= control.memref_count ||
            arg_seen[memref->arg_index] || memref->rank == 0 ||
            memref->rank > LINGQU_PTO_MAX_RANK ||
            memref->dtype > LINGQU_PTO_DTYPE_MAX ||
            memref->flags != 0 || memref->reserved0 != 0 ||
            memref->reserved1 != 0 || !linqu_uapi_role_access_valid(memref) ||
            memref->byte_offset > UINT64_MAX - memref->byte_length ||
            memref->ub_gm_addr > UINT64_MAX - memref->byte_offset) {
            error = LINGQU_PTO_UB_GM_BAD_MEMREF;
            goto out;
        }
        view_start = memref->ub_gm_addr + memref->byte_offset;
        if (view_start > UINT64_MAX - memref->byte_length) {
            error = LINGQU_PTO_UB_GM_BAD_MEMREF;
            goto out;
        }
        arg_seen[memref->arg_index] = true;
        shape_bytes = (uint64_t)memref->rank * sizeof(uint32_t);
        if (!linqu_uapi_wire_range_valid(memref->shape_table_iova,
                                         shape_bytes) ||
            !linqu_uapi_wire_range_valid(memref->stride_table_iova,
                                         shape_bytes)) {
            error = LINGQU_PTO_UB_GM_BAD_MEMREF;
            goto out;
        }
        if (authorization && !new_authorization) {
            memcpy(shape_wire,
                   linqu_uapi_authorization_shape_wire(
                       authorization, index),
                   shape_bytes);
            memcpy(stride_wire,
                   linqu_uapi_authorization_stride_wire(
                       authorization, index),
                   shape_bytes);
        } else if (dma_memory_read(
                       &address_space_memory, memref->shape_table_iova,
                       shape_wire, shape_bytes,
                       MEMTXATTRS_UNSPECIFIED) != MEMTX_OK ||
                   dma_memory_read(
                       &address_space_memory, memref->stride_table_iova,
                       stride_wire, shape_bytes,
                       MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            error = LINGQU_PTO_UB_GM_BAD_MEMREF;
            goto out;
        }
        if (new_authorization) {
            memcpy(linqu_uapi_authorization_shape_wire(
                       authorization, index),
                   shape_wire, shape_bytes);
            memcpy(linqu_uapi_authorization_stride_wire(
                       authorization, index),
                   stride_wire, shape_bytes);
        }
        crc = linqu_uapi_crc32_ieee_update(crc, shape_wire, shape_bytes);
        crc = linqu_uapi_crc32_ieee_update(crc, stride_wire, shape_bytes);
        for (dim = 0; dim < memref->rank; dim++) {
            item->shape[dim] = ldl_le_p(shape_wire + dim * sizeof(uint32_t));
            item->strides[dim] =
                ldl_le_p(stride_wire + dim * sizeof(uint32_t));
        }
        if (!linqu_uapi_validate_contiguous_memref(
                memref, item->shape, item->strides)) {
            error = LINGQU_PTO_UB_GM_BAD_MEMREF;
            goto out;
        }
    }

    for (index = 0; index < control.scalar_count; index++) {
        const uint8_t *entry_wire =
            scalar_wire + ((size_t)index * LINQU_PTO_SCALAR_WIRE_BYTES);
        LingquPtoScalarV1 *scalar = &scalars[index];

        linqu_uapi_decode_scalar(entry_wire, scalar);
        if (scalar->abi_version != LINGQU_PTO_SCALAR_ABI_V1 ||
            scalar->struct_bytes < sizeof(*scalar) ||
            scalar->arg_index < control.memref_count ||
            scalar->arg_index >= total_args || arg_seen[scalar->arg_index] ||
            scalar->dtype > LINGQU_PTO_DTYPE_MAX || scalar->flags != 0) {
            goto out;
        }
        arg_seen[scalar->arg_index] = true;
    }
    for (index = 0; index < total_args; index++) {
        if (!arg_seen[index]) {
            goto out;
        }
    }
    if ((crc ^ 0xffffffffu) != control.metadata_crc32) {
        qemu_log("QEMU_UB_GM_METADATA_CRC_FAIL op=%" PRIu64
                 " request=%" PRIu64 " expected=0x%x actual=0x%x\n",
                 op_id, control.request_id, control.metadata_crc32,
                 crc ^ 0xffffffffu);
        goto out;
    }
    if (new_authorization) {
        g_assert(!ubc_dev->linqu_uapi_authorization);
        ubc_dev->linqu_uapi_authorization = authorization;
    }

    for (index = 0; index < control.memref_count; index++) {
        PtoSimUbGmAuthorizedMemrefV1 *item = &authorized[index];
        LingquShmemMemrefV1 *memref = &item->memref;
        LinquUbGmBindingState *binding = &bindings[index];
        UbcObmmResolvedMap current;
        UbcObmmResolvedMap resolved;
        uint64_t view_start = memref->ub_gm_addr + memref->byte_offset;
        uint64_t next_aperture;

        if (authorization && !authorization->range_authorized[index]) {
            if (index != authorization->memref_cursor) {
                error = LINGQU_PTO_UB_GM_UNBOUND;
                goto out;
            }
            if (!authorization->completion_ready) {
                if (!authorization->waiting &&
                    !linqu_uapi_authorization_start_range(
                        ubc_dev, authorization, index)) {
                    error = LINGQU_PTO_UB_GM_EXECUTION_FAILED;
                    goto out;
                }
                error = LINQU_PTO_UB_GM_INTERNAL_PENDING;
                goto out;
            }
            authorization->completion_ready = false;
            if (authorization->completion_status != LINGQU_PTO_UB_GM_OK) {
                error = authorization->completion_status;
                qemu_log("QEMU_UB_GM_AUTHORIZATION_TIMEOUT op=%" PRIu64
                         " request=%" PRIu64 " slot=%u cursor=%u"
                         " sequence=%" PRIu64 "\n",
                         authorization->op_id, authorization->request_id,
                         authorization->cmdq_slot, index,
                         authorization->sequence);
                goto out;
            }
            if (!ub_obmm_async_resolve_mapping_ref(
                    ubc_dev->obmm_async, memref->opaque_mapping_ref,
                    view_start, memref->byte_length, &resolved)) {
                error = LINGQU_PTO_UB_GM_BAD_MEMREF;
                goto out;
            }
            authorization->resolved[index] = resolved;
            authorization->range_authorized[index] = true;
            authorization->memref_cursor++;
        } else if (authorization) {
            resolved = authorization->resolved[index];
        } else if (!ub_obmm_async_resolve_mapping_ref(
                       ubc_dev->obmm_async, memref->opaque_mapping_ref,
                       view_start, memref->byte_length, &resolved)) {
            error = LINGQU_PTO_UB_GM_BAD_MEMREF;
            goto out;
        }
        if (authorization &&
            (!ub_obmm_async_resolve_mapping_ref(
                 ubc_dev->obmm_async, memref->opaque_mapping_ref,
                 view_start, memref->byte_length, &current) ||
             current.map_id != resolved.map_id ||
             current.map_generation != resolved.map_generation ||
             current.local_pa != resolved.local_pa ||
             current.remote_uba != resolved.remote_uba ||
             current.token_id != resolved.token_id ||
             current.peer_cna != resolved.peer_cna ||
             current.access_flags != resolved.access_flags)) {
            error = LINGQU_PTO_UB_GM_UNBOUND;
            goto out;
        }
        if ((memref->access & LINGQU_PTO_UB_GM_WRITE) &&
            (resolved.access_flags & SIM_DEC_GVA_ACCESS_READ_ONLY)) {
            error = LINGQU_PTO_UB_GM_ACCESS_DENIED;
            goto out;
        }
        if (!linqu_uapi_aperture_advance(
                aperture, memref->byte_length, &next_aperture)) {
            error = LINGQU_PTO_UB_GM_BAD_MEMREF;
            goto out;
        }

        item->binding = (PtoSimUbGmBindingV1) {
            .request_id = control.request_id,
            .binding_id = (uint64_t)memref->arg_index + 1,
            .aperture_base = aperture,
            .aperture_length = memref->byte_length,
            .ub_gm_base = view_start,
            .mapped_length = memref->byte_length,
            .access = memref->access,
            .flags = 0,
            .backend_cookie = memref->opaque_mapping_ref,
        };
        *binding = (LinquUbGmBindingState) {
            .request_id = control.request_id,
            .binding_id = item->binding.binding_id,
            .mapping_ref = memref->opaque_mapping_ref,
            .local_base = view_start,
            .length = memref->byte_length,
            .map_id = resolved.map_id,
            .map_generation = resolved.map_generation,
            .remote_base = resolved.remote_uba,
            .token_id = resolved.token_id,
            .peer_cna = resolved.peer_cna,
            .access = memref->access,
            .active = true,
        };
        aperture = next_aperture;
    }
    for (index = 0; index < control.memref_count; index++) {
        const LingquShmemMemrefV1 *memref = &authorized[index].memref;
        const LinquUbGmBindingState *binding = &bindings[index];

        qemu_log("QEMU_UB_GM_%s_AUTHORIZE op=%" PRIu64
                 " request=%" PRIu64 " binding=%" PRIu64
                 " requester_cna=0x%x local=0x%" PRIx64
                 " length=%" PRIu64 " map=%" PRIu64
                 " generation=%" PRIu64 " peer_cna=0x%x\n",
                 memref->role == LINGQU_PTO_MEMREF_INPUT ? "INPUT" :
                 memref->role == LINGQU_PTO_MEMREF_OUTPUT ? "OUTPUT" :
                 "INOUT", op_id, control.request_id, binding->binding_id,
                 control.requester_cna, binding->local_base,
                 memref->byte_length, binding->map_id,
                 binding->map_generation, binding->peer_cna);
    }
    if (!linqu_ub_gm_register_dispatch(
            ubc_dev, op_id, control.request_id, bindings,
            control.memref_count)) {
        error = LINGQU_PTO_UB_GM_UNBOUND;
        goto out;
    }
    rc = linqu_ub_bridge_submit_ub_gm_v2(
        ubc_dev->linqu_uapi_bridge, LINQU_UAPI_ENDPOINT_ID, op_id,
        &control, authorized, control.memref_count, scalars,
        control.scalar_count);
    if (rc != 0) {
        linqu_ub_gm_remove_dispatch(ubc_dev, op_id, "bridge_submit_failed");
        error = rc < 0 && -rc <= LINGQU_PTO_UB_GM_EXECUTION_FAILED ?
                -rc : LINGQU_PTO_UB_GM_EXECUTION_FAILED;
        goto out;
    }
    qemu_log("SIM_QEMU_UB_GM_BIND_REGISTER op=%" PRIu64
             " request=%" PRIu64 " bindings=%u requester_cna=0x%x\n",
             op_id, control.request_id, control.memref_count,
             control.requester_cna);
    error = LINGQU_PTO_UB_GM_OK;

out:
    g_free(scalar_wire);
    g_free(memref_wire);
    g_free(bindings);
    g_free(scalars);
    g_free(authorized);
    if (error != LINQU_PTO_UB_GM_INTERNAL_PENDING && authorization) {
        if (ubc_dev->linqu_uapi_authorization == authorization) {
            linqu_uapi_authorization_discard(ubc_dev);
        } else if (new_authorization) {
            linqu_uapi_authorization_free(authorization);
        }
    }
    return error;
}

static const char *linqu_uapi_ub_gm_error_code(int error)
{
    switch (error) {
    case LINGQU_PTO_UB_GM_UNSUPPORTED_CALLABLE:
        return "pto_ub_gm_unsupported_callable";
    case LINGQU_PTO_UB_GM_BAD_CONTROL_TABLE:
        return "pto_ub_gm_bad_control_table";
    case LINGQU_PTO_UB_GM_BAD_MEMREF:
        return "pto_ub_gm_bad_memref";
    case LINGQU_PTO_UB_GM_UNBOUND:
        return "pto_ub_gm_unbound";
    case LINGQU_PTO_UB_GM_ACCESS_DENIED:
        return "pto_ub_gm_access_denied";
    case LINGQU_PTO_UB_GM_AUTHORIZATION_TIMEOUT:
        return "pto_ub_gm_authorization_timeout";
    case LINGQU_PTO_UB_GM_CALLBACK_FAILED:
        return "pto_ub_gm_callback_failed";
    case LINGQU_PTO_UB_GM_AUTHORIZATION_CANCELLED:
        return "pto_ub_gm_authorization_cancelled";
    case LINGQU_PTO_UB_GM_EXECUTION_FAILED:
    default:
        return "pto_ub_gm_execution_failed";
    }
}

static bool linqu_uapi_publish_ub_gm_failure(BusControllerDev *ubc_dev,
                                             uint64_t op_id,
                                             int error)
{
    uint8_t slot[LINQU_UAPI_DESC_BYTES] = { 0 };
    const char *code = linqu_uapi_ub_gm_error_code(error);
    size_t code_len = strlen(code);
    size_t time_offset;
    uint32_t cq_slot;

    if (!ubc_dev || ubc_dev->linqu_uapi_cq_depth == 0 ||
        ((ubc_dev->linqu_uapi_cq_tail + 1) %
         ubc_dev->linqu_uapi_cq_depth) == ubc_dev->linqu_uapi_cq_head ||
        code_len > UINT8_MAX ||
        12 + code_len + sizeof(uint64_t) > sizeof(slot)) {
        if (ubc_dev) {
            ubc_dev->linqu_uapi_irq_status |= LINQU_UAPI_IRQ_CQ_OVERFLOW;
        }
        return false;
    }
    stq_le_p(slot, op_id);
    slot[8] = 0;
    slot[9] = 1;
    slot[10] = 3;
    slot[11] = code_len;
    memcpy(slot + 12, code, code_len);
    time_offset = 12 + code_len;
    stq_le_p(slot + time_offset,
             qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    cq_slot = ubc_dev->linqu_uapi_cq_tail;
    if (linqu_uapi_write_slot(ubc_dev->linqu_uapi_cq_iova, cq_slot,
                              slot) != MEMTX_OK) {
        return false;
    }
    ubc_dev->linqu_uapi_cq_tail =
        (ubc_dev->linqu_uapi_cq_tail + 1) % ubc_dev->linqu_uapi_cq_depth;
    ubc_dev->linqu_uapi_last_error = error;
    ubc_dev->linqu_uapi_irq_status |=
        LINQU_UAPI_IRQ_COMPLETION | LINQU_UAPI_IRQ_ERROR;
    qemu_log("QEMU_UB_GM_FAILURE_COMPLETION op=%" PRIu64
             " cq_slot=%u cq_tail=%u status=3 code=%s\n",
             op_id, cq_slot, ubc_dev->linqu_uapi_cq_tail, code);
    qemu_log("QEMU_UB_GM_DISPATCH_REJECT op=%" PRIu64
             " error=%d code=%s\n", op_id, error, code);
    return true;
}

static bool linqu_uapi_cancel_authorization(BusControllerDev *ubc_dev,
                                             uint64_t op_id)
{
    LinquPtoAuthorizationState *authorization;
    uint64_t request_id;
    uint64_t sequence;
    uint32_t cmdq_slot;
    uint32_t resume_batch;

    if (!ubc_dev || !ubc_dev->linqu_uapi_authorization) {
        qemu_log("QEMU_UB_GM_AUTHORIZATION_CANCEL_IGNORED op=%" PRIu64
                 " reason=no_pending\n", op_id);
        return false;
    }
    authorization = ubc_dev->linqu_uapi_authorization;
    if (op_id == 0 || authorization->op_id != op_id) {
        qemu_log("QEMU_UB_GM_AUTHORIZATION_CANCEL_IGNORED op=%" PRIu64
                 " active_op=%" PRIu64 " reason=op_mismatch\n",
                 op_id, authorization->op_id);
        return false;
    }
    if (ubc_dev->linqu_uapi_cmdq_depth == 0 ||
        ubc_dev->linqu_uapi_cmdq_head != authorization->cmdq_slot) {
        qemu_log("QEMU_UB_GM_AUTHORIZATION_CANCEL_IGNORED op=%" PRIu64
                 " slot=%u head=%u reason=queue_identity_mismatch\n",
                 op_id, authorization->cmdq_slot,
                 ubc_dev->linqu_uapi_cmdq_head);
        return false;
    }
    if (!linqu_uapi_publish_ub_gm_failure(
            ubc_dev, op_id,
            LINGQU_PTO_UB_GM_AUTHORIZATION_CANCELLED)) {
        qemu_log("QEMU_UB_GM_AUTHORIZATION_CANCEL_IGNORED op=%" PRIu64
                 " reason=cq_unavailable\n", op_id);
        return false;
    }

    request_id = authorization->request_id;
    sequence = authorization->sequence;
    cmdq_slot = authorization->cmdq_slot;
    resume_batch = MAX(authorization->resume_batch, 1u);
    ubc_dev->linqu_uapi_cmdq_head =
        (cmdq_slot + 1) % ubc_dev->linqu_uapi_cmdq_depth;
    qemu_log("QEMU_UB_GM_AUTHORIZATION_CANCEL op=%" PRIu64
             " request=%" PRIu64 " slot=%u sequence=%" PRIu64
             " cmdq_head=%u cq_tail=%u\n",
             op_id, request_id, cmdq_slot, sequence,
             ubc_dev->linqu_uapi_cmdq_head,
             ubc_dev->linqu_uapi_cq_tail);
    linqu_uapi_authorization_discard(ubc_dev);

    if (ubc_dev->pto_authorization_inject_late_completion) {
        linqu_uapi_authorization_complete(
            ubc_dev, op_id, request_id, sequence,
            LINGQU_PTO_UB_GM_OK, "cancel-late-injection");
    }
    if (ubc_dev->linqu_uapi_cmdq_head !=
        ubc_dev->linqu_uapi_cmdq_tail) {
        linqu_uapi_schedule_kick(ubc_dev, resume_batch);
    }
    return true;
}

static int linqu_uapi_poll_completion(BusControllerDev *ubc_dev,
                                      uint8_t *slot)
{
    int rc;

    /*
     * The bridge waits synchronously for a Simpler PTO worker.  That worker
     * calls back into this QEMU device to perform UB_GM accesses, so it must
     * be able to acquire the BQL while the main loop waits for completion.
     */
    g_assert(qemu_mutex_iothread_locked());
    qemu_mutex_unlock_iothread();
    rc = linqu_ub_bridge_poll_completion(ubc_dev->linqu_uapi_bridge,
                                         LINQU_UAPI_ENDPOINT_ID,
                                         slot,
                                         LINQU_UAPI_DESC_BYTES);
    qemu_mutex_lock_iothread();
    return rc;
}

static void linqu_uapi_flush_cq(BusControllerDev *ubc_dev)
{
    uint8_t slot[LINQU_UAPI_DESC_BYTES];
    int rc;

    qemu_log("linqu-uapi flush_cq begin head=%u tail=%u depth=%u\n",
             ubc_dev->linqu_uapi_cq_head,
             ubc_dev->linqu_uapi_cq_tail,
             ubc_dev->linqu_uapi_cq_depth);
    while (((ubc_dev->linqu_uapi_cq_tail + 1) % ubc_dev->linqu_uapi_cq_depth) !=
           ubc_dev->linqu_uapi_cq_head) {
        rc = linqu_uapi_poll_completion(ubc_dev, slot);
        if (rc == 1) {
            qemu_log("linqu-uapi flush_cq empty head=%u tail=%u\n",
                     ubc_dev->linqu_uapi_cq_head,
                     ubc_dev->linqu_uapi_cq_tail);
            break;
        }
        if (rc < 0) {
            ubc_dev->linqu_uapi_last_error = 5;
            ubc_dev->linqu_uapi_irq_status |= LINQU_UAPI_IRQ_ERROR;
            qemu_log("linqu-uapi flush_cq poll failed last_error=%" PRIu64 "\n",
                     ubc_dev->linqu_uapi_last_error);
            return;
        }
        if (linqu_uapi_write_slot(ubc_dev->linqu_uapi_cq_iova,
                                  ubc_dev->linqu_uapi_cq_tail,
                                  slot) != MEMTX_OK) {
            ubc_dev->linqu_uapi_last_error = 6;
            ubc_dev->linqu_uapi_irq_status |= LINQU_UAPI_IRQ_ERROR;
            qemu_log("linqu-uapi flush_cq write failed cq_iova=%#" PRIx64 " tail=%u\n",
                     ubc_dev->linqu_uapi_cq_iova,
                     ubc_dev->linqu_uapi_cq_tail);
            return;
        }
        qemu_log("linqu-uapi flush_cq wrote completion tail=%u op_id=%" PRIu64 " source=%u status=%u code_len=%u code=%.*s\n",
                 ubc_dev->linqu_uapi_cq_tail,
                 ldq_le_p(slot),
                 slot[9],
                 slot[10],
                 slot[11],
                 slot[11],
                 (const char *)(slot + 12));
        linqu_ub_gm_remove_dispatch(
            ubc_dev, ldq_le_p(slot),
            slot[10] == 1 ? "completion_success" : "completion_failure");
        ubc_dev->linqu_uapi_cq_tail =
            (ubc_dev->linqu_uapi_cq_tail + 1) % ubc_dev->linqu_uapi_cq_depth;
        ubc_dev->linqu_uapi_irq_status |= LINQU_UAPI_IRQ_COMPLETION;
    }
}

static void linqu_uapi_kick(BusControllerDev *ubc_dev, uint32_t batch)
{
    uint8_t slot[LINQU_UAPI_DESC_BYTES];
    uint64_t ub_gm_ops[LINQU_UAPI_DEFAULT_CMDQ_DEPTH] = { 0 };
    uint32_t ub_gm_op_count = 0;
    uint32_t consumed = 0;
    uint32_t bridge_queued = 0;
    uint32_t ring_submitted = 0;
    uint32_t pending = 0;
    uint32_t head;
    int rc;

    if (!linqu_uapi_init_bridge(ubc_dev)) {
        return;
    }
    if (ubc_dev->linqu_uapi_cmdq_depth == 0 || ubc_dev->linqu_uapi_cq_depth == 0) {
        ubc_dev->linqu_uapi_last_error = 7;
        qemu_log("linqu-uapi kick invalid queue depth cmdq=%u cq=%u\n",
                 ubc_dev->linqu_uapi_cmdq_depth,
                 ubc_dev->linqu_uapi_cq_depth);
        return;
    }

    qemu_log("linqu-uapi kick begin batch=%u cmdq_base=%#" PRIx64 " cmdq_head=%u cmdq_tail=%u cmdq_depth=%u cq_base=%#" PRIx64 " cq_head=%u cq_tail=%u cq_depth=%u\n",
             batch,
             ubc_dev->linqu_uapi_cmdq_iova,
             ubc_dev->linqu_uapi_cmdq_head,
             ubc_dev->linqu_uapi_cmdq_tail,
             ubc_dev->linqu_uapi_cmdq_depth,
             ubc_dev->linqu_uapi_cq_iova,
             ubc_dev->linqu_uapi_cq_head,
             ubc_dev->linqu_uapi_cq_tail,
             ubc_dev->linqu_uapi_cq_depth);
    head = ubc_dev->linqu_uapi_cmdq_head;
    while (head != ubc_dev->linqu_uapi_cmdq_tail && consumed < batch) {
        LinquPtoAuthorizationState *authorization =
            ubc_dev->linqu_uapi_authorization;
        uint64_t op_id;

        if (authorization) {
            if (authorization->cmdq_slot != head) {
                linqu_uapi_authorization_discard(ubc_dev);
                ubc_dev->linqu_uapi_last_error =
                    LINGQU_PTO_UB_GM_UNBOUND;
                ubc_dev->linqu_uapi_irq_status |= LINQU_UAPI_IRQ_ERROR;
                return;
            }
            memcpy(slot, authorization->slot, sizeof(slot));
        } else if (linqu_uapi_read_slot(
                       ubc_dev->linqu_uapi_cmdq_iova, head,
                       slot) != MEMTX_OK) {
            ubc_dev->linqu_uapi_last_error = 8;
            ubc_dev->linqu_uapi_irq_status |= LINQU_UAPI_IRQ_ERROR;
            qemu_log("linqu-uapi kick read slot failed base=%#" PRIx64 " head=%u\n",
                     ubc_dev->linqu_uapi_cmdq_iova,
                     head);
            return;
        }
        op_id = ldq_le_p(slot + 1);
        qemu_log("linqu-uapi kick submit slot=%u opcode=%u op_id=%" PRIu64 "\n",
                 head,
                 slot[0],
                 op_id);
        if (slot[0] == LINGQU_PTO_DISPATCH_SLOT_TAG_V2) {
            rc = linqu_uapi_submit_ub_gm_v2(
                ubc_dev, slot, head, &op_id);
            if (rc == LINQU_PTO_UB_GM_INTERNAL_PENDING) {
                authorization = ubc_dev->linqu_uapi_authorization;
                if (!authorization) {
                    ubc_dev->linqu_uapi_last_error =
                        LINGQU_PTO_UB_GM_EXECUTION_FAILED;
                    ubc_dev->linqu_uapi_irq_status |=
                        LINQU_UAPI_IRQ_ERROR;
                    return;
                }
                authorization->resume_batch = MAX(
                    authorization->resume_batch, batch - consumed);
                break;
            } else if (rc != LINGQU_PTO_UB_GM_OK) {
                if (!linqu_uapi_publish_ub_gm_failure(
                        ubc_dev, op_id, rc)) {
                    ubc_dev->linqu_uapi_last_error = rc;
                    ubc_dev->linqu_uapi_irq_status |=
                        LINQU_UAPI_IRQ_ERROR;
                    return;
                }
            } else {
                if (ub_gm_op_count >= G_N_ELEMENTS(ub_gm_ops)) {
                    linqu_ub_gm_remove_dispatch(
                        ubc_dev, op_id, "batch_capacity");
                    ubc_dev->linqu_uapi_last_error =
                        LINGQU_PTO_UB_GM_EXECUTION_FAILED;
                    return;
                }
                ub_gm_ops[ub_gm_op_count++] = op_id;
                bridge_queued++;
            }
        } else {
            rc = linqu_ub_bridge_submit_slot(
                ubc_dev->linqu_uapi_bridge, LINQU_UAPI_ENDPOINT_ID,
                slot, sizeof(slot));
            if (rc != 0) {
                ubc_dev->linqu_uapi_last_error = 9;
                ubc_dev->linqu_uapi_irq_status |= LINQU_UAPI_IRQ_ERROR;
                qemu_log("linqu-uapi kick submit failed slot=%u rc=%d\n",
                         head, rc);
                return;
            }
            bridge_queued++;
        }
        head = (head + 1) % ubc_dev->linqu_uapi_cmdq_depth;
        consumed++;
    }

    qemu_log("linqu-uapi kick ring queued=%u consumed=%u"
             " pending_head=%u tail=%u\n",
             bridge_queued,
             consumed,
             head,
             ubc_dev->linqu_uapi_cmdq_tail);
    ubc_dev->linqu_uapi_cmdq_head = head;
    if (bridge_queued == 0) {
        return;
    }
    rc = linqu_ub_bridge_ring_doorbell(ubc_dev->linqu_uapi_bridge,
                                       LINQU_UAPI_ENDPOINT_ID,
                                       bridge_queued,
                                       &ring_submitted,
                                       &pending);
    if (rc != 0) {
        uint32_t index;

        for (index = 0; index < ub_gm_op_count; index++) {
            linqu_ub_gm_remove_dispatch(
                ubc_dev, ub_gm_ops[index], "doorbell_failed");
            linqu_uapi_publish_ub_gm_failure(
                ubc_dev, ub_gm_ops[index],
                LINGQU_PTO_UB_GM_EXECUTION_FAILED);
        }
        ubc_dev->linqu_uapi_last_error = 10;
        ubc_dev->linqu_uapi_irq_status |= LINQU_UAPI_IRQ_ERROR;
        qemu_log("linqu-uapi kick ring failed rc=%d\n", rc);
        return;
    }
    qemu_log("linqu-uapi kick ring done submitted=%u pending=%u\n",
             ring_submitted,
             pending);
    linqu_uapi_flush_cq(ubc_dev);
    qemu_log("linqu-uapi kick done cmdq_head=%u cq_tail=%u irq=%#" PRIx64 " last_error=%" PRIu64 "\n",
             ubc_dev->linqu_uapi_cmdq_head,
             ubc_dev->linqu_uapi_cq_tail,
             ubc_dev->linqu_uapi_irq_status,
             ubc_dev->linqu_uapi_last_error);
}

static void linqu_uapi_kick_bh(void *opaque)
{
    BusControllerDev *ubc_dev = opaque;

    if (!ubc_dev) {
        return;
    }

    ubc_dev->linqu_uapi_kick_running = true;
    while (ubc_dev->linqu_uapi_kick_pending) {
        uint32_t batch = ubc_dev->linqu_uapi_kick_batch;

        ubc_dev->linqu_uapi_kick_pending = false;
        ubc_dev->linqu_uapi_kick_batch = 0;
        linqu_uapi_kick(ubc_dev, batch);
    }
    ubc_dev->linqu_uapi_kick_running = false;
}

static void linqu_uapi_schedule_kick(BusControllerDev *ubc_dev, uint32_t batch)
{
    bool was_idle;

    if (!ubc_dev) {
        return;
    }

    was_idle = !ubc_dev->linqu_uapi_kick_pending &&
               !ubc_dev->linqu_uapi_kick_running;
    if (batch > ubc_dev->linqu_uapi_kick_batch) {
        ubc_dev->linqu_uapi_kick_batch = batch;
    }
    ubc_dev->linqu_uapi_kick_pending = true;
    if (was_idle) {
        aio_bh_schedule_oneshot(qemu_get_aio_context(),
                                linqu_uapi_kick_bh,
                                ubc_dev);
    }
}

static uint64_t linqu_uapi_access_extract(uint64_t full_value, hwaddr reg,
                                          unsigned len)
{
    if (len == sizeof(uint64_t)) {
        return full_value;
    }
    if (len == DWORD_SIZE) {
        return (reg & 0x4) ? (full_value >> 32) & 0xffffffffULL
                           : full_value & 0xffffffffULL;
    }
    return full_value;
}

static uint64_t linqu_uapi_access_merge(uint64_t current_value, hwaddr reg,
                                        uint64_t value, unsigned len)
{
    if (len == sizeof(uint64_t)) {
        return value;
    }
    if (len == DWORD_SIZE) {
        if (reg & 0x4) {
            return (current_value & 0x00000000ffffffffULL) |
                   ((value & 0xffffffffULL) << 32);
        }
        return (current_value & 0xffffffff00000000ULL) |
               (value & 0xffffffffULL);
    }
    return current_value;
}

static uint64_t linqu_uapi_reg_read(BusControllerDev *ubc_dev, hwaddr reg,
                                    unsigned len)
{
    uint64_t value = 0;

    if (reg == LINQU_UAPI_REG_VERSION) {
        return linqu_uapi_access_extract(LINQU_UAPI_VERSION, reg, len);
    }
    if (!linqu_uapi_init_bridge(ubc_dev)) {
        return 0;
    }

    switch (reg & ~0x7ULL) {
    case LINQU_UAPI_REG_CMDQ_BASE_LO:
        value = (uint32_t)ubc_dev->linqu_uapi_cmdq_iova;
        break;
    case LINQU_UAPI_REG_CMDQ_BASE_HI:
        value = ubc_dev->linqu_uapi_cmdq_iova >> 32;
        break;
    case LINQU_UAPI_REG_CMDQ_SIZE:
        value = ubc_dev->linqu_uapi_cmdq_depth;
        break;
    case LINQU_UAPI_REG_CMDQ_HEAD:
        value = ubc_dev->linqu_uapi_cmdq_head;
        break;
    case LINQU_UAPI_REG_CMDQ_TAIL:
        value = ubc_dev->linqu_uapi_cmdq_tail;
        break;
    case LINQU_UAPI_REG_CQ_BASE_LO:
        value = (uint32_t)ubc_dev->linqu_uapi_cq_iova;
        break;
    case LINQU_UAPI_REG_CQ_BASE_HI:
        value = ubc_dev->linqu_uapi_cq_iova >> 32;
        break;
    case LINQU_UAPI_REG_CQ_SIZE:
        value = ubc_dev->linqu_uapi_cq_depth;
        break;
    case LINQU_UAPI_REG_CQ_HEAD:
        value = ubc_dev->linqu_uapi_cq_head;
        break;
    case LINQU_UAPI_REG_CQ_TAIL:
        linqu_uapi_flush_cq(ubc_dev);
        value = ubc_dev->linqu_uapi_cq_tail;
        break;
    case LINQU_UAPI_REG_STATUS:
        linqu_uapi_flush_cq(ubc_dev);
        value = linqu_uapi_status(ubc_dev);
        break;
    case LINQU_UAPI_REG_LAST_ERROR:
        value = ubc_dev->linqu_uapi_last_error;
        break;
    case LINQU_UAPI_REG_IRQ_STATUS:
        value = ubc_dev->linqu_uapi_irq_status;
        break;
    case LINQU_UAPI_REG_DEFAULT_SEGMENT:
        value = ubc_dev->linqu_uapi_default_segment;
        break;
    case LINQU_UAPI_REG_SEG_DATA_OFFSET:
        value = ubc_dev->linqu_uapi_segment_data_offset;
        break;
    case LINQU_UAPI_REG_SEG_DATA_VALUE:
        if (linqu_ub_bridge_read_segment_payload(ubc_dev->linqu_uapi_bridge,
                                                 ubc_dev->linqu_uapi_default_segment,
                                                 ubc_dev->linqu_uapi_segment_data_offset,
                                                 (uint8_t *)&value,
                                                 sizeof(value)) != 0) {
            ubc_dev->linqu_uapi_last_error = 11;
            return 0;
        }
        break;
    case LINQU_UAPI_REG_CANCEL_OP_ID:
        value = ubc_dev->linqu_uapi_cancel_op_id;
        break;
    default:
        return 0;
    }
    return linqu_uapi_access_extract(value, reg, len);
}

static bool linqu_uapi_reg_write(BusControllerDev *ubc_dev, hwaddr reg,
                                 uint64_t value, unsigned len)
{
    hwaddr base_reg = reg & ~0x7ULL;
    uint64_t current;

    if (!linqu_uapi_init_bridge(ubc_dev)) {
        return false;
    }
    switch (base_reg) {
    case LINQU_UAPI_REG_CMDQ_BASE_LO:
        current = ubc_dev->linqu_uapi_cmdq_iova;
        ubc_dev->linqu_uapi_cmdq_iova =
            linqu_uapi_access_merge(current, reg, value, len);
        return true;
    case LINQU_UAPI_REG_CQ_BASE_LO:
        current = ubc_dev->linqu_uapi_cq_iova;
        ubc_dev->linqu_uapi_cq_iova =
            linqu_uapi_access_merge(current, reg, value, len);
        return true;
    case LINQU_UAPI_REG_CQ_HEAD:
        if (len == DWORD_SIZE && (reg & 0x4)) {
            return true;
        }
        if (value >= ubc_dev->linqu_uapi_cq_depth) {
            ubc_dev->linqu_uapi_last_error = 12;
            return true;
        }
        ubc_dev->linqu_uapi_cq_head = value;
        if (ubc_dev->linqu_uapi_cq_head == ubc_dev->linqu_uapi_cq_tail) {
            ubc_dev->linqu_uapi_irq_status &= ~LINQU_UAPI_IRQ_COMPLETION;
        }
        return true;
    case LINQU_UAPI_REG_CMDQ_TAIL:
        if (len == DWORD_SIZE && (reg & 0x4)) {
            return true;
        }
        if (value >= ubc_dev->linqu_uapi_cmdq_depth) {
            ubc_dev->linqu_uapi_last_error = 13;
            return true;
        }
        ubc_dev->linqu_uapi_cmdq_tail = value;
        return true;
    case LINQU_UAPI_REG_DOORBELL:
        if (len == DWORD_SIZE && (reg & 0x4)) {
            return true;
        }
        linqu_uapi_schedule_kick(ubc_dev,
                                 value ? value : ubc_dev->linqu_uapi_cmdq_depth);
        return true;
    case LINQU_UAPI_REG_IRQ_ACK:
        if (len == DWORD_SIZE && (reg & 0x4)) {
            return true;
        }
        ubc_dev->linqu_uapi_irq_status &= ~value;
        return true;
    case LINQU_UAPI_REG_SEG_DATA_OFFSET:
        current = ubc_dev->linqu_uapi_segment_data_offset;
        ubc_dev->linqu_uapi_segment_data_offset =
            linqu_uapi_access_merge(current, reg, value, len);
        return true;
    case LINQU_UAPI_REG_SEG_DATA_VALUE:
        current = 0;
        if (linqu_ub_bridge_read_segment_payload(ubc_dev->linqu_uapi_bridge,
                                                 ubc_dev->linqu_uapi_default_segment,
                                                 ubc_dev->linqu_uapi_segment_data_offset,
                                                 (uint8_t *)&current,
                                                 sizeof(current)) != 0) {
            ubc_dev->linqu_uapi_last_error = 11;
            return true;
        }
        value = linqu_uapi_access_merge(current, reg, value, len);
        if (linqu_ub_bridge_write_segment_payload(ubc_dev->linqu_uapi_bridge,
                                                  ubc_dev->linqu_uapi_default_segment,
                                                  ubc_dev->linqu_uapi_segment_data_offset,
                                                  (const uint8_t *)&value,
                                                  sizeof(value)) != 0) {
            ubc_dev->linqu_uapi_last_error = 14;
        } else {
            linqu_uapi_maybe_register_model_runtime_object_payload(
                ubc_dev,
                ubc_dev->linqu_uapi_default_segment,
                ubc_dev->linqu_uapi_segment_data_offset);
        }
        return true;
    case LINQU_UAPI_REG_CANCEL_OP_ID:
        current = ubc_dev->linqu_uapi_cancel_op_id;
        ubc_dev->linqu_uapi_cancel_op_id =
            linqu_uapi_access_merge(current, reg, value, len);
        return true;
    case LINQU_UAPI_REG_CANCEL_DOORBELL:
        if (len == DWORD_SIZE && (reg & 0x4)) {
            return true;
        }
        if (value != 1 || ubc_dev->linqu_uapi_cancel_op_id == 0 ||
            !linqu_uapi_cancel_authorization(
                ubc_dev, ubc_dev->linqu_uapi_cancel_op_id)) {
            ubc_dev->linqu_uapi_last_error = LINGQU_PTO_UB_GM_UNBOUND;
        }
        ubc_dev->linqu_uapi_cancel_op_id = 0;
        return true;
    default:
        return false;
    }
}

static void ub_bus_controller_dev_reset(DeviceState *device)
{
    BusControllerDev *ubc_dev = BUS_CONTROLLER_DEV(device);
    LinquPtoAuthorizationState *authorization =
        ubc_dev->linqu_uapi_authorization;
    uint64_t op_id = authorization ? authorization->op_id : 0;
    uint64_t request_id = authorization ? authorization->request_id : 0;
    uint64_t sequence = authorization ? authorization->sequence : 0;
    uint32_t cmdq_slot = authorization ? authorization->cmdq_slot : 0;
    bool had_authorization = authorization != NULL;

    linqu_uapi_authorization_discard(ubc_dev);
    if (had_authorization &&
        ubc_dev->pto_authorization_inject_late_completion) {
        linqu_uapi_authorization_complete(
            ubc_dev, op_id, request_id, sequence,
            LINGQU_PTO_UB_GM_OK, "reset-late-injection");
    }

    ubc_dev->linqu_uapi_kick_pending = false;
    ubc_dev->linqu_uapi_kick_batch = 0;
    if (ubc_dev->linqu_uapi_bridge) {
        linqu_ub_bridge_free(ubc_dev->linqu_uapi_bridge);
        ubc_dev->linqu_uapi_bridge = NULL;
    }
    linqu_ub_gm_registry_free(ubc_dev->linqu_uapi_ub_gm_registry);
    ubc_dev->linqu_uapi_ub_gm_registry = NULL;
    ubc_dev->linqu_uapi_bridge_ready = false;
    ubc_dev->linqu_uapi_cmdq_iova = 0;
    ubc_dev->linqu_uapi_cq_iova = 0;
    ubc_dev->linqu_uapi_cmdq_depth = 0;
    ubc_dev->linqu_uapi_cq_depth = 0;
    ubc_dev->linqu_uapi_cmdq_head = 0;
    ubc_dev->linqu_uapi_cmdq_tail = 0;
    ubc_dev->linqu_uapi_cq_head = 0;
    ubc_dev->linqu_uapi_cq_tail = 0;
    ubc_dev->linqu_uapi_default_segment = 0;
    ubc_dev->linqu_uapi_segment_data_offset = 0;
    ubc_dev->linqu_uapi_last_error = 0;
    ubc_dev->linqu_uapi_irq_status = 0;
    ubc_dev->linqu_uapi_cancel_op_id = 0;

    qemu_log("QEMU_UB_GM_RESET authorization_pending=%u op=%" PRIu64
             " request=%" PRIu64 " slot=%u sequence=%" PRIu64
             " cq_completion=0 next_sequence=%" PRIu64 "\n",
             had_authorization, op_id, request_id, cmdq_slot, sequence,
             ubc_dev->linqu_uapi_next_authorization_sequence);
}

static uint64_t ub_ers_region_read(void *opaque, hwaddr addr, unsigned len)
{
    typeof(((BusControllerDev *)0)->ers[0]) *ers = opaque;
    BusControllerDev *ubc_dev = ers->owner;
    uint32_t entity_idx = 0;
    hwaddr obmm_async_reg;
    hwaddr ub_async_load_reg;
    hwaddr linqu_reg;

    if (ers->idx == 1 && len >= DWORD_SIZE &&
        addr < sizeof(uint64_t)) {
        return linqu_uapi_access_extract(LINQU_UAPI_VERSION, addr, len);
    }

    if (ers->idx == 2 && len >= DWORD_SIZE) {
        if (ubc_dev->obmm_async &&
            ub_obmm_async_decode(addr, &obmm_async_reg)) {
            return ub_obmm_async_read(ubc_dev->obmm_async,
                                      obmm_async_reg, len);
        }
        if (ubc_dev->ub_async_load &&
            ub_async_load_device_decode(addr, &ub_async_load_reg)) {
            return ub_async_load_device_read(ubc_dev->ub_async_load,
                                      ub_async_load_reg, len);
        }
        if (addr < sizeof(uint64_t)) {
            return linqu_uapi_reg_read(ubc_dev, addr, len);
        }
        if (linqu_uapi_decode_endpoint(addr, &linqu_reg)) {
            return linqu_uapi_reg_read(ubc_dev, linqu_reg, len);
        }
    }

    /* Multi-entity routing for ERS0 (Config), ERS1 (Doorbell), and ERS2 (Resource) */
    if ((ers->idx == 0 || ers->idx == 1 || ers->idx == 2) && addr >= 0x400000) {
        entity_idx = (uint32_t)(addr / 0x400000);
        addr %= 0x400000;
        
        if (ubc_dev && entity_idx < ubc_dev->entity_count) {
            if (ers->idx == 0) {
                uint8_t *cfg = ubc_dev->entity_cfg_spaces[entity_idx].cfg_base;
                uint32_t cfg_size = ubc_dev->entity_cfg_spaces[entity_idx].cfg_size;
                if (cfg && addr < cfg_size) {
                    switch (len) {
                    case 1: return cfg[addr];
                    case 2: return lduw_le_p(cfg + addr);
                    case 4: return ldl_le_p(cfg + addr);
                    case 8: return ldq_le_p(cfg + addr);
                    }
                }
            }
            /* Note: ERS1 (Doorbell) reads for entities > 0 return 0 for now as it's write-only */
        }
        return 0;
    }

    if (!ers->storage || addr > ers->storage_size || len > ers->storage_size - addr) {
        return 0;
    }

    switch (len) {
    case BYTE_SIZE:
        return ub_get_byte(ers->storage + addr);
    case WORD_SIZE:
        return ub_get_word(ers->storage + addr);
    case DWORD_SIZE:
        return ub_get_long(ers->storage + addr);
    case sizeof(uint64_t):
        return ub_get_quad(ers->storage + addr);
    default:
        return 0;
    }
}

static void ubc_fill_remote_dcna_from_link(UBDevice *ub_dev,
                                           UBFMManagedLink *fm_link,
                                           uint32_t *dcna)
{
    UBLinkState *link;

    if (!ub_dev || !fm_link || !dcna || *dcna) {
        return;
    }

    link = fm_link->runtime;
    if (!link) {
        return;
    }

    if (link->a.device == ub_dev && link->b.device) {
        *dcna = link->b.device->cna;
    } else if (link->b.device == ub_dev && link->a.device) {
        *dcna = link->a.device->cna;
    } else if (link->a.device && link->a.device != ub_dev) {
        *dcna = link->a.device->cna;
    } else if (link->b.device && link->b.device != ub_dev) {
        *dcna = link->b.device->cna;
    }
}

static uint32_t ubc_remote_cna_from_link(UBDevice *ub_dev, UBLinkState *link)
{
    if (!ub_dev || !link) {
        return 0;
    }
    if (link->a.device == ub_dev && link->b.device) {
        return link->b.device->cna;
    }
    if (link->b.device == ub_dev && link->a.device) {
        return link->a.device->cna;
    }
    if (link->a.device && link->a.device != ub_dev) {
        return link->a.device->cna;
    }
    if (link->b.device && link->b.device != ub_dev) {
        return link->b.device->cna;
    }
    return 0;
}

static uint32_t ubc_primary_cna_from_ub_ssd_cna(uint32_t ssd_cna)
{
    if ((ssd_cna & 0xf0002000U) != 0x10002000U) {
        return 0;
    }
    return ((ssd_cna & 0x0fff0000U) >> 12) | 0x2U;
}

static uint8_t ubc_node_ip_suffix_from_id(const char *node_id)
{
    if (!node_id) {
        return 0;
    }
    if (g_str_equal(node_id, "nodeA")) {
        return 1;
    }
    if (g_str_equal(node_id, "nodeB")) {
        return 2;
    }
    if (g_str_equal(node_id, "nodeC")) {
        return 3;
    }
    if (g_str_equal(node_id, "nodeD")) {
        return 4;
    }
    if (g_str_equal(node_id, "nodeE")) {
        return 5;
    }
    if (g_str_equal(node_id, "nodeF")) {
        return 6;
    }
    if (g_str_equal(node_id, "nodeG")) {
        return 7;
    }
    if (g_str_equal(node_id, "nodeH")) {
        return 8;
    }
    return 0;
}

static void ubc_fill_link_local_eid_hw(uint8_t eid_hw[16], uint8_t suffix)
{
    memset(eid_hw, 0, 16);
    eid_hw[0] = suffix;
    eid_hw[14] = 0x80;
    eid_hw[15] = 0xfe;
}

static bool ubc_fill_link_local_eid_by_scna(UBDevice *ub_dev, uint32_t scna,
                                            uint8_t eid_hw[16])
{
    uint32_t i;

    if (!ub_dev || !eid_hw || !scna) {
        return false;
    }

    for (i = 0; i < ub_dev->port.port_num; i++) {
        NeighborInfo *ni = &ub_dev->port.neighbors[i];

        if (!ni->is_remote_neighbor || !ni->remote_primary_cna_valid) {
            continue;
        }
        if (ni->remote_primary_cna != scna || !ni->remote_node_ip_suffix) {
            continue;
        }
        ubc_fill_link_local_eid_hw(eid_hw, ni->remote_node_ip_suffix);
        return true;
    }

    return false;
}

static bool ubc_eid_is_zero(const uint8_t eid[16])
{
    int i;

    if (!eid) {
        return true;
    }
    for (i = 0; i < 16; i++) {
        if (eid[i] != 0) {
            return false;
        }
    }
    return true;
}

static bool ubc_extract_link_local_mcast_group(const uint8_t *rmt_eid,
                                               uint8_t *group)
{
    int i;

    if (!rmt_eid || !group) {
        return false;
    }
    if (rmt_eid[0] == 0xff && rmt_eid[1] == 0x02) {
        for (i = 2; i < 15; i++) {
            if (rmt_eid[i] != 0) {
                return false;
            }
        }
        if (rmt_eid[15] == 0) {
            return false;
        }
        *group = rmt_eid[15];
        return true;
    }
    if (rmt_eid[15] == 0xff && rmt_eid[14] == 0x02) {
        for (i = 1; i < 14; i++) {
            if (rmt_eid[i] != 0) {
                return false;
            }
        }
        if (rmt_eid[0] == 0) {
            return false;
        }
        *group = rmt_eid[0];
        return true;
    }
    return false;
}

static bool ubc_extract_ipv4_suffix_from_rmt_eid(const uint8_t *rmt_eid,
                                                 uint8_t *suffix)
{
    int i;

    if (!rmt_eid || !suffix) {
        return false;
    }
    if (rmt_eid[0] == 0xfe && rmt_eid[1] == 0x80) {
        for (i = 2; i < 15; i++) {
            if (rmt_eid[i] != 0) {
                break;
            }
        }
        if (i == 15 && rmt_eid[15] != 0) {
            *suffix = rmt_eid[15];
            return true;
        }
    }
    if (rmt_eid[15] == 0xfe && rmt_eid[14] == 0x80) {
        for (i = 1; i < 14; i++) {
            if (rmt_eid[i] != 0) {
                break;
            }
        }
        if (i == 14 && rmt_eid[0] != 0) {
            *suffix = rmt_eid[0];
            return true;
        }
    }
    if (rmt_eid[0] == 0xfe && rmt_eid[1] == 0x80) {
        for (i = 2; i < 12; i++) {
            if (rmt_eid[i] != 0) {
                return false;
            }
        }
        if (rmt_eid[12] != 10 || rmt_eid[13] != 0 || rmt_eid[14] != 0) {
            return false;
        }
        *suffix = rmt_eid[15];
        return *suffix != 0;
    }
    if (rmt_eid[15] == 0xfe && rmt_eid[14] == 0x80) {
        for (i = 4; i < 14; i++) {
            if (rmt_eid[i] != 0) {
                return false;
            }
        }
        if (rmt_eid[3] != 10 || rmt_eid[2] != 0 || rmt_eid[1] != 0) {
            return false;
        }
        *suffix = rmt_eid[0];
        return *suffix != 0;
    }
    return false;
}

static void ubc_try_fill_dcna_from_rmt_eid(UBDevice *ub_dev, const uint8_t *rmt_eid,
                                           uint32_t *dcna)
{
    uint8_t target_suffix = 0;
    uint32_t i;

    if (!ub_dev || !dcna || *dcna) {
        return;
    }
    if (!ubc_extract_ipv4_suffix_from_rmt_eid(rmt_eid, &target_suffix)) {
        qemu_log("ubc route by rmt_eid: unsupported eid=%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\n",
                 rmt_eid ? rmt_eid[0] : 0, rmt_eid ? rmt_eid[1] : 0, rmt_eid ? rmt_eid[2] : 0, rmt_eid ? rmt_eid[3] : 0,
                 rmt_eid ? rmt_eid[4] : 0, rmt_eid ? rmt_eid[5] : 0, rmt_eid ? rmt_eid[6] : 0, rmt_eid ? rmt_eid[7] : 0,
                 rmt_eid ? rmt_eid[8] : 0, rmt_eid ? rmt_eid[9] : 0, rmt_eid ? rmt_eid[10] : 0, rmt_eid ? rmt_eid[11] : 0,
                 rmt_eid ? rmt_eid[12] : 0, rmt_eid ? rmt_eid[13] : 0, rmt_eid ? rmt_eid[14] : 0, rmt_eid ? rmt_eid[15] : 0);
        return;
    }

    for (i = 0; i < ub_dev->port.port_num; i++) {
        NeighborInfo *ni = &ub_dev->port.neighbors[i];

        if (!ni->is_remote_neighbor || !ni->remote_primary_cna_valid) {
            continue;
        }
        if (ni->remote_node_ip_suffix == target_suffix) {
            *dcna = ni->remote_primary_cna;
            if (ubc_trace_link_lookup_enabled()) {
                qemu_log("ubc route by rmt_eid: target_suffix=%u remote_id=%s dcna=%#x\n",
                         target_suffix,
                         ni->neighbor_id[0] ? ni->neighbor_id : "<unknown>",
                         *dcna);
            }
            return;
        }
    }
}

static UBFMManagedLink *ubc_find_fm_link(UBDevice *ub_dev, uint32_t *dcna)
{
    UBFMManagedLink *fm_link = NULL;
    uint32_t i;

    if (!ub_dev) {
        return NULL;
    }
    if (ubc_trace_link_lookup_enabled()) {
        qemu_log("ubc_find_fm_link: dev=%s local_cna=%#x input_dcna=%#x\n",
                 ub_dev->qdev.id ? ub_dev->qdev.id : "<null>", ub_dev->cna,
                 dcna ? *dcna : 0);
    }

    if (dcna && *dcna) {
        fm_link = ub_fm_find_link_for_device_cna(ub_dev, *dcna);
        if (fm_link) {
            ubc_fill_remote_dcna_from_link(ub_dev, fm_link, dcna);
            if (ubc_trace_link_lookup_enabled()) {
                qemu_log("ubc_find_fm_link: matched input dcna=%#x\n",
                         dcna ? *dcna : 0);
            }
            return fm_link;
        }
    }

    for (i = 0; i < ub_dev->port.port_num; i++) {
        NeighborInfo *ni = &ub_dev->port.neighbors[i];

        if (!ni->is_remote_neighbor || !ni->remote_primary_cna_valid) {
            continue;
        }
        fm_link = ub_fm_find_link_for_device_cna(ub_dev, ni->remote_primary_cna);
        if (fm_link) {
            if (dcna && !*dcna) {
                *dcna = ni->remote_primary_cna;
            }
            ubc_fill_remote_dcna_from_link(ub_dev, fm_link, dcna);
            if (ubc_trace_link_lookup_enabled()) {
                qemu_log("ubc_find_fm_link: matched neighbor[%u] remote_cna=%#x final_dcna=%#x\n",
                         i, ni->remote_primary_cna, dcna ? *dcna : 0);
            }
            return fm_link;
        }
    }

    fm_link = ub_fm_find_link_by_cna(ub_dev->cna);
    if (fm_link) {
        ubc_fill_remote_dcna_from_link(ub_dev, fm_link, dcna);
        if (ubc_trace_link_lookup_enabled()) {
            qemu_log("ubc_find_fm_link: matched by local_cna final_dcna=%#x\n",
                     dcna ? *dcna : 0);
        }
    }
    if (!fm_link) {
        qemu_log("ubc_find_fm_link: miss dev=%s local_cna=%#x\n",
                 ub_dev->qdev.id ? ub_dev->qdev.id : "<null>", ub_dev->cna);
    }
    return fm_link;
}

static UBLinkState *ubc_find_active_link(BusControllerDev *ubc_dev, uint32_t *dcna)
{
    UBDevice *ub_dev = ubc_dev ? &ubc_dev->parent : NULL;
    UBFMManagedLink *fm_link;

    if (!ub_dev) {
        return NULL;
    }

    fm_link = ubc_find_fm_link(ub_dev, dcna);
    if (!fm_link || !fm_link->runtime) {
        return NULL;
    }
    if (!fm_link->runtime->link_up ||
        (!fm_link->runtime->ioc && !fm_link->runtime->shmem_ready)) {
        return NULL;
    }

    return fm_link->runtime;
}

static bool ubc_link_has_transport(const UBLinkState *link)
{
    return link && link->link_up && (link->ioc || link->shmem_ready);
}

static void ubc_sim_dec_process_wait_links(BusControllerState *bcs,
                                           BusControllerDev *ubc_dev,
                                           UBLinkState *preferred)
{
    UBDevice *ub_dev = ubc_dev ? &ubc_dev->parent : NULL;
    uint32_t i;

    if (!bcs || !ub_dev) {
        return;
    }
    if (preferred) {
        ub_link_process_incoming_message(bcs, preferred);
    }
    for (i = 0; i < ub_dev->port.port_num; ++i) {
        NeighborInfo *ni = &ub_dev->port.neighbors[i];
        UBLinkState *link;
        uint32_t dcna;

        if (!ni->is_remote_neighbor || !ni->remote_primary_cna_valid) {
            continue;
        }
        dcna = ni->remote_primary_cna;
        link = ubc_find_active_link(ubc_dev, &dcna);
        if (!link || link == preferred) {
            continue;
        }
        ub_link_process_incoming_message(bcs, link);
    }
}

static int ubc_send_msg_over_link(BusControllerDev *ubc_dev, UBLinkState *link,
                                  uint32_t dcna, uint8_t sub_msg_code,
                                  const void *payload, uint32_t payload_len)
{
    UBDevice *ub_dev = &ubc_dev->parent;
    Error *local_err = NULL;
    size_t total_len = sizeof(MsgPktHeader) + payload_len;
    uint8_t *pkt = g_malloc0(total_len);
    MsgPktHeader *hdr = (MsgPktHeader *)pkt;
    int rc;

    hdr->ulh.cfg = UB_CLAN_LINK_CFG;
    hdr->ulh.plen = payload_len;
    hdr->nth.scna = ub_dev->cna;
    hdr->nth.dcna = dcna;
    hdr->msgetah.msg_code = UBC_MSG_CODE_URMA_DATA;
    hdr->msgetah.sub_msg_code = sub_msg_code;
    hdr->msgetah.type = 1;
    hdr->msgetah.plen = payload_len & 0xFFF;
    if (payload_len) {
        memcpy(pkt + sizeof(MsgPktHeader), payload, payload_len);
    }

    if (sub_msg_code == UBC_MSG_SUB_GSVA_COH ||
        (sub_msg_code >= UBC_MSG_SUB_GSVA_COH_READ_ACQ &&
         sub_msg_code <= UBC_MSG_SUB_GSVA_COH_TOKEN_ACK)) {
        qemu_log("GSVA_COH: ub_link send sub=%u scna=%#x dcna=%#x"
                 " payload_len=%u link=%s:%u<->%s:%u\n",
                 sub_msg_code, ub_dev->cna, dcna, payload_len,
                 link->a.device_id ? link->a.device_id : "<null>",
                 link->a.port_idx,
                 link->b.device_id ? link->b.device_id : "<null>",
                 link->b.port_idx);
    }

    if (ubc_trace_data_path_enabled() &&
        sub_msg_code == UBC_MSG_SUB_SIM_DEC_READ_REQ &&
        payload_len >= sizeof(UBCSimDecReadReqPld)) {
        const UBCSimDecReadReqPld *req = payload;
        qemu_log("ubc sim_dec send read_req req=%u uba=%#" PRIx64
                 " len=%u scna=%#x dcna=%#x link=%s:%u<->%s:%u\n",
                 req->req_id, (uint64_t)req->remote_uba,
                 req->read_len, ub_dev->cna, dcna,
                 link->a.device_id ? link->a.device_id : "<null>",
                 link->a.port_idx,
                 link->b.device_id ? link->b.device_id : "<null>",
                 link->b.port_idx);
    }

    rc = ub_link_write_message(link, pkt, total_len, &local_err);
    if (rc < 0) {
        qemu_log("ubc sim_dec: ub_link write failed sub=%u: %s\n",
                 sub_msg_code,
                 local_err ? error_get_pretty(local_err) : "unknown");
        if (local_err) {
            error_free(local_err);
        }
    } else {
        if (ubc_trace_data_path_enabled() &&
            sub_msg_code == UBC_MSG_SUB_SIM_DEC_READ_REQ &&
            payload_len >= sizeof(UBCSimDecReadReqPld)) {
            const UBCSimDecReadReqPld *req = payload;
            qemu_log("ubc sim_dec send read_req done req=%u uba=%#" PRIx64
                     " len=%u dcna=%#x\n",
                     req->req_id, (uint64_t)req->remote_uba,
                     req->read_len, dcna);
        }
        ub_link_kick_remote(link, NULL);
    }
    g_free(pkt);
    return rc;
}

/* Coherence message send helper (non-static, used by obmm_coherence.c) */
int obmm_coh_send_ub_link_msg(BusControllerDev *ubc_dev, uint32_t dcna,
                               uint8_t sub_msg_code,
                               const void *payload, uint32_t payload_len)
{
    UBLinkState *link = ubc_find_active_link(ubc_dev, &dcna);
    if (!link) {
        qemu_log("OBMM_COH: no active link to dcna=%#x\n", dcna);
        return -1;
    }
    return ubc_send_msg_over_link(ubc_dev, link, dcna,
                                  sub_msg_code, payload, payload_len);
}

void obmm_coh_poll_rx_links(BusControllerDev *ubc_dev)
{
    ub_fm_poll_rx_links_now();
}

static int sim_dec_send_batch_writes(BusControllerDev *ubc_dev, uint32_t dcna,
                                       uint32_t token_id,
                                       SimDecBatchWriteOp *ops,
                                       uint8_t **datas,
                                       uint32_t op_count,
                                       uint32_t barrier_epoch)
{
    UBLinkState *link;
    size_t payload_len;
    size_t data_offset;
    uint8_t *payload;
    SimDecBatchHdr *hdr;
    SimDecBatchWriteOp *out_ops;
    uint32_t i;
    size_t cur_data_off;
    int rc;

    if (!ubc_dev || op_count == 0 || op_count > SIM_DEC_BATCH_MAX_OPS ||
        dcna == 0) {
        return -1;
    }

    link = ubc_find_active_link(ubc_dev, &dcna);
    if (!link) {
        qemu_log("ubc sim_dec batch: no active link\n");
        return -1;
    }

    payload_len = sizeof(SimDecBatchHdr) + sizeof(SimDecBatchWriteOp) * op_count;
    data_offset = payload_len;
    for (i = 0; i < op_count; i++) {
        if (!datas[i] || ops[i].data_len == 0 ||
            ops[i].data_len > SIM_DEC_BATCH_MAX_DATA ||
            payload_len > SIZE_MAX - ops[i].data_len ||
            payload_len + ops[i].data_len >
                sizeof(SimDecBatchHdr) +
                    sizeof(SimDecBatchWriteOp) * op_count +
                    SIM_DEC_BATCH_MAX_DATA) {
            qemu_log("ubc sim_dec batch: invalid op=%u len=%u\n",
                     i, ops[i].data_len);
            return -1;
        }
        payload_len += ops[i].data_len;
    }

    payload = g_malloc0(payload_len);
    hdr = (SimDecBatchHdr *)payload;
    hdr->version = 1;
    hdr->op_count = (uint8_t)op_count;
    hdr->flags = 0;
    hdr->barrier_epoch = barrier_epoch;

    out_ops = (SimDecBatchWriteOp *)(payload + sizeof(SimDecBatchHdr));
    cur_data_off = data_offset;
    for (i = 0; i < op_count; i++) {
        out_ops[i].remote_uba = ops[i].remote_uba;
        out_ops[i].token_id = token_id;
        out_ops[i].data_len = ops[i].data_len;
        memcpy(payload + cur_data_off, datas[i], ops[i].data_len);
        cur_data_off += ops[i].data_len;
    }

    rc = ubc_send_msg_over_link(ubc_dev, link, dcna,
                                UBC_MSG_SUB_SIM_DEC_BATCH,
                                payload, payload_len);
    if (rc >= 0 && g_sim_decoder) {
        g_sim_decoder->stats.batch_frames++;
        g_sim_decoder->stats.batch_ops += op_count;
        g_sim_decoder->stats.batch_bytes += payload_len;
    }
    g_free(payload);
    return rc < 0 ? -1 : 0;
}

static MemTxResult ubc_dma_read_local_data_tid_strict(BusControllerDev *ubc_dev,
                                                      dma_addr_t iova,
                                                      void *buf, size_t len,
                                                      uint32_t tid)
{
    AddressSpace *as = ubc_dma_as(ubc_dev);
    bool tid_override_active = false;
    UMMUTidOverrideScope tid_scope = { 0 };
    MemTxResult ret;

    if (ubc_dev && ubc_dev->ummu && tid != UBC_DMA_TID_AUTO) {
        tid_scope = ummu_dma_tid_override_enter(ubc_dev->ummu, tid);
        tid_override_active = true;
    }
    ret = address_space_read(as, iova, MEMTXATTRS_UNSPECIFIED, buf, len);
    if (ubc_trace_data_path_enabled() && len <= 8) {
        qemu_log("ubc sim_dec strict read iova=%#" PRIx64 " len=%zu tid=%u ret=%d\n",
                 (uint64_t)iova, len, tid, ret);
        if (ret == MEMTX_OK &&
            len == 8 &&
            (((uint64_t)iova & 0xfffULL) >= 0x40) &&
            (((uint64_t)iova & 0xfffULL) <= 0x50)) {
            uint64_t value = 0;

            memcpy(&value, buf, sizeof(value));
            qemu_log("ubc sim_dec strict read data iova=%#" PRIx64 " value=%#" PRIx64 "\n",
                     (uint64_t)iova, value);
        }
    }
    if (tid_override_active) {
        ummu_dma_tid_override_leave(ubc_dev->ummu, tid_scope);
    }
    return ret;
}

static MemTxResult ubc_dma_write_local_data_tid_strict(BusControllerDev *ubc_dev,
                                                       dma_addr_t iova,
                                                       const void *buf,
                                                       size_t len,
                                                       uint32_t tid)
{
    AddressSpace *as = ubc_dma_as(ubc_dev);
    bool tid_override_active = false;
    UMMUTidOverrideScope tid_scope = { 0 };
    MemTxResult ret;

    if (ubc_dev && ubc_dev->ummu && tid != UBC_DMA_TID_AUTO) {
        tid_scope = ummu_dma_tid_override_enter(ubc_dev->ummu, tid);
        tid_override_active = true;
    }
    ret = address_space_write(as, iova, MEMTXATTRS_UNSPECIFIED, buf, len);
    if (tid_override_active) {
        ummu_dma_tid_override_leave(ubc_dev->ummu, tid_scope);
    }
    return ret;
}

MemTxResult obmm_coh_local_read(BusControllerDev *ubc_dev, uint64_t uba,
                                uint32_t token_id, uint8_t *buf,
                                uint32_t len)
{
    MemTxResult ret;

    if (!ubc_dev || !buf || len == 0) {
        return MEMTX_DECODE_ERROR;
    }
    ret = ubc_dma_read_local_data_tid_strict(ubc_dev, uba, buf, len,
                                             ubc_tid_or_auto(token_id));
    if (ret != MEMTX_OK && token_id != 0) {
        ret = ubc_dma_read_local_data_tid_strict(ubc_dev, uba, buf, len,
                                                 UBC_DMA_TID_AUTO);
    }
    return ret;
}

MemTxResult obmm_coh_local_write(BusControllerDev *ubc_dev, uint64_t uba,
                                 uint32_t token_id, const uint8_t *buf,
                                 uint32_t len)
{
    MemTxResult ret;

    if (!ubc_dev || !buf || len == 0) {
        return MEMTX_DECODE_ERROR;
    }
    ret = ubc_dma_write_local_data_tid_strict(ubc_dev, uba, buf, len,
                                              ubc_tid_or_auto(token_id));
    if (ret != MEMTX_OK && token_id != 0) {
        ret = ubc_dma_write_local_data_tid_strict(ubc_dev, uba, buf, len,
                                                  UBC_DMA_TID_AUTO);
    }
    return ret;
}

/* Forward declarations for GSVA route fallback in SIM_DEC handlers. */
static GsvaRouteTable g_gsva_routes;
static void gsva_tables_init(void);
static SimDecMapEntry *sim_dec_find_entry_by_pa(uint64_t pa);
static SimDecMapEntry *sim_dec_find_entry_by_uba(uint64_t uba, uint64_t len);

typedef struct ObmmExportEntry {
    uint64_t remote_uba;
    uint64_t backing_uba;
    uint64_t size;
    uint32_t export_cna;
    QTAILQ_ENTRY(ObmmExportEntry) next;
} ObmmExportEntry;

static QTAILQ_HEAD(, ObmmExportEntry) g_obmm_exports = QTAILQ_HEAD_INITIALIZER(g_obmm_exports);
static ObmmExportEntry *obmm_export_lookup(uint64_t uba, uint64_t len);

static bool ubc_cpu_window_detach(MemoryRegion *cpu_window)
{
    if (!cpu_window || !memory_region_is_mapped(cpu_window)) {
        return false;
    }
    memory_region_del_subregion(cpu_window->container, cpu_window);
    return true;
}

void ubc_handle_sim_dec_rx_write(BusControllerDev *ubc_dev,
                                 const UBCSimDecWritePldHdr *hdr,
                                 const uint8_t *data, uint32_t data_len)
{
    uint32_t eff_tid;
    MemTxResult ret;

    if (!ubc_dev || !hdr || !data || data_len == 0) {
        return;
    }
    if (hdr->data_len != data_len) {
        qemu_log("ubc sim_dec rx write: len mismatch hdr=%u data=%u\n",
                 hdr->data_len, data_len);
        return;
    }

    eff_tid = ubc_tid_or_auto(hdr->token_id);
    ret = ubc_dma_write_local_data_tid_strict(ubc_dev, hdr->remote_uba,
                                              data, data_len, eff_tid);
    if (ret != MEMTX_OK) {
        gsva_tables_init();
        GsvaRouteEntry *route = gsva_route_lookup_home_va(
            &g_gsva_routes, hdr->remote_uba, data_len);
        if (route && route->local_pa) {
            bool restore_cpu_window;
            uint64_t offset = hdr->remote_uba - route->key.home_va;
            uint64_t pa = route->local_pa + offset;
            restore_cpu_window = ubc_cpu_window_detach(&route->cpu_window);
            ret = address_space_write(&address_space_memory, pa,
                                      MEMTXATTRS_UNSPECIFIED,
                                      data, data_len);
            if (restore_cpu_window) {
                memory_region_add_subregion_overlap(get_system_memory(),
                                                    route->local_pa,
                                                    &route->cpu_window, 10);
            }
        } else {
            SimDecMapEntry *me = sim_dec_find_entry_by_uba(
                hdr->remote_uba, data_len);
            if (me && me->local_pa) {
                bool restore_cpu_window;
                uint64_t offset = hdr->remote_uba - me->remote_uba;
                uint64_t pa = me->local_pa + offset;
                restore_cpu_window = ubc_cpu_window_detach(&me->cpu_window);
                ret = address_space_write(&address_space_memory, pa,
                                          MEMTXATTRS_UNSPECIFIED,
                                          data, data_len);
                if (restore_cpu_window) {
                    memory_region_add_subregion_overlap(get_system_memory(),
                                                        me->local_pa,
                                                        &me->cpu_window, 10);
                }
            }
        }
        if (ret != MEMTX_OK) {
            /* OBMM export fallback: resolve backing_uba from export table. */
            ObmmExportEntry *exp = obmm_export_lookup(hdr->remote_uba, data_len);
            if (exp && exp->backing_uba) {
                uint64_t offset = hdr->remote_uba - exp->remote_uba;
                uint64_t backing_addr = exp->backing_uba + offset;
                ret = ubc_dma_write_local_data_tid_strict(ubc_dev, backing_addr,
                                                          data, data_len,
                                                          UBC_DMA_TID_AUTO);
                if (ret != MEMTX_OK) {
                    ret = address_space_write(&address_space_memory, backing_addr,
                                              MEMTXATTRS_UNSPECIFIED,
                                              data, data_len);
                }
            } else {
                ret = address_space_write(&address_space_memory, hdr->remote_uba,
                                          MEMTXATTRS_UNSPECIFIED,
                                          data, data_len);
            }
        }
    }
    if (ret != MEMTX_OK) {
        qemu_log("ubc sim_dec rx write: DMA write failed uba=%#" PRIx64
                 " len=%u tid=%u ret=%d\n",
                 (uint64_t)hdr->remote_uba, data_len, eff_tid, ret);
    }
}

void ubc_handle_sim_dec_rx_read_req(BusControllerDev *ubc_dev,
                                    const UBCSimDecReadReqPld *req,
                                    uint32_t dcna)
{
    UBLinkState *link;
    uint32_t payload_len;
    uint32_t eff_tid;
    uint8_t *payload;
    UBCSimDecReadRespPldHdr *resp;
    MemTxResult ret;
    int rc;

    if (!ubc_dev || !req || req->read_len == 0 ||
        req->read_len > UBC_SIM_DEC_READ_CHUNK_MAX) {
        return;
    }

    link = ubc_find_active_link(ubc_dev, &dcna);
    if (!link) {
        qemu_log("ubc sim_dec rx read_req: no active link dcna=%#x\n", dcna);
        return;
    }

    payload_len = sizeof(*resp) + req->read_len;
    payload = g_malloc0(payload_len);
    resp = (UBCSimDecReadRespPldHdr *)payload;
    resp->req_id = req->req_id;
    resp->status = 0;
    resp->data_len = req->read_len;
    eff_tid = ubc_tid_or_auto(req->token_id);
    if (ubc_trace_data_path_enabled()) {
        qemu_log("ubc sim_dec rx read_req req=%u uba=%#" PRIx64
                 " len=%u tid=%u dcna=%#x local_cna=%#x\n",
                 req->req_id, (uint64_t)req->remote_uba,
                 req->read_len, eff_tid, dcna, ubc_dev->parent.cna);
    }

    ret = ubc_dma_read_local_data_tid_strict(ubc_dev, req->remote_uba,
                                             payload + sizeof(*resp),
                                             req->read_len, eff_tid);
    if (ret != MEMTX_OK) {
        /*
         * The IOMMU (UMMU) may not have page table entries for GSVA
         * export UBA ranges.  Try looking up the GSVA route by home_va
         * to find the export memory's local_pa and read from there.
         */
        gsva_tables_init();
        GsvaRouteEntry *route = gsva_route_lookup_home_va(
            &g_gsva_routes, req->remote_uba, req->read_len);
        if (route && route->local_pa) {
            bool restore_cpu_window;
            uint64_t offset = req->remote_uba - route->key.home_va;
            uint64_t pa = route->local_pa + offset;
            restore_cpu_window = ubc_cpu_window_detach(&route->cpu_window);
            ret = address_space_read(&address_space_memory, pa,
                                     MEMTXATTRS_UNSPECIFIED,
                                     payload + sizeof(*resp),
                                     req->read_len);
            if (restore_cpu_window) {
                memory_region_add_subregion_overlap(get_system_memory(),
                                                    route->local_pa,
                                                    &route->cpu_window, 10);
            }
        } else {
            SimDecMapEntry *me = sim_dec_find_entry_by_uba(
                req->remote_uba, req->read_len);
            if (me && me->local_pa) {
                bool restore_cpu_window;
                uint64_t offset = req->remote_uba - me->remote_uba;
                uint64_t pa = me->local_pa + offset;
                restore_cpu_window = ubc_cpu_window_detach(&me->cpu_window);
                ret = address_space_read(&address_space_memory, pa,
                                         MEMTXATTRS_UNSPECIFIED,
                                         payload + sizeof(*resp),
                                         req->read_len);
                if (restore_cpu_window) {
                    memory_region_add_subregion_overlap(get_system_memory(),
                                                        me->local_pa,
                                                        &me->cpu_window, 10);
                }
            }
        }
        if (ret != MEMTX_OK) {
            /* OBMM export fallback: resolve backing_uba from export table. */
            ObmmExportEntry *exp = obmm_export_lookup(req->remote_uba,
                                                      req->read_len);
            if (exp && exp->backing_uba) {
                uint64_t offset = req->remote_uba - exp->remote_uba;
                uint64_t backing_addr = exp->backing_uba + offset;
                ret = ubc_dma_read_local_data_tid_strict(ubc_dev, backing_addr,
                                                         payload + sizeof(*resp),
                                                         req->read_len,
                                                         UBC_DMA_TID_AUTO);
                if (ret != MEMTX_OK) {
                    ret = address_space_read(&address_space_memory, backing_addr,
                                             MEMTXATTRS_UNSPECIFIED,
                                             payload + sizeof(*resp),
                                             req->read_len);
                }
            } else {
                ret = address_space_read(&address_space_memory, req->remote_uba,
                                         MEMTXATTRS_UNSPECIFIED,
                                         payload + sizeof(*resp),
                                         req->read_len);
            }
        }
    }
    if (ret != MEMTX_OK) {
        resp->status = 1;
        resp->data_len = 0;
        qemu_log("ubc sim_dec rx read_req dma_failed req=%u uba=%#" PRIx64
                 " len=%u tid=%u ret=%d\n",
                 req->req_id, (uint64_t)req->remote_uba,
                 req->read_len, eff_tid, ret);
    }

    rc = ubc_send_msg_over_link(ubc_dev, link, dcna,
                                UBC_MSG_SUB_SIM_DEC_READ_RESP,
                                payload, sizeof(*resp) + resp->data_len);
    if (rc < 0) {
        qemu_log("ubc sim_dec rx read_req: send resp failed req=%u\n",
                 req->req_id);
    } else {
        if (ubc_trace_data_path_enabled()) {
            qemu_log("ubc sim_dec rx read_req send_resp done req=%u status=%u"
                     " data_len=%u dcna=%#x local_cna=%#x\n",
                     req->req_id, resp->status, resp->data_len, dcna,
                     ubc_dev->parent.cna);
        }
    }
    g_free(payload);
}

typedef struct UbcSimDecModeledReadResponse {
    BusControllerDev *ubc_dev;
    UBCSimDecReadRespPldHdr header;
    uint32_t peer_cna;
    uint32_t data_len;
    uint8_t data[];
} UbcSimDecModeledReadResponse;

typedef struct UbcSimDecModeledAsyncResponse {
    BusControllerDev *ubc_dev;
    uint32_t child_slot;
    uint32_t req_id;
    UBCSimDecReadRespPldHdr header;
    uint32_t data_len;
    uint8_t data[];
} UbcSimDecModeledAsyncResponse;

static const char *ubc_obmm_remote_outcome_name(UbObmmRemoteOutcome outcome)
{
    switch (outcome) {
    case UB_OBMM_REMOTE_SUCCESS:
        return "success";
    case UB_OBMM_REMOTE_ERROR:
        return "error";
    case UB_OBMM_REMOTE_DROP:
        return "drop";
    default:
        return "invalid";
    }
}

static void ubc_deliver_sim_dec_read_resp(BusControllerDev *ubc_dev,
                                          const UBCSimDecReadRespPldHdr *hdr,
                                          const uint8_t *data,
                                          uint32_t data_len,
                                          uint32_t peer_cna)
{
    uint32_t copy_len;

    if (!ubc_dev || !hdr) {
        return;
    }
    if (!ubc_dev->sim_dec_sync_read.pending ||
        ubc_dev->sim_dec_sync_read.req_id != hdr->req_id ||
        ubc_dev->sim_dec_sync_read.peer_cna != peer_cna) {
        qemu_log("ubc sim_dec rx read_resp: stale req=%u peer=%#x pending=%u cur=%u cur_peer=%#x\n",
                 hdr->req_id, peer_cna, ubc_dev->sim_dec_sync_read.pending,
                 ubc_dev->sim_dec_sync_read.req_id,
                 ubc_dev->sim_dec_sync_read.peer_cna);
        return;
    }

    copy_len = MIN(hdr->data_len, ubc_dev->sim_dec_sync_read.expect_len);
    copy_len = MIN(copy_len, data_len);
    if (ubc_trace_data_path_enabled()) {
        qemu_log("ubc sim_dec rx read_resp req=%u status=%u data_len=%u"
                 " wire_len=%u copy_len=%u expect=%u peer=%#x\n",
                 hdr->req_id, hdr->status, hdr->data_len, data_len,
                 copy_len, ubc_dev->sim_dec_sync_read.expect_len, peer_cna);
    }
    if (copy_len > 0 && ubc_dev->sim_dec_sync_read.buf) {
        memcpy(ubc_dev->sim_dec_sync_read.buf, data, copy_len);
    }
    ubc_dev->sim_dec_sync_read.actual_len = copy_len;
    ubc_dev->sim_dec_sync_read.status = hdr->status ? -EIO : 0;
    ubc_dev->sim_dec_sync_read.pending = false;
    ubc_dev->sim_dec_sync_read.peer_cna = 0;
}

static void ubc_obmm_remote_model_due(
    void *opaque, const UbObmmRemoteDecision *decision,
    uint64_t model_accept_ns, uint64_t model_due_ns)
{
    UbcSimDecModeledReadResponse *response = opaque;

    trace_ub_obmm_model_due(
        decision->operation_key, response->header.req_id,
        ubc_obmm_remote_outcome_name(decision->outcome),
        model_accept_ns, model_due_ns);
}

static void ubc_obmm_remote_model_publish(
    void *opaque, const UbObmmRemoteDecision *decision, bool duplicate,
    uint64_t model_accept_ns, uint64_t model_due_ns,
    uint64_t model_publish_ns)
{
    UbcSimDecModeledReadResponse *response = opaque;
    UBCSimDecReadRespPldHdr header = response->header;
    const uint8_t *data = response->data;
    uint32_t data_len = response->data_len;

    if (decision->outcome == UB_OBMM_REMOTE_ERROR) {
        header.status = 1;
        header.data_len = 0;
        data = NULL;
        data_len = 0;
    }
    trace_ub_obmm_model_publish(
        decision->operation_key, header.req_id, duplicate,
        model_accept_ns, model_due_ns, model_publish_ns);
    ubc_deliver_sim_dec_read_resp(response->ubc_dev, &header, data,
                                  data_len, response->peer_cna);
}

static void ubc_obmm_remote_model_arm(BusControllerDev *ubc_dev)
{
    uint64_t next_due;

    if (!ubc_dev->remote_memory_model_timer) {
        return;
    }
    next_due = ub_obmm_remote_model_next_due_ns(
        &ubc_dev->remote_memory_model);
    if (next_due == UINT64_MAX) {
        timer_del(ubc_dev->remote_memory_model_timer);
    } else {
        timer_mod_ns(ubc_dev->remote_memory_model_timer, next_due);
    }
}

static void ubc_obmm_remote_model_timer(void *opaque)
{
    BusControllerDev *ubc_dev = opaque;
    uint64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    ub_obmm_remote_model_run_due(&ubc_dev->remote_memory_model, now_ns);
    ubc_obmm_remote_model_arm(ubc_dev);
}

static void ubc_obmm_async_model_due(
    void *opaque, const UbObmmRemoteDecision *decision,
    uint64_t model_accept_ns, uint64_t model_due_ns)
{
    UbcSimDecModeledAsyncResponse *response = opaque;

    trace_ub_obmm_model_due(
        decision->operation_key, response->req_id,
        ubc_obmm_remote_outcome_name(decision->outcome),
        model_accept_ns, model_due_ns);
}

static void ubc_obmm_async_model_publish(
    void *opaque, const UbObmmRemoteDecision *decision, bool duplicate,
    uint64_t model_accept_ns, uint64_t model_due_ns,
    uint64_t model_publish_ns)
{
    UbcSimDecModeledAsyncResponse *response = opaque;
    UbcObmmAsyncChild *child;
    ObmmRemoteStatus status;
    uint32_t bytes_done;

    child = &response->ubc_dev->obmm_async_children[response->child_slot];
    if (!child->active || child->req_id != response->req_id) {
        trace_ub_obmm_p1_late(
            response->req_id, "model-duplicate",
            decision->operation_key);
        return;
    }
    status = response->header.status == 0 &&
        decision->outcome == UB_OBMM_REMOTE_SUCCESS ?
        OBMM_REMOTE_STATUS_SUCCESS : OBMM_REMOTE_STATUS_REMOTE_IO;
    bytes_done = status == OBMM_REMOTE_STATUS_SUCCESS ?
        MIN(response->header.data_len, response->data_len) : 0;
    trace_ub_obmm_model_publish(
        decision->operation_key, response->req_id, duplicate,
        model_accept_ns, model_due_ns, model_publish_ns);
    child->complete(child->opaque, child->token, child->child_index,
                    status, response->data, bytes_done, model_publish_ns);
    memset(child, 0, sizeof(*child));
}

static bool ubc_handle_sim_dec_async_read_resp(
    BusControllerDev *ubc_dev, const UBCSimDecReadRespPldHdr *header,
    const uint8_t *data, uint32_t data_len, uint32_t peer_cna)
{
    UbcObmmAsyncChild *child = NULL;
    UbcSimDecModeledAsyncResponse *response;
    UbObmmRemoteDecision decision;
    uint64_t now_ns;
    uint32_t child_slot;

    if (!ubc_dev || !ubc_dev->obmm_async_children) {
        return false;
    }
    for (child_slot = 0; child_slot < UBC_OBMM_ASYNC_CHILD_CAPACITY;
         child_slot++) {
        UbcObmmAsyncChild *candidate =
            &ubc_dev->obmm_async_children[child_slot];

        if (candidate->active && candidate->req_id == header->req_id &&
            candidate->peer_cna == peer_cna) {
            child = candidate;
            break;
        }
    }
    if (!child) {
        return false;
    }
    if (child->model_queued) {
        trace_ub_obmm_p1_late(header->req_id, "wire-duplicate", 0);
        return true;
    }
    now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (!ubc_dev->remote_memory_model.loaded || header->status != 0) {
        ObmmRemoteStatus status = header->status == 0 ?
            OBMM_REMOTE_STATUS_SUCCESS : OBMM_REMOTE_STATUS_REMOTE_IO;
        uint32_t bytes_done = status == OBMM_REMOTE_STATUS_SUCCESS ?
            MIN(header->data_len, data_len) : 0;

        child->complete(child->opaque, child->token, child->child_index,
                        status, data, bytes_done, now_ns);
        memset(child, 0, sizeof(*child));
        return true;
    }

    response = g_malloc0(sizeof(*response) + data_len);
    response->ubc_dev = ubc_dev;
    response->child_slot = child_slot;
    response->req_id = header->req_id;
    response->header = *header;
    response->data_len = data_len;
    if (data_len > 0) {
        memcpy(response->data, data, data_len);
    }
    if (!ub_obmm_remote_model_enqueue(
            &ubc_dev->remote_memory_model, &child->operation, now_ns,
            ubc_obmm_async_model_due, ubc_obmm_async_model_publish,
            g_free, response, &decision)) {
        g_free(response);
        child->complete(child->opaque, child->token, child->child_index,
                        OBMM_REMOTE_STATUS_CAPACITY, NULL, 0, now_ns);
        memset(child, 0, sizeof(*child));
        return true;
    }
    child->model_queued = true;
    trace_ub_obmm_model_accept(
        decision.operation_key, header->req_id, now_ns);
    if (ub_obmm_remote_model_next_due_ns(
            &ubc_dev->remote_memory_model) <= now_ns) {
        ub_obmm_remote_model_run_due(&ubc_dev->remote_memory_model, now_ns);
    }
    ubc_obmm_remote_model_arm(ubc_dev);
    return true;
}

void ubc_handle_sim_dec_rx_read_resp(BusControllerDev *ubc_dev,
                                     const UBCSimDecReadRespPldHdr *hdr,
                                     const uint8_t *data, uint32_t data_len,
                                     uint32_t peer_cna)
{
    UbcSimDecModeledReadResponse *response;
    UbObmmRemoteOperation operation;
    UbObmmRemoteDecision decision;
    uint64_t now_ns;

    if (!ubc_dev || !hdr) {
        return;
    }
    if (ubc_handle_sim_dec_async_read_resp(ubc_dev, hdr, data, data_len,
                                            peer_cna)) {
        return;
    }
    if (!ubc_dev->remote_memory_model.loaded || hdr->status != 0) {
        ubc_deliver_sim_dec_read_resp(ubc_dev, hdr, data, data_len,
                                      peer_cna);
        return;
    }
    if (!ubc_dev->sim_dec_sync_read.pending ||
        ubc_dev->sim_dec_sync_read.req_id != hdr->req_id ||
        ubc_dev->sim_dec_sync_read.peer_cna != peer_cna ||
        ubc_dev->sim_dec_sync_read.model_queued) {
        ubc_deliver_sim_dec_read_resp(ubc_dev, hdr, data, data_len,
                                      peer_cna);
        return;
    }

    response = g_malloc0(sizeof(*response) + data_len);
    response->ubc_dev = ubc_dev;
    response->header = *hdr;
    response->peer_cna = peer_cna;
    response->data_len = data_len;
    if (data_len > 0) {
        memcpy(response->data, data, data_len);
    }
    operation = (UbObmmRemoteOperation) {
        .map_id = ubc_dev->sim_dec_sync_read.map_id,
        .map_generation = ubc_dev->sim_dec_sync_read.map_generation,
        .remote_offset = ubc_dev->sim_dec_sync_read.remote_offset,
        .length = ubc_dev->sim_dec_sync_read.expect_len,
        .per_range_ordinal =
            ubc_dev->sim_dec_sync_read.per_range_ordinal,
    };
    now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (!ub_obmm_remote_model_enqueue(
            &ubc_dev->remote_memory_model, &operation, now_ns,
            ubc_obmm_remote_model_due, ubc_obmm_remote_model_publish,
            g_free, response, &decision)) {
        g_free(response);
        ubc_dev->sim_dec_sync_read.status = -EAGAIN;
        ubc_dev->sim_dec_sync_read.actual_len = 0;
        ubc_dev->sim_dec_sync_read.pending = false;
        ubc_dev->sim_dec_sync_read.peer_cna = 0;
        trace_ub_obmm_model_capacity(hdr->req_id, now_ns);
        return;
    }
    ubc_dev->sim_dec_sync_read.model_queued = true;
    trace_ub_obmm_model_accept(decision.operation_key, hdr->req_id,
                               now_ns);
    if (ub_obmm_remote_model_next_due_ns(
            &ubc_dev->remote_memory_model) <= now_ns) {
        ub_obmm_remote_model_run_due(&ubc_dev->remote_memory_model, now_ns);
    }
    ubc_obmm_remote_model_arm(ubc_dev);
}

int ubc_ub_ssd_send_read_resp(BusControllerDev *ubc_dev, uint32_t dcna,
                              const UBCUbSsdReadRespPldHdr *hdr,
                              const uint8_t *data, uint32_t data_len)
{
    UBLinkState *link;
    uint8_t *payload;
    uint32_t payload_len;
    int rc;

    if (!ubc_dev || !hdr || (data_len > 0 && !data) ||
        data_len > UBC_SIM_DEC_MAX_MSG_PAYLOAD - sizeof(*hdr)) {
        return -EINVAL;
    }
    if (hdr->magic != UBC_UB_SSD_READ_RESP_MAGIC) {
        return -EINVAL;
    }
    link = ubc_find_active_link(ubc_dev, &dcna);
    if (!link) {
        qemu_log("UB_SSD_REMOTE: read_resp no active link dcna=%#x\n", dcna);
        return -ENODEV;
    }
    payload_len = sizeof(*hdr) + data_len;
    payload = g_malloc0(payload_len);
    memcpy(payload, hdr, sizeof(*hdr));
    if (data_len > 0) {
        memcpy(payload + sizeof(*hdr), data, data_len);
    }
    rc = ubc_send_msg_over_link(ubc_dev, link, dcna,
                                UBC_MSG_SUB_UB_SSD_READ_RESP,
                                payload, payload_len);
    g_free(payload);
    return rc;
}

void ubc_handle_ub_ssd_rx_read_resp(BusControllerDev *ubc_dev,
                                    const UBCUbSsdReadRespPldHdr *hdr,
                                    const uint8_t *data,
                                    uint32_t data_len,
                                    uint32_t peer_cna)
{
    uint32_t copy_len;

    if (!ubc_dev || !hdr) {
        return;
    }
    if (!ubc_dev->sim_dec_sync_read.pending ||
        ubc_dev->sim_dec_sync_read.req_id != hdr->req_id ||
        ubc_dev->sim_dec_sync_read.peer_cna != peer_cna) {
        qemu_log("UB_SSD_REMOTE: stale read_resp req=%u peer=%#x"
                 " pending=%u cur=%u cur_peer=%#x\n",
                 hdr->req_id, peer_cna, ubc_dev->sim_dec_sync_read.pending,
                 ubc_dev->sim_dec_sync_read.req_id,
                 ubc_dev->sim_dec_sync_read.peer_cna);
        return;
    }

    copy_len = MIN(hdr->data_len, ubc_dev->sim_dec_sync_read.expect_len);
    copy_len = MIN(copy_len, data_len);
    if (copy_len > 0 && ubc_dev->sim_dec_sync_read.buf) {
        memcpy(ubc_dev->sim_dec_sync_read.buf, data, copy_len);
    }
    ubc_dev->sim_dec_sync_read.actual_len = copy_len;
    ubc_dev->sim_dec_sync_read.status = hdr->status;
    ubc_dev->sim_dec_sync_read.block_version = hdr->block_version;
    ubc_dev->sim_dec_sync_read.block_bytes = hdr->block_bytes;
    ubc_dev->sim_dec_sync_read.checksum64 = hdr->checksum64;
    ubc_dev->sim_dec_sync_read.pending = false;
    ubc_dev->sim_dec_sync_read.peer_cna = 0;
}

int ubc_ub_ssd_remote_block_read(BusControllerDev *ubc_dev,
                                 uint32_t target_ssd_cna,
                                 uint32_t source_cna,
                                 const UBCUbSsdBlockRefPld *block_ref,
                                 uint8_t *data,
                                 uint32_t len,
                                 uint64_t *block_version,
                                 uint64_t *block_bytes,
                                 uint64_t *checksum64)
{
    UBLinkState *link;
    BusControllerState *bcs;
    uint32_t owner_cna = ubc_primary_cna_from_ub_ssd_cna(target_ssd_cna);
    uint32_t dcna = owner_cna ? owner_cna : target_ssd_cna;
    uint32_t response_peer_cna;
    uint32_t done = 0;

    if (!ubc_dev || !block_ref || !data || len == 0 || target_ssd_cna == 0) {
        return -EINVAL;
    }
    link = ubc_find_active_link(ubc_dev, &dcna);
    if (!link) {
        qemu_log("UB_SSD_REMOTE: read no active link target_ssd_cna=%#x\n",
                 target_ssd_cna);
        return -ENODEV;
    }
    bcs = container_of_ubbus(ub_get_bus(&ubc_dev->parent));
    if (!bcs) {
        return -ENODEV;
    }
    response_peer_cna = owner_cna;
    if (response_peer_cna == 0) {
        response_peer_cna = ubc_remote_cna_from_link(&ubc_dev->parent, link);
    }
    if (response_peer_cna == 0) {
        response_peer_cna = dcna;
    }

    while (done < len) {
        UBCUbSsdReadReqPld req = { 0 };
        uint32_t chunk = MIN(len - done,
                             UBC_SIM_DEC_MAX_MSG_PAYLOAD -
                             (uint32_t)sizeof(UBCUbSsdReadRespPldHdr));
        int rc;
        int loop;

        if (ubc_dev->sim_dec_sync_read.pending) {
            qemu_log("UB_SSD_REMOTE: another sync read pending req=%u\n",
                     ubc_dev->sim_dec_sync_read.req_id);
            return -EBUSY;
        }
        ubc_dev->sim_dec_sync_read.pending = true;
        ubc_dev->sim_dec_sync_read.req_id = ++ubc_dev->next_sim_dec_read_req_id;
        if (ubc_dev->sim_dec_sync_read.req_id == 0) {
            ubc_dev->sim_dec_sync_read.req_id =
                ++ubc_dev->next_sim_dec_read_req_id;
        }
        ubc_dev->sim_dec_sync_read.peer_cna = response_peer_cna;
        ubc_dev->sim_dec_sync_read.expect_len = chunk;
        ubc_dev->sim_dec_sync_read.actual_len = 0;
        ubc_dev->sim_dec_sync_read.status = -ETIMEDOUT;
        ubc_dev->sim_dec_sync_read.block_version = 0;
        ubc_dev->sim_dec_sync_read.block_bytes = 0;
        ubc_dev->sim_dec_sync_read.checksum64 = 0;
        ubc_dev->sim_dec_sync_read.buf = data + done;

        req.magic = UBC_UB_SSD_READ_REQ_MAGIC;
        req.req_id = ubc_dev->sim_dec_sync_read.req_id;
        req.target_ssd_cna = target_ssd_cna;
        req.source_cna = source_cna;
        req.read_offset = done;
        req.read_len = chunk;
        req.block_ref = *block_ref;

        rc = ubc_send_msg_over_link(ubc_dev, link, dcna,
                                    UBC_MSG_SUB_UB_SSD_READ_REQ,
                                    &req, sizeof(req));
        if (rc < 0) {
            ubc_dev->sim_dec_sync_read.pending = false;
            return rc;
        }

        for (loop = 0; loop < UBC_SIM_DEC_READ_WAIT_LOOPS; loop++) {
            if (!ubc_dev->sim_dec_sync_read.pending) {
                break;
            }
            ub_fm_poll_rx_links_now();
            if (!ubc_dev->sim_dec_sync_read.pending) {
                break;
            }
            ubc_sim_dec_process_wait_links(bcs, ubc_dev, link);
            if (!ubc_dev->sim_dec_sync_read.pending) {
                break;
            }
            g_usleep(link->shmem_ready ? UBC_SIM_DEC_SHM_READ_WAIT_USEC :
                                         UBC_SIM_DEC_READ_WAIT_USEC);
        }

        if (ubc_dev->sim_dec_sync_read.pending) {
            qemu_log("UB_SSD_REMOTE: read timeout req=%u target=%#x"
                     " offset=%u len=%u\n",
                     req.req_id, target_ssd_cna, done, chunk);
            ubc_dev->sim_dec_sync_read.pending = false;
            ubc_dev->sim_dec_sync_read.peer_cna = 0;
            return -ETIMEDOUT;
        }
        if (ubc_dev->sim_dec_sync_read.status != 0 ||
            ubc_dev->sim_dec_sync_read.actual_len != chunk) {
            qemu_log("UB_SSD_REMOTE: bad read_resp req=%u status=%d"
                     " actual=%u expect=%u\n",
                     req.req_id, ubc_dev->sim_dec_sync_read.status,
                     ubc_dev->sim_dec_sync_read.actual_len, chunk);
            return ubc_dev->sim_dec_sync_read.status ?
                ubc_dev->sim_dec_sync_read.status : -EIO;
        }
        if (block_version) {
            *block_version = ubc_dev->sim_dec_sync_read.block_version;
        }
        if (block_bytes) {
            *block_bytes = ubc_dev->sim_dec_sync_read.block_bytes;
        }
        if (checksum64) {
            *checksum64 = ubc_dev->sim_dec_sync_read.checksum64;
        }
        done += chunk;
    }
    qemu_log("UB_SSD_REMOTE: read ok target=%#x block=%#" PRIx64
             ":%#" PRIx64 " bytes=%u\n",
             target_ssd_cna, block_ref->block_hi, block_ref->block_lo, len);
    return 0;
}

MemTxResult ubc_sim_dec_remote_write(BusControllerDev *ubc_dev,
                                     uint64_t remote_uba,
                                     uint32_t token_id,
                                     uint32_t dcna,
                                     const uint8_t *data,
                                     uint32_t len)
{
    UBLinkState *link;
    uint32_t done = 0;

    if (!ubc_dev || !data || !len || dcna == 0) {
        return MEMTX_DECODE_ERROR;
    }
    link = ubc_find_active_link(ubc_dev, &dcna);
    if (!link) {
        qemu_log("ubc sim_dec write: no active link\n");
        return MEMTX_DECODE_ERROR;
    }

    while (done < len) {
        uint32_t chunk = MIN(len - done, UBC_SIM_DEC_WRITE_CHUNK_MAX);
        uint32_t payload_len = sizeof(UBCSimDecWritePldHdr) + chunk;
        uint8_t *payload = g_malloc(payload_len);
        UBCSimDecWritePldHdr *hdr = (UBCSimDecWritePldHdr *)payload;
        int rc;

        hdr->remote_uba = remote_uba + done;
        hdr->token_id = token_id;
        hdr->data_len = chunk;
        memcpy(payload + sizeof(*hdr), data + done, chunk);

        rc = ubc_send_msg_over_link(ubc_dev, link, dcna,
                                    UBC_MSG_SUB_SIM_DEC_WRITE,
                                    payload, payload_len);
        g_free(payload);
        if (rc < 0) {
            return MEMTX_DECODE_ERROR;
        }
        done += chunk;
    }

    if (g_sim_decoder) {
        g_sim_decoder->stats.remote_writes++;
        g_sim_decoder->stats.remote_write_bytes += len;
    }
    return MEMTX_OK;
}

bool ubc_obmm_resolve_async_map(BusControllerDev *ubc_dev,
                                uint64_t local_pa, uint64_t length,
                                UbcObmmResolvedMap *resolved)
{
    SimDecMapEntry *entry;
    uint64_t offset;
    bool found = false;

    if (!ubc_dev || !resolved || !g_sim_decoder || length == 0 ||
        local_pa > UINT64_MAX - length) {
        return false;
    }
    qemu_mutex_lock(&g_sim_decoder->lock);
    entry = sim_dec_find_entry_by_pa(local_pa);
    if (!entry || !entry->active || local_pa < entry->local_pa) {
        goto out;
    }
    offset = local_pa - entry->local_pa;
    if (offset > entry->size || length > entry->size - offset ||
        entry->remote_uba > UINT64_MAX - offset ||
        entry->remote_uba + offset > UINT64_MAX - length) {
        goto out;
    }
    *resolved = (UbcObmmResolvedMap) {
        .map_id = entry->map_id,
        .map_generation = entry->gva_id ? entry->gva_id : entry->map_id,
        .local_pa = local_pa,
        .remote_uba = entry->remote_uba + offset,
        .length = length,
        .token_id = entry->token_id,
        .peer_cna = entry->dcna,
        .access_flags = entry->access_flags,
    };
    found = true;
out:
    qemu_mutex_unlock(&g_sim_decoder->lock);
    return found;
}

bool ubc_sim_dec_remote_read_async_submit(
    BusControllerDev *ubc_dev, const UbcObmmResolvedMap *map,
    uint64_t remote_offset, uint32_t length,
    const UbObmmRemoteOperation *operation, ObmmRemoteToken token,
    uint16_t child_index, UbcObmmAsyncReadCompleteFn complete,
    void *opaque)
{
    UbcObmmAsyncChild *child = NULL;
    UBCSimDecReadReqPld request = { 0 };
    UBLinkState *link;
    uint32_t peer_cna;
    uint32_t index;
    int rc;

    if (!ubc_dev || !map || !operation || !complete || length == 0 ||
        remote_offset > map->length || length > map->length - remote_offset) {
        return false;
    }
    peer_cna = map->peer_cna;
    link = ubc_find_active_link(ubc_dev, &peer_cna);
    if (!link) {
        return false;
    }
    for (index = 0; index < UBC_OBMM_ASYNC_CHILD_CAPACITY; index++) {
        if (!ubc_dev->obmm_async_children[index].active) {
            child = &ubc_dev->obmm_async_children[index];
            break;
        }
    }
    if (!child) {
        return false;
    }
    request.req_id = ++ubc_dev->next_sim_dec_read_req_id;
    if (request.req_id == 0) {
        request.req_id = ++ubc_dev->next_sim_dec_read_req_id;
    }
    request.token_id = map->token_id;
    request.remote_uba = map->remote_uba + remote_offset;
    request.read_len = length;
    *child = (UbcObmmAsyncChild) {
        .active = true,
        .req_id = request.req_id,
        .peer_cna = peer_cna,
        .expected_len = length,
        .token = token,
        .child_index = child_index,
        .operation = *operation,
        .complete = complete,
        .opaque = opaque,
    };
    rc = ubc_send_msg_over_link(ubc_dev, link, peer_cna,
                                UBC_MSG_SUB_SIM_DEC_READ_REQ,
                                &request, sizeof(request));
    if (rc < 0) {
        memset(child, 0, sizeof(*child));
        return false;
    }
    return true;
}

void ubc_sim_dec_remote_read_async_cancel(BusControllerDev *ubc_dev,
                                          ObmmRemoteToken token)
{
    uint32_t index;

    if (!ubc_dev || !ubc_dev->obmm_async_children) {
        return;
    }
    for (index = 0; index < UBC_OBMM_ASYNC_CHILD_CAPACITY; index++) {
        UbcObmmAsyncChild *child = &ubc_dev->obmm_async_children[index];

        if (child->active && child->token.owner_id == token.owner_id &&
            child->token.slot == token.slot &&
            child->token.generation == token.generation) {
            memset(child, 0, sizeof(*child));
        }
    }
}

void ubc_obmm_async_irq_notify(BusControllerDev *ubc_dev)
{
    BusControllerState *bcs;

    if (!ubc_dev) {
        return;
    }
    bcs = container_of_ubbus(ub_get_bus(&ubc_dev->parent));
    if (!bcs) {
        return;
    }
    qemu_set_irq(bcs->irq, 1);
    qemu_set_irq(bcs->irq, 0);
}

MemTxResult ubc_sim_dec_remote_read(BusControllerDev *ubc_dev,
                                    uint64_t remote_uba,
                                    uint32_t token_id,
                                    uint32_t dcna,
                                    uint8_t *data,
                                    uint32_t len)
{
    UBLinkState *link;
    BusControllerState *bcs;
    uint32_t done = 0;

    if (!ubc_dev || !data || !len || dcna == 0) {
        return MEMTX_DECODE_ERROR;
    }
    link = ubc_find_active_link(ubc_dev, &dcna);
    if (!link) {
        qemu_log("ubc sim_dec read: no active link\n");
        return MEMTX_DECODE_ERROR;
    }
    bcs = container_of_ubbus(ub_get_bus(&ubc_dev->parent));
    if (!bcs) {
        return MEMTX_DECODE_ERROR;
    }

    while (done < len) {
        UBCSimDecReadReqPld req = { 0 };
        SimDecMapEntry *model_entry;
        uint32_t chunk = MIN(len - done, UBC_SIM_DEC_READ_CHUNK_MAX);
        int rc;
        int loop;
        if (ubc_dev->sim_dec_sync_read.pending) {
            qemu_log("ubc sim_dec read: another sync read pending req=%u\n",
                     ubc_dev->sim_dec_sync_read.req_id);
            return MEMTX_DECODE_ERROR;
        }

        ubc_dev->sim_dec_sync_read.pending = true;
        ubc_dev->sim_dec_sync_read.model_queued = false;
        ubc_dev->sim_dec_sync_read.req_id = ++ubc_dev->next_sim_dec_read_req_id;
        if (ubc_dev->sim_dec_sync_read.req_id == 0) {
            ubc_dev->sim_dec_sync_read.req_id = ++ubc_dev->next_sim_dec_read_req_id;
        }
        ubc_dev->sim_dec_sync_read.peer_cna = dcna;
        ubc_dev->sim_dec_sync_read.expect_len = chunk;
        ubc_dev->sim_dec_sync_read.actual_len = 0;
        ubc_dev->sim_dec_sync_read.status = -ETIMEDOUT;
        ubc_dev->sim_dec_sync_read.buf = data + done;
        model_entry = sim_dec_find_entry_by_uba(remote_uba + done, chunk);
        if (model_entry) {
            ubc_dev->sim_dec_sync_read.map_id = model_entry->map_id;
            ubc_dev->sim_dec_sync_read.map_generation =
                model_entry->gva_id ? model_entry->gva_id :
                model_entry->map_id;
            ubc_dev->sim_dec_sync_read.remote_offset =
                remote_uba + done - model_entry->remote_uba;
            ubc_dev->sim_dec_sync_read.per_range_ordinal =
                model_entry->remote_read_ordinal++;
        } else {
            ubc_dev->sim_dec_sync_read.map_id = token_id;
            ubc_dev->sim_dec_sync_read.map_generation = 1;
            ubc_dev->sim_dec_sync_read.remote_offset = remote_uba + done;
            ubc_dev->sim_dec_sync_read.per_range_ordinal =
                ubc_dev->next_sim_dec_read_req_id;
        }

        req.req_id = ubc_dev->sim_dec_sync_read.req_id;
        req.token_id = token_id;
        req.remote_uba = remote_uba + done;
        req.read_len = chunk;
        if (ubc_trace_data_path_enabled()) {
            qemu_log("ubc sim_dec read send req=%u uba=%#" PRIx64
                     " len=%u done=%u dcna=%#x token=%u local_cna=%#x\n",
                     req.req_id, (uint64_t)req.remote_uba, req.read_len,
                     done, dcna, token_id, ubc_dev->parent.cna);
        }
        rc = ubc_send_msg_over_link(ubc_dev, link, dcna,
                                    UBC_MSG_SUB_SIM_DEC_READ_REQ,
                                    &req, sizeof(req));
        if (rc < 0) {
            ubc_dev->sim_dec_sync_read.pending = false;
            return MEMTX_DECODE_ERROR;
        }

        for (loop = 0; loop < UBC_SIM_DEC_READ_WAIT_LOOPS; loop++) {
            if (!ubc_dev->sim_dec_sync_read.pending) {
                break;
            }
            ub_fm_poll_rx_links_now();
            if (!ubc_dev->sim_dec_sync_read.pending) {
                break;
            }
            ubc_sim_dec_process_wait_links(bcs, ubc_dev, link);
            if (!ubc_dev->sim_dec_sync_read.pending) {
                break;
            }
            qemu_clock_run_timers(QEMU_CLOCK_VIRTUAL);
            if (!ubc_dev->sim_dec_sync_read.pending) {
                break;
            }
            g_usleep(link->shmem_ready ? UBC_SIM_DEC_SHM_READ_WAIT_USEC :
                                         UBC_SIM_DEC_READ_WAIT_USEC);
        }

        if (ubc_dev->sim_dec_sync_read.pending) {
            qemu_log("ubc sim_dec read: timeout req=%u uba=%#" PRIx64
                     " len=%u done=%u dcna=%#x token=%u local_cna=%#x"
                     " status=%d actual=%u expect=%u\n",
                     req.req_id, (uint64_t)req.remote_uba, req.read_len,
                     done, dcna, token_id, ubc_dev->parent.cna,
                     ubc_dev->sim_dec_sync_read.status,
                     ubc_dev->sim_dec_sync_read.actual_len,
                     ubc_dev->sim_dec_sync_read.expect_len);
            if (g_sim_decoder) {
                g_sim_decoder->stats.read_timeouts++;
            }
            ubc_dev->sim_dec_sync_read.pending = false;
            ubc_dev->sim_dec_sync_read.peer_cna = 0;
            return MEMTX_DECODE_ERROR;
        }
        if (ubc_dev->sim_dec_sync_read.status != 0 ||
            ubc_dev->sim_dec_sync_read.actual_len != chunk) {
            qemu_log("ubc sim_dec read: bad resp req=%u status=%d len=%u expect=%u\n",
                     req.req_id, ubc_dev->sim_dec_sync_read.status,
                     ubc_dev->sim_dec_sync_read.actual_len, chunk);
            return MEMTX_DECODE_ERROR;
        }
        done += chunk;
    }

    if (g_sim_decoder) {
        g_sim_decoder->stats.remote_reads++;
        g_sim_decoder->stats.remote_read_bytes += len;
    }
    return MEMTX_OK;
}

/*
 * Send URMA data payload over ub_link to the remote node.
 * Wraps the data in a MsgPktHeader with msg_code=UB_MSG_CODE_URMA_DATA
 * so the receive side can distinguish it from control messages.
 *
 * Packet layout: [MsgPktHeader (32 bytes)] [URMA data (payload)]
 * The URMA data payload starts at offset 32 and contains the raw data.
 * src_jetty is in MsgPktHeader.sjetty, dst_jetty is in MsgPktHeader.djetty
 * (using the sjetty/rjetty fields at DW6).
 *
 * For RDMA WRITE: if remote_addr != 0, uses UB_MSG_CODE_URMA_WRITE and
 * prepends UBCWritePayloadHdr before the actual data.
 */
static void ubc_send_data_to_remote_ex(BusControllerDev *ubc_dev, UBCJettyState *js,
                                       const uint8_t *payload, uint32_t payload_len,
                                       uint32_t rmt_obj_id, const uint8_t *rmt_eid,
                                       uint64_t remote_addr, bool is_write)
{
    UBDevice *ub_dev = &ubc_dev->parent;
    UBFMManagedLink *fm_link = NULL;
    UBLinkState *link;
    Error *local_err = NULL;
    uint32_t dcna = 0;
    uint8_t mcast_group = 0;
    bool is_mcast = ubc_extract_link_local_mcast_group(rmt_eid, &mcast_group);

    if (ubc_trace_data_path_enabled()) {
        qemu_log("ubc SEND: jetty=%u rmt_obj_id=%u payload_len=%u rmt_eid=%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\n",
                 js->jetty_id, rmt_obj_id, payload_len,
                 rmt_eid[0], rmt_eid[1], rmt_eid[2], rmt_eid[3],
                 rmt_eid[4], rmt_eid[5], rmt_eid[6], rmt_eid[7],
                 rmt_eid[8], rmt_eid[9], rmt_eid[10], rmt_eid[11],
                 rmt_eid[12], rmt_eid[13], rmt_eid[14], rmt_eid[15]);
    }
    if (ubc_trace_data_path_enabled() && payload && payload_len > 0) {
        uint32_t dump = payload_len < 32 ? payload_len : 32;
        GString *hex = g_string_new(NULL);
        uint32_t di;
        for (di = 0; di < dump; di++) {
            g_string_append_printf(hex, "%02x", payload[di]);
            if (di + 1 < dump) {
                g_string_append_c(hex, ':');
            }
        }
        qemu_log("ubc SEND payload[0:%u]=%s\n", dump, hex->str);
        g_string_free(hex, true);
    }

    if (!is_mcast) {
        ubc_try_fill_dcna_from_rmt_eid(ub_dev, rmt_eid, &dcna);
        if (dcna == 0) {
            qemu_log("ubc SEND: unresolved remote dcna for jetty=%u rmt_obj_id=%u; refusing fallback send\n",
                     js->jetty_id, rmt_obj_id);
            return;
        }
        fm_link = ubc_find_fm_link(ub_dev, &dcna);

        if (!fm_link) {
            qemu_log("ubc SEND: no FM link found (local_cna=%#x), skipping send\n",
                     ub_dev->cna);
            return;
        }

        link = fm_link->runtime;
        if (!ubc_link_has_transport(link)) {
            if (ubc_trace_data_path_enabled()) {
                qemu_log("ubc SEND: link not up or no ioc, skipping send\n");
            }
            return;
        }
    }

    /*
     * Build a MsgPktHeader-wrapped URMA data packet.
     * The URMA data payload goes in the payload[] section of MsgPktHeader.
     * We use msg_code=7 for the receive side to distinguish from control messages.
     *
     * For WRITE: prepend UBCWritePayloadHdr (8 bytes) before payload
     */
    {
        size_t write_hdr_len = is_write ? sizeof(UBCWritePayloadHdr) : 0;
        size_t total_len = sizeof(MsgPktHeader) + write_hdr_len + payload_len;
        uint8_t *pkt = g_malloc0(total_len);
        MsgPktHeader *hdr = (MsgPktHeader *)pkt;

        /* Fill UbLinkHeader (DW0) */
        hdr->ulh.cfg = UB_CLAN_LINK_CFG;
        hdr->ulh.plen = write_hdr_len + payload_len;

        /* Fill ClanNetworkHeader (DW1-DW2) */
        hdr->nth.scna = ub_dev->cna;
        hdr->nth.dcna = dcna;

        /* DW6: sjetty/rjetty */
        hdr->sjetty = js->jetty_id;
        hdr->jetty_en = 1;
        /* Store dst_jetty in the deid field (DW4) */
        hdr->deid = rmt_obj_id & 0xFFFFF;

        /* MsgExtendedHeader (DW7): use different msg_code for WRITE vs SEND */
        hdr->msgetah.msg_code = is_write ? UB_MSG_CODE_URMA_WRITE : UBC_MSG_CODE_URMA_DATA;
        hdr->msgetah.sub_msg_code = is_write ? 0 : UBC_MSG_SUB_URMA_DATA;
        hdr->msgetah.type = 1;  /* data (not request) */
        hdr->msgetah.plen = (write_hdr_len + payload_len) & 0xFFF;

        /* For WRITE: prepend remote_addr header */
        if (is_write) {
            UBCWritePayloadHdr *write_hdr = (UBCWritePayloadHdr *)(pkt + sizeof(MsgPktHeader));
            write_hdr->remote_addr = remote_addr;
            memcpy(pkt + sizeof(MsgPktHeader) + sizeof(UBCWritePayloadHdr), payload, payload_len);
            if (ubc_trace_data_path_enabled()) {
                qemu_log("ubc SEND WRITE: remote_addr=%#" PRIx64 " payload_len=%u\n",
                         remote_addr, payload_len);
            }
        } else {
            /* Copy payload after the MsgPktHeader */
            memcpy(pkt + sizeof(MsgPktHeader), payload, payload_len);
        }

        if (is_mcast) {
            uint32_t i;
            bool sent_any = false;

            if (ubc_trace_data_path_enabled()) {
                qemu_log("ubc SEND: multicast group=%u fanout start\n", mcast_group);
            }
            for (i = 0; i < ub_dev->port.port_num; i++) {
                NeighborInfo *ni = &ub_dev->port.neighbors[i];
                uint32_t fanout_dcna;
                UBFMManagedLink *fanout_fm_link;
                UBLinkState *fanout_link;
                int rc;

                if (!ni->is_remote_neighbor || !ni->remote_primary_cna_valid) {
                    continue;
                }
                fanout_dcna = ni->remote_primary_cna;
                fanout_fm_link = ubc_find_fm_link(ub_dev, &fanout_dcna);
                if (!fanout_fm_link || !fanout_fm_link->runtime) {
                    continue;
                }
                fanout_link = fanout_fm_link->runtime;
                if (!ubc_link_has_transport(fanout_link)) {
                    continue;
                }

                hdr->nth.dcna = fanout_dcna;
                local_err = NULL;
                rc = ub_link_write_message(fanout_link, pkt, total_len, &local_err);
                if (rc < 0) {
                    qemu_log("ubc SEND: multicast fanout to %s failed: %s\n",
                             ni->neighbor_id[0] ? ni->neighbor_id : "<unknown>",
                             local_err ? error_get_pretty(local_err) : "unknown");
                    if (local_err) {
                        error_free(local_err);
                    }
                    continue;
                }
                sent_any = true;
                if (ubc_trace_data_path_enabled()) {
                    qemu_log("ubc SEND: multicast sent %zu bytes to %s dcna=%#x\n",
                             total_len,
                             ni->neighbor_id[0] ? ni->neighbor_id : "<unknown>",
                             fanout_dcna);
                }
                ub_link_kick_remote(fanout_link, NULL);
            }
            if (!sent_any && ubc_trace_data_path_enabled()) {
                qemu_log("ubc SEND: multicast group=%u had no active remote links\n",
                         mcast_group);
            }
        } else {
            int rc = ub_link_write_message(link, pkt, total_len, &local_err);
            if (rc < 0) {
                qemu_log("ubc SEND: ub_link write failed: %s\n",
                         local_err ? error_get_pretty(local_err) : "unknown");
                if (local_err) {
                    error_free(local_err);
                }
            } else {
                if (ubc_trace_data_path_enabled()) {
                    qemu_log("ubc SEND: sent %zu bytes (hdr=%zu data=%u) via ub_link\n",
                             total_len, sizeof(MsgPktHeader), payload_len);
                }
                ub_link_kick_remote(link, NULL);
            }
        }

        g_free(pkt);
    }
}

/* Wrapper for backward compatibility (SEND operation) */
static void ubc_send_data_to_remote(BusControllerDev *ubc_dev, UBCJettyState *js,
                                       const uint8_t *payload, uint32_t payload_len,
                                       uint32_t rmt_obj_id, const uint8_t *rmt_eid)
{
    ubc_send_data_to_remote_ex(ubc_dev, js, payload, payload_len, rmt_obj_id, rmt_eid, 0, false);
}

/*
 * Process a single WQE (64 bytes) from the SQ buffer.
 * udma_sqe_ctl layout (little-endian, 16 uint32_t words = 64 bytes):
 *   DW0:  sqe_bb_idx[15:0] | place_odr[17:16] | comp_order[18] | fence[19]
 *          | se[20] | cqe[21] | inline_en[22] | rsv[27:23] | token_en[28]
 *          | rmt_jetty_type[30:29] | owner[31] | target_hint[39:32]
 *   DW1:  opcode[39:32] | rsv1[45:40] | inline_msg_len[55:46]
 *   DW2:  tpn[23:0] | sge_num[31:24]
 *   DW3:  rmt_obj_id[19:0] | rsv2[31:20]
 *   DW4-7: rmt_eid[0:15]
 *   DW8:  rmt_token_value
 *   DW9:  rsv3
 *   DW10: rmt_addr_l_or_token_id
 *   DW11: rmt_addr_h_or_token_value
 *
 * Followed by SGE entries (16 bytes each):
 *   udma_normal_sge: length(4) | token_id(4) | va(8)
 *
 * For inline SEND: data starts at byte 64 (after the 64-byte header).
 */

/*
 * Send an RDMA READ request to the remote node.
 * The remote node will read from its guest memory at remote_addr and
 * send back the data via a READ_RESP message.
 */

/* Forward declaration: ubc_generate_cqe is defined after ubc_process_sq_wqe */
static void ubc_generate_cqe(BusControllerDev *ubc_dev, UBCJettyState *js,
                              uint32_t jfc_id, uint32_t wqe_idx,
                              uint32_t byte_cnt, uint32_t status, bool is_send,
                              uint32_t rmt_idx, const uint8_t *rmt_eid);

static void ubc_send_read_request(BusControllerDev *ubc_dev, UBCJettyState *js,
                                   uint32_t rmt_obj_id, const uint8_t *rmt_eid,
                                   uint64_t remote_addr, uint32_t read_len,
                                   uint32_t req_id)
{
    UBDevice *ub_dev = &ubc_dev->parent;
    UBFMManagedLink *fm_link = NULL;
    UBLinkState *link;
    Error *local_err = NULL;
    uint32_t dcna = 0;

    ubc_try_fill_dcna_from_rmt_eid(ub_dev, rmt_eid, &dcna);
    if (dcna == 0) {
        qemu_log("ubc READ_REQ: unresolved remote dcna for jetty=%u rmt_obj_id=%u; refusing fallback request\n",
                 js->jetty_id, rmt_obj_id);
        return;
    }
    fm_link = ubc_find_fm_link(ub_dev, &dcna);

    if (!fm_link) {
        qemu_log("ubc READ_REQ: no FM link found (local_cna=%#x)\n", ub_dev->cna);
        return;
    }

    link = fm_link->runtime;
    if (!ubc_link_has_transport(link)) {
        qemu_log("ubc READ_REQ: link not up\n");
        return;
    }

    size_t total_len = sizeof(MsgPktHeader) + sizeof(UBCReadReqPld);
    uint8_t *pkt = g_malloc0(total_len);
    MsgPktHeader *hdr = (MsgPktHeader *)pkt;
    UBCReadReqPld *req = (UBCReadReqPld *)(pkt + sizeof(MsgPktHeader));

    hdr->ulh.cfg = UB_CLAN_LINK_CFG;
    hdr->ulh.plen = sizeof(UBCReadReqPld);
    hdr->nth.scna = ub_dev->cna;
    hdr->nth.dcna = dcna;
    hdr->sjetty = js->jetty_id;
    hdr->jetty_en = 1;
    hdr->deid = rmt_obj_id & 0xFFFFF;
    hdr->msgetah.msg_code = UB_MSG_CODE_URMA_READ_REQ;
    hdr->msgetah.type = 1;
    hdr->msgetah.plen = sizeof(UBCReadReqPld) & 0xFFF;

    req->req_id = req_id;
    req->src_jetty_id = js->jetty_id;
    req->remote_addr = remote_addr;
    req->read_len = read_len;
    req->rmt_obj_id = rmt_obj_id;

    int rc = ub_link_write_message(link, pkt, total_len, &local_err);
    if (rc < 0) {
        qemu_log("ubc READ_REQ: ub_link write failed: %s\n",
                 local_err ? error_get_pretty(local_err) : "unknown");
        if (local_err) {
            error_free(local_err);
        }
    } else {
        qemu_log("ubc READ_REQ: sent req_id=%u remote_addr=%#" PRIx64
                 " len=%u via ub_link\n", req_id, remote_addr, read_len);
        ub_link_kick_remote(link, NULL);
    }
    g_free(pkt);
}

/*
 * Handle an incoming RDMA READ request from a remote node.
 * Read data from local guest memory and send a READ_RESP back.
 */
void ubc_handle_read_request(BusControllerDev *ubc_dev,
                              const UBCReadReqPld *req,
                              uint32_t dcna)
{
    UBDevice *ub_dev = &ubc_dev->parent;
    UBFMManagedLink *fm_link = NULL;
    UBLinkState *link;
    Error *local_err = NULL;
    uint32_t read_len = req->read_len;
    uint32_t status = 0;
    uint32_t dma_tid = UBC_DMA_TID_AUTO;

    ubc_close_dma_fallback_window(ubc_dev, "read_request");

    /* Limit read length to prevent excessive memory allocation */
    #define UBC_MAX_READ_LEN (256 * 1024)  /* 256KB max per READ */
    if (read_len > UBC_MAX_READ_LEN) {
        qemu_log("ubc READ_RESP: read_len %u > max %u, truncating\n",
                 read_len, UBC_MAX_READ_LEN);
        read_len = UBC_MAX_READ_LEN;
    }

    fm_link = ubc_find_fm_link(ub_dev, &dcna);

    if (!fm_link) {
        qemu_log("ubc READ_RESP: no FM link found (local_cna=%#x req_dcna=%#x)\n",
                 ub_dev->cna, dcna);
        return;
    }

    link = fm_link->runtime;
    if (!ubc_link_has_transport(link)) {
        qemu_log("ubc READ_RESP: link not up\n");
        return;
    }

    /* Build READ response packet */
    size_t resp_pld_len = sizeof(UBCReadRespPld) + read_len;
    size_t total_len = sizeof(MsgPktHeader) + resp_pld_len;
    uint8_t *pkt = g_malloc0(total_len);
    MsgPktHeader *hdr = (MsgPktHeader *)pkt;
    UBCReadRespPld *resp = (UBCReadRespPld *)(pkt + sizeof(MsgPktHeader));
    uint8_t *data_out = pkt + sizeof(MsgPktHeader) + sizeof(UBCReadRespPld);

    hdr->ulh.cfg = UB_CLAN_LINK_CFG;
    hdr->ulh.plen = resp_pld_len;
    hdr->nth.scna = ub_dev->cna;
    hdr->nth.dcna = dcna;
    hdr->msgetah.msg_code = UB_MSG_CODE_URMA_READ_RESP;
    hdr->msgetah.type = 1;
    hdr->msgetah.plen = resp_pld_len & 0xFFF;

    resp->req_id = req->req_id;
    resp->src_jetty_id = req->src_jetty_id;
    resp->data_len = read_len;
    resp->status = status;

    if (req->rmt_obj_id < UBC_MAX_JETTIES &&
        ubc_dev->jetties[req->rmt_obj_id].active) {
        dma_tid = ubc_tid_or_auto(ubc_dev->jetties[req->rmt_obj_id].sqe_token_id);
    }

    /* Read from guest memory at the requested address */
    MemTxResult ret = ubc_dma_read_data_tid(ubc_dev, req->remote_addr,
                                            data_out, read_len, dma_tid);
    if (ret != MEMTX_OK) {
        qemu_log("ubc READ_RESP: DMA read from %#" PRIx64 " failed\n",
                 (uint64_t)req->remote_addr);
        resp->status = 1;  /* error */
        resp->data_len = 0;
    }

    int rc = ub_link_write_message(link, pkt, total_len, &local_err);
    if (rc < 0) {
        qemu_log("ubc READ_RESP: ub_link write failed: %s\n",
                 local_err ? error_get_pretty(local_err) : "unknown");
        if (local_err) {
            error_free(local_err);
        }
    } else {
        qemu_log("ubc READ_RESP: sent req_id=%u data_len=%u\n",
                 req->req_id, resp->data_len);
        ub_link_kick_remote(link, NULL);
    }
    g_free(pkt);
}

/*
 * Handle an incoming RDMA READ response from a remote node.
 * Write data to the local SGE buffer and generate a READ completion CQE.
 */
void ubc_handle_read_response(BusControllerDev *ubc_dev,
                                const UBCReadRespPld *resp,
                                const uint8_t *data, uint32_t data_len)
{
    uint32_t req_id = resp->req_id;
    int i;

    ubc_close_dma_fallback_window(ubc_dev, "read_response");

    /* Find the pending read request by req_id */
    for (i = 0; i < UBC_MAX_PENDING_READS; i++) {
        if (ubc_dev->pending_reads.entries[i].pending &&
            ubc_dev->pending_reads.entries[i].req_id == req_id) {
            break;
        }
    }

    if (i >= UBC_MAX_PENDING_READS) {
        qemu_log("ubc READ_RESP: no pending request for req_id=%u\n", req_id);
        return;
    }

    uint32_t jetty_id = ubc_dev->pending_reads.entries[i].jetty_id;
    uint32_t wqe_idx = ubc_dev->pending_reads.entries[i].wqe_idx;
    uint32_t jfc_id = ubc_dev->pending_reads.entries[i].jfc_id;
    uint64_t local_va = ubc_dev->pending_reads.entries[i].local_va;
    uint32_t local_len = ubc_dev->pending_reads.entries[i].local_len;
    uint32_t local_token_id = ubc_dev->pending_reads.entries[i].local_token_id;
    uint32_t cqe_status = CQE_STATUS_SUCCESS;
    uint32_t actual_len = data_len;

    /* Clear the pending entry */
    ubc_dev->pending_reads.entries[i].pending = false;
    ubc_dev->pending_reads.count--;

    /* Validate jetty */
    if (jetty_id >= UBC_MAX_JETTIES || !ubc_dev->jetties[jetty_id].active) {
        qemu_log("ubc READ_RESP: jetty %u not active\n", jetty_id);
        return;
    }

    UBCJettyState *js = &ubc_dev->jetties[jetty_id];

    /* Check for error status from remote */
    if (resp->status != 0) {
        qemu_log("ubc READ_RESP: remote error status=%u\n", resp->status);
        cqe_status = CQE_STATUS_REMOTE_ERR;
        actual_len = 0;
    } else if (actual_len > local_len) {
        actual_len = local_len;
        cqe_status = CQE_STATUS_LOCAL_LEN_ERR;
    }

    /* Write data to local SGE buffer */
    if (actual_len > 0 && local_va != 0) {
        if (local_token_id == 0) {
            local_token_id = js->sqe_token_id;
        }
        MemTxResult ret = ubc_dma_write_data_tid(ubc_dev, local_va, data,
                                                 actual_len,
                                                 ubc_tid_or_auto(local_token_id));
        if (ret != MEMTX_OK) {
            qemu_log("ubc READ_RESP: DMA write to local VA failed\n");
            cqe_status = CQE_STATUS_DMA_ERR;
            actual_len = 0;
        }
    }

    /* Generate READ completion CQE */
    ubc_generate_cqe(ubc_dev, js, jfc_id, wqe_idx, actual_len, cqe_status, true,
                     0, NULL);

    qemu_log("ubc READ_RESP complete: jetty=%u wqe=%u byte_cnt=%u status=%u\n",
             jetty_id, wqe_idx, actual_len, cqe_status);
}

static void ubc_process_sq_wqe(BusControllerDev *ubc_dev, UBCJettyState *js,
                                uint32_t wqe_idx)
{
    uint8_t wqe_buf[UDMA_SQE_SIZE];
    dma_addr_t wqe_addr = js->sq_buf_addr + (dma_addr_t)wqe_idx * UDMA_SQE_SIZE;
    MemTxResult ret;
    uint32_t opcode, inline_en, sge_num, inline_msg_len, token_en;
    uint32_t rmt_obj_id;
    uint8_t rmt_eid[16];
    uint32_t i;
    uint32_t byte_cnt = 0;
    uint32_t cqe_status = CQE_STATUS_SUCCESS;

    ubc_close_dma_fallback_window(ubc_dev, "first_sq_wqe");

    /* DMA-read the 64-byte WQE header */
    ret = ubc_dma_read_data_tid(ubc_dev, wqe_addr, wqe_buf, UDMA_SQE_SIZE,
                                ubc_tid_or_auto(js->sqe_token_id));
    if (ret != MEMTX_OK) {
        qemu_log("ubc WQE: DMA read failed addr=%#" PRIx64 "\n", (uint64_t)wqe_addr);
        ubc_generate_cqe(ubc_dev, js, js->tx_jfcn, wqe_idx, 0, CQE_STATUS_DMA_ERR, true,
                         0, NULL);
        return;
    }
    {
        const uint32_t *dw = (const uint32_t *)wqe_buf;
        if (ubc_trace_data_path_enabled()) {
            qemu_log("ubc WQE RAW: jetty=%u idx=%u addr=%#" PRIx64
                     " dw0=%08x dw1=%08x dw2=%08x dw3=%08x dw4=%08x dw5=%08x dw6=%08x dw7=%08x\n",
                     js->jetty_id, wqe_idx, (uint64_t)wqe_addr,
                     dw[0], dw[1], dw[2], dw[3], dw[4], dw[5], dw[6], dw[7]);
        }
    }

    /* Parse DW0-DW1 fields (little-endian bitfield layout):
     *   DW0: sqe_bb_idx[15:0]|place_odr[17:16]|comp_order[18]|fence[19]|se[20]|cqe[21]
     *        |inline_en[22]|rsv[27:23]|token_en[28]|rmt_jetty_type[30:29]|owner[31]
     *   DW1: target_hint[7:0]|opcode[15:8]|rsv1[21:16]|inline_msg_len[31:22]
     *   DW2: tpn[23:0]|sge_num[31:24]
     *   DW3: rmt_obj_id[19:0]|rsv2[31:20]
     *   DW4-7: rmt_eid[0:15]
     */
    {
        const uint32_t *dw = (const uint32_t *)wqe_buf;
        inline_en = (dw[0] >> 22) & 0x1;
        token_en = (dw[0] >> 28) & 0x1;
        opcode = (dw[1] >> 8) & 0xFF;
        inline_msg_len = (dw[1] >> 22) & 0x3FF;
        sge_num = (dw[2] >> 24) & 0xFF;
        rmt_obj_id = dw[3] & 0xFFFFF;
        memcpy(rmt_eid, &dw[4], 16);
    }

    if (ubc_trace_data_path_enabled()) {
        qemu_log("ubc WQE: jetty=%u idx=%u op=0x%02x inline=%u sge_num=%u "
                 "inline_len=%u rmt_obj_id=%u\n",
                 js->jetty_id, wqe_idx, opcode, inline_en, sge_num,
                 inline_msg_len, rmt_obj_id);
    }

    /* Opcode dispatch */
    switch (opcode) {
    case UDMA_SQE_OPCODE_SEND:
    case UDMA_SQE_OPCODE_SEND_WITH_IMM:
    case UDMA_SQE_OPCODE_SEND_WITH_INVALID: {
        /*
         * Extract payload from inline data or SGEs.
         * For inline: data starts at offset 48 in the SQ buffer.
         * For SGE: each SGE has {length, token_id, va} and we DMA-read from va.
         */
        if (inline_en && inline_msg_len > 0) {
            uint8_t *payload = g_malloc(inline_msg_len);
            dma_addr_t payload_addr = js->sq_buf_addr +
                (dma_addr_t)wqe_idx * UDMA_SQE_SIZE + UDMA_SQE_CTL_LEN_SEND;

            ret = ubc_dma_read_data_tid(ubc_dev, payload_addr, payload,
                                        inline_msg_len,
                                        ubc_tid_or_auto(js->sqe_token_id));
            if (ret != MEMTX_OK) {
                qemu_log("ubc WQE SEND inline: DMA read payload failed len=%u\n",
                         inline_msg_len);
                cqe_status = CQE_STATUS_DMA_ERR;
                g_free(payload);
            } else {
                byte_cnt = inline_msg_len;
                ubc_send_data_to_remote(ubc_dev, js, payload, byte_cnt,
                                        rmt_obj_id, rmt_eid);
                g_free(payload);
            }
        } else if (sge_num > 0) {
            for (i = 0; i < sge_num && i < 8; i++) {
                dma_addr_t sge_addr = js->sq_buf_addr +
                    (dma_addr_t)wqe_idx * UDMA_SQE_SIZE + UDMA_SQE_CTL_LEN_SEND +
                    (dma_addr_t)i * UDMA_JFS_SGE_SIZE;
                uint8_t sge_buf[UDMA_JFS_SGE_SIZE];
                uint32_t sge_len;
                uint64_t sge_va;
                uint32_t token_id;

                ret = ubc_dma_read_data_tid(ubc_dev, sge_addr, sge_buf,
                                            UDMA_JFS_SGE_SIZE,
                                            ubc_tid_or_auto(js->sqe_token_id));
                if (ret != MEMTX_OK) {
                    qemu_log("ubc WQE SEND SGE[%u]: DMA read failed\n", i);
                    cqe_status = CQE_STATUS_DMA_ERR;
                    continue;
                }

                /* Parse udma_normal_sge: length(4) token_id(4) va(8) */
                memcpy(&sge_len, sge_buf, 4);
                memcpy(&token_id, sge_buf + 4, 4);
                memcpy(&sge_va, sge_buf + 8, 8);

                if (sge_len == 0 || sge_va == 0) {
                    continue;
                }

                /* Token validation: if WQE has token_en=1, observe token_id.
                 * For simulation: non-zero token_id is allowed, zero is logged
                 * as a warning but not rejected (graceful degradation). */
                if (token_en && token_id == 0) {
                    qemu_log("ubc WQE SEND SGE[%u]: token_en=1 but token_id=0 "
                             "(warning: no access token)\n", i);
                } else if (token_id != 0) {
                    if (ubc_trace_data_path_enabled()) {
                        qemu_log("ubc WQE SEND SGE[%u]: token_id=%u (valid)\n",
                                 i, token_id);
                    }
                }

                if (ubc_trace_data_path_enabled()) {
                    qemu_log("ubc WQE SEND SGE[%u]: va=%#" PRIx64 " len=%u\n",
                             i, sge_va, sge_len);
                }

                uint8_t *payload = g_malloc(sge_len);
                ret = ubc_dma_read_data_tid(ubc_dev, sge_va, payload, sge_len,
                                            ubc_tid_or_auto(token_id ?
                                                            token_id :
                                                            js->sqe_token_id));
                if (ret != MEMTX_OK) {
                    qemu_log("ubc WQE SEND SGE[%u]: data DMA read failed\n", i);
                    cqe_status = CQE_STATUS_DMA_ERR;
                    g_free(payload);
                    continue;
                }

                ubc_send_data_to_remote(ubc_dev, js, payload, sge_len,
                                        rmt_obj_id, rmt_eid);
                byte_cnt += sge_len;
                g_free(payload);
                /* For simplicity, send only the first SGE's data */
                break;
            }
        }
        break;
    }

    case UDMA_SQE_OPCODE_WRITE:
    case UDMA_SQE_OPCODE_WRITE_WITH_IMM: {
        /*
         * RDMA WRITE: writes to remote memory directly at the specified address.
         * DW10-DW11 contain the remote address (rmt_addr_l/rmt_addr_h).
         */
        const uint32_t *wqe_dw = (const uint32_t *)wqe_buf;
        uint32_t sqe_ctl_len = (opcode == UDMA_SQE_OPCODE_WRITE_WITH_IMM) ?
                               UDMA_SQE_SIZE : UDMA_SQE_CTL_LEN_SEND;
        uint64_t remote_addr = ((uint64_t)wqe_dw[11] << 32) | wqe_dw[10];

        if (ubc_trace_data_path_enabled()) {
            qemu_log("ubc WQE WRITE: jetty=%u idx=%u remote_addr=%#" PRIx64 "\n",
                     js->jetty_id, wqe_idx, remote_addr);
        }

        if (remote_addr == 0) {
            qemu_log("ubc WQE WRITE: remote_addr is zero, failing\n");
            cqe_status = CQE_STATUS_LOCAL_OP_ERR;
            break;
        }

        if (inline_en && inline_msg_len > 0) {
            uint8_t *payload = g_malloc(inline_msg_len);
            dma_addr_t payload_addr = js->sq_buf_addr +
                (dma_addr_t)wqe_idx * UDMA_SQE_SIZE + sqe_ctl_len;
            ret = ubc_dma_read_data_tid(ubc_dev, payload_addr, payload,
                                        inline_msg_len,
                                        ubc_tid_or_auto(js->sqe_token_id));
            if (ret == MEMTX_OK) {
                byte_cnt = inline_msg_len;
                ubc_send_data_to_remote_ex(ubc_dev, js, payload, byte_cnt,
                                           rmt_obj_id, rmt_eid, remote_addr, true);
            } else {
                cqe_status = CQE_STATUS_DMA_ERR;
            }
            g_free(payload);
        } else if (sge_num > 0) {
            for (i = 0; i < sge_num && i < 8; i++) {
                dma_addr_t sge_addr = js->sq_buf_addr +
                    (dma_addr_t)wqe_idx * UDMA_SQE_SIZE + sqe_ctl_len +
                    (dma_addr_t)i * UDMA_JFS_SGE_SIZE;
                uint8_t sge_buf[UDMA_JFS_SGE_SIZE];
                uint32_t sge_len;
                uint32_t token_id;
                uint64_t sge_va;

                ret = ubc_dma_read_data_tid(ubc_dev, sge_addr, sge_buf,
                                            UDMA_JFS_SGE_SIZE,
                                            ubc_tid_or_auto(js->sqe_token_id));
                if (ret != MEMTX_OK) {
                    continue;
                }
                memcpy(&sge_len, sge_buf, 4);
                memcpy(&token_id, sge_buf + 4, 4);
                memcpy(&sge_va, sge_buf + 8, 8);
                if (sge_len == 0 || sge_va == 0) {
                    continue;
                }
                uint8_t *payload = g_malloc(sge_len);
                ret = ubc_dma_read_data_tid(ubc_dev, sge_va, payload, sge_len,
                                            ubc_tid_or_auto(token_id ?
                                                            token_id :
                                                            js->sqe_token_id));
                if (ret == MEMTX_OK) {
                    ubc_send_data_to_remote_ex(ubc_dev, js, payload, sge_len,
                                               rmt_obj_id, rmt_eid, remote_addr, true);
                    byte_cnt = sge_len;
                } else {
                    cqe_status = CQE_STATUS_DMA_ERR;
                }
                g_free(payload);
                break;
            }
        }
        break;
    }

    case UDMA_SQE_OPCODE_READ: {
        /*
         * RDMA READ: send a READ request to the remote node.
         * The remote node reads from its guest memory and sends back a
         * READ_RESP. On response receipt, data is written to local SGE
         * and a completion CQE is generated.
         *
         * WQE layout for READ:
         *   DW10: rmt_addr_l (remote address low bits)
         *   DW11: rmt_addr_h (remote address high bits)
         *   SGE[0]: local buffer { length, token_id, va }
         */
        const uint32_t *wqe_dw = (const uint32_t *)wqe_buf;
        uint64_t remote_addr = ((uint64_t)wqe_dw[11] << 32) | wqe_dw[10];
        uint32_t sge_len = 0;
        uint32_t local_token_id = 0;
        uint64_t local_va = 0;
        uint32_t req_id;
        int slot = -1;

        /* Parse local SGE to know where to write response data */
        if (sge_num > 0) {
            dma_addr_t sge_addr = js->sq_buf_addr +
                (dma_addr_t)wqe_idx * UDMA_SQE_SIZE + UDMA_SQE_CTL_LEN_SEND;
            uint8_t sge_buf[UDMA_JFS_SGE_SIZE];
            ret = ubc_dma_read_data_tid(ubc_dev, sge_addr, sge_buf,
                                        UDMA_JFS_SGE_SIZE,
                                        ubc_tid_or_auto(js->sqe_token_id));
            if (ret == MEMTX_OK) {
                memcpy(&sge_len, sge_buf, 4);
                memcpy(&local_token_id, sge_buf + 4, 4);
                memcpy(&local_va, sge_buf + 8, 8);
            }
        }

        if (sge_len == 0 || local_va == 0) {
            qemu_log("ubc WQE READ: no valid local SGE\n");
            cqe_status = CQE_STATUS_LOCAL_LEN_ERR;
            break;
        }

        /* Find a free pending read slot */
        for (int si = 0; si < UBC_MAX_PENDING_READS; si++) {
            if (!ubc_dev->pending_reads.entries[si].pending) {
                slot = si;
                break;
            }
        }

        if (slot < 0) {
            qemu_log("ubc WQE READ: no free pending read slots\n");
            cqe_status = CQE_STATUS_LOCAL_OP_ERR;
            break;
        }

        /* Allocate request ID and register pending read */
        req_id = ubc_dev->next_read_req_id++;
        ubc_dev->pending_reads.entries[slot].pending = true;
        ubc_dev->pending_reads.entries[slot].jetty_id = js->jetty_id;
        ubc_dev->pending_reads.entries[slot].wqe_idx = wqe_idx;
        ubc_dev->pending_reads.entries[slot].jfc_id = js->tx_jfcn;
        ubc_dev->pending_reads.entries[slot].local_va = local_va;
        ubc_dev->pending_reads.entries[slot].local_len = sge_len;
        ubc_dev->pending_reads.entries[slot].local_token_id =
            local_token_id ? local_token_id : js->sqe_token_id;
        ubc_dev->pending_reads.entries[slot].req_id = req_id;
        ubc_dev->pending_reads.count++;

        /* Send READ request to remote node */
        ubc_send_read_request(ubc_dev, js, rmt_obj_id, rmt_eid,
                              remote_addr, sge_len, req_id);

        qemu_log("ubc WQE READ: jetty=%u idx=%u remote_addr=%#" PRIx64
                 " len=%u req_id=%u (pending)\n",
                 js->jetty_id, wqe_idx, remote_addr, sge_len, req_id);

        /* Return WITHOUT generating CQE — CQE will be generated when
         * READ response arrives via ubc_handle_read_response. */
        return;
    }

    case UDMA_SQE_OPCODE_CAS:
    case UDMA_SQE_OPCODE_FAA: {
        /*
         * Atomic operations (CAS/FAA): currently only local memory ATOMIC
         * is supported in simulation. Remote ATOMIC would require a
         * request-response protocol similar to READ.
         */
        const uint32_t *wqe_dw = (const uint32_t *)wqe_buf;
        uint64_t remote_addr = ((uint64_t)wqe_dw[11] << 32) | wqe_dw[10];
        /*
        uint64_t atomic_arg1 = ((uint64_t)wqe_dw[13] << 32) | wqe_dw[12];
        uint64_t atomic_arg2 = ((uint64_t)wqe_dw[15] << 32) | wqe_dw[14];
        */

        qemu_log("ubc WQE ATOMIC op=0x%02x: jetty=%u idx=%u remote=%#" PRIx64
                 " (remote ATOMIC not yet implemented)\n",
                 opcode, js->jetty_id, wqe_idx, remote_addr);

        /* TODO: Implement remote ATOMIC using request-response protocol */
        cqe_status = CQE_STATUS_LOCAL_OP_ERR;
        byte_cnt = 0;
        break;
    }

    default:
        qemu_log("ubc WQE: unhandled opcode 0x%02x jetty=%u idx=%u\n",
                 opcode, js->jetty_id, wqe_idx);
        cqe_status = CQE_STATUS_LOCAL_OP_ERR;
        break;
    }

    /* Generate TX completion CQE */
    ubc_generate_cqe(ubc_dev, js, js->tx_jfcn, wqe_idx, byte_cnt, cqe_status, true,
                     0, NULL);
}

/*
 * Generate a CQE in the specified JFC's CQ buffer.
 *
 * CQE layout (struct cdma_jfc_cqe, little-endian bitfield):
 *   DW0: s_r[0] | is_jetty[1] | owner[2] | inline_en[3] | opcode[6:4]
 *        | fd[7] | rsv[15:8] | substatus[23:16] | status[31:24]
 *   DW1: entry_idx[15:0] | local_num_l[31:16]
 *   DW2: local_num_h[3:0] | rmt_idx[23:4] | rsv1[31:24]
 *   DW3: tpn[23:0] | rsv2[31:24]
 *   DW4: byte_cnt
 *   DW5: user_data_l
 *   DW6: user_data_h
 *
 * Owner bit semantics (from driver cdma_get_next_cqe):
 *   valid_owner = (ci >> entry_cnt_mask_ilog2) & 1
 *   CQE is NEW when cqe->owner != valid_owner
 *   Round 0 (ci 0..depth-1): valid_owner=0, so CQE owner must be 1
 *   Round 1 (ci depth..2*depth-1): valid_owner=1, so CQE owner must be 0
 *   Owner flips each round; tracked by cq_owner_phase counter.
 */
static void ubc_generate_cqe(BusControllerDev *ubc_dev, UBCJettyState *js,
                              uint32_t jfc_id, uint32_t wqe_idx,
                              uint32_t byte_cnt, uint32_t status, bool is_send,
                              uint32_t rmt_idx, const uint8_t *rmt_eid)
{
    UBCJfcState *jfc;
    uint8_t cqe[64];
    uint32_t *dw = (uint32_t *)cqe;
    dma_addr_t cqe_addr;
    MemTxResult ret;
    uint32_t owner;

    if (jfc_id >= UBC_MAX_JFCS) {
        qemu_log("ubc CQE: jfc_id=%u out of range\n", jfc_id);
        return;
    }

    jfc = &ubc_dev->jfcs[jfc_id];
    if (!jfc->active) {
        qemu_log("ubc CQE: JFC %u not active\n", jfc_id);
        return;
    }

    if (!jfc->cq_depth) {
        qemu_log("ubc CQE: JFC %u cq_depth=0\n", jfc_id);
        return;
    }

    memset(cqe, 0, sizeof(cqe));

    /*
     * Owner bit: initial phase is 1 (so first round CQEs have owner=1,
     * matching driver's expectation that valid_owner=0 in round 0).
     * Flip each time cq_pi wraps to 0.
     */
    owner = jfc->cq_owner_phase;

    /* DW0 bitfield: s_r | is_jetty | owner | inline_en | opcode | fd | rsv | substatus | status */
    dw[0] = ((is_send ? 0 : 1) << 0)    /* s_r: 0=send, 1=recv */
          | (1 << 1)                     /* is_jetty = 1 */
          | (owner << 2)                 /* owner/phase bit */
          | (0 << 3)                     /* inline_en = 0 */
          | (0 << 4)                     /* opcode = 0 (3 bits) */
          | (0 << 7)                     /* fd = 0 */
          | (0 << 8)                     /* rsv (8 bits) */
          | ((status & 0xFF) << 16)      /* substatus (8 bits) - use status as substatus for now */
          | ((status & 0xFF) << 24);     /* status (8 bits) */

    /* DW1: entry_idx[15:0] | local_num_l[31:16]
     * local_num is the jetty_id for identifying the queue pair */
    dw[1] = (wqe_idx & 0xFFFF) | ((js ? js->jetty_id : 0) << 16);

    /* DW2: local_num_h[3:0] | rmt_idx[23:4] | rsv1[31:24] */
    dw[2] = (rmt_idx & 0xFFFFF) << 4;

    /* DW3: tpn[23:0] | rsv[31:24]
     * tpn: transport port number (use jetty_id as tpn for simulation) */
    dw[3] = js ? (js->jetty_id & 0xFFFFFF) : 0;

    /* DW4: byte_cnt */
    dw[4] = byte_cnt;

    /* DW5-DW6: user_data from jetty context (jfs kernel addr) */
    if (js) {
        dw[5] = js->user_data_l;
        dw[6] = js->user_data_h;
    }

    if (rmt_eid) {
        memcpy(&dw[7], rmt_eid, 16);
    }

    /* Write CQE to CQ buffer at cq_pi */
    cqe_addr = jfc->cq_buf_addr + (dma_addr_t)jfc->cq_pi * 64;
    ret = ubc_dma_write_data_tid(ubc_dev, cqe_addr, cqe, 64,
                                 ubc_tid_or_auto(jfc->cqe_token_id));
    if (ret != MEMTX_OK) {
        qemu_log("ubc CQE: DMA write failed addr=%#" PRIx64 "\n", (uint64_t)cqe_addr);
        return;
    }

    /* Advance cq_pi and flip owner on wrap */
    jfc->cq_pi = (jfc->cq_pi + 1) % jfc->cq_depth;
    if (jfc->cq_pi == 0) {
        jfc->cq_owner_phase ^= 1;
    }

    if (ubc_trace_data_path_enabled()) {
        qemu_log("ubc CQE: jfc=%u wqe_idx=%u byte_cnt=%u status=%u owner=%u cq_pi=%u rmt_idx=%u\n",
                 jfc_id, wqe_idx, byte_cnt, status, owner, jfc->cq_pi, rmt_idx);
    }

    /*
     * Completion path expected by guest:
     *   JFC CQE in JFC buffer + CEQE(jfcn) in CEQ buffer + CEQ vector interrupt.
     * Keep CMDQ event as fallback when CEQ state is not available yet.
     */
    if (!ubc_push_ceq_event(ubc_dev, jfc->ceqn, jfc_id)) {
        ubc_raise_cmdq_event(ubc_dev);
    }
}

/*
 * Process all pending WQEs on a jetty's SQ from sq_ci up to sq_pi.
 * Each WQE is dispatched by opcode; a CQE is generated for each.
 */
static void ubc_process_sq(BusControllerDev *ubc_dev, UBCJettyState *js)
{
    uint32_t pi = js->sq_pi;
    uint32_t ci = js->sq_ci;

    while (ci != pi) {
        ubc_process_sq_wqe(ubc_dev, js, ci);
        ci = (ci + 1) % js->sq_depth;
    }
    js->sq_ci = ci;
}

/*
 * Handle incoming URMA data from a remote node.
 * This is called from ubc_msgq.c when a URMA data packet arrives via ub_link.
 *
 * For now, we log the data and raise an interrupt to notify the guest.
 * The guest kernel's ipourma driver will process the received data through
 * the normal URMA completion path.
 */
/* Note: UBC_URMA_RX_BUF_* macros are defined at the top of this file */

static void ubc_buffer_urma_rx(BusControllerDev *ubc_dev, uint32_t dst_jetty,
                                   uint32_t src_jetty, uint32_t src_scna,
                                   const uint8_t src_eid[16],
                                   const uint8_t *data, uint32_t data_len)
{
    if (ubc_dev->urma_rx_buf.count >= UBC_URMA_RX_BUF_MAX) {
        qemu_log("ubc URMA RX: buffer full (%u), dropping packet for jetty %u\n",
                      ubc_dev->urma_rx_buf.count, dst_jetty);
        return;
    }
    if (data_len > UBC_URMA_RX_BUF_DATA_MAX) {
        qemu_log("ubc URMA RX: packet too large (%u) for buffer, jetty %u\n",
                      data_len, dst_jetty);
        return;
    }
    ubc_dev->urma_rx_buf.entries[ubc_dev->urma_rx_buf.count].dst_jetty = dst_jetty;
    ubc_dev->urma_rx_buf.entries[ubc_dev->urma_rx_buf.count].src_jetty = src_jetty;
    ubc_dev->urma_rx_buf.entries[ubc_dev->urma_rx_buf.count].src_scna = src_scna;
    ubc_dev->urma_rx_buf.entries[ubc_dev->urma_rx_buf.count].data_len = data_len;
    if (src_eid) {
        memcpy(ubc_dev->urma_rx_buf.entries[ubc_dev->urma_rx_buf.count].src_eid,
               src_eid, 16);
    } else {
        memset(ubc_dev->urma_rx_buf.entries[ubc_dev->urma_rx_buf.count].src_eid, 0, 16);
    }
    memcpy(ubc_dev->urma_rx_buf.entries[ubc_dev->urma_rx_buf.count].data, data, data_len);
    ubc_dev->urma_rx_buf.count++;
    qemu_log("ubc URMA RX: buffered packet for jetty %u len=%u (total buffered=%u)\n",
             dst_jetty, data_len, ubc_dev->urma_rx_buf.count);
}

static void ubc_flush_urma_rx_buffer(BusControllerDev *ubc_dev, uint32_t jetty_id)
{
    uint32_t i;
    uint32_t new_count = 0;

    for (i = 0; i < ubc_dev->urma_rx_buf.count; i++) {
        if (ubc_dev->urma_rx_buf.entries[i].dst_jetty == jetty_id) {
            qemu_log("ubc URMA RX flush: delivering buffered packet %u/%u for jetty %u\n",
                     i, ubc_dev->urma_rx_buf.count, jetty_id);
            if (!ubc_try_handle_urma_rx_data(ubc_dev, jetty_id,
                                             ubc_dev->urma_rx_buf.entries[i].src_jetty,
                                             ubc_dev->urma_rx_buf.entries[i].src_scna,
                                             ubc_dev->urma_rx_buf.entries[i].src_eid,
                                             ubc_dev->urma_rx_buf.entries[i].data,
                                             ubc_dev->urma_rx_buf.entries[i].data_len,
                                             false)) {
                qemu_log("ubc URMA RX flush: jetty %u not ready yet, keep packet %u buffered\n",
                         jetty_id, i);
            }
        }

        if (new_count != i) {
            ubc_dev->urma_rx_buf.entries[new_count] = ubc_dev->urma_rx_buf.entries[i];
        }
        new_count++;
    }

    ubc_dev->urma_rx_buf.count = new_count;
}

static bool ubc_try_handle_urma_rx_data(BusControllerDev *ubc_dev, uint32_t dst_jetty,
                                        uint32_t src_jetty, uint32_t src_scna,
                                        const uint8_t src_eid[16],
                                        const uint8_t *data, uint32_t data_len,
                                        bool allow_buffer)
{
    UBCJettyState *js;
    UBCJfrState  *jfr;
    uint8_t effective_src_eid[16];
    uint8_t sge_buf[16];
    dma_addr_t rqe_addr;
    uint64_t sge_va;
    uint32_t sge_len;
    uint32_t sge_token_id;
    uint32_t rq_wqe_idx;
    uint32_t cqe_status = CQE_STATUS_SUCCESS;
    uint32_t actual_len;

    ubc_close_dma_fallback_window(ubc_dev, "urma_rx_data");

    memset(effective_src_eid, 0, sizeof(effective_src_eid));
    if (src_eid) {
        memcpy(effective_src_eid, src_eid, sizeof(effective_src_eid));
    }
    if (ubc_eid_is_zero(effective_src_eid)) {
        (void)ubc_fill_link_local_eid_by_scna(&ubc_dev->parent, src_scna,
                                              effective_src_eid);
    }

    if (ubc_trace_data_path_enabled()) {
        qemu_log("ubc URMA RX: dst_jetty=%u src_jetty=%u src_scna=%#x len=%u\n",
                 dst_jetty, src_jetty, src_scna, data_len);
    }

    if (!ubc_dev || dst_jetty >= UBC_MAX_JETTIES) {
        qemu_log("ubc URMA RX: dst_jetty %u out of range\n", dst_jetty);
        if (allow_buffer) {
            ubc_buffer_urma_rx(ubc_dev, dst_jetty, src_jetty, src_scna, effective_src_eid,
                               data, data_len);
        }
        return false;
    }

    if (!ubc_dev->jetties[dst_jetty].active) {
        qemu_log("ubc URMA RX: dst jetty %u not active, buffering\n", dst_jetty);
        if (allow_buffer) {
            ubc_buffer_urma_rx(ubc_dev, dst_jetty, src_jetty, src_scna, effective_src_eid,
                               data, data_len);
        }
        return false;
    }

    js = &ubc_dev->jetties[dst_jetty];

    if (js->jfrn >= UBC_MAX_JFRS || !ubc_dev->jfrs[js->jfrn].active) {
        qemu_log("ubc URMA RX: JFR %u not active for jetty %u\n", js->jfrn, dst_jetty);
        return false;
    }

    jfr = &ubc_dev->jfrs[js->jfrn];
    if (!jfr->rq_buf_addr || !jfr->rq_depth) {
        qemu_log("ubc URMA RX: no RQ buffer for JFR %u\n", js->jfrn);
        return false;
    }

    /* Boundary check: RQ full (rq_ci == rq_pi means empty, but if RQ has no
     * posted receives, rq_ci may equal rq_depth - 1 after wrapping). */
    rq_wqe_idx = jfr->rq_ci;

    /* DMA-read the next RECV WQE (16-byte SGE) from the RQ */
    rqe_addr = jfr->rq_buf_addr + (dma_addr_t)rq_wqe_idx * 16;
    if (ubc_dma_read_data_tid(ubc_dev, rqe_addr, sge_buf, 16,
                              ubc_tid_or_auto(jfr->rqe_token_id)) != MEMTX_OK) {
        qemu_log("ubc URMA RX: RQE DMA read failed at ci=%u addr=%#" PRIx64 "\n",
                 jfr->rq_ci, rqe_addr);
        return false;
    }

    /* Parse SGE: length(4) + token_id(4) + va(8) */
    memcpy(&sge_len, sge_buf, 4);
    memcpy(&sge_token_id, sge_buf + 4, 4);
    memcpy(&sge_va, sge_buf + 8, 8);

    if (ubc_trace_data_path_enabled()) {
        qemu_log("ubc URMA RX: RQE ci=%u sge_va=%#" PRIx64
                 " sge_len=%u sge_tid=%u data_len=%u\n",
                 jfr->rq_ci, sge_va, sge_len, sge_token_id, data_len);
    }

    /* Boundary check: null VA or zero length → RQ empty error */
    if (!sge_va || sge_len == 0) {
        qemu_log("ubc URMA RX: RQE null va or zero len at ci=%u\n", jfr->rq_ci);
        return false;
    }

    /* Length mismatch: truncate and set error if SGE buffer too small */
    actual_len = data_len;
    if (sge_len < data_len) {
        qemu_log("ubc URMA RX: RQE buffer too small (%u < %u), truncating\n",
                 sge_len, data_len);
        actual_len = sge_len;
        cqe_status = CQE_STATUS_LOCAL_LEN_ERR;
    }

    /* DMA-write the received data into the guest's RECV buffer */
    if (ubc_dma_write_data_tid(ubc_dev, sge_va, data, actual_len,
                               ubc_tid_or_auto(sge_token_id ?
                                               sge_token_id :
                                               jfr->rqe_token_id)) != MEMTX_OK) {
        qemu_log("ubc URMA RX: data DMA write failed\n");
        ubc_generate_cqe(ubc_dev, js, js->rx_jfcn, rq_wqe_idx, 0,
                         CQE_STATUS_DMA_ERR, false, src_jetty, effective_src_eid);
        return false;
    }

    /* Advance RQ consumer index */
    jfr->rq_ci = (jfr->rq_ci + 1) % jfr->rq_depth;

    /* Generate RX completion CQE using shared function */
    ubc_generate_cqe(ubc_dev, js, js->rx_jfcn, rq_wqe_idx, actual_len,
                     cqe_status, false, src_jetty, effective_src_eid);

    if (ubc_trace_data_path_enabled()) {
        qemu_log("ubc URMA RX: CQE done jetty=%u rq_ci=%u byte_cnt=%u status=%u\n",
                 dst_jetty, jfr->rq_ci, actual_len, cqe_status);
    }
    return true;
}

void ubc_handle_urma_rx_data(BusControllerDev *ubc_dev, uint32_t dst_jetty,
                              uint32_t src_jetty, uint32_t src_scna,
                              const uint8_t src_eid[16],
                              const uint8_t *data, uint32_t data_len)
{
    (void)ubc_try_handle_urma_rx_data(ubc_dev, dst_jetty, src_jetty, src_scna,
                                      src_eid, data, data_len, true);
}

/*
 * Handle RDMA WRITE data from remote node.
 * Unlike SEND/RECV, WRITE writes directly to the specified remote_addr
 * without consuming an RQ entry.
 */
void ubc_handle_urma_rx_write(BusControllerDev *ubc_dev, uint32_t dst_jetty,
                              uint64_t remote_addr, const uint8_t *data,
                              uint32_t data_len)
{
    UBCJettyState *js;

    ubc_close_dma_fallback_window(ubc_dev, "urma_rx_write");

    qemu_log("ubc URMA RX WRITE: dst_jetty=%u remote_addr=%#" PRIx64 " len=%u\n",
             dst_jetty, remote_addr, data_len);

    if (!ubc_dev || dst_jetty >= UBC_MAX_JETTIES) {
        qemu_log("ubc URMA RX WRITE: dst_jetty %u out of range\n", dst_jetty);
        return;
    }

    if (!ubc_dev->jetties[dst_jetty].active) {
        qemu_log("ubc URMA RX WRITE: dst jetty %u not active\n", dst_jetty);
        return;
    }
    js = &ubc_dev->jetties[dst_jetty];

    /* For WRITE: directly write to remote_addr, no RQ consumption */
    if (ubc_dma_write_data_tid(ubc_dev, remote_addr, data, data_len,
                               ubc_tid_or_auto(js->sqe_token_id)) != MEMTX_OK) {
        qemu_log("ubc URMA RX WRITE: DMA write to %#" PRIx64 " failed\n",
                 remote_addr);
        return;
    }

    qemu_log("ubc URMA RX WRITE: wrote %u bytes to %#" PRIx64 "\n",
             data_len, remote_addr);
    /* No CQE generated for WRITE on the receive side (sender gets completion) */
}

static void ub_ers_region_write(void *opaque, hwaddr addr, uint64_t val, unsigned len)
{
    typeof(((BusControllerDev *)0)->ers[0]) *ers = opaque;
    BusControllerDev *ubc_dev = ers->owner;
    uint32_t entity_idx = 0;
    hwaddr obmm_async_reg;
    hwaddr ub_async_load_reg;
    hwaddr linqu_reg;

    if (ers->idx == 2 && len >= DWORD_SIZE) {
        if (ubc_dev->obmm_async &&
            ub_obmm_async_decode(addr, &obmm_async_reg) &&
            ub_obmm_async_write(ubc_dev->obmm_async, obmm_async_reg,
                                val, len)) {
            return;
        }
        if (ubc_dev->ub_async_load &&
            ub_async_load_device_decode(addr, &ub_async_load_reg) &&
            ub_async_load_device_write(ubc_dev->ub_async_load, ub_async_load_reg,
                                val, len)) {
            return;
        }
        if (linqu_uapi_decode_endpoint(addr, &linqu_reg) &&
            linqu_uapi_reg_write(ubc_dev, linqu_reg, val, len)) {
            return;
        }
    }

    /* Multi-entity routing: each entity has a 4MB aperture in the alias window */
    if ((ers->idx == 0 || ers->idx == 1 || ers->idx == 2) && addr >= 0x400000) {
        entity_idx = (uint32_t)(addr / 0x400000);
        addr %= 0x400000;
    }

    if (!ubc_dev || entity_idx >= ubc_dev->entity_count) {
        return;
    }

    /* ERS0 (Config) routing: Direct access to per-entity shadow config space */
    if (ers->idx == 0) {
        uint8_t *cfg = ubc_dev->entity_cfg_spaces[entity_idx].cfg_base;
        uint32_t cfg_size = ubc_dev->entity_cfg_spaces[entity_idx].cfg_size;
        if (cfg && addr < cfg_size) {
            switch (len) {
            case 1: cfg[addr] = (uint8_t)val; break;
            case 2: stw_le_p(cfg + addr, (uint16_t)val); break;
            case 4: stl_le_p(cfg + addr, (uint32_t)val); break;
            case 8: stq_le_p(cfg + addr, val); break;
            }
        }
        return;
    }

    if (ers->idx == 1 && (len == sizeof(uint64_t) || len == sizeof(uint32_t)) &&
        addr >= UDMA_JETTY_DSQE_OFFSET) {
        uint32_t local_jetty_id = ((addr & ~(UDMA_HW_PAGE_SIZE - 1)) -
                                   UDMA_JETTY_DSQE_OFFSET) / UDMA_HW_PAGE_SIZE;
        uint32_t within_page = addr & (UDMA_HW_PAGE_SIZE - 1);

        if (within_page < UDMA_SQE_SIZE) {
            qemu_log("ubc ERS1 DSQE write: entity=%u jetty_local=%u off=0x%x val=%#" PRIx64 "\n",
                     entity_idx, local_jetty_id, within_page, val);
        }
    }

    /* ERS1 Doorbell logic (with entity isolation)
     * Doorbell offset = JETTY_DSQE_OFFSET + (jetty_id * 4K) + DOORBELL_OFFSET
     */
    if (ers->idx == 1 && len == DWORD_SIZE &&
        addr >= (UDMA_JETTY_DSQE_OFFSET + UDMA_DOORBELL_OFFSET) &&
        (addr & (UDMA_HW_PAGE_SIZE - 1)) == UDMA_DOORBELL_OFFSET) {
        
        uint32_t local_jetty_id = ((addr & ~(UDMA_HW_PAGE_SIZE - 1)) - UDMA_JETTY_DSQE_OFFSET) / UDMA_HW_PAGE_SIZE;
        /* Map local jetty space to global array: each entity gets 128 jetties */
        uint32_t global_jetty_id = local_jetty_id + (entity_idx * 128);

        if (global_jetty_id < UBC_MAX_JETTIES && ubc_dev->jetties[global_jetty_id].active) {
            UBCJettyState *js = &ubc_dev->jetties[global_jetty_id];
            if (ubc_trace_data_path_enabled()) {
                qemu_log("ubc ERS1 doorbell: entity=%u jetty=%u new_pi=%u sq_ci=%u sq_depth=%u\n",
                         entity_idx, global_jetty_id, (uint32_t)val, js->sq_ci, js->sq_depth);
            }
            js->sq_pi = (uint32_t)val;
            ubc_process_sq(ubc_dev, js);
            return;
        }
    }

    /* ERS2 custom handling */
    if (ers->idx == 2) {
        ubc_ers2_mmio_write(ubc_dev, addr, val, len);
        return;
    }

    /* Standard storage fallback (only for non-routed accesses within bounds) */
    if (!ers->storage || addr > ers->storage_size || len > ers->storage_size - addr) {
        return;
    }

    switch (len) {
    case BYTE_SIZE:
        ub_set_byte(ers->storage + addr, val);
        break;
    case WORD_SIZE:
        ub_set_word(ers->storage + addr, val);
        break;
    case DWORD_SIZE:
        ub_set_long(ers->storage + addr, val);
        break;
    case sizeof(uint64_t):
        ub_set_quad(ers->storage + addr, val);
        break;
    }
}

static const MemoryRegionOps ub_ers_region_ops = {
    .read = ub_ers_region_read,
    .write = ub_ers_region_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = sizeof(uint64_t),
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = sizeof(uint64_t),
    },
};

static void ub_bus_controller_init_ers_regions(UBDevice *dev)
{
    BusControllerDev *ubc_dev = BUS_CONTROLLER_DEV(dev);
    static const uint64_t ers_size_pages[UB_NUM_REGIONS] = {
        UBC_ERS0_SPACE_SIZE,
        UBC_ERS1_SPACE_SIZE,
        UBC_ERS2_SPACE_SIZE,
    };
    static const char *ers_names[UB_NUM_REGIONS] = {
        "ubc-ers0",
        "ubc-ers1",
        "ubc-ers2",
    };
    /*
    static const uint64_t ers_start_addr[UB_NUM_REGIONS] = {
        UBC_ERS0_SPACE_ADDR,
        UBC_ERS1_SPACE_ADDR,
        UBC_ERS2_SPACE_ADDR,
    };
    */
    uint8_t i;

    for (i = 0; i < UB_NUM_REGIONS; i++) {
        typeof(ubc_dev->ers[0]) *ers = &ubc_dev->ers[i];
        uint64_t region_size = ers_size_pages[i] * UBC_ERS_PAGE_SIZE;

        /* For ERS0, ERS1, and ERS2, expand size to cover all entity apertures (4MB per entity) */
        if ((i == 0 || i == 1 || i == 2) && ubc_dev->entity_count > 1) {
            region_size = (uint64_t)ubc_dev->entity_count * 0x400000ULL;
        }

        ers->owner = ubc_dev;
        ers->idx = i;
        ers->storage_size = (i == 1) ? UBC_ERS_PAGE_SIZE : region_size;
        ers->storage = g_malloc0(ers->storage_size);
        memory_region_init_io(&ers->region, OBJECT(dev), &ub_ers_region_ops,
                              ers, ers_names[i], region_size);
        ub_register_ers(dev, i, &ers->region);

        qemu_log("ubc ers%u initialized size=%#" PRIx64 "\n",
                 i, region_size);
    }
}

static void ub_bus_controller_activate_ers_mappings(UBDevice *dev)
{
    UbCfg1Basic *cfg1_basic;
    uint64_t emulated_offset;
    uint64_t ers_base = ub_ers_phys_base();
    uint64_t ers_ubba[UB_NUM_REGIONS];
    uint8_t i;

    ers_ubba[0] = ers_base;
    ers_ubba[1] = ers_base + 0x100000ULL;
    ers_ubba[2] = ers_base + 0x200000ULL;

    /* 
     * Limit Addressing Capability: 
     * Many platforms (like macOS) have issues mapping physical addresses 
     * near the 64-bit boundary. Force guest to use lower memory for CMDQ/EVTQ.
     */
    emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG1_BASIC_START, true);
    cfg1_basic = (UbCfg1Basic *)(dev->config + emulated_offset);
    /* Set address width capability to 40 bits (instead of 64) */
    *(uint32_t *)(dev->config + emulated_offset + 0x80) = 40; 
    cfg1_basic->dev_rs_access_en = 1;

    for (i = 0; i < UB_NUM_REGIONS; i++) {
        uint64_t cfg_off = UB_CFG1_BASIC_START + offsetof(UbCfg1Basic, ers_ubba) +
                           i * sizeof(uint64_t);
        uint32_t data_l = (uint32_t)(ers_ubba[i] & UINT32_MAX);
        uint32_t data_h = (uint32_t)(ers_ubba[i] >> 32);

        ub_default_write_config(dev, cfg_off, &data_l, ~0U);
        ub_default_write_config(dev, cfg_off + sizeof(uint32_t), &data_h, ~0U);
        qemu_log("ubc ers%u activate: ubba=0x%" PRIx64 " mapped_addr=0x%" PRIx64
                 " size=0x%" PRIx64 "\n",
                 i, ers_ubba[i], dev->io_regions[i].addr,
                 dev->io_regions[i].size);
    }
}

static void ub_reg_alloc(DeviceState *dev)
{
    BusControllerState *s = BUS_CONTROLLER(dev);

    s->msgq_reg = g_malloc0(s->msgq_reg_size);
    s->fm_msgq_reg = g_malloc0(s->fm_msgq_reg_size);
    qemu_log("alloc ub reg mem size: msgq_reg %u, "
             "fm_msgq_reg %u\n",
             s->msgq_reg_size, s->fm_msgq_reg_size);
}

static void ub_reg_free(DeviceState *dev)
{
    BusControllerState *s = BUS_CONTROLLER(dev);

    g_free(s->msgq_reg);
    g_free(s->fm_msgq_reg);
    qemu_log("free ub reg mem\n");
}

void ub_notify_retry_timer_cb(void *opaque)
{
    BusControllerState *s = BUS_CONTROLLER(opaque);
    ub_try_inject_remote_cfg_notifies(s);
}

static void ub_bus_controller_realize(DeviceState *dev, Error **errp)
{
    BusControllerState *s = BUS_CONTROLLER(dev);
    SysBusDevice *sysdev = SYS_BUS_DEVICE(dev);
    static uint8_t NO = 0;
    char *name = g_strdup_printf("ubus.%u", NO);

    sysdev->parent_obj.id = g_strdup_printf("ubc.%u", NO++);
    /* for msgq reg */
    memory_region_init_io(&s->msgq_reg_mem, OBJECT(s), &ub_msgq_reg_ops,
                          s, TYPE_BUS_CONTROLLER, s->msgq_reg_size);
    sysbus_init_mmio(sysdev, &s->msgq_reg_mem);
    sysbus_init_irq(sysdev, &s->irq);
    /* for fm msgq reg */
    memory_region_init_io(&s->fm_msgq_reg_mem, OBJECT(s), &ub_fm_msgq_reg_ops,
                          s, TYPE_BUS_CONTROLLER, s->fm_msgq_reg_size);
    sysbus_init_mmio(sysdev, &s->fm_msgq_reg_mem);
    ub_reg_alloc(dev);
    /* for ub controller mmio */
    memory_region_init(&s->io_mmio, OBJECT(s), "UB_MMIO", UINT64_MAX);
    sysbus_init_mmio(sysdev, &s->io_mmio);

    s->bus = ub_register_root_bus(dev, name, &s->io_mmio);
    ub_save_ubc_list(s);
    s->notify_retry_timer = timer_new_ms(QEMU_CLOCK_REALTIME,
                                         ub_notify_retry_timer_cb, s);

    /* Initialize SIM decoder for cross-node memory access */
    sim_dec_init(s);

    g_free(name);
}

static void ub_bus_instance_guid_unlock(BusControllerDev *ubc_dev);
static void ub_bus_controller_unrealize(DeviceState *dev)
{
    BusControllerState *s = BUS_CONTROLLER(dev);
    SysBusDevice *sysdev = SYS_BUS_DEVICE(dev);
    g_free(sysdev->parent_obj.id);
    ub_fm_controller_unregister(s);
    QLIST_REMOVE(s, node);
    ub_unregister_root_bus(s->bus);
    ub_reg_free(dev);
    ub_bus_instance_guid_unlock(s->ubc_dev);

    /* Cleanup SIM decoder */
    sim_dec_cleanup();
}

static bool ub_bus_controller_needed(void *opaque)
{
    BusControllerState *s = opaque;
    return s->mig_enabled;
}

static Property ub_bus_controller_properties[] = {
    DEFINE_PROP_UINT32("ub-msgq-reg-size", BusControllerState,
                       msgq_reg_size, 0),
    DEFINE_PROP_UINT32("ub-fm-msgq-reg-size", BusControllerState,
                       fm_msgq_reg_size, 0),
    DEFINE_PROP_BOOL("ub-migration-enabled", BusControllerState,
                     mig_enabled, true),
    DEFINE_PROP_END_OF_LIST(),
};

const VMStateDescription vmstate_ub_bus_controller = {
    .name = TYPE_BUS_CONTROLLER,
    .needed = ub_bus_controller_needed,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        /* support migration later */
        VMSTATE_END_OF_LIST()
    }
};

const VMStateDescription vmstate_ub_bus_controller_dev = {
    .name = TYPE_BUS_CONTROLLER_DEV,
    .needed = ub_bus_controller_needed,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        /* support migration later */
        VMSTATE_END_OF_LIST()
    }
};

static void ub_bus_controller_class_init(ObjectClass *class, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(class);

    device_class_set_props(dc, ub_bus_controller_properties);
    dc->realize = ub_bus_controller_realize;
    dc->unrealize = ub_bus_controller_unrealize;
    dc->vmsd = &vmstate_ub_bus_controller;
}

static void ub_bus_controller_instance_init(Object *obj)
{
    /* do nothing now */
}

static void ub_bus_controller_instance_finalize(Object *obj)
{
    /* do nothing now */
}
static const TypeInfo ub_bus_controller_type_info = {
    .name = TYPE_BUS_CONTROLLER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BusControllerState),
    .instance_init = ub_bus_controller_instance_init,
    .instance_finalize = ub_bus_controller_instance_finalize,
    .class_size = sizeof(BusControllerClass),
    .class_init = ub_bus_controller_class_init,
};

static void ub_bus_controller_cfg0_route_table_init(UBDevice *ub_dev)
{
    uint64_t emulated_offset = ub_cfg_offset_to_emulated_offset(UB_ROUTE_TABLE_START, true);
    UbRouteTable *route_table = (UbRouteTable *)(ub_dev->config + emulated_offset);
    uint32_t port_num = MAX(ub_dev->port.port_num, 1);

    /*
     * The route-table shape needs to follow the guest-visible port topology of the
     * controller. Keep the controller entry plus one local and one remote-facing
     * entry per visible port so multi-port UBC models can scale without reworking
     * the cfg0 contract again when inter-node links are added later.
     */
    route_table->entry_num = port_num * 2 + 1;
    route_table->ers = 1;  /* support exact route */
}

static void ub_bus_controller_space_cfg0_init(UBDevice *ub_dev)
{
    UbCfg0Basic *cfg0_basic;
    Cfg0SupportFeature *support_feature;
    UbCfg0ShpCap *shp_cap;
    UbSlotInfo *slot_info;
    BusControllerDev *ubc_dev = BUS_CONTROLLER_DEV(ub_dev);
    uint64_t emulated_offset;

    emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG0_BASIC_START, true);
    cfg0_basic = (UbCfg0Basic *)(ub_dev->config + emulated_offset);
    cfg0_basic->header.slice_version = UB_SLICE_VERSION;
    cfg0_basic->header.slice_used_size = UB_CFG0_BASIC_SLICE_USED_SIZE;
    cfg0_basic->total_num_of_port = ub_dev->port.port_num & UINT16_MASK;
    cfg0_basic->total_num_of_ue = ubc_dev->entity_count;
    cfg0_basic->cap_bitmap[CFG0_CAP2_SHP_INDEX / BITS_PER_BYTE] =
        1 << (CFG0_CAP2_SHP_INDEX % BITS_PER_BYTE);
    support_feature = &cfg0_basic->support_feature;
    support_feature->bits.entity_available = 1;
    support_feature->bits.mtu_supported = 1;
    support_feature->bits.route_table_supported = SUPPORTED;
    support_feature->bits.upi_supported = SUPPORTED;
    support_feature->bits.switch_supported = SUPPORTED;
    support_feature->bits.cc_supported = NOT_SUPPORTED;
    /* SHP CAP */
    emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG0_CAP2_SHP_START, true);
    shp_cap = (UbCfg0ShpCap *)(ub_dev->config + emulated_offset);
    shp_cap->slot_num = 1;
    shp_cap->header.slice_version = UB_SLICE_VERSION;
    shp_cap->header.slice_used_size = (shp_cap->slot_num * sizeof(UbSlotInfo) + sizeof(UbCfg0ShpCap)) / DWORD_SIZE;
    for (int i = 0; i < shp_cap->slot_num; ++i) {
        slot_info = (UbSlotInfo *)((uint8_t *)shp_cap->slot_info + i * sizeof(UbSlotInfo));
        slot_info->pps = 1;
        slot_info->wlps = 1;
        slot_info->plps = 1;
        slot_info->pdss = 1;
        slot_info->pwcs = 1;
        slot_info->start_port_idx = 0;
        slot_info->end_port_idx = 0;
        slot_info->pp_ctrl = 1;
        slot_info->ms_ctrl = 1;
        slot_info->pd_ctrl = 1;
        slot_info->pds_ctrl = 1;
    }
    ub_bus_controller_cfg0_route_table_init(ub_dev);
}

static void ub_bus_controller_space_cfg1_init(UBDevice *ub_dev)
{
    UbCfg1Basic *cfg1_basic;
    Cfg1SupportFeature *support_feature;
    UbCfg1DecoderCap *dec_cap;
    uint8_t *int_type1_raw;
    uint64_t emulated_offset;
    uint8_t *cfg1_raw;
    uint32_t support_feature_l = 0;

    /* basic */
    emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG1_BASIC_START, true);
    cfg1_basic = (UbCfg1Basic *)(ub_dev->config + emulated_offset);
    cfg1_raw = ub_dev->config + emulated_offset;
    cfg1_basic->header.slice_version = UB_SLICE_VERSION;
    cfg1_basic->header.slice_used_size = UB_CFG1_BASIC_SLICE_USED_SIZE;

    cfg1_basic->cap_bitmap[CFG1_DECODER_CAP_INDEX / BITS_PER_BYTE] |=
        1 << (CFG1_DECODER_CAP_INDEX % BITS_PER_BYTE);
    cfg1_basic->cap_bitmap[CFG1_INT_CAP_INDEX / BITS_PER_BYTE] |=
        1 << (CFG1_INT_CAP_INDEX % BITS_PER_BYTE);

    support_feature = &cfg1_basic->support_feature;
    support_feature->bits.mgs = SUPPORTED;
    support_feature->bits.ubbas = SUPPORTED;
    support_feature->bits.ers0s = SUPPORTED;
    support_feature->bits.ers1s = SUPPORTED;
    support_feature->bits.ers2s = SUPPORTED;
    support_feature->bits.matt_juris = UB_DRIVE;
    support_feature_l |= BIT(2);  /* MGS */
    support_feature_l |= BIT(5);  /* UBBAS */
    support_feature_l |= BIT(6);  /* ERS0S */
    support_feature_l |= BIT(7);  /* ERS1S */
    support_feature_l |= BIT(8);  /* ERS2S */
    support_feature_l |= BIT(10); /* DECODER_JURIS */
    cfg1_basic->ers_space_size[0] = UBC_ERS0_SPACE_SIZE;
    cfg1_basic->ers_space_size[1] = UBC_ERS1_SPACE_SIZE;
    cfg1_basic->ers_space_size[2] = UBC_ERS2_SPACE_SIZE;
    {
        uint64_t ers_base = ub_ers_phys_base();
        cfg1_basic->ers_start_addr[0] = ers_base;
        cfg1_basic->ers_start_addr[1] = ers_base + 0x100000ULL;
        cfg1_basic->ers_start_addr[2] = ers_base + 0x200000ULL;
        cfg1_basic->ers_ubba[0] = ers_base;
        cfg1_basic->ers_ubba[1] = ers_base + 0x100000ULL;
        cfg1_basic->ers_ubba[2] = ers_base + 0x200000ULL;
    }
    cfg1_basic->eid_upi_ten = UBC_EID_UPI_TEN_DEFAULT_VAL;
    cfg1_basic->class_code = UBC_CLASS_CODE;
    /*
     * Keep the raw config image aligned with the guest-visible register layout.
     * Some packed struct fields in the current tree do not land on the exact
     * offsets the Linux UB driver reads.
     */
    *(uint32_t *)(cfg1_raw + 0x24) = support_feature_l;
    *(uint32_t *)(cfg1_raw + 0x34) = UBC_ERS0_SPACE_SIZE;
    *(uint32_t *)(cfg1_raw + 0x38) = UBC_ERS1_SPACE_SIZE;
    *(uint32_t *)(cfg1_raw + 0x3c) = UBC_ERS2_SPACE_SIZE;
    {
        uint64_t eb = ub_ers_phys_base();
        /* ers_start_addr[0..2] at cfg1_raw+0x40 */
        *(uint32_t *)(cfg1_raw + 0x40) = (uint32_t)(eb & UINT32_MAX);
        *(uint32_t *)(cfg1_raw + 0x44) = (uint32_t)(eb >> 32);
        *(uint32_t *)(cfg1_raw + 0x48) = (uint32_t)((eb + 0x100000ULL) & UINT32_MAX);
        *(uint32_t *)(cfg1_raw + 0x4c) = (uint32_t)((eb + 0x100000ULL) >> 32);
        *(uint32_t *)(cfg1_raw + 0x50) = (uint32_t)((eb + 0x200000ULL) & UINT32_MAX);
        *(uint32_t *)(cfg1_raw + 0x54) = (uint32_t)((eb + 0x200000ULL) >> 32);
        /* ers_ubba[0..2] at cfg1_raw+0x58 */
        *(uint32_t *)(cfg1_raw + 0x58) = (uint32_t)(eb & UINT32_MAX);
        *(uint32_t *)(cfg1_raw + 0x5c) = (uint32_t)(eb >> 32);
        *(uint32_t *)(cfg1_raw + 0x60) = (uint32_t)((eb + 0x100000ULL) & UINT32_MAX);
        *(uint32_t *)(cfg1_raw + 0x64) = (uint32_t)((eb + 0x100000ULL) >> 32);
        *(uint32_t *)(cfg1_raw + 0x68) = (uint32_t)((eb + 0x200000ULL) & UINT32_MAX);
        *(uint32_t *)(cfg1_raw + 0x6c) = (uint32_t)((eb + 0x200000ULL) >> 32);
    }
    cfg1_basic->dev_rs_access_en = 1;
    *(uint32_t *)(cfg1_raw + 0xbc) = 1;
    *(uint32_t *)(cfg1_raw + 0x90) = UBC_EID_UPI_TEN_DEFAULT_VAL;
    *(uint32_t *)(cfg1_raw + 0xa4) = UBC_CLASS_CODE;
    
    /* LIMIT ADDRESS WIDTH: Set addr_width_cap (at offset 0x80 in CFG1) to 40 bits */
    *(uint32_t *)(cfg1_raw + 0x80) = 40; 
    
    cfg1_raw[0x04 + (CFG1_INT_CAP_INDEX / BITS_PER_BYTE)] |=
        1 << (CFG1_INT_CAP_INDEX % BITS_PER_BYTE);
    /* decoder cap */
    emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG1_CAP1_DECODER, true);
    dec_cap = (UbCfg1DecoderCap *)(ub_dev->config + emulated_offset);
    dec_cap->header.slice_version = UB_SLICE_VERSION;
    dec_cap->header.slice_used_size = sizeof(UbCfg1DecoderCap) / DWORD_SIZE;
    dec_cap->decoder.event_size_sup = DECODER_CAP_EVENT_SIZE;
    dec_cap->decoder.cmd_size_sup = DECODER_CAP_CMD_SIZE;
    dec_cap->decoder.mmio_size_sup = DECODER_CAP_MMIO_SIZE;

    emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG1_CAP3_INT_TYPE1, true);
    int_type1_raw = ub_dev->config + emulated_offset;
    *(uint32_t *)(int_type1_raw + 0x0) = UB_SLICE_VERSION | (sizeof(UbCfg1IntType1Cap) / DWORD_SIZE) << 16;
    *(uint16_t *)(int_type1_raw + 0x8) = 6;
}

static void ub_bus_controller_wmask_init(UBDevice *ub_dev)
{
    UbCfg1DecoderCap *dec_cap_mask;
    UbCfg0ShpCap *cfg0_shp_wmask, *cfg0_shp;
    uint64_t emulated_offset;
    uint8_t *cfg1_wmask_raw;
    uint8_t *int_type1_wmask_raw;

    /* cfg0 cap */
    emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG0_CAP2_SHP_START, true);
    cfg0_shp_wmask = (UbCfg0ShpCap *)(ub_dev->wmask + emulated_offset);
    cfg0_shp = (UbCfg0ShpCap *)(ub_dev->config + emulated_offset);
    memset(cfg0_shp_wmask, 0, UB_SLICE_SZ);
    for (int i = 0; i < cfg0_shp->slot_num; ++i) {
        cfg0_shp_wmask->slot_info[i].pp_ctrl = ~0;
        cfg0_shp_wmask->slot_info[i].wl_ctrl = ~0;
        cfg0_shp_wmask->slot_info[i].pl_ctrl = ~0;
        cfg0_shp_wmask->slot_info[i].ms_ctrl = ~0;
        cfg0_shp_wmask->slot_info[i].pd_ctrl = ~0;
        cfg0_shp_wmask->slot_info[i].pds_ctrl = ~0;
        cfg0_shp_wmask->slot_info[i].pw_ctrl = ~0;
    }

    /* cfg1 cap */
    emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG1_CAP1_DECODER, true);
    dec_cap_mask = (UbCfg1DecoderCap *)(ub_dev->wmask + emulated_offset);
    memset(dec_cap_mask, 0, sizeof(UbCfg1DecoderCap));
    dec_cap_mask->decoder_ctrl.decoder_en = ~0;
    memset(&dec_cap_mask->dec_matt_ba, 0xff, sizeof(dec_cap_mask->dec_matt_ba));
    memset(&dec_cap_mask->dec_mmio_ba, 0xff, sizeof(dec_cap_mask->dec_mmio_ba));
    dec_cap_mask->decoder_cmdq_cfg.cmdq_en = ~0;
    dec_cap_mask->decoder_cmdq_cfg.cmdq_size_use = ~0;
    dec_cap_mask->decoder_cmdq_prod.cmdq_wr_idx = ~0;
    dec_cap_mask->decoder_cmdq_prod.cmdq_err_resp = ~0;
    dec_cap_mask->decoder_cmdq_cons.cmdq_rd_idx = ~0;
    dec_cap_mask->decoder_cmdq_cons.cmdq_err = ~0;
    dec_cap_mask->decoder_cmdq_cons.cmdq_err_res = ~0;
    dec_cap_mask->decoder_cmdq_ba.cmdq_ba = ~0;
    dec_cap_mask->decoder_evtq_cfg.evtq_en = ~0;
    dec_cap_mask->decoder_evtq_cfg.evtq_size_use = ~0;
    dec_cap_mask->decoder_evtq_prod.evtq_wr_idx = ~0;
    dec_cap_mask->decoder_evtq_cons.evtq_rd_idx = ~0;
    dec_cap_mask->decoder_evtq_ba.evtq_ba = ~0;

    emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG1_BASIC_START, true);
    cfg1_wmask_raw = ub_dev->wmask + emulated_offset;
    *(uint32_t *)(cfg1_wmask_raw + 0x88) = ~0U;
    *(uint32_t *)(cfg1_wmask_raw + 0x8c) = ~0U;
    *(uint32_t *)(cfg1_wmask_raw + 0xb4) = ~0U;

    emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG1_CAP3_INT_TYPE1, true);
    int_type1_wmask_raw = ub_dev->wmask + emulated_offset;
    *(uint8_t *)(int_type1_wmask_raw + 0x4) = 0x1;
    *(uint32_t *)(int_type1_wmask_raw + 0xc) = ~0U;
    *(uint32_t *)(int_type1_wmask_raw + 0x10) = ~0U;
    *(uint32_t *)(int_type1_wmask_raw + 0x14) = ~0U;
    *(uint32_t *)(int_type1_wmask_raw + 0x18) = ~0U;
    *(uint32_t *)(int_type1_wmask_raw + 0x1c) = ~0U;
    *(uint32_t *)(int_type1_wmask_raw + 0x20) = ~0U;
}

static void ub_bus_controller_w1cmask_init(UBDevice *ub_dev)
{
    UbCfg0ShpCap *cfg0_shp_w1cmask, *cfg0_shp;
    uint64_t emulated_offset;

    /* cfg0 cap */
    emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG0_CAP2_SHP_START, true);
    cfg0_shp_w1cmask = (UbCfg0ShpCap *)(ub_dev->w1cmask + emulated_offset);
    cfg0_shp = (UbCfg0ShpCap *)(ub_dev->config + emulated_offset);
    memset(cfg0_shp_w1cmask, 0, UB_SLICE_SZ);
    for (int i = 0; i < cfg0_shp->slot_num; ++i) {
        cfg0_shp_w1cmask->slot_info[i].pp_st = ~0;
        cfg0_shp_w1cmask->slot_info[i].pdsc_st = ~0;
    }
}

static void ub_bus_controller_dev_config_space_init(UBDevice *dev)
{
    ub_bus_controller_space_cfg0_init(dev);
    ub_bus_controller_space_cfg1_init(dev);
    ub_bus_controller_wmask_init(dev);
    ub_bus_controller_w1cmask_init(dev);
}

static bool ub_ubc_is_empty(UBBus *bus)
{
    UBDevice *dev;
    QLIST_FOREACH(dev, &bus->devices, node) {
        if (dev->dev_type == UB_TYPE_IBUS_CONTROLLER) {
            return false;
        }
    }
    return true;
}

static char *ub_bus_instance_guid_lock_dir(void)
{
    const char *runtime_dir = g_get_user_runtime_dir();

    if (runtime_dir && runtime_dir[0] != '\0') {
        return g_build_filename(runtime_dir, "ub-qemu", NULL);
    }

    return g_strdup_printf("%s/ub-qemu-%u", g_get_tmp_dir(),
                           (unsigned int)getuid());
}

static int ub_bus_instance_guid_lock(UbGuid *guid)
{
    char path[256] = {0};
    char guid_str[UB_DEV_GUID_STRING_LENGTH + 1] = {0};
    g_autofree char *lock_dir = NULL;
    int lock_fd;

    lock_dir = ub_bus_instance_guid_lock_dir();

    if (g_mkdir_with_parents(lock_dir, 0700) < 0) {
        qemu_log("failed to create bus instance lock dir %s: %s\n",
                 lock_dir, strerror(errno));
        return -1;
    }

    ub_device_get_str_from_guid(guid, guid_str, UB_DEV_GUID_STRING_LENGTH + 1);
    snprintf(path, sizeof(path), "%s/ub-bus-instance-%s.lock", lock_dir,
             guid_str);
    lock_fd = open(path, O_RDONLY | O_CREAT, 0600);
    if (lock_fd < 0) {
        qemu_log("failed to open lock file %s: %s\n", path, strerror(errno));
        return -1;
    }

    if (flock(lock_fd, LOCK_EX | LOCK_NB)) {
        qemu_log("lock %s failed: %s\n", path, strerror(errno));
        close(lock_fd);
        return -1;
    }

    return lock_fd;
}

static void ub_bus_instance_guid_unlock(BusControllerDev *ubc_dev)
{
    char guid_str[UB_DEV_GUID_STRING_LENGTH + 1] = {0};

    if (ubc_dev->bus_instance_lock_fd <= 0) {
        return;
    }

    ub_device_get_str_from_guid(&ubc_dev->bus_instance_guid, guid_str,
                                UB_DEV_GUID_STRING_LENGTH + 1);
    qemu_log("unlock ub bus instance lock for guid: %s\n", guid_str);
    if (flock(ubc_dev->bus_instance_lock_fd, LOCK_UN)) {
        qemu_log("failed to unlock for bus instance guid %s: %s\n",
                 guid_str, strerror(errno));
    }
    close(ubc_dev->bus_instance_lock_fd);
}

static int ub_bus_instance_process(BusControllerDev *ubc_dev, Error **errp)
{
    int lock_fd;

    if (ub_guid_is_none(&ubc_dev->bus_instance_guid)) {
        const char *node_id = g_getenv("UB_FM_NODE_ID");
        if (node_id) {
            /* Generate a stable GUID from node_id */
            uint32_t hash = g_str_hash(node_id);
            ubc_dev->bus_instance_guid.seq_num = hash;
            ubc_dev->bus_instance_guid.vendor = 0xCC08;
            ubc_dev->bus_instance_guid.device_id = 0x0541;
            ubc_dev->bus_instance_guid.version = 0;
            ubc_dev->bus_instance_guid.type = UB_TYPE_BUS_INSTANCE;
            
            char guid_str[UB_DEV_GUID_STRING_LENGTH + 1];
            ub_device_get_str_from_guid(&ubc_dev->bus_instance_guid, guid_str, sizeof(guid_str));
            qemu_log("ubc: auto-generated bus_instance_guid from node_id %s: %s\n",
                     node_id, guid_str);
        } else {
            error_setg(errp, "ubc bus instance guid is required (or UB_FM_NODE_ID)");
            return -1;
        }
    }

    lock_fd = ub_bus_instance_guid_lock(&ubc_dev->bus_instance_guid);
    if (lock_fd < 0) {
        error_setg(errp, "ubc bus instance guid lock failed, it may used by other vm");
        return -1;
    }

    ubc_dev->bus_instance_lock_fd = lock_fd;
    return 0;
}

void ub_entity_table_init(BusControllerDev *ubc_dev)
{
    uint32_t i;

    memset(ubc_dev->entities, 0, sizeof(ubc_dev->entities));
    for (i = 0; i < ubc_dev->entity_count && i < UB_MAX_ENTITIES; i++) {
        UBEntityDesc *e = &ubc_dev->entities[i];
        e->entity_idx = i;
        e->device_id = (i == 0) ? 0x0541 : 0x0542;
        /* entity_idx=0 (controller) is statically enumerated by the guest,
         * so it starts PRESENT.  entity_idx>=1 must be injected via msgq
         * by the entity_plan diff logic, so they start ABSENT. */
        e->state = (i == 0) ? UB_ENTITY_STATE_PRESENT : UB_ENTITY_STATE_ABSENT;
        e->upi = 1;
        e->cna = ubc_dev->parent.cna ? (ubc_dev->parent.cna + i) : (0x200 + i);
        e->eid[0] = ubc_dev->parent.eid + i;
        e->ueid[0] = ubc_dev->parent.eid;
        e->guid[0] = (uint32_t)ubc_dev->parent.guid.seq_num + i;
        e->guid[1] = (uint32_t)((ubc_dev->parent.guid.seq_num + i) >> 32);
        /*
         * GUID DW2 layout (matches guest struct ub_guid bits):
         *   bits 28-31: version (4 bits)
         *   bits 24-27: type (4 bits)
         *   bits 0-23:  reserved (24 bits)
         * GUID DW3 layout:
         *   bits 0-15:  vendor
         *   bits 16-31: device_id
         *
         * For FE entities, type must be UB_GUID_TYPE_BUS_CONTROLLER (=1)
         * because the guest's ub_entity_reg_check expects guid type ==
         * UB_TYPE_CONTROLLER (== 1).  The parent IBUS_CONTROLLER (type=2)
         * is only for the bus controller entity itself (entity_idx=0).
         */
        e->guid[2] = ((uint32_t)ubc_dev->parent.guid.version << 28) |
                     ((uint32_t)UB_GUID_TYPE_BUS_CONTROLLER << 24) |
                     ((uint32_t)ubc_dev->parent.guid.rsv);
        e->guid[3] = ((uint32_t)e->device_id << 16) |
                     (uint32_t)ubc_dev->parent.guid.vendor;
        e->ers[0].ss = UBC_ERS0_SPACE_SIZE;
        e->ers[0].sa_l = (uint32_t)((ub_ers_phys_base() + i * 0x400000ULL) & UINT32_MAX);
        e->ers[0].sa_h = (uint32_t)((ub_ers_phys_base() + i * 0x400000ULL) >> 32);
        e->ers[1].ss = UBC_ERS1_SPACE_SIZE;
        e->ers[1].sa_l = (uint32_t)((ub_ers_phys_base() + 0x100000ULL + i * 0x400000ULL) & UINT32_MAX);
        e->ers[1].sa_h = (uint32_t)((ub_ers_phys_base() + 0x100000ULL + i * 0x400000ULL) >> 32);
        e->ers[2].ss = UBC_ERS2_SPACE_SIZE;
        e->ers[2].sa_l = (uint32_t)((ub_ers_phys_base() + 0x200000ULL + i * 0x400000ULL) & UINT32_MAX);
        e->ers[2].sa_h = (uint32_t)((ub_ers_phys_base() + 0x200000ULL + i * 0x400000ULL) >> 32);
        qemu_log("entity_table_init: [%u] device_id=0x%04x eid=0x%x ueid=0x%x "
                 "cna=0x%x upi=%u state=%s\n",
                 i, e->device_id, e->eid[0], e->ueid[0], e->cna, e->upi,
                 e->state == UB_ENTITY_STATE_PRESENT ? "present" : "absent");
    }
    qemu_log("entity_table_init: %u entities initialized\n", ubc_dev->entity_count);
}

UBEntityDesc *ub_entity_desc_for_idx(BusControllerDev *ubc_dev, uint32_t entity_idx)
{
    if (!ubc_dev || entity_idx >= ubc_dev->entity_count || entity_idx >= UB_MAX_ENTITIES) {
        return NULL;
    }
    return &ubc_dev->entities[entity_idx];
}

void ub_entity_cfg_spaces_init(BusControllerDev *ubc_dev)
{
    uint32_t i;
    UBDevice *ub_dev = &ubc_dev->parent;
    uint32_t base_cfg_size = ub_emulated_config_size();

    for (i = 0; i < ubc_dev->entity_count && i < UB_MAX_ENTITIES; i++) {
        UBEntityCfgSpace *space = &ubc_dev->entity_cfg_spaces[i];
        UBEntityDesc *e = &ubc_dev->entities[i];

        /* 分配独立配置空间 */
        space->cfg_base = g_malloc0(base_cfg_size);
        if (!space->cfg_base) {
            qemu_log("entity_cfg_spaces_init: failed to alloc for entity %u\n", i);
            continue;
        }

        /* 复制基础配置 */
        memcpy(space->cfg_base, ub_dev->config, base_cfg_size);

        /* 设置实体特定字段 */
        space->cfg_size = base_cfg_size;
        space->eid = e->eid[0];
        space->cna = e->cna;
        space->upi = e->upi;
        space->initialized = true;

        /* 修改该实体配置空间中的 EID/CNA/UPICNA */
        uint64_t emulated_offset;

        /* 修改 EID */
        emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG0_EID_0_OFFSET, true);
        uint32_t *eid_ptr = (uint32_t *)(space->cfg_base + emulated_offset);
        *eid_ptr = cpu_to_le32(e->eid[0]);

        /* 修改 UEID (device UEID — used by UMMU for address translation) */
        emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG0_DEV_UEID_OFFSET, true);
        uint32_t *ueid_ptr = (uint32_t *)(space->cfg_base + emulated_offset);
        *ueid_ptr = cpu_to_le32(e->ueid[0]);

        /* 修改 UPICNA */
        emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG0_UPI_OFFSET, true);
        uint32_t *upi_cna_ptr = (uint32_t *)(space->cfg_base + emulated_offset);
        *upi_cna_ptr = cpu_to_le32((e->upi & 0x7FFF) | ((e->cna & 0xFF) << 16));

        /* 修改 FM CNA */
        emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG0_FM_CNA_OFFSET, true);
        uint32_t *cna_ptr = (uint32_t *)(space->cfg_base + emulated_offset);
        *cna_ptr = cpu_to_le32(e->cna);

        /* FE entity (entity_idx > 0) config space overrides.
         * The controller config (entity_idx=0) is correct as-is;
         * FE entities need different GUID, class_code to pass guest
         * validation in ub_setup_ent + ub_entity_type_init. */
        if (i > 0) {
            /* Override GUID — must match what UB_DEV_REG carries */
            emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG0_BASIC_GUID_START, true);
            memcpy(space->cfg_base + emulated_offset, e->guid, sizeof(e->guid));

            /* Override DeviceID and VendorID in CFG0 (DW0) */
            emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG0_BASIC_START, true);
            stl_le_p(space->cfg_base + emulated_offset, (e->device_id << 16) | 0xCC08);

            /* Override entity_idx and type in CFG0 (DW1) 
             * Layout: [31:24] type | [23:16] rsvd | [15:0] entity_idx
             * type=1 (PERIPHERAL) for all entities > 0
             */
            stl_le_p(space->cfg_base + emulated_offset + 4, (1 << 24) | (i & 0xFFFF));

            /* Override class_code in CFG1 (0x02 = UB_BASE_CODE_NETWORK) */
            emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG1_BASIC_START, true);
            stl_le_p(space->cfg_base + emulated_offset + 0xa4, 0x0002);

            /* Enable device and bus access for this entity */
            *(space->cfg_base + emulated_offset + 0xb8) = 1; /* bus_access_en */
            *(space->cfg_base + emulated_offset + 0xbc) = 1; /* dev_rs_access_en */

            /* Override ERS base addresses in CFG1 for this entity's view */
            uint64_t entity_ers_base = ub_ers_phys_base() + (uint64_t)i * 0x400000ULL;
            for (int r = 0; r < 3; r++) {
                uint64_t reg_off = emulated_offset + offsetof(UbCfg1Basic, ers_ubba) + r * 8;
                uint64_t r_addr = entity_ers_base + r * 0x100000ULL;
                stq_le_p(space->cfg_base + reg_off, r_addr);
            }
        }

        qemu_log("entity_cfg_spaces_init: [%u] eid=%#x cna=%#x upi=%u cfg_size=%u"
                 " guid=[%08x %08x %08x %08x]\n",
                 i, space->eid, space->cna, space->upi, space->cfg_size,
                 e->guid[0], e->guid[1], e->guid[2], e->guid[3]);
    }

    qemu_log("entity_cfg_spaces_init: %u entity cfg spaces initialized\n",
             ubc_dev->entity_count);
}

void ub_entity_cfg_spaces_cleanup(BusControllerDev *ubc_dev)
{
    uint32_t i;

    for (i = 0; i < UB_MAX_ENTITIES; i++) {
        UBEntityCfgSpace *space = &ubc_dev->entity_cfg_spaces[i];
        if (space->initialized && space->cfg_base) {
            g_free(space->cfg_base);
            space->cfg_base = NULL;
            space->initialized = false;
        }
    }
}

int ub_inject_entity_reg(BusControllerState *s, const UBEntityDesc *e, Error **errp)
{
    static uint16_t msn_seed = 1;
    BusControllerDev *ubc_dev = s->ubc_dev;
    uint32_t *dw;
    uint8_t *msg_buf;
    uint32_t msg_size;
    uint32_t pi;
    HiMsgCqe cqe;

    if (!s->msgq.rq_inited || !s->msgq.cq_inited) {
        error_setg(errp, "msgq not initialized");
        return -1;
    }

    /* Total size: 32 (header) + 92 (payload) = 124 */
    msg_size = 124;
    msg_buf = g_malloc0(msg_size);

    /* 构造消息头 */
    MsgPktHeader *header = (MsgPktHeader *)msg_buf;
    header->ta_opcode = TAH_OPCODE_MSG;
    
    /* Routing info */
    header->nth.dcna = cpu_to_le16(ubc_dev->parent.cna & 0xFFFF);
    header->nth.scna = header->nth.dcna;
    header->nth.mgmt = 1;
    
    /* EID info: SEID is typically controller's EID for local management cmds */
    uint32_t bus_eid = ubc_dev->parent.eid;
    header->seid_l = bus_eid & 0xFFF;
    header->seid_h = (bus_eid >> 12) & 0xFF;
    header->deid = bus_eid & 0xFFFFF;

    /* 
     * Manually construct DW7 (MsgExtendedHeader) to match guest bitfields precisely:
     * DW7 layout: [code:8 | rsp_status:8 | rsvd:4 | plen:12]
     * In UB, plen is payload size (excluding header). So 92 bytes.
     */
    uint32_t code = (MSG_REQ << 7) | (UB_MSG_CODE_POOL << 4) | UB_DEV_REG;
    uint32_t dw7 = (code << 24) | (92 & 0xFFF); 
    header->msgetah_dw = cpu_to_le32(dw7);

    /* 
     * Correct payload offset: MSG_PKT_HEADER_SIZE is 32 in the guest kernel.
     * DW0 of payload starts at msg_buf + 32.
     */
    dw = (uint32_t *)(msg_buf + 32);

    /* DW0: entity_idx[15:0], upi[30:16] */
    dw[0] = cpu_to_le32((e->entity_idx & 0xFFFF) | ((e->upi & 0x7FFF) << 16));
    
    /* DW1-4: EID */
    for (int i = 0; i < 4; i++) dw[1+i] = cpu_to_le32(e->eid[i]);
    
    /* DW5-8: GUID */
    for (int i = 0; i < 4; i++) dw[5+i] = cpu_to_le32(e->guid[i]);
    
    /* DW9: CNA[23:0] */
    dw[9] = cpu_to_le32(e->cna & 0xFFFFFF);
    
    /* DW10-13: UEID */
    for (int i = 0; i < 4; i++) dw[10+i] = cpu_to_le32(e->ueid[i]);

    /* DW14-22: ERS (3 entries * 3 DW each) */
    for (int i = 0; i < 3; i++) {
        dw[14 + i*3] = cpu_to_le32(e->ers[i].ss);
        dw[15 + i*3] = cpu_to_le32(e->ers[i].sa_l);
        dw[16 + i*3] = cpu_to_le32(e->ers[i].sa_h);
    }

    /* 注入到 RQ */
    pi = fill_rq(s, msg_buf, msg_size);
    g_free(msg_buf);

    if (pi == UINT32_MAX) {
        error_setg(errp, "fill_rq failed");
        return -1;
    }

    /* 填充 CQE — matches cfg_cpl_notify pattern */
    memset(&cqe, 0, sizeof(cqe));
    cqe.task_type = PROTOCOL_MSG;
    cqe.type = MSG_REQ;
    cqe.msg_code = UB_MSG_CODE_POOL;
    cqe.sub_msg_code = UB_DEV_REG;
    cqe.p_len = msg_size;
    cqe.msn = msn_seed++;
    cqe.rq_pi = pi;
    cqe.status = CQE_SUCCESS;

    if (fill_cq(s, &cqe) == UINT32_MAX) {
        error_setg(errp, "fill_cq failed");
        return -1;
    }

    qemu_log("entity_reg inject SUCCESS: entity_idx=%u eid=%#x ueid=%#x device_id=%#x cna=%#x\n",
             e->entity_idx, e->eid[0], e->ueid[0], e->device_id, e->cna);

    return 0;
}

int ub_inject_entity_rls(BusControllerState *s, uint32_t eid, uint8_t reason, Error **errp)
{
    static uint16_t msn_seed = 1;
    BusControllerDev *ubc_dev = s->ubc_dev;
    UBPoolEntityRlsMsg *rls_msg;
    uint8_t *msg_buf;
    uint32_t msg_size;
    uint32_t pi;
    HiMsgCqe cqe;

    if (!s->msgq.rq_inited || !s->msgq.cq_inited) {
        error_setg(errp, "msgq not initialized");
        return -1;
    }

    if (!ubc_dev) {
        error_setg(errp, "ubc_dev not initialized");
        return -1;
    }

    msg_size = MSG_PKT_HEADER_SIZE + UB_POOL_ENTITY_RLS_SIZE;
    msg_buf = g_malloc0(msg_size);

    /* 构造消息头 — CFM->guest request, matches cfg_cpl_notify pattern */
    MsgPktHeader *header = (MsgPktHeader *)msg_buf;
    header->ta_opcode = TAH_OPCODE_MSG;
    header->msgetah.type = MSG_REQ;
    header->msgetah.msg_code = UB_MSG_CODE_POOL;
    header->msgetah.sub_msg_code = UB_DEV_RLS;
    header->msgetah.plen = UB_POOL_ENTITY_RLS_SIZE;

    /* 构造 entity_rls_msg_pld */
    rls_msg = (UBPoolEntityRlsMsg *)(msg_buf + MSG_PKT_HEADER_SIZE);
    rls_msg->eid[0] = eid;
    rls_msg->eid[1] = 0;
    rls_msg->eid[2] = 0;
    rls_msg->eid[3] = 0;
    rls_msg->reason = reason;
    rls_msg->rsvd1 = 0;

    /* 注入到 RQ */
    pi = fill_rq(s, msg_buf, msg_size);
    g_free(msg_buf);

    if (pi == UINT32_MAX) {
        error_setg(errp, "fill_rq failed");
        return -1;
    }

    /* 填充 CQE — matches cfg_cpl_notify pattern */
    memset(&cqe, 0, sizeof(cqe));
    cqe.task_type = PROTOCOL_MSG;
    cqe.type = MSG_REQ;
    cqe.msg_code = UB_MSG_CODE_POOL;
    cqe.sub_msg_code = UB_DEV_RLS;
    cqe.p_len = msg_size;
    cqe.msn = msn_seed++;
    cqe.rq_pi = pi;
    cqe.status = CQE_SUCCESS;

    if (fill_cq(s, &cqe) == UINT32_MAX) {
        error_setg(errp, "fill_cq failed");
        return -1;
    }

    qemu_log("entity_rls inject SUCCESS: eid=%#x reason=%#x\n", eid, reason);

    return 0;
}

static void ub_bus_controller_dev_realize(UBDevice *dev, Error **errp)
{
    UBBus *bus = UB_BUS(qdev_get_parent_bus(DEVICE(dev)));
    BusControllerState *ubc = container_of_ubbus(bus);
    VirtMachineState *vms = VIRT_MACHINE(qdev_get_machine());

    vms->ub_bus = bus;

    ub_obmm_remote_model_init(&BUS_CONTROLLER_DEV(dev)->remote_memory_model);
    if (BUS_CONTROLLER_DEV(dev)->remote_memory_model_manifest &&
        !ub_obmm_remote_model_load(
            &BUS_CONTROLLER_DEV(dev)->remote_memory_model,
            BUS_CONTROLLER_DEV(dev)->remote_memory_model_manifest, errp)) {
        return;
    }
    BUS_CONTROLLER_DEV(dev)->remote_memory_model_timer = timer_new_ns(
        QEMU_CLOCK_VIRTUAL, ubc_obmm_remote_model_timer,
        BUS_CONTROLLER_DEV(dev));
    BUS_CONTROLLER_DEV(dev)->obmm_async_children = g_new0(
        UbcObmmAsyncChild, UBC_OBMM_ASYNC_CHILD_CAPACITY);
    BUS_CONTROLLER_DEV(dev)->obmm_async = ub_obmm_async_new(
        BUS_CONTROLLER_DEV(dev));
    if (!BUS_CONTROLLER_DEV(dev)->obmm_async) {
        error_setg(errp, "failed to create OBMM asynchronous endpoint");
        return;
    }
    BUS_CONTROLLER_DEV(dev)->linqu_uapi_authorization_timer = timer_new_ns(
        QEMU_CLOCK_VIRTUAL, linqu_uapi_authorization_timer,
        BUS_CONTROLLER_DEV(dev));
    BUS_CONTROLLER_DEV(dev)->ub_async_load = ub_async_load_device_new(
        BUS_CONTROLLER_DEV(dev),
        BUS_CONTROLLER_DEV(dev)->async_load_model, errp);
    if (!BUS_CONTROLLER_DEV(dev)->ub_async_load) {
        if (!errp || !*errp) {
            error_setg(errp,
                       "failed to create OBMM async-load endpoint");
        }
        return;
    }
    if (BUS_CONTROLLER_DEV(dev)->remote_memory_model.loaded) {
        qemu_log("OBMM_REMOTE_MODEL: loaded manifest=%s manifest_hash=%s enabled=%u\n",
                 BUS_CONTROLLER_DEV(dev)->remote_memory_model_manifest,
                 BUS_CONTROLLER_DEV(dev)->remote_memory_model.manifest_hash,
                 BUS_CONTROLLER_DEV(dev)->remote_memory_model.config.enabled);
    }

    if (!ub_ubc_is_empty(bus)) {
        qemu_log("ubc realize repetitively\n");
        error_setg(errp, "ubc realize repetitively");
        return;
    }

    ubc->ubc_dev = BUS_CONTROLLER_DEV(dev);
    ubc->ubc_dev->dma_fallback_init_window = true;
    ubc->ubc_dev->next_tp_id = 1;
    ub_entity_table_init(ubc->ubc_dev);
    ub_entity_cfg_spaces_init(ubc->ubc_dev);
    if (dev->guid.type != UB_GUID_TYPE_IBUS_CONTROLLER &&
        dev->guid.type != UB_GUID_TYPE_BUS_CONTROLLER) {
        qemu_log("%s device type set error, expect: %u or %u, actual: %u\n",
                 dev->qdev.id, UB_GUID_TYPE_IBUS_CONTROLLER,
                 UB_GUID_TYPE_BUS_CONTROLLER, dev->guid.type);
        error_setg(errp, "%s device type set error, expect: %u or %u, actual: %u\n",
                   dev->qdev.id, UB_GUID_TYPE_IBUS_CONTROLLER,
                   UB_GUID_TYPE_BUS_CONTROLLER, dev->guid.type);
        return;
    }

    dev->dev_type = UB_TYPE_IBUS_CONTROLLER;
    ub_bus_controller_dev_config_space_init(dev);
    ub_bus_controller_init_ers_regions(dev);
    ub_bus_controller_activate_ers_mappings(dev);
    if (0 > ummu_associating_with_ubc(ubc)) {
        qemu_log("failed to associating ubc with ummu. %s\n", dev->name);
    }

    qemu_log("set type UB_TYPE_CONTROLLER, ubc %p, "
             "ubc->ubc_dev %p, bus %p\n", ubc, ubc->ubc_dev, bus);
    if (ub_bus_instance_process(ubc->ubc_dev, errp)) {
        qemu_log("ub bus instance process failed\n");
        return;
    }
    ub_fm_controller_register(ubc);

    /* Publish endpoint info for discovery scripts */
    {
        const char *node_id = g_getenv("UB_FM_NODE_ID");
        const char *shared_dir = g_getenv("UB_FM_SHARED_DIR");
        if (node_id && shared_dir) {
            g_autofree char *token = g_strdup_printf("%s_%s", node_id, DEVICE(dev)->id);
            g_autofree char *path = g_strdup_printf("%s/%s__1.ini", shared_dir, token);
            GKeyFile *keyfile = g_key_file_new();
            char guid_str[UB_DEV_GUID_STRING_LENGTH + 1];

            ub_device_get_str_from_guid(&dev->guid, guid_str, sizeof(guid_str));
            g_key_file_set_string(keyfile, "endpoint", "device_id", DEVICE(dev)->id);
            g_key_file_set_uint64(keyfile, "endpoint", "port_idx", 1);
            g_key_file_set_string(keyfile, "endpoint", "guid", guid_str);
            g_key_file_set_uint64(keyfile, "endpoint", "primary_cna", dev->cna);

            g_autofree char *data = g_key_file_to_data(keyfile, NULL, NULL);
            g_mkdir_with_parents(shared_dir, 0755);
            g_file_set_contents(path, data, -1, NULL);
            g_key_file_free(keyfile);
            qemu_log("ubc: published endpoint info to %s\n", path);
        }
    }

    /* Finally, publish the full device snapshot */
    ub_publish_device_snapshot(dev, errp);
}

static Property ub_bus_controller_dev_properties[] = {
    DEFINE_PROP_UB_DEV_GUID("bus_instance_guid", BusControllerDev, bus_instance_guid),
    DEFINE_PROP_UINT32("entity_count", BusControllerDev, entity_count, 1),
    DEFINE_PROP_UINT32("pto-device-cna", BusControllerDev, pto_device_cna, 0),
    DEFINE_PROP_UINT64("pto-authorization-delay-ns", BusControllerDev,
                       pto_authorization_delay_ns, 0),
    DEFINE_PROP_UINT64("pto-authorization-timeout-ns", BusControllerDev,
                       pto_authorization_timeout_ns,
                       LINQU_PTO_AUTHORIZATION_TIMEOUT_NS_DEFAULT),
    DEFINE_PROP_BOOL("pto-authorization-inject-duplicate-completion",
                     BusControllerDev,
                     pto_authorization_inject_duplicate_completion,
                     false),
    DEFINE_PROP_BOOL("pto-authorization-inject-late-completion",
                     BusControllerDev,
                     pto_authorization_inject_late_completion,
                     false),
    DEFINE_PROP_STRING("remote-memory-model-manifest", BusControllerDev,
                       remote_memory_model_manifest),
    DEFINE_PROP_STRING("async-load-model", BusControllerDev,
                       async_load_model),
    DEFINE_PROP_END_OF_LIST(),
};

static void ub_bus_controller_dev_class_init(ObjectClass *class, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(class);
    UBDeviceClass *uc = UB_DEVICE_CLASS(class);

    device_class_set_props(dc, ub_bus_controller_dev_properties);
    uc->realize = ub_bus_controller_dev_realize;
    dc->reset = ub_bus_controller_dev_reset;
    dc->vmsd = &vmstate_ub_bus_controller_dev;
}

static void ub_bus_controller_dev_finalize(Object *object)
{
    BusControllerDev *ubc_dev = BUS_CONTROLLER_DEV(object);

    linqu_uapi_authorization_discard(ubc_dev);
    if (ubc_dev->linqu_uapi_authorization_timer) {
        timer_free(ubc_dev->linqu_uapi_authorization_timer);
        ubc_dev->linqu_uapi_authorization_timer = NULL;
    }
    linqu_ub_gm_registry_free(ubc_dev->linqu_uapi_ub_gm_registry);
    ubc_dev->linqu_uapi_ub_gm_registry = NULL;
    if (ubc_dev->linqu_uapi_bridge) {
        linqu_ub_bridge_free(ubc_dev->linqu_uapi_bridge);
        ubc_dev->linqu_uapi_bridge = NULL;
    }
    if (ubc_dev->remote_memory_model_timer) {
        timer_free(ubc_dev->remote_memory_model_timer);
        ubc_dev->remote_memory_model_timer = NULL;
    }
    ub_obmm_async_free(ubc_dev->obmm_async);
    ubc_dev->obmm_async = NULL;
    ub_async_load_device_free(ubc_dev->ub_async_load);
    ubc_dev->ub_async_load = NULL;
    ub_obmm_remote_model_cleanup(&ubc_dev->remote_memory_model);
    g_free(ubc_dev->obmm_async_children);
    ubc_dev->obmm_async_children = NULL;
}

static const TypeInfo ub_bus_controller_dev_type_info = {
    .name = TYPE_BUS_CONTROLLER_DEV,
    .parent = TYPE_UB_DEVICE,
    .instance_size = sizeof(BusControllerDev),
    .instance_finalize = ub_bus_controller_dev_finalize,
    .class_size = sizeof(BusControllerDevClass),
    .class_init = ub_bus_controller_dev_class_init,
};

/*
 * ============================================================================
 * SIM Decoder Implementation
 * ============================================================================
 */

static void sim_dec_init(BusControllerState *bcs)
{
    const char *env;

    g_sim_decoder = g_malloc0(sizeof(*g_sim_decoder));
    g_sim_decoder->bcs = bcs;
    g_sim_decoder->next_map_id = 1;
    QTAILQ_INIT(&g_sim_decoder->map_list);
    QTAILQ_INIT(&g_sim_decoder->retired_map_list);
    qemu_mutex_init(&g_sim_decoder->lock);
    g_sim_decoder->enabled = true;

    /*
     * Imported OBMM mappings are dynamic shared-memory views.  The producer
     * can update its local export pool without going through this importer's
     * SIM_DEC CPU window, so a default importer-side read cache can return
     * stale queue/object metadata.  Keep the cache opt-in until the mapping
     * contract carries explicit coherency or read-only lifetime semantics.
     */
    g_sim_decoder->page_cache_max_per_map = SIM_DEC_CACHE_MAX_PER_MAP;
    env = g_getenv("SIM_DEC_PAGE_CACHE_PER_MAP");
    if (env) {
        g_sim_decoder->page_cache_max_per_map = g_ascii_strtoull(env, NULL, 0);
    }

    g_sim_decoder->page_cache_max_global = SIM_DEC_CACHE_MAX_GLOBAL;
    env = g_getenv("SIM_DEC_PAGE_CACHE_GLOBAL");
    if (env) {
        g_sim_decoder->page_cache_max_global = g_ascii_strtoull(env, NULL, 0);
    }

    env = g_getenv("SIM_DEC_PREFETCH");
    g_sim_decoder->page_cache_prefetch = env && env[0] && strcmp(env, "0") != 0;

    g_sim_decoder->write_mode = SIM_DEC_WRITE_THROUGH;
    env = g_getenv("SIM_DEC_WRITE_MODE");
    if (env && strcmp(env, "write-back") == 0) {
        g_sim_decoder->write_mode = SIM_DEC_WRITE_BACK;
    }

    env = g_getenv("GSVA_MODE");
    g_sim_decoder->experimental_gsva_enabled =
        sim_dec_experimental_feature_enabled("gsva") ||
        (env && env[0]);

    atexit(sim_dec_print_global_stats);
    qemu_log("SIM_DEC: decoder simulation initialized cache_per_map=%" PRIu64
             " cache_global=%" PRIu64 " prefetch=%d write_mode=%s\n",
             g_sim_decoder->page_cache_max_per_map,
             g_sim_decoder->page_cache_max_global,
             g_sim_decoder->page_cache_prefetch,
             g_sim_decoder->write_mode == SIM_DEC_WRITE_BACK ? "write-back" : "write-through");
}

static void sim_dec_map_entry_destroy(SimDecMapEntry *entry)
{
    if (entry->mapped) {
        ubc_cpu_window_detach(&entry->cpu_window);
    }
    entry->mapped = false;
    object_unparent(OBJECT(&entry->cpu_window));
    g_free(entry->sync_shadow);
    sim_dec_page_cache_free(entry->page_cache);
    entry->page_cache = NULL;
    g_free(entry);
}

static void sim_dec_cleanup_coherence_entry(SimDecMapEntry *entry)
{
    int fence_ret;

    if (!entry || entry->cache_policy != SIM_DEC_CACHE_POLICY_DIRECTORY_MESI ||
        !g_sim_decoder || !g_sim_decoder->bcs || !g_sim_decoder->bcs->ubc_dev ||
        entry->size == 0) {
        return;
    }

    fence_ret = obmm_coh_send_fence(g_sim_decoder->bcs->ubc_dev, entry->dcna,
                                    entry->remote_uba, entry->size,
                                    entry->token_id);
    if (fence_ret != 0) {
        qemu_log("SIM_DEC: cleanup coherence fence failed map_id=%" PRIx64
                 " ret=%d\n", entry->map_id, fence_ret);
        return;
    }

    obmm_coh_invalidate_local_range(g_sim_decoder->bcs->ubc_dev, entry->dcna,
                                    entry->remote_uba, entry->size,
                                    entry->token_id);
}

static void sim_dec_cleanup(void)
{
    SimDecMapEntry *entry;

    if (!g_sim_decoder)
        return;

    sim_dec_print_global_stats();

    qemu_mutex_lock(&g_sim_decoder->lock);
    while ((entry = QTAILQ_FIRST(&g_sim_decoder->map_list)) != NULL) {
        QTAILQ_REMOVE(&g_sim_decoder->map_list, entry, next);
        qemu_mutex_unlock(&g_sim_decoder->lock);

        sim_dec_cleanup_coherence_entry(entry);

        qemu_mutex_lock(&g_sim_decoder->lock);
        if (sim_dec_is_gva_entry(entry)) {
            sim_dec_gva_ownership_unregister_entry(entry);
            entry->gva_ownership_registered = false;
        }
        qemu_mutex_unlock(&g_sim_decoder->lock);

        sim_dec_map_entry_destroy(entry);

        qemu_mutex_lock(&g_sim_decoder->lock);
    }
    while ((entry = QTAILQ_FIRST(&g_sim_decoder->retired_map_list)) != NULL) {
        QTAILQ_REMOVE(&g_sim_decoder->retired_map_list, entry, next);
        sim_dec_map_entry_destroy(entry);
    }
    qemu_mutex_unlock(&g_sim_decoder->lock);

    qemu_mutex_destroy(&g_sim_decoder->lock);
    g_free(g_sim_decoder);
    g_sim_decoder = NULL;
}

static SimDecMapEntry *sim_dec_find_entry_by_pa(uint64_t pa)
{
    SimDecMapEntry *entry;

    QTAILQ_FOREACH(entry, &g_sim_decoder->map_list, next) {
        if (entry->active &&
            pa >= entry->local_pa &&
            pa < entry->local_pa + entry->size) {
            return entry;
        }
    }
    return NULL;
}

static SimDecMapEntry *sim_dec_find_entry_by_uba(uint64_t uba, uint64_t len)
{
    SimDecMapEntry *entry;

    if (!g_sim_decoder) {
        return NULL;
    }
    QTAILQ_FOREACH(entry, &g_sim_decoder->map_list, next) {
        if (entry->active &&
            uba >= entry->remote_uba &&
            uba + len <= entry->remote_uba + entry->size) {
            return entry;
        }
    }
    return NULL;
}

static void obmm_export_register(const SimDecObmmBootstrapRecord *record)
{
    ObmmExportEntry *entry;
    if (!record || record->backing_uba == 0 || record->size == 0) {
        return;
    }
    QTAILQ_FOREACH(entry, &g_obmm_exports, next) {
        if (entry->remote_uba == record->remote_uba &&
            entry->export_cna == record->export_cna) {
            entry->backing_uba = record->backing_uba;
            entry->size = record->size;
            return;
        }
    }
    entry = g_new0(ObmmExportEntry, 1);
    entry->remote_uba = record->remote_uba;
    entry->backing_uba = record->backing_uba;
    entry->size = record->size;
    entry->export_cna = record->export_cna;
    QTAILQ_INSERT_TAIL(&g_obmm_exports, entry, next);
}

static void sim_dec_register_obmm_gsva_route(
    const SimDecObmmBootstrapRecord *record);


static ObmmExportEntry *obmm_export_lookup(uint64_t uba, uint64_t len)
{
    ObmmExportEntry *entry;
    QTAILQ_FOREACH(entry, &g_obmm_exports, next) {
        if (uba >= entry->remote_uba &&
            uba + len <= entry->remote_uba + entry->size) {
            return entry;
        }
    }
    return NULL;
}

static SimDecMapEntry *sim_dec_find_entry_by_id(uint64_t map_id)
{
    SimDecMapEntry *entry;

    QTAILQ_FOREACH(entry, &g_sim_decoder->map_list, next) {
        if (entry->map_id == map_id)
            return entry;
    }
    return NULL;
}

static SimDecMapEntry *sim_dec_find_gva_route_by_uba_locked(uint32_t vmid,
                                                            uint32_t asid,
                                                            uint64_t uba,
                                                            uint64_t size)
{
    SimDecMapEntry *entry;
    uint64_t end;

    if (!g_sim_decoder || size == 0 || UINT64_MAX - uba < size) {
        return NULL;
    }
    end = uba + size;

    QTAILQ_FOREACH(entry, &g_sim_decoder->map_list, next) {
        uint64_t route_end;

        if (!entry->active || entry->address_profile == 0 ||
            entry->vmid != vmid || entry->asid != asid ||
            UINT64_MAX - entry->remote_uba < entry->size) {
            continue;
        }
        route_end = entry->remote_uba + entry->size;
        if (uba >= entry->remote_uba && end <= route_end) {
            return entry;
        }
    }
    return NULL;
}

static int sim_dec_gva_lookup_ma_by_uba(uint32_t vmid, uint32_t asid,
                                        uint64_t uba, uint64_t size,
                                        SimDecLookupResult *result)
{
    SimDecMapEntry *entry;
    uint64_t offset;

    if (!result || !g_sim_decoder || !g_sim_decoder->enabled) {
        return -1;
    }

    memset(result, 0, sizeof(*result));

    qemu_mutex_lock(&g_sim_decoder->lock);
    entry = sim_dec_find_gva_route_by_uba_locked(vmid, asid, uba, size);
    if (!entry) {
        qemu_mutex_unlock(&g_sim_decoder->lock);
        return -1;
    }

    offset = uba - entry->remote_uba;
    result->map_id = entry->map_id;
    result->gva_id = entry->gva_id;
    result->local_pa = entry->local_pa + offset;
    result->local_va = entry->local_va ? entry->local_va + offset : 0;
    result->remote_uba = uba;
    result->token_id = entry->token_id;
    result->token_value = entry->token_value;
    result->src_eid = entry->src_eid;
    result->address_profile = entry->address_profile;
    result->access_flags = entry->access_flags;
    result->vmid = entry->vmid;
    result->asid = entry->asid;
    result->dcna = entry->dcna;
    result->tid = entry->tid;
    result->upi = entry->upi;
    result->p_tag = entry->p_tag;
    result->mp_ubc_port = entry->mp_ubc_port;
    result->mp_lane = entry->mp_lane;
    result->mp_link_id = entry->mp_link_id;
    result->cache_policy = entry->cache_policy;
    qemu_mutex_unlock(&g_sim_decoder->lock);
    return 0;
}

static SimDecMapEntry *sim_dec_find_gva_route_by_va_locked(uint64_t va,
                                                           bool is_write)
{
    SimDecMapEntry *entry;
    uint64_t page_va = va & ~(uint64_t)(SIM_DEC_PAGE_SIZE - 1);
    uint64_t page_end;

    if (UINT64_MAX - page_va < SIM_DEC_PAGE_SIZE) {
        return NULL;
    }
    page_end = page_va + SIM_DEC_PAGE_SIZE;

    QTAILQ_FOREACH(entry, &g_sim_decoder->map_list, next) {
        uint64_t route_end;
        uint64_t uba;

        if (!entry->active || entry->address_profile == 0 ||
            entry->local_va == 0 ||
            UINT64_MAX - entry->local_va < entry->size) {
            continue;
        }

        route_end = entry->local_va + entry->size;
        if (va < entry->local_va || page_end > route_end) {
            continue;
        }

        if (is_write &&
            (entry->access_flags & SIM_DEC_GVA_ACCESS_READ_ONLY)) {
            qemu_log("GVA_TCG_FAULT reason=write_to_read_only map_id=%" PRIx64
                     " gva_id=%" PRIx64 " va=%" PRIx64
                     " access_flags=%" PRIu32 "\n",
                     entry->map_id, entry->gva_id, va, entry->access_flags);
            continue;
        }

        if (UINT64_MAX - va < entry->pte_offset) {
            qemu_log("GVA_TCG_FAULT reason=pte_offset_overflow map_id=%" PRIx64
                     " gva_id=%" PRIx64 " va=%" PRIx64
                     " pte_offset=%" PRIx64 "\n",
                     entry->map_id, entry->gva_id, va, entry->pte_offset);
            continue;
        }
        uba = va + entry->pte_offset;

        /*
         * Phase C uses the route metadata as the PTE.offset side table.
         * Validate that VA + offset resolves through the same ma_table entry
         * before returning the backend CPU-window PA.
         */
        if (sim_dec_find_gva_route_by_uba_locked(entry->vmid, entry->asid, uba,
                                                 SIM_DEC_PAGE_SIZE) != entry) {
            qemu_log("GVA_TCG_FAULT reason=ma_table map_id=%" PRIx64
                     " gva_id=%" PRIx64 " va=%" PRIx64
                     " uba=%" PRIx64 " vmid=%" PRIu32
                     " asid=%" PRIu32 "\n",
                     entry->map_id, entry->gva_id, va, uba,
                     entry->vmid, entry->asid);
            continue;
        }

        return entry;
    }

    return NULL;
}

static bool sim_dec_gva_tcg_enabled(void)
{
    static int cached = -1;

    if (cached >= 0) {
        return cached != 0;
    }

    if (gsva_arm_mmu_enabled()) {
        cached = 0;
        return false;
    }

    const char *mode = g_getenv("GSVA_MODE");
    const char *env = g_getenv("SIM_GVA_TCG");
    cached = ((mode && strcmp(mode, "sim_gva_tcg") == 0) ||
              (env && env[0] && strcmp(env, "0") != 0)) ? 1 : 0;
    if (cached) {
        qemu_log("GVA_TCG enabled: ARM tlb_fill will probe GVA S3 routes\n");
    }
    return cached != 0;
}

bool gsva_arm_mmu_enabled(void)
{
    static int cached = -1;
    const char *mode;
    bool feature_enabled;

    if (cached >= 0) {
        return cached != 0;
    }

    mode = g_getenv("GSVA_MODE");
    feature_enabled = sim_dec_experimental_feature_enabled("gsva");
    cached = ((feature_enabled &&
               (!mode || mode[0] == '\0' ||
                strcmp(mode, "arm_mmu") == 0)) ||
              (mode && strcmp(mode, "arm_mmu") == 0)) ? 1 : 0;
    if (cached) {
        qemu_log("GSVA_MODE arm_mmu: ARM tlb_fill will use GSVA route/coherence\n");
    }
    return cached != 0;
}

bool sim_dec_gva_tcg_translate(uint64_t va, bool is_write,
                               uint64_t *local_pa, uint64_t *page_size)
{
    SimDecMapEntry *entry;
    uint64_t page_va = va & ~(uint64_t)(SIM_DEC_PAGE_SIZE - 1);
    uint64_t offset;

    if (!sim_dec_gva_tcg_enabled() || !local_pa || !page_size ||
        !g_sim_decoder || !g_sim_decoder->enabled) {
        return false;
    }

    qemu_mutex_lock(&g_sim_decoder->lock);
    entry = sim_dec_find_gva_route_by_va_locked(page_va, is_write);
    if (!entry) {
        qemu_mutex_unlock(&g_sim_decoder->lock);
        return false;
    }

    offset = page_va - entry->local_va;
    *local_pa = entry->local_pa + offset;
    *page_size = SIM_DEC_PAGE_SIZE;
    qemu_log("GVA_TCG_TRANSLATE va=%" PRIx64 " local_pa=%" PRIx64
             " map_id=%" PRIx64 " gva_id=%" PRIx64
             " pte_offset=%" PRIx64 " uba=%" PRIx64
             " vmid=%" PRIu32 " asid=%" PRIu32
             " p_tag=%" PRIu32 " is_write=%u\n",
             page_va, *local_pa, entry->map_id, entry->gva_id,
             entry->pte_offset, page_va + entry->pte_offset,
             entry->vmid, entry->asid, entry->p_tag, is_write);
    qemu_mutex_unlock(&g_sim_decoder->lock);
    return true;
}

static const char *sim_dec_route_state_name(SimDecRouteState state)
{
    switch (state) {
    case SIM_DEC_ROUTE_CREATING:
        return "creating";
    case SIM_DEC_ROUTE_ACTIVE:
        return "active";
    case SIM_DEC_ROUTE_STALE:
        return "stale";
    case SIM_DEC_ROUTE_ERROR:
        return "error";
    case SIM_DEC_ROUTE_RETIRED:
        return "retired";
    default:
        return "unknown";
    }
}

static bool sim_dec_route_blocks_overlap(const SimDecMapEntry *entry)
{
    return entry &&
           (entry->state == SIM_DEC_ROUTE_CREATING ||
            entry->state == SIM_DEC_ROUTE_ACTIVE ||
            entry->state == SIM_DEC_ROUTE_STALE ||
            entry->state == SIM_DEC_ROUTE_ERROR);
}

static bool sim_dec_range_end(uint64_t base, uint64_t size, uint64_t *end)
{
    if (size == 0 || UINT64_MAX - base < size) {
        return false;
    }
    if (end) {
        *end = base + size;
    }
    return true;
}

static bool sim_dec_check_overlap(uint64_t pa, uint64_t size)
{
    SimDecMapEntry *entry;
    uint64_t end;

    if (!sim_dec_range_end(pa, size, &end)) {
        return true;
    }

    QTAILQ_FOREACH(entry, &g_sim_decoder->map_list, next) {
        if (sim_dec_route_blocks_overlap(entry)) {
            uint64_t entry_end;

            if (!sim_dec_range_end(entry->local_pa, entry->size, &entry_end)) {
                return true;
            }
            if (!(end <= entry->local_pa || pa >= entry_end)) {
                return true;
            }
        }
    }
    return false;
}

static bool sim_dec_is_gva_entry(const SimDecMapEntry *entry)
{
    return entry && entry->address_profile != 0;
}

static void sim_dec_ensure_gva_write_back_cache(SimDecMapEntry *entry)
{
    uint64_t pages;

    if (!entry || entry->page_cache ||
        entry->cache_policy != SIM_DEC_CACHE_POLICY_WRITE_BACK) {
        return;
    }

    pages = (entry->size + SIM_DEC_PAGE_SIZE - 1) / SIM_DEC_PAGE_SIZE;
    if (pages == 0) {
        return;
    }

    entry->page_cache = sim_dec_page_cache_new(pages);
    qemu_log("GVA_WRITE_BACK_CACHE map_id=%" PRIx64
             " gva_id=%" PRIx64 " pages=%" PRIu64
             " size=%#" PRIx64 " cache_policy=%" PRIu32 "\n",
             entry->map_id, entry->gva_id, pages, entry->size,
             entry->cache_policy);
}

static bool sim_dec_entry_write_back(const SimDecMapEntry *entry)
{
    if (!entry) {
        return false;
    }
    if (sim_dec_is_gva_entry(entry)) {
        return entry->cache_policy == SIM_DEC_CACHE_POLICY_WRITE_BACK;
    }
    return g_sim_decoder && g_sim_decoder->write_mode == SIM_DEC_WRITE_BACK;
}

static bool sim_dec_is_explicit_gva_route_entry(const SimDecMapEntry *entry)
{
    return entry &&
           (entry->map_source == SIM_DEC_MAP_SOURCE_GVA_MANAGER ||
            entry->address_profile == SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY);
}

static bool sim_dec_is_explicit_gva_route_req(const SimDecGvaMapReq *req)
{
    return req &&
           (req->map_source == SIM_DEC_MAP_SOURCE_GVA_MANAGER ||
            req->address_profile == SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY);
}

static bool sim_dec_gva_ownership_required_req(const SimDecGvaMapReq *req)
{
    return sim_dec_is_explicit_gva_route_req(req) &&
           req->address_profile == SIM_DEC_ADDRESS_PROFILE_GENERIC_GVA;
}

static bool sim_dec_check_gva_route_overlap(const SimDecGvaMapReq *req)
{
    SimDecMapEntry *entry;
    uint64_t end;

    if (!sim_dec_is_explicit_gva_route_req(req)) {
        return false;
    }

    if (!sim_dec_range_end(req->map_req.remote_uba, req->map_req.size, &end)) {
        return true;
    }
    QTAILQ_FOREACH(entry, &g_sim_decoder->map_list, next) {
        uint64_t entry_end;

        if (!sim_dec_route_blocks_overlap(entry) ||
            !sim_dec_is_explicit_gva_route_entry(entry) ||
            entry->vmid != req->vmid ||
            entry->asid != req->asid) {
            continue;
        }

        if (!sim_dec_range_end(entry->remote_uba, entry->size, &entry_end)) {
            return true;
        }
        if (!(end <= entry->remote_uba ||
              req->map_req.remote_uba >= entry_end)) {
            qemu_log("SIM_DEC: GVA route overlap detected vmid=%" PRIu32
                     " asid=%" PRIu32 " uba[%" PRIx64 "-%" PRIx64
                     "] existing[%" PRIx64 "-%" PRIx64 "]\n",
                     req->vmid, req->asid, req->map_req.remote_uba, end,
                     entry->remote_uba, entry_end);
            return true;
        }
    }
    return false;
}

typedef struct SimDecGvaOwnershipRecord {
    char node_id[64];
    char role[16];
    uint64_t map_id;
    uint64_t gva_id;
    uint64_t uba;
    uint64_t size;
    uint32_t vmid;
    uint32_t asid;
} SimDecGvaOwnershipRecord;

static bool sim_dec_gva_ownership_enabled(void)
{
    const char *env = g_getenv("SIM_GVA_OWNERSHIP");

    return !env || strcmp(env, "0") != 0;
}

static bool sim_dec_gva_read_only_req(const SimDecGvaMapReq *req)
{
    return req && (req->access_flags & SIM_DEC_GVA_ACCESS_READ_ONLY);
}

static bool sim_dec_gva_ranges_overlap(uint64_t base_a, uint64_t size_a,
                                       uint64_t base_b, uint64_t size_b)
{
    uint64_t end_a;
    uint64_t end_b;

    if (size_a == 0 || size_b == 0 ||
        UINT64_MAX - base_a < size_a ||
        UINT64_MAX - base_b < size_b) {
        return true;
    }

    end_a = base_a + size_a;
    end_b = base_b + size_b;
    return !(end_a <= base_b || base_a >= end_b);
}

static const char *sim_dec_gva_node_id(void)
{
    const char *node_id = g_getenv("UB_FM_NODE_ID");

    return (node_id && node_id[0]) ? node_id : "unknown";
}

static char *sim_dec_gva_ownership_dir(void)
{
    const char *shared_dir = g_getenv("UB_FM_SHARED_DIR");

    if (!shared_dir || !shared_dir[0]) {
        return NULL;
    }
    return g_build_filename(shared_dir, "gva_ownership", NULL);
}

static int sim_dec_gva_ownership_lock(char **dir_out,
                                      char **registry_path_out)
{
    g_autofree char *lock_path = NULL;
    char *dir;
    int fd;

    if (!sim_dec_gva_ownership_enabled()) {
        return -ENOENT;
    }

    dir = sim_dec_gva_ownership_dir();
    if (!dir) {
        return -ENOENT;
    }
    if (g_mkdir_with_parents(dir, 0755) != 0) {
        qemu_log("GVA_OWNERSHIP error=create_dir dir=%s err=%s\n",
                 dir, strerror(errno));
        g_free(dir);
        return -errno;
    }

    lock_path = g_build_filename(dir, "registry.lock", NULL);
    fd = open(lock_path, O_RDWR | O_CREAT, 0600);
    if (fd < 0) {
        qemu_log("GVA_OWNERSHIP error=open_lock path=%s err=%s\n",
                 lock_path, strerror(errno));
        g_free(dir);
        return -errno;
    }

    if (flock(fd, LOCK_EX) != 0) {
        qemu_log("GVA_OWNERSHIP error=lock path=%s err=%s\n",
                 lock_path, strerror(errno));
        close(fd);
        g_free(dir);
        return -errno;
    }

    *dir_out = dir;
    *registry_path_out = g_build_filename(dir, "registry.tsv", NULL);
    return fd;
}

static void sim_dec_gva_ownership_unlock(int fd)
{
    if (fd < 0) {
        return;
    }
    if (flock(fd, LOCK_UN) != 0) {
        qemu_log("GVA_OWNERSHIP error=unlock err=%s\n", strerror(errno));
    }
    close(fd);
}

static bool sim_dec_gva_ownership_parse_line(const char *line,
                                             SimDecGvaOwnershipRecord *rec)
{
    unsigned long long map_id;
    unsigned long long gva_id;
    unsigned long long uba;
    unsigned long long size;
    unsigned int vmid;
    unsigned int asid;

    if (!line || !rec || line[0] == '\0' || line[0] == '#') {
        return false;
    }

    memset(rec, 0, sizeof(*rec));
    if (sscanf(line,
               "node=%63s map_id=%llx gva_id=%llx vmid=%u asid=%u uba=%llx size=%llx role=%15s",
               rec->node_id, &map_id, &gva_id, &vmid, &asid, &uba, &size,
               rec->role) != 8) {
        return false;
    }

    rec->map_id = (uint64_t)map_id;
    rec->gva_id = (uint64_t)gva_id;
    rec->vmid = (uint32_t)vmid;
    rec->asid = (uint32_t)asid;
    rec->uba = (uint64_t)uba;
    rec->size = (uint64_t)size;
    return true;
}

static int sim_dec_gva_ownership_register_req(const SimDecGvaMapReq *req,
                                              uint64_t map_id)
{
    g_autofree char *dir = NULL;
    g_autofree char *registry_path = NULL;
    g_autofree char *data = NULL;
    g_autoptr(GError) err = NULL;
    g_auto(GStrv) lines = NULL;
    g_autoptr(GString) next_data = NULL;
    const char *node_id;
    const char *role;
    bool want_writer;
    int lock_fd;
    int i;

    if (!sim_dec_is_explicit_gva_route_req(req)) {
        return 0;
    }

    lock_fd = sim_dec_gva_ownership_lock(&dir, &registry_path);
    if (lock_fd == -ENOENT) {
        return 0;
    }
    if (lock_fd < 0) {
        return lock_fd;
    }

    if (g_file_test(registry_path, G_FILE_TEST_EXISTS) &&
        !g_file_get_contents(registry_path, &data, NULL, &err)) {
        qemu_log("GVA_OWNERSHIP error=read path=%s err=%s\n",
                 registry_path, err ? err->message : "unknown");
        sim_dec_gva_ownership_unlock(lock_fd);
        return -EIO;
    }

    node_id = sim_dec_gva_node_id();
    role = sim_dec_gva_read_only_req(req) ? "reader" : "writer";
    want_writer = strcmp(role, "writer") == 0;
    lines = g_strsplit(data ? data : "", "\n", -1);
    next_data = g_string_new(NULL);

    for (i = 0; lines && lines[i]; i++) {
        SimDecGvaOwnershipRecord rec;
        bool rec_writer;

        if (lines[i][0] == '\0') {
            continue;
        }
        if (!sim_dec_gva_ownership_parse_line(lines[i], &rec)) {
            g_string_append_printf(next_data, "%s\n", lines[i]);
            continue;
        }
        if (strcmp(rec.node_id, node_id) == 0 &&
            rec.map_id == map_id &&
            rec.gva_id == req->gva_id) {
            continue;
        }

        rec_writer = strcmp(rec.role, "writer") == 0;
        if (sim_dec_gva_ranges_overlap(req->map_req.remote_uba,
                                       req->map_req.size, rec.uba,
                                       rec.size) &&
            (want_writer || rec_writer)) {
            qemu_log("GVA_OWNERSHIP_CONFLICT node=%s map_id=%" PRIx64
                     " role=%s uba=%" PRIx64 " size=%" PRIx64
                     " existing_node=%s existing_map_id=%" PRIx64
                     " existing_role=%s existing_uba=%" PRIx64
                     " existing_size=%" PRIx64 "\n",
                     node_id, map_id, role, req->map_req.remote_uba,
                     req->map_req.size, rec.node_id, rec.map_id, rec.role,
                     rec.uba, rec.size);
            sim_dec_gva_ownership_unlock(lock_fd);
            return -EBUSY;
        }
        g_string_append_printf(next_data, "%s\n", lines[i]);
    }

    g_string_append_printf(next_data,
                           "node=%s map_id=%" PRIx64 " gva_id=%" PRIx64
                           " vmid=%" PRIu32 " asid=%" PRIu32
                           " uba=%" PRIx64 " size=%" PRIx64
                           " role=%s\n",
                           node_id, map_id, req->gva_id, req->vmid,
                           req->asid, req->map_req.remote_uba,
                           req->map_req.size, role);
    if (!g_file_set_contents(registry_path, next_data->str, -1, &err)) {
        qemu_log("GVA_OWNERSHIP error=write path=%s err=%s\n",
                 registry_path, err ? err->message : "unknown");
        sim_dec_gva_ownership_unlock(lock_fd);
        return -EIO;
    }

    qemu_log("GVA_OWNERSHIP_REGISTER node=%s map_id=%" PRIx64
             " gva_id=%" PRIx64 " role=%s uba=%" PRIx64
             " size=%" PRIx64 " vmid=%" PRIu32 " asid=%" PRIu32 "\n",
             node_id, map_id, req->gva_id, role, req->map_req.remote_uba,
             req->map_req.size, req->vmid, req->asid);
    sim_dec_gva_ownership_unlock(lock_fd);
    return 0;
}

static void sim_dec_gva_ownership_unregister_entry(const SimDecMapEntry *entry)
{
    g_autofree char *dir = NULL;
    g_autofree char *registry_path = NULL;
    g_autofree char *data = NULL;
    g_autoptr(GError) err = NULL;
    g_auto(GStrv) lines = NULL;
    g_autoptr(GString) next_data = NULL;
    const char *node_id;
    bool removed = false;
    int lock_fd;
    int i;

    if (!entry || !entry->gva_ownership_registered) {
        return;
    }

    lock_fd = sim_dec_gva_ownership_lock(&dir, &registry_path);
    if (lock_fd == -ENOENT) {
        return;
    }
    if (lock_fd < 0) {
        return;
    }

    if (g_file_test(registry_path, G_FILE_TEST_EXISTS) &&
        !g_file_get_contents(registry_path, &data, NULL, &err)) {
        qemu_log("GVA_OWNERSHIP error=read path=%s err=%s\n",
                 registry_path, err ? err->message : "unknown");
        sim_dec_gva_ownership_unlock(lock_fd);
        return;
    }

    node_id = sim_dec_gva_node_id();
    lines = g_strsplit(data ? data : "", "\n", -1);
    next_data = g_string_new(NULL);
    for (i = 0; lines && lines[i]; i++) {
        SimDecGvaOwnershipRecord rec;

        if (lines[i][0] == '\0') {
            continue;
        }
        if (sim_dec_gva_ownership_parse_line(lines[i], &rec) &&
            strcmp(rec.node_id, node_id) == 0 &&
            rec.map_id == entry->map_id &&
            rec.gva_id == entry->gva_id) {
            removed = true;
            continue;
        }
        g_string_append_printf(next_data, "%s\n", lines[i]);
    }

    if (!g_file_set_contents(registry_path, next_data->str, -1, &err)) {
        qemu_log("GVA_OWNERSHIP error=write path=%s err=%s\n",
                 registry_path, err ? err->message : "unknown");
        sim_dec_gva_ownership_unlock(lock_fd);
        return;
    }

    if (removed) {
        qemu_log("GVA_OWNERSHIP_UNREGISTER node=%s map_id=%" PRIx64
                 " gva_id=%" PRIx64 " uba=%" PRIx64
                 " size=%" PRIx64 "\n",
                 node_id, entry->map_id, entry->gva_id,
                 entry->remote_uba, entry->size);
    }
    sim_dec_gva_ownership_unlock(lock_fd);
}

static bool sim_dec_should_log_gva_path(uint64_t count)
{
    return count <= 4 || (count != 0 && (count & (count - 1)) == 0);
}

static void sim_dec_log_gva_path(const SimDecMapEntry *entry, const char *op,
                                 hwaddr addr, unsigned size, uint64_t count)
{
    if (!sim_dec_is_gva_entry(entry) || !sim_dec_should_log_gva_path(count)) {
        return;
    }

    qemu_log("GVA_PATH gva_path=cpu_window op=%s map_id=%" PRIx64
             " gva_id=%" PRIx64 " local_va=%" PRIx64
             " offset=%" PRIx64 " remote_uba=%" PRIx64
             " size=%u count=%" PRIu64 " vmid=%" PRIu32
             " asid=%" PRIu32 " address_profile=%" PRIu32 "\n",
             op, entry->map_id, entry->gva_id, entry->local_va,
             (uint64_t)addr, entry->remote_uba + addr, size, count,
             entry->vmid, entry->asid, entry->address_profile);
}

static void sim_dec_log_gva_dma_path(const SimDecLookupResult *result,
                                     const char *op, uint64_t iova,
                                     size_t len, uint64_t count)
{
    if (!result || result->address_profile == 0 ||
        !sim_dec_should_log_gva_path(count)) {
        return;
    }

    qemu_log("GVA_PATH gva_path=dma op=%s map_id=%" PRIx64
             " gva_id=%" PRIx64 " local_va=%" PRIx64
             " iova=%" PRIx64 " remote_uba=%" PRIx64
             " len=%zu count=%" PRIu64 " dcna=%" PRIu32
             " tid=%" PRIu32 " upi=%" PRIu32 " p_tag=%" PRIu32
             " address_profile=%" PRIu32 "\n",
             op ? op : "unknown", result->map_id, result->gva_id,
             result->local_va, iova, result->remote_uba, len, count,
             result->dcna, result->tid, result->upi, result->p_tag,
             result->address_profile);
}

static bool sim_dec_gva_access_fault(const SimDecMapEntry *entry,
                                     const char *op, hwaddr addr,
                                     unsigned size)
{
    SimDecLookupResult ma = {0};
    uint64_t uba;

    if (!sim_dec_is_gva_entry(entry)) {
        return false;
    }

    uba = entry->remote_uba + addr;
    if (sim_dec_gva_lookup_ma_by_uba(entry->vmid, entry->asid, uba, size,
                                     &ma) != 0 ||
        ma.map_id != entry->map_id) {
        qemu_log("GVA_ROUTE_MISS reason=ma_table map_id=%" PRIx64
                 " gva_id=%" PRIx64 " op=%s vmid=%" PRIu32
                 " asid=%" PRIu32 " uba=%" PRIx64 " size=%u\n",
                 entry->map_id, entry->gva_id, op ? op : "unknown",
                 entry->vmid, entry->asid, uba, size);
        return true;
    }

    if (entry->access_flags & SIM_DEC_GVA_ACCESS_READ_ONLY &&
        op && strcmp(op, "write") == 0) {
        qemu_log("GVA_FAULT reason=write_to_read_only map_id=%" PRIx64
                 " gva_id=%" PRIx64 " op=%s cache_policy=%" PRIu32
                 " access_flags=%" PRIu32 " local_va=%" PRIx64
                 " offset=%" PRIx64 " size=%u\n",
                 entry->map_id, entry->gva_id, op, entry->cache_policy,
                 entry->access_flags, entry->local_va, (uint64_t)addr, size);
        return true;
    }

    if (entry->p_tag == SIM_DEC_GVA_FAULT_P_TAG_ROUTE_MISS) {
        qemu_log("GVA_ROUTE_MISS reason=p_tag map_id=%" PRIx64
                 " gva_id=%" PRIx64 " op=%s p_tag=%" PRIu32
                 " local_va=%" PRIx64 " offset=%" PRIx64
                 " size=%u\n",
                 entry->map_id, entry->gva_id, op ? op : "unknown",
                 entry->p_tag, entry->local_va, (uint64_t)addr, size);
        return true;
    }

    if (entry->mp_link_id == SIM_DEC_GVA_MP_UNRESOLVED) {
        qemu_log("GVA_ROUTE_MISS reason=dcna map_id=%" PRIx64
                 " gva_id=%" PRIx64 " op=%s dcna=%" PRIu32
                 " local_va=%" PRIx64 " offset=%" PRIx64
                 " size=%u\n",
                 entry->map_id, entry->gva_id, op ? op : "unknown",
                 entry->dcna, entry->local_va, (uint64_t)addr, size);
        return true;
    }

    if (entry->p_tag != 0 && entry->p_tag != entry->mp_link_id) {
        qemu_log("GVA_ROUTE_MISS reason=p_tag_mismatch map_id=%" PRIx64
                 " gva_id=%" PRIx64 " op=%s p_tag=%" PRIu32
                 " expected_link_id=%" PRIu32 " local_va=%" PRIx64
                 " offset=%" PRIx64 " size=%u\n",
                 entry->map_id, entry->gva_id, op ? op : "unknown",
                 entry->p_tag, entry->mp_link_id, entry->local_va,
                 (uint64_t)addr, size);
        return true;
    }

    if (entry->access_flags & SIM_DEC_GVA_ACCESS_FAULT_UPI_MISMATCH) {
        qemu_log("GVA_FAULT reason=upi_mismatch map_id=%" PRIx64
                 " gva_id=%" PRIx64 " op=%s upi=%" PRIu32
                 " access_flags=%" PRIu32 " local_va=%" PRIx64
                 " offset=%" PRIx64 " size=%u\n",
                 entry->map_id, entry->gva_id, op ? op : "unknown",
                 entry->upi, entry->access_flags, entry->local_va,
                 (uint64_t)addr, size);
        return true;
    }

    if (entry->token_value != 0 && entry->token_value != entry->token_id) {
        qemu_log("GVA_FAULT reason=token_mismatch map_id=%" PRIx64
                 " gva_id=%" PRIx64 " op=%s token=%" PRIu32
                 " token_value=%" PRIu32 " local_va=%" PRIx64
                 " offset=%" PRIx64 " size=%u\n",
                 entry->map_id, entry->gva_id, op ? op : "unknown",
                 entry->token_id, entry->token_value, entry->local_va,
                 (uint64_t)addr, size);
        return true;
    }

    return false;
}

static bool sim_dec_gva_dma_access_fault(const SimDecLookupResult *result,
                                         const char *op, uint64_t iova,
                                         size_t len)
{
    if (!result || result->address_profile == 0) {
        return false;
    }

    if (result->access_flags & SIM_DEC_GVA_ACCESS_READ_ONLY &&
        op && strcmp(op, "write") == 0) {
        qemu_log("GVA_FAULT reason=write_to_read_only gva_path=dma map_id=%" PRIx64
                 " gva_id=%" PRIx64 " op=%s access_flags=%" PRIu32
                 " local_va=%" PRIx64 " iova=%" PRIx64 " len=%zu\n",
                 result->map_id, result->gva_id, op, result->access_flags,
                 result->local_va, iova, len);
        return true;
    }

    if (result->p_tag == SIM_DEC_GVA_FAULT_P_TAG_ROUTE_MISS) {
        qemu_log("GVA_ROUTE_MISS reason=p_tag gva_path=dma map_id=%" PRIx64
                 " gva_id=%" PRIx64 " op=%s p_tag=%" PRIu32
                 " local_va=%" PRIx64 " iova=%" PRIx64 " len=%zu\n",
                 result->map_id, result->gva_id, op ? op : "unknown",
                 result->p_tag, result->local_va, iova, len);
        return true;
    }

    if (result->mp_link_id == SIM_DEC_GVA_MP_UNRESOLVED) {
        qemu_log("GVA_ROUTE_MISS reason=dcna gva_path=dma map_id=%" PRIx64
                 " gva_id=%" PRIx64 " op=%s dcna=%" PRIu32
                 " local_va=%" PRIx64 " iova=%" PRIx64 " len=%zu\n",
                 result->map_id, result->gva_id, op ? op : "unknown",
                 result->dcna, result->local_va, iova, len);
        return true;
    }

    if (result->p_tag != 0 && result->p_tag != result->mp_link_id) {
        qemu_log("GVA_ROUTE_MISS reason=p_tag_mismatch gva_path=dma map_id=%" PRIx64
                 " gva_id=%" PRIx64 " op=%s p_tag=%" PRIu32
                 " expected_link_id=%" PRIu32 " local_va=%" PRIx64
                 " iova=%" PRIx64 " len=%zu\n",
                 result->map_id, result->gva_id, op ? op : "unknown",
                 result->p_tag, result->mp_link_id, result->local_va,
                 iova, len);
        return true;
    }

    if (result->access_flags & SIM_DEC_GVA_ACCESS_FAULT_UPI_MISMATCH) {
        qemu_log("GVA_FAULT reason=upi_mismatch gva_path=dma map_id=%" PRIx64
                 " gva_id=%" PRIx64 " op=%s upi=%" PRIu32
                 " access_flags=%" PRIu32 " local_va=%" PRIx64
                 " iova=%" PRIx64 " len=%zu\n",
                 result->map_id, result->gva_id, op ? op : "unknown",
                 result->upi, result->access_flags, result->local_va,
                 iova, len);
        return true;
    }

    if (result->token_value != 0 &&
        result->token_value != result->token_id) {
        qemu_log("GVA_FAULT reason=token_mismatch gva_path=dma map_id=%" PRIx64
                 " gva_id=%" PRIx64 " op=%s token=%" PRIu32
                 " token_value=%" PRIu32 " local_va=%" PRIx64
                 " iova=%" PRIx64 " len=%zu\n",
                 result->map_id, result->gva_id, op ? op : "unknown",
                 result->token_id, result->token_value, result->local_va,
                 iova, len);
        return true;
    }

    return false;
}

static void sim_dec_resolve_gva_mp_entry(SimDecMapEntry *entry)
{
    UBDevice *udev;
    uint32_t dcna;
    uint32_t i;

    if (!entry) {
        return;
    }

    entry->mp_ubc_port = SIM_DEC_GVA_MP_UNRESOLVED;
    entry->mp_lane = SIM_DEC_GVA_MP_UNRESOLVED;
    entry->mp_link_id = SIM_DEC_GVA_MP_UNRESOLVED;

    if (!g_sim_decoder || !g_sim_decoder->bcs ||
        !g_sim_decoder->bcs->ubc_dev) {
        return;
    }

    udev = &g_sim_decoder->bcs->ubc_dev->parent;
    if (!udev->port.neighbors || udev->port.port_num == 0) {
        return;
    }

    dcna = entry->dcna & 0x00ffffffU;
    for (i = 0; i < udev->port.port_num; i++) {
        NeighborInfo *neighbor = &udev->port.neighbors[i];
        uint32_t peer_cna = 0;

        if (neighbor->is_remote_neighbor) {
            if (!neighbor->remote_primary_cna_valid) {
                continue;
            }
            peer_cna = neighbor->remote_primary_cna & 0x00ffffffU;
        } else if (neighbor->neighbor_dev) {
            peer_cna = neighbor->neighbor_dev->cna & 0x00ffffffU;
        } else {
            continue;
        }

        if (peer_cna != dcna) {
            continue;
        }

        entry->mp_ubc_port = i;
        entry->mp_lane = neighbor->neighbor_port_idx;
        entry->mp_link_id = ((i & 0xffffU) << 16) |
                            (neighbor->neighbor_port_idx & 0xffffU);
        return;
    }
}

static void sim_dec_derive_gva_p_tag(SimDecMapEntry *entry)
{
    if (!entry || entry->p_tag != 0 ||
        entry->mp_link_id == SIM_DEC_GVA_MP_UNRESOLVED) {
        return;
    }

    entry->p_tag = entry->mp_link_id;
    qemu_log("GVA_MP_TAG_DERIVE map_id=%" PRIx64 " gva_id=%" PRIx64
             " p_tag=%" PRIu32 " ubc_port=%" PRIu32
             " lane=%" PRIu32 " link_id=%" PRIu32 "\n",
             entry->map_id, entry->gva_id, entry->p_tag,
             entry->mp_ubc_port, entry->mp_lane, entry->mp_link_id);
}

static void sim_dec_log_gva_route_dump(const SimDecMapEntry *entry,
                                       const char *state)
{
    if (!sim_dec_is_gva_entry(entry)) {
        return;
    }

    qemu_log("GVA_ROUTE_DUMP state=%s map_id=%" PRIx64
             " last_error=%d gva_id=%" PRIx64 " vmid=%" PRIu32 " asid=%" PRIu32
             " local_va=%" PRIx64 " home_va=%" PRIx64
             " pte_offset=%" PRIx64 " uba=%" PRIx64
             " pa=%" PRIx64 " size=%" PRIx64
             " ma_table.dcna=%" PRIu32 " ma_table.tid=%" PRIu32
             " ma_table.token=%" PRIu32 " ma_table.upi=%" PRIu32
             " mp_table.p_tag=%" PRIu32 " mp_table.ubc_port=%" PRIu32
             " mp_table.lane=%" PRIu32 " mp_table.link_id=%" PRIu32
             " map_source=%" PRIu32
             " address_profile=%" PRIu32 " cache_policy=%" PRIu32
             " access_flags=%" PRIu32 "\n",
             state ? state : "unknown", entry->map_id, entry->last_error,
             entry->gva_id, entry->vmid, entry->asid, entry->local_va, entry->home_va,
             entry->pte_offset, entry->remote_uba, entry->local_pa,
             entry->size, entry->dcna, entry->tid, entry->token_id,
             entry->upi, entry->p_tag, entry->mp_ubc_port, entry->mp_lane,
             entry->mp_link_id, entry->map_source, entry->address_profile,
             entry->cache_policy, entry->access_flags);
}

static int sim_dec_populate_map_entry(SimDecMapEntry *entry,
                                     const SimDecMapReq *req)
{
    if (!entry || !req) {
        return -1;
    }

    entry->local_pa = req->local_pa;
    entry->size = req->size;
    entry->remote_uba = req->remote_uba;
    entry->token_id = req->token_id;
    entry->token_value = req->token_value;
    entry->scna = req->scna;
    entry->dcna = req->dcna;
    memcpy(entry->seid, req->seid, 16);
    memcpy(entry->deid, req->deid, 16);
    entry->upi = req->upi;
    entry->src_eid = req->src_eid;
    entry->sync_shadow = g_malloc0(entry->size);
    entry->sync_valid_off = 0;
    entry->sync_valid_len = 0;
    entry->page_cache = NULL;
    entry->next_batch_seqno = 0;
    if (g_sim_decoder->page_cache_max_per_map > 0) {
        entry->page_cache = sim_dec_page_cache_new(g_sim_decoder->page_cache_max_per_map);
    }
    return 0;
}

static int sim_dec_handle_gva_map(const SimDecGvaMapReq *req,
                                 SimDecMapResp *resp)
{
    SimDecMapEntry *entry;
    uint64_t map_id;
    bool ownership_required;

    if (!g_sim_decoder || !g_sim_decoder->enabled) {
        resp->status = SIM_DEC_STATUS_BACKEND_ERROR;
        return -1;
    }

    if (!req) {
        resp->status = SIM_DEC_STATUS_INVALID_PARAM;
        return -1;
    }

    if (req->map_req.size == 0 || req->map_req.size > (1ULL << 40)) {
        qemu_log("SIM_DEC: GVA invalid size %" PRIx64 "\n", req->map_req.size);
        resp->status = SIM_DEC_STATUS_INVALID_PARAM;
        return -1;
    }
    if (!sim_dec_range_end(req->map_req.local_pa, req->map_req.size, NULL) ||
        !sim_dec_range_end(req->map_req.remote_uba, req->map_req.size, NULL)) {
        qemu_log("SIM_DEC: GVA invalid range local_pa=%" PRIx64
                 " remote_uba=%" PRIx64 " size=%" PRIx64 "\n",
                 req->map_req.local_pa, req->map_req.remote_uba,
                 req->map_req.size);
        resp->status = SIM_DEC_STATUS_INVALID_PARAM;
        return -1;
    }

    if (req->map_req.token_id == 0) {
        qemu_log("SIM_DEC: GVA token_id cannot be 0\n");
        resp->status = SIM_DEC_STATUS_INVALID_PARAM;
        return -1;
    }

    if (req->map_source != SIM_DEC_MAP_SOURCE_LEGACY_OBMM &&
        req->map_source != SIM_DEC_MAP_SOURCE_GVA_MANAGER) {
        qemu_log("SIM_DEC: GVA unsupported map_source=%" PRIu32 "\n",
                 req->map_source);
        resp->status = SIM_DEC_STATUS_INVALID_PARAM;
        return -1;
    }

    if (req->address_profile != SIM_DEC_ADDRESS_PROFILE_GENERIC_GVA &&
        req->address_profile != SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY) {
        qemu_log("SIM_DEC: GVA unsupported address_profile=%" PRIu32 "\n",
                 req->address_profile);
        resp->status = SIM_DEC_STATUS_INVALID_PARAM;
        return -1;
    }

    if (req->cache_policy != SIM_DEC_CACHE_POLICY_NC &&
        req->cache_policy != SIM_DEC_CACHE_POLICY_WRITE_THROUGH &&
        req->cache_policy != SIM_DEC_CACHE_POLICY_READ_CACHE &&
        req->cache_policy != SIM_DEC_CACHE_POLICY_WRITE_BACK &&
        req->cache_policy != SIM_DEC_CACHE_POLICY_DIRECTORY_MESI) {
        qemu_log("SIM_DEC: GVA unsupported cache_policy=%" PRIu32 "\n",
                 req->cache_policy);
        resp->status = SIM_DEC_STATUS_INVALID_PARAM;
        return -1;
    }

    if (req->cache_policy == SIM_DEC_CACHE_POLICY_READ_CACHE &&
        !(req->access_flags & SIM_DEC_GVA_ACCESS_READ_ONLY)) {
        qemu_log("SIM_DEC: GVA read_cache requires READ_ONLY access_flags=%" PRIu32 "\n",
                 req->access_flags);
        resp->status = SIM_DEC_STATUS_INVALID_PARAM;
        return -1;
    }

    if (req->cache_policy == SIM_DEC_CACHE_POLICY_WRITE_BACK &&
        !(req->access_flags & SIM_DEC_GVA_ACCESS_EXPLICIT_SYNC)) {
        qemu_log("SIM_DEC: GVA write_back requires EXPLICIT_SYNC access_flags=%" PRIu32 "\n",
                 req->access_flags);
        resp->status = SIM_DEC_STATUS_INVALID_PARAM;
        return -1;
    }

    if (req->cache_policy == SIM_DEC_CACHE_POLICY_DIRECTORY_MESI &&
        g_sim_decoder->bcs && g_sim_decoder->bcs->ubc_dev &&
        req->map_req.dcna == g_sim_decoder->bcs->ubc_dev->parent.cna) {
        qemu_log("SIM_DEC: GVA directory_mesi requires remote home dcna=%" PRIu32
                 " local_cna=%" PRIu32 "\n",
                 req->map_req.dcna, g_sim_decoder->bcs->ubc_dev->parent.cna);
        resp->status = SIM_DEC_STATUS_INVALID_PARAM;
        return -1;
    }

    if (req->address_profile == SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY &&
        (req->pte_offset != 0 || req->local_va == 0 ||
         req->home_va != req->local_va || req->home_va != req->map_req.remote_uba)) {
        qemu_log("SIM_DEC: GSVA identity requires local_va/home_va/remote_uba equal and pte_offset=0\n");
        resp->status = SIM_DEC_STATUS_INVALID_PARAM;
        return -1;
    }

    qemu_mutex_lock(&g_sim_decoder->lock);
    if (sim_dec_check_overlap(req->map_req.local_pa, req->map_req.size)) {
        qemu_mutex_unlock(&g_sim_decoder->lock);
        qemu_log("SIM_DEC: PA overlap detected\n");
        resp->status = SIM_DEC_STATUS_RESOURCE_BUSY;
        return -1;
    }
    if (sim_dec_check_gva_route_overlap(req)) {
        qemu_mutex_unlock(&g_sim_decoder->lock);
        resp->status = SIM_DEC_STATUS_RESOURCE_BUSY;
        return -1;
    }

    map_id = g_sim_decoder->next_map_id++;
    if (map_id == 0)
        map_id = g_sim_decoder->next_map_id++;

    ownership_required = sim_dec_gva_ownership_required_req(req);
    if (ownership_required &&
        sim_dec_gva_ownership_register_req(req, map_id) != 0) {
        qemu_mutex_unlock(&g_sim_decoder->lock);
        resp->status = SIM_DEC_STATUS_RESOURCE_BUSY;
        return -1;
    }

    entry = g_malloc0(sizeof(*entry));
    if (sim_dec_populate_map_entry(entry, &req->map_req) != 0) {
        SimDecMapEntry unregister_entry = {
            .map_id = map_id,
            .gva_id = req->gva_id,
            .remote_uba = req->map_req.remote_uba,
            .size = req->map_req.size,
            .gva_ownership_registered = ownership_required,
        };

        sim_dec_gva_ownership_unregister_entry(&unregister_entry);
        qemu_mutex_unlock(&g_sim_decoder->lock);
        g_free(entry);
        resp->status = SIM_DEC_STATUS_BACKEND_ERROR;
        return -1;
    }

    entry->map_id = map_id;
    entry->state = SIM_DEC_ROUTE_CREATING;
    entry->last_error = 0;
    entry->map_source = req->map_source;
    entry->address_profile = req->address_profile;
    entry->cache_policy = req->cache_policy;
    entry->access_flags = req->access_flags;
    entry->vmid = req->vmid;
    entry->asid = req->asid;
    entry->tid = req->tid;
    entry->p_tag = req->p_tag;
    entry->local_va = req->local_va;
    entry->home_va = req->home_va;
    entry->pte_offset = req->pte_offset;
    entry->gva_id = req->gva_id;
    entry->gva_ownership_registered = ownership_required;
    sim_dec_resolve_gva_mp_entry(entry);
    sim_dec_derive_gva_p_tag(entry);
    sim_dec_ensure_gva_write_back_cache(entry);

    memory_region_init_io(&entry->cpu_window, OBJECT(DEVICE(g_sim_decoder->bcs->ubc_dev)),
                          &sim_dec_cpu_window_ops, entry, "ub-sim-decoder-cpu",
                          entry->size);
    memory_region_add_subregion_overlap(get_system_memory(), entry->local_pa,
                                        &entry->cpu_window, 10);
    entry->mapped = true;
    entry->state = SIM_DEC_ROUTE_ACTIVE;
    entry->active = true;

    QTAILQ_INSERT_TAIL(&g_sim_decoder->map_list, entry, next);
    qemu_mutex_unlock(&g_sim_decoder->lock);

    resp->map_id = map_id;
    resp->status = SIM_DEC_STATUS_SUCCESS;
    resp->p_tag = entry->p_tag;
    resp->mp_ubc_port = entry->mp_ubc_port;
    resp->mp_lane = entry->mp_lane;
    resp->mp_link_id = entry->mp_link_id;

    qemu_log("SIM_DEC: GVA_MAP success id=%" PRIx64 " pa=%" PRIx64
             " sz=%" PRIx64 " remote_uba=%" PRIx64 " token=%u"
             " map_source=%u address_profile=%u local_va=%" PRIx64
             " home_va=%" PRIx64 " pte_offset=%" PRIx64 " vmid=%" PRIu32
             " asid=%" PRIu32 " p_tag=%" PRIu32 " gva_id=%" PRIx64 "\n",
             map_id, req->map_req.local_pa, req->map_req.size,
             req->map_req.remote_uba, req->map_req.token_id,
             req->map_source, req->address_profile,
             req->local_va, req->home_va, req->pte_offset,
             req->vmid, req->asid, entry->p_tag, req->gva_id);
    qemu_log("GVA_S3_MAP id=%" PRIx64 " gva_id=%" PRIx64
             " vmid=%" PRIu32 " asid=%" PRIu32
             " local_va=%" PRIx64 " home_va=%" PRIx64
             " pte_offset=%" PRIx64 " uba=%" PRIx64
             " pa=%" PRIx64 " size=%" PRIx64
             " dcna=%" PRIu32 " tid=%" PRIu32 " token=%" PRIu32
             " upi=%" PRIu32 " p_tag=%" PRIu32
             " ubc_port=%" PRIu32 " lane=%" PRIu32
             " link_id=%" PRIu32
             " map_source=%" PRIu32 " address_profile=%" PRIu32
             " cache_policy=%" PRIu32 "\n",
             map_id, req->gva_id, req->vmid, req->asid,
             req->local_va, req->home_va, req->pte_offset,
             req->map_req.remote_uba, req->map_req.local_pa,
             req->map_req.size, req->map_req.dcna, req->tid,
             req->map_req.token_id, req->map_req.upi, entry->p_tag,
             entry->mp_ubc_port, entry->mp_lane, entry->mp_link_id,
             req->map_source, req->address_profile, req->cache_policy);
    sim_dec_log_gva_route_dump(entry, sim_dec_route_state_name(entry->state));
    sim_dec_flush_gva_tlbs("gva_map");
    return 0;
}

static int sim_dec_handle_map(const SimDecMapReq *req, SimDecMapResp *resp)
{
    SimDecMapEntry *entry;
    uint64_t map_id;

    if (!g_sim_decoder || !g_sim_decoder->enabled) {
        resp->status = SIM_DEC_STATUS_BACKEND_ERROR;
        return -1;
    }

    /* Validate request */
    if (req->size == 0 || req->size > (1ULL << 40)) {
        qemu_log("SIM_DEC: invalid size %" PRIx64 "\n", req->size);
        resp->status = SIM_DEC_STATUS_INVALID_PARAM;
        return -1;
    }
    if (!sim_dec_range_end(req->local_pa, req->size, NULL) ||
        !sim_dec_range_end(req->remote_uba, req->size, NULL)) {
        qemu_log("SIM_DEC: invalid range local_pa=%" PRIx64
                 " remote_uba=%" PRIx64 " size=%" PRIx64 "\n",
                 req->local_pa, req->remote_uba, req->size);
        resp->status = SIM_DEC_STATUS_INVALID_PARAM;
        return -1;
    }

    if (req->token_id == 0) {
        qemu_log("SIM_DEC: token_id cannot be 0\n");
        resp->status = SIM_DEC_STATUS_INVALID_PARAM;
        return -1;
    }

    qemu_mutex_lock(&g_sim_decoder->lock);

    /* Check for overlapping mappings */
    if (sim_dec_check_overlap(req->local_pa, req->size)) {
        qemu_mutex_unlock(&g_sim_decoder->lock);
        qemu_log("SIM_DEC: PA overlap detected\n");
        resp->status = SIM_DEC_STATUS_RESOURCE_BUSY;
        return -1;
    }

    /* Create new map entry */
    entry = g_malloc0(sizeof(*entry));
    map_id = g_sim_decoder->next_map_id++;
    if (map_id == 0)
        map_id = g_sim_decoder->next_map_id++;

    if (sim_dec_populate_map_entry(entry, req) != 0) {
        qemu_mutex_unlock(&g_sim_decoder->lock);
        g_free(entry);
        resp->status = SIM_DEC_STATUS_BACKEND_ERROR;
        return -1;
    }
    entry->map_id = map_id;
    entry->state = SIM_DEC_ROUTE_CREATING;
    entry->last_error = 0;

    memory_region_init_io(&entry->cpu_window, OBJECT(DEVICE(g_sim_decoder->bcs->ubc_dev)),
                          &sim_dec_cpu_window_ops, entry, "ub-sim-decoder-cpu",
                          entry->size);
    memory_region_add_subregion_overlap(get_system_memory(), entry->local_pa,
                                        &entry->cpu_window, 10);
    entry->mapped = true;
    entry->state = SIM_DEC_ROUTE_ACTIVE;
    entry->active = true;

    QTAILQ_INSERT_TAIL(&g_sim_decoder->map_list, entry, next);
    qemu_mutex_unlock(&g_sim_decoder->lock);

    resp->map_id = map_id;
    resp->status = SIM_DEC_STATUS_SUCCESS;

    qemu_log("SIM_DEC: MAP success id=%" PRIx64 " pa=%" PRIx64 " sz=%" PRIx64
             " remote_uba=%" PRIx64 " token=%u\n",
             map_id, req->local_pa, req->size, req->remote_uba, req->token_id);
    return 0;
}

static int sim_dec_handle_unmap(const SimDecUnmapReq *req)
{
    SimDecMapEntry *entry;
    bool gva_entry;
    int flush_ret;

    if (!g_sim_decoder)
        return -1;

    qemu_mutex_lock(&g_sim_decoder->lock);
    entry = sim_dec_find_entry_by_id(req->map_id);
    if (!entry) {
        qemu_mutex_unlock(&g_sim_decoder->lock);
        qemu_log("SIM_DEC: UNMAP failed - map_id %" PRIx64 " not found\n",
                 req->map_id);
        return SIM_DEC_STATUS_INVALID_PARAM;
    }

    /* Flush dirty pages before unmapping */
    if (entry->page_cache) {
        flush_ret = sim_dec_page_cache_flush_dirty(entry);
        if (flush_ret != 0) {
            entry->active = false;
            entry->state = SIM_DEC_ROUTE_ERROR;
            entry->last_error = flush_ret;
            gva_entry = sim_dec_is_gva_entry(entry);
            qemu_mutex_unlock(&g_sim_decoder->lock);
            qemu_log("SIM_DEC: UNMAP failed - flush dirty map_id=%" PRIx64
                     " ret=%d\n", req->map_id, flush_ret);
            if (gva_entry) {
                sim_dec_log_gva_route_dump(entry,
                                           sim_dec_route_state_name(entry->state));
                sim_dec_flush_gva_tlbs("gva_unmap_error");
            }
            return SIM_DEC_STATUS_BACKEND_ERROR;
        }
    }

    /* Flush dirty coherence cache lines before unmapping */
    if (entry->cache_policy == SIM_DEC_CACHE_POLICY_DIRECTORY_MESI) {
        uint32_t home_cna = entry->dcna;
        uint32_t token_id = entry->token_id;
        int fence_ret;

        qemu_mutex_unlock(&g_sim_decoder->lock);
        fence_ret = obmm_coh_send_fence(g_sim_decoder->bcs->ubc_dev, home_cna,
                                        entry->remote_uba, entry->size,
                                        token_id);
        qemu_mutex_lock(&g_sim_decoder->lock);
        if (fence_ret != 0) {
            qemu_mutex_unlock(&g_sim_decoder->lock);
            qemu_log("SIM_DEC: UNMAP failed - coherence fence map_id=%" PRIx64
                     " ret=%d\n", req->map_id, fence_ret);
            return SIM_DEC_STATUS_BACKEND_ERROR;
        }
        obmm_coh_invalidate_local_range(g_sim_decoder->bcs->ubc_dev, home_cna,
                                        entry->remote_uba, entry->size,
                                        token_id);
    }

    gva_entry = sim_dec_is_gva_entry(entry);
    entry->active = false;
    entry->state = SIM_DEC_ROUTE_RETIRED;
    entry->last_error = 0;
    if (gva_entry) {
        sim_dec_gva_ownership_unregister_entry(entry);
        entry->gva_ownership_registered = false;
    }
    if (entry->mapped) {
        ubc_cpu_window_detach(&entry->cpu_window);
        entry->mapped = false;
    }
    QTAILQ_REMOVE(&g_sim_decoder->map_list, entry, next);
    /*
     * Keep the MemoryRegion and its opaque entry alive until decoder teardown.
     * Memory dispatch and in-flight SIM_DEC sync paths can still hold stale
     * references briefly after del_subregion(); freeing here can corrupt QOM
     * owner refs or leave callbacks with a dangling opaque pointer.
     */
    QTAILQ_INSERT_TAIL(&g_sim_decoder->retired_map_list, entry, next);
    qemu_mutex_unlock(&g_sim_decoder->lock);

    qemu_log("SIM_DEC: UNMAP success id=%" PRIx64 "\n", req->map_id);
    if (gva_entry) {
        sim_dec_log_gva_route_dump(entry, sim_dec_route_state_name(entry->state));
        sim_dec_print_stats(&g_sim_decoder->stats, "");
        sim_dec_flush_gva_tlbs("gva_unmap");
    }
    return SIM_DEC_STATUS_SUCCESS;
}

static int sim_dec_handle_sync(const SimDecSyncReq *req)
{
    SimDecMapEntry *entry;
    uint8_t *shadow;
    uint64_t remote_uba;
    uint64_t offset;
    uint64_t len;
    MemTxResult ret;

    if (!g_sim_decoder)
        return SIM_DEC_STATUS_BACKEND_ERROR;

    qemu_mutex_lock(&g_sim_decoder->lock);
    entry = sim_dec_find_entry_by_id(req->map_id);
    if (!entry) {
        qemu_mutex_unlock(&g_sim_decoder->lock);
        return SIM_DEC_STATUS_INVALID_PARAM;
    }

    if (!entry->active) {
        qemu_mutex_unlock(&g_sim_decoder->lock);
        return SIM_DEC_STATUS_INVALID_PARAM;
    }

    if (req->offset > entry->size || req->len > entry->size - req->offset) {
        qemu_mutex_unlock(&g_sim_decoder->lock);
        return SIM_DEC_STATUS_INVALID_PARAM;
    }

    shadow = entry->sync_shadow;
    remote_uba = entry->remote_uba;
    offset = req->offset;
    len = req->len;
    if (entry->cache_policy == SIM_DEC_CACHE_POLICY_DIRECTORY_MESI) {
        uint32_t home_cna = entry->dcna;
        uint32_t token_id = entry->token_id;

        qemu_mutex_unlock(&g_sim_decoder->lock);
        if (obmm_coh_send_fence(g_sim_decoder->bcs->ubc_dev, home_cna,
                                remote_uba + offset, len, token_id) != 0) {
            return SIM_DEC_STATUS_BACKEND_ERROR;
        }
        return SIM_DEC_STATUS_SUCCESS;
    }

    /* Flush dirty write-back cache before sync read */
    if (entry->page_cache) {
        if (sim_dec_page_cache_flush_dirty(entry) != 0) {
            qemu_mutex_unlock(&g_sim_decoder->lock);
            return SIM_DEC_STATUS_BACKEND_ERROR;
        }
        uint64_t start_page = offset / SIM_DEC_PAGE_SIZE;
        uint64_t end_page = (offset + len + SIM_DEC_PAGE_SIZE - 1) / SIM_DEC_PAGE_SIZE;
        qemu_mutex_lock(&entry->page_cache->lock);
        sim_dec_page_cache_invalidate_range(entry->page_cache, start_page, end_page);
        qemu_mutex_unlock(&entry->page_cache->lock);
    }

    qemu_mutex_unlock(&g_sim_decoder->lock);

    ret = ubc_sim_dec_remote_read(g_sim_decoder->bcs->ubc_dev,
                                  remote_uba + offset,
                                  entry->token_id,
                                  entry->dcna,
                                  shadow + offset,
                                  len);
    if (ret != MEMTX_OK) {
        qemu_log("SIM_DEC: SYNC read failed map_id=%" PRIx64 " offset=%" PRIx64
                 " len=%" PRIx64 " ret=%d\n",
                 req->map_id, req->offset, req->len, ret);
        return SIM_DEC_STATUS_BACKEND_ERROR;
    }

    qemu_mutex_lock(&g_sim_decoder->lock);
    entry = sim_dec_find_entry_by_id(req->map_id);
    if (!entry || !entry->active) {
        qemu_mutex_unlock(&g_sim_decoder->lock);
        return SIM_DEC_STATUS_INVALID_PARAM;
    }
    if (entry->sync_valid_len == 0) {
        entry->sync_valid_off = offset;
        entry->sync_valid_len = len;
    } else {
        uint64_t start = MIN(entry->sync_valid_off, offset);
        uint64_t end = MAX(entry->sync_valid_off + entry->sync_valid_len, offset + len);
        entry->sync_valid_off = start;
        entry->sync_valid_len = end - start;
    }
    qemu_mutex_unlock(&g_sim_decoder->lock);
    return SIM_DEC_STATUS_SUCCESS;
}

static int sim_dec_handle_query(const SimDecQueryReq *req, SimDecQueryResp *resp)
{
    SimDecMapEntry *entry;

    if (!g_sim_decoder)
        return SIM_DEC_STATUS_BACKEND_ERROR;

    qemu_mutex_lock(&g_sim_decoder->lock);
    entry = sim_dec_find_entry_by_id(req->map_id);
    if (!entry) {
        qemu_mutex_unlock(&g_sim_decoder->lock);
        resp->status = SIM_DEC_STATUS_INVALID_PARAM;
        return -1;
    }

    resp->local_pa = entry->local_pa;
    resp->size = entry->size;
    resp->status = entry->active ? 0 : 1;
    resp->ref_count = 1; /* Simplified */
    qemu_mutex_unlock(&g_sim_decoder->lock);

    return SIM_DEC_STATUS_SUCCESS;
}

static char *sim_dec_obmm_bootstrap_dir(void)
{
    const char *shared_dir = g_getenv("UB_FM_SHARED_DIR");

    if (!shared_dir || !shared_dir[0]) {
        return NULL;
    }
    return g_build_filename(shared_dir, "obmm_bootstrap", NULL);
}

static char *sim_dec_obmm_bootstrap_path(uint32_t node_id,
                                         uint64_t generation)
{
    g_autofree char *dir = sim_dec_obmm_bootstrap_dir();
    g_autofree char *name = NULL;

    if (!dir) {
        return NULL;
    }
    name = g_strdup_printf("node%u-generation%" PRIu64 ".ini",
                           node_id, generation);
    return g_build_filename(dir, name, NULL);
}

static int sim_dec_handle_obmm_bootstrap_publish(
    const SimDecObmmBootstrapPublishReq *req)
{
    const SimDecObmmBootstrapRecord *record = &req->record;
    g_autofree char *dir = NULL;
    g_autofree char *path = NULL;
    g_autofree char *tmp_path = NULL;
    g_autofree char *data = NULL;
    g_autoptr(GKeyFile) keyfile = NULL;
    uint64_t generation;
    GError *err = NULL;

    if (record->node_count < 2 ||
        record->node_count > SIM_DEC_OBMM_BOOTSTRAP_MAX_NODES ||
        record->node_id >= record->node_count ||
        record->export_cna == 0 || record->token_id == 0 ||
        record->remote_uba == 0 || record->size == 0) {
        return SIM_DEC_STATUS_INVALID_PARAM;
    }

    dir = sim_dec_obmm_bootstrap_dir();
    path = sim_dec_obmm_bootstrap_path(record->node_id,
                                       record->generation);
    if (!dir || !path) {
        qemu_log("SIM_DEC: OBMM bootstrap publish requires UB_FM_SHARED_DIR\n");
        return SIM_DEC_STATUS_NOT_SUPPORTED;
    }
    if (g_mkdir_with_parents(dir, 0755) != 0) {
        return SIM_DEC_STATUS_BACKEND_ERROR;
    }

    if (record->generation == 0) {
        return SIM_DEC_STATUS_INVALID_PARAM;
    }
    generation = record->generation;
    keyfile = g_key_file_new();
    g_key_file_set_uint64(keyfile, "obmm_export", "export_mem_id",
                          record->export_mem_id);
    g_key_file_set_uint64(keyfile, "obmm_export", "remote_uba",
                          record->remote_uba);
    g_key_file_set_uint64(keyfile, "obmm_export", "size", record->size);
    g_key_file_set_uint64(keyfile, "obmm_export", "generation", generation);
    g_key_file_set_uint64(keyfile, "obmm_export", "flags", record->flags);
    g_key_file_set_uint64(keyfile, "obmm_export", "node_id", record->node_id);
    g_key_file_set_uint64(keyfile, "obmm_export", "node_count",
                          record->node_count);
    g_key_file_set_uint64(keyfile, "obmm_export", "export_cna",
                          record->export_cna);
    g_key_file_set_uint64(keyfile, "obmm_export", "token_id",
                          record->token_id);

    data = g_key_file_to_data(keyfile, NULL, NULL);
    tmp_path = g_strdup_printf("%s.tmp.%d", path, getpid());
    if (!g_file_set_contents(tmp_path, data, -1, &err)) {
        qemu_log("SIM_DEC: OBMM bootstrap publish write failed: %s\n",
                 err ? err->message : "unknown");
        g_clear_error(&err);
        return SIM_DEC_STATUS_BACKEND_ERROR;
    }

    /* Make the local payload resolvable before publishing peer visibility. */
    obmm_export_register(record);
    if (g_sim_decoder->experimental_gsva_enabled) {
        sim_dec_register_obmm_gsva_route(record);
    }
    if (g_rename(tmp_path, path) != 0) {
        qemu_log("SIM_DEC: OBMM bootstrap publish rename failed: %s\n",
                 g_strerror(errno));
        return SIM_DEC_STATUS_BACKEND_ERROR;
    }

    qemu_log("SIM_DEC: OBMM bootstrap publish node=%u cna=%u uba=%" PRIx64
             " backing=%" PRIx64 " token=%u size=%" PRIx64 "\n",
             record->node_id, record->export_cna, record->remote_uba,
             record->backing_uba, record->token_id, record->size);

    return SIM_DEC_STATUS_SUCCESS;
}

static bool sim_dec_obmm_bootstrap_load(uint32_t node_id, uint32_t node_count,
                                        uint64_t generation,
                                        SimDecObmmBootstrapRecord *record)
{
    g_autofree char *path = sim_dec_obmm_bootstrap_path(node_id,
                                                        generation);
    g_autoptr(GKeyFile) keyfile = NULL;
    GError *err = NULL;

    if (!path || !g_file_test(path, G_FILE_TEST_EXISTS)) {
        return false;
    }

    keyfile = g_key_file_new();
    if (!g_key_file_load_from_file(keyfile, path, G_KEY_FILE_NONE, &err)) {
        qemu_log("SIM_DEC: OBMM bootstrap lookup read failed: %s\n",
                 err ? err->message : "unknown");
        g_clear_error(&err);
        return false;
    }

    record->export_mem_id = g_key_file_get_uint64(keyfile, "obmm_export",
                                                  "export_mem_id", NULL);
    record->remote_uba = g_key_file_get_uint64(keyfile, "obmm_export",
                                               "remote_uba", NULL);
    record->size = g_key_file_get_uint64(keyfile, "obmm_export", "size", NULL);
    record->generation = g_key_file_get_uint64(keyfile, "obmm_export",
                                               "generation", NULL);
    record->flags = g_key_file_get_uint64(keyfile, "obmm_export", "flags", NULL);
    record->node_id = (uint32_t)g_key_file_get_uint64(keyfile, "obmm_export",
                                                      "node_id", NULL);
    record->node_count = (uint32_t)g_key_file_get_uint64(keyfile, "obmm_export",
                                                         "node_count", NULL);
    record->export_cna = (uint32_t)g_key_file_get_uint64(keyfile, "obmm_export",
                                                         "export_cna", NULL);
    record->token_id = (uint32_t)g_key_file_get_uint64(keyfile, "obmm_export",
                                                       "token_id", NULL);

    return record->node_id == node_id &&
           record->node_count == node_count &&
           record->generation == generation &&
           record->export_cna != 0 &&
           record->token_id != 0 &&
           record->remote_uba != 0 &&
           record->size != 0;
}

static uint32_t linqu_uapi_cluster_node_count(void)
{
    const char *env = getenv("LINQU_UB_NODE_COUNT");
    char *end = NULL;
    unsigned long parsed;

    if (!env || env[0] == '\0') {
        return 8;
    }
    parsed = strtoul(env, &end, 10);
    if (end == env || parsed == 0 || parsed > SIM_DEC_OBMM_BOOTSTRAP_MAX_NODES) {
        return 8;
    }
    return (uint32_t)parsed;
}

static MemTxResult linqu_uapi_read_obmm_export(BusControllerDev *ubc_dev,
                                                const SimDecObmmBootstrapRecord *record,
                                                uint64_t offset,
                                                uint8_t *buf,
                                                uint64_t len)
{
    uint64_t remote_uba;

    if (!ubc_dev || !record || !buf || len == 0 || len > UINT32_MAX ||
        offset > record->size || len > record->size - offset) {
        return MEMTX_DECODE_ERROR;
    }
    remote_uba = record->remote_uba + offset;
    if (record->export_cna == ubc_dev->parent.cna) {
        return ubc_dma_read_local_data_tid_strict(ubc_dev,
                                                  remote_uba,
                                                  buf,
                                                  (size_t)len,
                                                  ubc_tid_or_auto(record->token_id));
    }
    return ubc_sim_dec_remote_read(ubc_dev,
                                   remote_uba,
                                   record->token_id,
                                   record->export_cna,
                                   buf,
                                   (uint32_t)len);
}

static bool linqu_uapi_obmm_payload_region_offset(
    BusControllerDev *ubc_dev,
    const SimDecObmmBootstrapRecord *record,
    uint64_t *payload_region_offset_out)
{
    ObmmPoolHeaderWire header;
    uint32_t index;

    if (!payload_region_offset_out) {
        return false;
    }
    *payload_region_offset_out = 0;
    if (linqu_uapi_read_obmm_export(ubc_dev,
                                    record,
                                    0,
                                    (uint8_t *)&header,
                                    sizeof(header)) != MEMTX_OK) {
        return false;
    }
    if (header.region_size == 0 ||
        header.region_size > record->size ||
        header.directory_offset < OBMM_POOL_HEADER_BYTES ||
        header.directory_count == 0 ||
        header.directory_count > 64) {
        return false;
    }
    for (index = 0; index < header.directory_count; index++) {
        ObmmRegionDirentWire dirent;
        uint64_t dirent_offset = header.directory_offset +
                                 (uint64_t)index * OBMM_REGION_DIRENT_BYTES;

        if (dirent_offset > record->size ||
            sizeof(dirent) > record->size - dirent_offset ||
            linqu_uapi_read_obmm_export(ubc_dev,
                                        record,
                                        dirent_offset,
                                        (uint8_t *)&dirent,
                                        sizeof(dirent)) != MEMTX_OK) {
            return false;
        }
        if (dirent.kind == OBMM_REGION_W4_PAYLOAD) {
            if (dirent.offset == 0 ||
                dirent.offset > record->size ||
                dirent.size > record->size - dirent.offset) {
                return false;
            }
            *payload_region_offset_out = dirent.offset;
            return true;
        }
    }
    return false;
}

static bool linqu_uapi_object_ref_is_model_runtime_payload(
    const LinquObmmObjectRefWire *object_ref)
{
    if (!object_ref ||
        object_ref->magic != LINGQU_OBMM_OBJECT_REF_MAGIC ||
        object_ref->state != LINGQU_OBJECT_STATE_COMMITTED_WIRE ||
        object_ref->payload_bytes == 0 ||
        object_ref->payload_bytes > UINT32_MAX) {
        return false;
    }
    return object_ref->object_kind == MODEL_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT ||
           object_ref->object_kind == MODEL_OBMM_KIND_TOKEN_RESULT ||
           object_ref->object_kind == MODEL_OBMM_KIND_KV_STATE;
}

static void linqu_uapi_maybe_register_model_runtime_object_payload(
    BusControllerDev *ubc_dev, uint64_t segment, uint64_t write_offset)
{
    LinquObmmObjectRefWire object_ref;
    SimDecObmmBootstrapRecord record;
    uint64_t object_ref_offset = write_offset & ~0x3fULL;
    uint64_t payload_region_offset = 0;
    uint64_t export_payload_offset;
    uint8_t *payload;
    uint32_t node_count;

    if (!ubc_dev || !ubc_dev->linqu_uapi_bridge) {
        return;
    }
    if ((write_offset & 0x3fULL) != 0x38ULL) {
        return;
    }
    if (linqu_ub_bridge_read_segment_payload(ubc_dev->linqu_uapi_bridge,
                                             segment,
                                             object_ref_offset,
                                             (uint8_t *)&object_ref,
                                             sizeof(object_ref)) != 0 ||
        !linqu_uapi_object_ref_is_model_runtime_payload(&object_ref)) {
        return;
    }
    node_count = linqu_uapi_cluster_node_count();
    if (object_ref.owner_entity >= node_count ||
        !sim_dec_obmm_bootstrap_load(object_ref.owner_entity,
                                     node_count,
                                     1,
                                     &record) ||
        !linqu_uapi_obmm_payload_region_offset(ubc_dev,
                                               &record,
                                               &payload_region_offset)) {
        return;
    }
    if (object_ref.payload_offset > record.size ||
        payload_region_offset > record.size ||
        object_ref.payload_bytes > record.size - payload_region_offset ||
        object_ref.payload_offset >
            record.size - payload_region_offset - object_ref.payload_bytes) {
        return;
    }
    export_payload_offset = payload_region_offset + object_ref.payload_offset;
    payload = g_malloc0((gsize)object_ref.payload_bytes);
    if (linqu_uapi_read_obmm_export(ubc_dev,
                                    &record,
                                    export_payload_offset,
                                    payload,
                                    object_ref.payload_bytes) == MEMTX_OK) {
        (void)linqu_ub_bridge_register_model_runtime_object_payload(
            ubc_dev->linqu_uapi_bridge,
            (const uint8_t *)&object_ref,
            sizeof(object_ref),
            payload,
            (size_t)object_ref.payload_bytes);
    }
    g_free(payload);
}

static int sim_dec_handle_obmm_bootstrap_lookup(
    const SimDecObmmBootstrapLookupReq *req,
    SimDecObmmBootstrapLookupResp *resp)
{
    g_autofree char *dir = NULL;
    uint32_t i;

    if (req->node_count < 2 ||
        req->node_count > SIM_DEC_OBMM_BOOTSTRAP_MAX_NODES ||
        req->generation == 0) {
        return SIM_DEC_STATUS_INVALID_PARAM;
    }
    dir = sim_dec_obmm_bootstrap_dir();
    if (!dir) {
        qemu_log("SIM_DEC: OBMM bootstrap lookup requires UB_FM_SHARED_DIR\n");
        return SIM_DEC_STATUS_NOT_SUPPORTED;
    }

    memset(resp, 0, sizeof(*resp));
    for (i = 0; i < req->node_count; i++) {
        SimDecObmmBootstrapRecord record = {0};

        if (!sim_dec_obmm_bootstrap_load(i, req->node_count, req->generation,
                                         &record)) {
            continue;
        }
        if (resp->count >= SIM_DEC_OBMM_BOOTSTRAP_MAX_NODES) {
            return SIM_DEC_STATUS_INVALID_PARAM;
        }
        resp->records[resp->count++] = record;
    }
    return SIM_DEC_STATUS_SUCCESS;
}

/*
 * sim_dec_lookup_by_pa - Lookup decoder entry for address translation
 * Called by UMMU to check if a PA is in decoder map for remote access
 */
static int sim_dec_lookup_result_by_pa(uint64_t pa,
                                       SimDecLookupResult *result)
{
    SimDecMapEntry *entry;
    uint64_t offset;

    if (!result || !g_sim_decoder || !g_sim_decoder->enabled)
        return -1;

    memset(result, 0, sizeof(*result));

    qemu_mutex_lock(&g_sim_decoder->lock);
    entry = sim_dec_find_entry_by_pa(pa);
    if (entry && entry->active) {
        offset = pa - entry->local_pa;
        result->map_id = entry->map_id;
        result->gva_id = entry->gva_id;
        result->local_pa = entry->local_pa;
        result->local_va = entry->local_va;
        result->remote_uba = entry->remote_uba + offset;
        result->token_id = entry->token_id;
        result->token_value = entry->token_value;
        result->src_eid = entry->src_eid;
        result->address_profile = entry->address_profile;
        result->access_flags = entry->access_flags;
        result->vmid = entry->vmid;
        result->asid = entry->asid;
        result->dcna = entry->dcna;
        result->tid = entry->tid;
        result->upi = entry->upi;
        result->p_tag = entry->p_tag;
        result->mp_ubc_port = entry->mp_ubc_port;
        result->mp_lane = entry->mp_lane;
        result->mp_link_id = entry->mp_link_id;
        result->cache_policy = entry->cache_policy;
        qemu_mutex_unlock(&g_sim_decoder->lock);
        return 0;
    }
    qemu_mutex_unlock(&g_sim_decoder->lock);
    return -1;
}

int sim_dec_lookup_by_pa(uint64_t pa, uint64_t *remote_uba,
                         uint32_t *token_id, uint32_t *src_eid,
                         uint32_t *address_profile, uint32_t *dcna)
{
    SimDecLookupResult result;

    if (sim_dec_lookup_result_by_pa(pa, &result) != 0) {
        return -1;
    }

    if (remote_uba)
        *remote_uba = result.remote_uba;
    if (token_id)
        *token_id = result.token_id;
    if (src_eid)
        *src_eid = result.src_eid;
    if (address_profile)
        *address_profile = result.address_profile;
    if (dcna)
        *dcna = result.dcna;
    return 0;
}

/* Global GSVA route + coherence tables
 * (g_gsva_routes forward-declared before SIM_DEC handlers) */
static GsvaCohTable g_gsva_coh;
static GsvaStats g_gsva_stats;
static bool g_gsva_initialized;

/* TLB side table: VA -> epoch for stale detection */
#define GSVA_TLB_STABLE_SIZE 256
typedef struct GsvaTlbSideEntry {
    uint64_t va;
    uint64_t epoch;
    uint64_t segment_id;
    bool valid;
} GsvaTlbSideEntry;
static GsvaTlbSideEntry g_gsva_tlb_stable[GSVA_TLB_STABLE_SIZE];

static void gsva_tables_init(void)
{
    if (!g_gsva_initialized) {
        gsva_route_table_init(&g_gsva_routes);
        gsva_coh_table_init(&g_gsva_coh);
        gsva_coh_set_default_table(&g_gsva_coh);
        gsva_coh_set_default_route_table(&g_gsva_routes);
        gsva_stats_init(&g_gsva_stats);
        memset(g_gsva_tlb_stable, 0, sizeof(g_gsva_tlb_stable));
        g_gsva_initialized = true;
    }
}

static void sim_dec_register_obmm_gsva_route(
    const SimDecObmmBootstrapRecord *record)
{
    GsvaKeyV1 key = { 0 };
    uint64_t map_id = 0;
    int rc;
    int coh_rc;

    if (!record || record->export_mem_id == 0 || record->remote_uba == 0 ||
        record->backing_uba == 0 || record->size == 0 ||
        record->export_cna == 0 || record->token_id == 0) {
        return;
    }

    gsva_tables_init();

    key.version = 1;
    key.segment_id = record->export_mem_id;
    key.home_va = record->remote_uba;
    key.size = record->size;
    key.p_tag = record->export_cna & 0x00ffffffu;
    key.cache_policy = 4;
    key.epoch = 1;

    if (gsva_route_lookup_base(&g_gsva_routes, &key)) {
        return;
    }

    rc = gsva_route_map(&g_gsva_routes,
                        &key,
                        record->backing_uba,
                        record->remote_uba,
                        record->remote_uba,
                        SIM_DEC_MAP_SOURCE_LEGACY_OBMM,
                        GSVA_ADDRESS_PROFILE_STRICT_GSVA,
                        record->export_cna,
                        record->token_id,
                        record->token_id,
                        UB_GSVA_DEVICE_ACCESS_READ_WRITE,
                        &map_id);
    if (rc != GSVA_OK) {
        qemu_log("GSVA_MAP: obmm bootstrap route failed segment=%#" PRIx64
                 " home_va=%#" PRIx64 " backing=%#" PRIx64
                 " size=%#" PRIx64 " rc=%d\n",
                 key.segment_id, key.home_va, record->backing_uba,
                 key.size, rc);
        return;
    }

    coh_rc = gsva_coh_object_create(&g_gsva_coh, &key, 0, map_id);
    if (coh_rc != GSVA_OK) {
        qemu_log("GSVA_MAP: obmm bootstrap coh create failed segment=%#"
                 PRIx64 " rc=%d\n",
                 key.segment_id, coh_rc);
        gsva_route_unmap(&g_gsva_routes, map_id, true);
        return;
    }

    gsva_stats_map(&g_gsva_stats, true);
    qemu_log("GSVA_MAP: obmm bootstrap route segment=%#" PRIx64
             " home_va=%#" PRIx64 " backing=%#" PRIx64
             " size=%#" PRIx64 " token=%" PRIu32
             " map_id=%#" PRIx64 "\n",
             key.segment_id, key.home_va, record->backing_uba,
             key.size, record->token_id, map_id);
}

/* Helper for sim_dec_gva_tcg_translate to look up GSVA routes.
 * Defined here after g_gsva_routes/g_gsva_initialized are declared. */
static unsigned gsva_tlb_stable_index(uint64_t va)
{
    return (unsigned)((va >> 12) % GSVA_TLB_STABLE_SIZE);
}

static void gsva_tlb_stable_set(uint64_t va, uint64_t epoch,
                                 uint64_t segment_id)
{
    unsigned idx = gsva_tlb_stable_index(va);
    g_gsva_tlb_stable[idx].va = va;
    g_gsva_tlb_stable[idx].epoch = epoch;
    g_gsva_tlb_stable[idx].segment_id = segment_id;
    g_gsva_tlb_stable[idx].valid = true;
}

static void gsva_tlb_stable_flush_all(const char *reason)
{
    memset(g_gsva_tlb_stable, 0, sizeof(g_gsva_tlb_stable));
    qemu_log("GSVA_TLB: flush reason=%s\n",
             reason ? reason : "unspecified");
}

static void gsva_tlb_stable_flush_key(const GsvaKeyV1 *key,
                                      const char *reason)
{
    CPUState *src = current_cpu ? current_cpu : first_cpu;
    uint64_t end;
    unsigned cleared = 0;
    int i;

    if (!key || key->size == 0 || UINT64_MAX - key->home_va < key->size) {
        return;
    }

    end = key->home_va + key->size;
    for (i = 0; i < GSVA_TLB_STABLE_SIZE; i++) {
        GsvaTlbSideEntry *e = &g_gsva_tlb_stable[i];

        if (!e->valid || e->segment_id != key->segment_id ||
            e->va < key->home_va || e->va >= end) {
            continue;
        }
        memset(e, 0, sizeof(*e));
        cleared++;
    }

    if (src && gsva_arm_mmu_enabled()) {
        tlb_flush_all_cpus_synced(src);
    }

    qemu_log("GSVA_TLB: flush reason=%s segment_id=%#" PRIx64
             " home_va=%#" PRIx64 " size=%#" PRIx64
             " cleared=%u\n",
             reason ? reason : "unspecified",
             key->segment_id, key->home_va, key->size, cleared);
}

static int gsva_tlb_stale_check(uint64_t va, uint64_t current_epoch)
{
    unsigned idx = gsva_tlb_stable_index(va);
    GsvaTlbSideEntry *e = &g_gsva_tlb_stable[idx];
    if (!e->valid || e->va != va) {
        return 0; /* no entry, not stale */
    }
    if (e->epoch != current_epoch) {
        return GSVA_ERR_TLB_STALE;
    }
    return 0;
}

/*
 * GSVA ARM MMU translate hook.
 *
 * Called from arm_cpu_tlb_fill after GVA route lookup succeeds.
 * Checks GSVA coherence permissions for the VA access.
 * Returns: 0 = GSVA permission OK, negative = access denied.
 */
int gsva_arm_mmu_translate_full(uint64_t va, bool is_write,
                                uint32_t cpu_index,
                                uint64_t *local_pa,
                                uint64_t *page_size)
{
    GsvaRouteEntry *route;
    GsvaCohObject *coh_obj;
    uint64_t page_va = va & ~(uint64_t)(SIM_DEC_PAGE_SIZE - 1);
    uint64_t offset;
    uint32_t requester_cna = cpu_index;
    bool identity_route;
    int acq_rc;

    if (!gsva_arm_mmu_enabled() || !local_pa || !page_size) {
        return 0;
    }

    if (!g_gsva_initialized) {
        return 0;
    }

    route = gsva_route_lookup_va(&g_gsva_routes, 0, 0, page_va);
    if (!route) {
        return 0;
    }
    if (route->source != SIM_DEC_MAP_SOURCE_GVA_MANAGER) {
        return 0;
    }

    identity_route =
        route->local_va == route->key.home_va &&
        route->remote_uba == route->key.home_va &&
        (route->address_profile == GSVA_ADDRESS_PROFILE_STRICT_GSVA ||
         route->address_profile == GSVA_ADDRESS_PROFILE_COMPAT_GSVA);

    if (!identity_route || route->local_va == 0 || route->local_pa == 0 ||
        page_va < route->local_va ||
        page_va >= route->local_va + route->key.size) {
        qemu_log("GSVA_MMU: unsupported route va=%#" PRIx64
                 " local_va=%#" PRIx64 " home_va=%#" PRIx64
                 " remote_uba=%#" PRIx64 " source=%" PRIu32
                 " profile=%" PRIu32 "\n",
                 page_va, route->local_va, route->key.home_va,
                 route->remote_uba, route->source, route->address_profile);
        return GSVA_ERR_ROUTE_MISSING;
    }

    offset = page_va - route->local_va;
    *local_pa = route->local_pa + offset;
    *page_size = SIM_DEC_PAGE_SIZE;
    if (g_sim_decoder && g_sim_decoder->bcs && g_sim_decoder->bcs->ubc_dev) {
        requester_cna = g_sim_decoder->bcs->ubc_dev->parent.cna;
    }

    if (is_write) {
        acq_rc = gsva_coh_write_acquire_tx(&g_gsva_coh, &g_gsva_routes,
                                           g_sim_decoder &&
                                           g_sim_decoder->bcs ?
                                           g_sim_decoder->bcs->ubc_dev : NULL,
                                           &route->key, requester_cna,
                                           route->token.token_id,
                                           route->token.token_value);
    } else {
        acq_rc = gsva_coh_read_acquire_tx(&g_gsva_coh, &g_gsva_routes,
                                          g_sim_decoder &&
                                          g_sim_decoder->bcs ?
                                          g_sim_decoder->bcs->ubc_dev : NULL,
                                          &route->key, requester_cna,
                                          route->token.token_id,
                                          route->token.token_value);
    }
    if (acq_rc != GSVA_OK) {
        if (acq_rc == GSVA_ERR_COH_TIMEOUT) {
            gsva_tlb_stable_flush_key(&route->key, "coh_timeout");
        }
        qemu_log("GSVA_MMU: acquire failed va=%#" PRIx64
                 " segment_id=%#" PRIx64 " is_write=%u rc=%d\n",
                 page_va, route->key.segment_id, is_write, acq_rc);
        return acq_rc;
    }

    coh_obj = gsva_coh_lookup(&g_gsva_coh, &route->key);
    if (!coh_obj) {
        return GSVA_ERR_ROUTE_MISSING;
    }

    if (coh_obj->state == GSVA_COH_RETIRED) {
        qemu_log("GSVA_MMU: access to RETIRED segment va=%#" PRIx64
                 " segment_id=%#" PRIx64 "\n",
                 va, coh_obj->key.segment_id);
        return GSVA_ERR_SEGMENT_RETIRED;
    }

    if (coh_obj->state == GSVA_COH_TIMEOUT || coh_obj->state == GSVA_COH_I) {
        qemu_log("GSVA_MMU: access to %s segment va=%#" PRIx64
                 " segment_id=%#" PRIx64 "\n",
                 gsva_coh_state_name(coh_obj->state),
                 va, coh_obj->key.segment_id);
        return coh_obj->state == GSVA_COH_TIMEOUT ?
               GSVA_ERR_COH_TIMEOUT : GSVA_ERR_COH_PENDING;
    }

    if (is_write && coh_obj->state != GSVA_COH_M &&
        coh_obj->state != GSVA_COH_E) {
        qemu_log("GSVA_MMU: write to non-exclusive segment va=%#" PRIx64
                 " state=%s\n",
                 va, gsva_coh_state_name(coh_obj->state));
        return GSVA_ERR_COH_PENDING;
    }

    /* TLB stale detection */
    int stale_rc = gsva_tlb_stale_check(va, coh_obj->epoch);
    if (stale_rc == GSVA_ERR_TLB_STALE) {
        qemu_log("GSVA_TLB: STALE va=%#" PRIx64 " tlb_epoch=%" PRIu64
                 " current_epoch=%" PRIu64 " segment_id=%#" PRIx64 "\n",
                 va, g_gsva_tlb_stable[gsva_tlb_stable_index(va)].epoch,
                 coh_obj->epoch, coh_obj->key.segment_id);
        return GSVA_ERR_TLB_STALE;
    }

    /* Update TLB side table */
    gsva_tlb_stable_set(va, coh_obj->epoch, coh_obj->key.segment_id);

    qemu_log("GSVA_TLB: lookup va=%#" PRIx64 " state=%s"
             " is_write=%u cpu=%" PRIu32 " segment_id=%#" PRIx64
             " epoch=%" PRIu64 " local_pa=%#" PRIx64 "\n",
             va, gsva_coh_state_name(coh_obj->state),
             is_write, cpu_index, coh_obj->key.segment_id,
             coh_obj->epoch, *local_pa);

    return 1;
}

int gsva_arm_mmu_translate(uint64_t va, bool is_write, uint32_t cpu_index)
{
    uint64_t local_pa = 0;
    uint64_t page_size = 0;
    int rc = gsva_arm_mmu_translate_full(va, is_write, cpu_index,
                                         &local_pa, &page_size);

    return rc > 0 ? 0 : rc;
}

/*
 * GSVA query handler - capability and object queries.
 */
static int sim_dec_handle_gsva_query(const SimDecGsvaQueryReq *req,
                                     SimDecGsvaQueryResp *resp)
{
    memset(resp, 0, sizeof(*resp));
    resp->version = 1;

    if (!req) {
        resp->error = GSVA_ERR_BAD_VERSION;
        return -1;
    }

    qemu_log("GSVA_KEY: query type=%" PRIu32 "\n", req->query_type);

    switch (req->query_type) {
    case GSVA_QUERY_CAPS: {
        SimDecGsvaCapsResp caps;
        memset(&caps, 0, sizeof(caps));
        caps.version = 1;
        caps.max_nodes = 8;
        caps.supported_cache_policies = (1u << SIM_DEC_CACHE_POLICY_DIRECTORY_MESI);
        caps.supported_modes = 0
            | (1u << 0)  /* legacy_sim_dec */
            | (1u << 1)  /* sim_gva_tcg */
            | (1u << 2); /* arm_mmu */
        caps.flags = 0
            | GSVA_CAP_STRICT_ADDRESS_IDENTITY
            | GSVA_CAP_ROUTE_LAYER
            | GSVA_CAP_COHERENCE_LAYER
            | GSVA_CAP_ARM_MMU_MODE;
        qemu_log("GSVA_QUERY_CAPS: flags=%" PRIx32 " max_nodes=%" PRIu32
                 " cache_policies=%" PRIx32 " modes=%" PRIx32 "\n",
                 caps.flags, caps.max_nodes,
                 caps.supported_cache_policies, caps.supported_modes);
        memcpy(resp->data, &caps, sizeof(caps));
        resp->error = GSVA_OK;
        break;
    }
    case GSVA_QUERY_COHERENCE: {
        if (req->key.segment_id || req->key.home_va) {
            GsvaCohObject *obj = NULL;

            if (g_gsva_initialized) {
                QTAILQ_FOREACH(obj, &g_gsva_coh.objects, next) {
                    if (req->key.segment_id &&
                        obj->key.segment_id != req->key.segment_id) {
                        continue;
                    }
                    if (req->key.home_va &&
                        obj->key.home_va != req->key.home_va) {
                        continue;
                    }
                    break;
                }
            }

            if (!g_gsva_initialized || !obj) {
                resp->error = GSVA_ERR_ROUTE_MISSING;
            } else if (obj->state == GSVA_COH_TIMEOUT) {
                resp->error = GSVA_ERR_COH_TIMEOUT;
            } else if (obj->state == GSVA_COH_RETIRED) {
                resp->error = GSVA_ERR_SEGMENT_RETIRED;
            } else {
                resp->error = GSVA_OK;
            }
            {
                uint32_t state_code = obj ? (uint32_t)obj->state : UINT32_MAX;
                uint64_t pending_seq = obj ? obj->pending_seq : 0;
                uint64_t pending_ack_bitmap = obj ? obj->pending_ack_bitmap : 0;

                memcpy(resp->data, &state_code, sizeof(state_code));
                memcpy(resp->data + sizeof(state_code), &pending_seq,
                       sizeof(pending_seq));
                memcpy(resp->data + sizeof(state_code) + sizeof(pending_seq),
                       &pending_ack_bitmap, sizeof(pending_ack_bitmap));
            }
            qemu_log("GSVA_QUERY_COHERENCE: segment_id=%#" PRIx64
                     " home_va=%#" PRIx64 " state=%s error=%d"
                     " pending=%u seq=%" PRIu64 " waiting_for=%#" PRIx64 "\n",
                     req->key.segment_id, req->key.home_va,
                     obj ? gsva_coh_state_name(obj->state) : "MISSING",
                     resp->error, obj ? obj->pending : 0,
                     obj ? obj->pending_seq : 0,
                     obj ? obj->pending_ack_bitmap : 0);
            break;
        }

        /* Return stats: maps, unmaps, coh objects, read/write acquires */
        if (!g_gsva_initialized) {
            resp->error = GSVA_OK;
            break;
        }
        gsva_stats_set_objects(&g_gsva_stats,
                               (uint64_t)g_gsva_routes.route_count,
                               (uint64_t)g_gsva_coh.object_count);
        memcpy(resp->data, &g_gsva_stats, sizeof(g_gsva_stats));
        resp->error = GSVA_OK;
        qemu_log("GSVA_QUERY_COHERENCE: maps=%" PRIu64 " coh_objects=%"
                 PRIu64 " rd_acq=%" PRIu64 " wr_acq=%" PRIu64 "\n",
                 g_gsva_stats.map_total, g_gsva_stats.coh_objects,
                 g_gsva_stats.read_acquire_total,
                 g_gsva_stats.write_acquire_total);
        break;
    }
    case GSVA_QUERY_ROUTE:
    case GSVA_QUERY_SEGMENT:
        qemu_log("GSVA_KEY: query type %d not yet implemented\n",
                 req->query_type);
        resp->error = GSVA_ERR_FEATURE_MISSING;
        break;
    default:
        qemu_log("GSVA_KEY: unknown query type %" PRIu32 "\n",
                 req->query_type);
        resp->error = GSVA_ERR_BAD_VERSION;
        break;
    }

    return resp->error == GSVA_OK ? 0 : -1;
}

/* GSVA MAP handler */
static int sim_dec_handle_gsva_map(const SimDecGsvaMapReq *req,
                                   SimDecGsvaMapResp *resp)
{
    memset(resp, 0, sizeof(*resp));

    if (!req) {
        resp->error = GSVA_ERR_BAD_VERSION;
        return -1;
    }

    if (req->version != 1) {
        qemu_log("GSVA_MAP: bad version %" PRIu32 "\n", req->version);
        resp->error = GSVA_ERR_BAD_VERSION;
        return -1;
    }

    gsva_tables_init();

    resp->error = gsva_route_map(&g_gsva_routes,
                                  &req->key,
                                  req->local_pa,
                                  req->local_va,
                                  req->remote_uba,
                                  req->source,
                                  req->address_profile,
                                  req->dcna,
                                  (uint32_t)req->token_id,
                                  (uint32_t)req->token_value,
                                  req->access_flags,
                                  &resp->map_id);

    if (resp->error != GSVA_OK) {
        qemu_log("GSVA_MAP: failed: %s\n", gsva_error_name(resp->error));
        gsva_stats_map(&g_gsva_stats, false);
        return -1;
    }

    /* Create coherence object for this route */
    int coh_rc = gsva_coh_object_create(&g_gsva_coh, &req->key,
                                         0, resp->map_id);
    if (coh_rc != GSVA_OK) {
        qemu_log("GSVA_MAP: coh create failed: %s\n",
                 gsva_error_name(coh_rc));
        gsva_route_unmap(&g_gsva_routes, resp->map_id, true);
        resp->error = coh_rc;
        gsva_stats_map(&g_gsva_stats, false);
        return -1;
    }

    /* Register IO memory region so CPU accesses to local_pa are intercepted */
    if (g_sim_decoder && g_sim_decoder->bcs && g_sim_decoder->bcs->ubc_dev) {
        GsvaRouteEntry *route = NULL;
        QTAILQ_FOREACH(route, &g_gsva_routes.routes, next) {
            if (route->map_id == resp->map_id)
                break;
        }
        if (route && route->local_pa && req->key.size) {
            memory_region_init_io(&route->cpu_window,
                                  OBJECT(DEVICE(g_sim_decoder->bcs->ubc_dev)),
                                  &sim_dec_gsva_cpu_window_ops, route,
                                  "gsva-cpu-window", req->key.size);
            memory_region_add_subregion_overlap(get_system_memory(),
                                                route->local_pa,
                                                &route->cpu_window, 10);
            route->cpu_window_mapped = true;
            qemu_log("GSVA_MAP: cpu_window registered at pa=%" PRIx64
                     " size=%" PRIx64 "\n", route->local_pa, req->key.size);
        }
    }

    gsva_stats_map(&g_gsva_stats, true);
    sim_dec_flush_gva_tlbs("gsva_map");
    return 0;
}

/* GSVA UNMAP handler */
static int sim_dec_handle_gsva_unmap(const SimDecGsvaUnmapReq *req,
                                     SimDecGsvaUnmapResp *resp)
{
    memset(resp, 0, sizeof(*resp));

    if (!req) {
        resp->error = GSVA_ERR_BAD_VERSION;
        return -1;
    }

    if (req->version != 1) {
        qemu_log("GSVA_UNMAP: bad version %" PRIu32 "\n", req->version);
        resp->error = GSVA_ERR_BAD_VERSION;
        return -1;
    }

    gsva_tables_init();

    /* Retire coherence object first */
    GsvaRouteEntry *route = NULL;
    QTAILQ_FOREACH(route, &g_gsva_routes.routes, next) {
        if (route->map_id == req->map_id) {
            break;
        }
    }
    if (route) {
        int coh_rc = gsva_coh_retire(&g_gsva_coh, &route->key,
                                      0 /* requester_cna */);
        if (coh_rc != GSVA_OK) {
            qemu_log("GSVA_UNMAP: coh retire failed: %s\n",
                     gsva_error_name(coh_rc));
        }

        /* PA-MESI fence before route removal (V1 sim: best-effort) */
        if (coh_rc == GSVA_OK && g_sim_decoder && g_sim_decoder->bcs &&
            g_sim_decoder->bcs->ubc_dev) {
            int fence_rc = obmm_coh_send_fence(
                g_sim_decoder->bcs->ubc_dev,
                route->home_cna,
                route->key.home_va,
                route->key.size,
                route->token.token_id);
            if (fence_rc != 0) {
                qemu_log("GSVA_UNMAP: PA-MESI fence failed: %d\n", fence_rc);
            } else {
                obmm_coh_invalidate_local_range(
                    g_sim_decoder->bcs->ubc_dev,
                    route->home_cna,
                    route->key.home_va,
                    route->key.size,
                    route->token.token_id);
                qemu_log("GSVA_UNMAP: PA-MESI fence+invalidate done"
                         " segment_id=%#" PRIx64 "\n",
                         route->key.segment_id);
            }
        }
    }

    /* Remove IO memory region before route cleanup */
    if (route && route->cpu_window_mapped) {
        ubc_cpu_window_detach(&route->cpu_window);
        object_unparent(OBJECT(&route->cpu_window));
        route->cpu_window_mapped = false;
        qemu_log("GSVA_UNMAP: cpu_window removed from pa=%" PRIx64 "\n",
                 route->local_pa);
    }

    /* Always keep tombstone for GSVA unmap (epoch tracking) */
    resp->error = gsva_route_unmap(&g_gsva_routes, req->map_id, true);

    if (resp->error != GSVA_OK) {
        qemu_log("GSVA_UNMAP: failed: %s\n", gsva_error_name(resp->error));
        gsva_stats_unmap(&g_gsva_stats, false);
        return -1;
    }
    gsva_stats_unmap(&g_gsva_stats, true);
    sim_dec_flush_gva_tlbs("gsva_unmap");
    return 0;
}

/*
 * ubc_handle_sim_dec_message - Handle incoming SIM_DEC control message
 * This is called from the control channel/message handler
 */
int ubc_handle_sim_dec_message(const uint8_t *data, uint32_t len,
                                uint8_t *resp, uint32_t *resp_len)
{
    const SimDecMsgHdr *hdr;
    uint32_t min_len;
    int ret = 0;

    if (len < sizeof(*hdr)) {
        qemu_log("SIM_DEC: message too short\n");
        return -1;
    }

    hdr = (const SimDecMsgHdr *)data;

    if (hdr->version != SIM_DEC_PROTO_VERSION) {
        qemu_log("SIM_DEC: unsupported version %u\n", hdr->version);
        return -1;
    }

    /* Build response header */
    SimDecMsgHdr *resp_hdr = (SimDecMsgHdr *)resp;
    resp_hdr->version = SIM_DEC_PROTO_VERSION;
    resp_hdr->opcode = hdr->opcode;
    resp_hdr->seq = hdr->seq;
    resp_hdr->status = SIM_DEC_STATUS_SUCCESS;
    resp_hdr->payload_len = 0;

    switch (hdr->opcode) {
    case SIM_DEC_OP_MAP:
        min_len = sizeof(*hdr) + sizeof(SimDecMapReq);
        if (len < min_len) {
            resp_hdr->status = SIM_DEC_STATUS_INVALID_PARAM;
            break;
        }
        {
            SimDecMapResp map_resp = {0};
            (void)sim_dec_handle_map((const SimDecMapReq *)(data + sizeof(*hdr)),
                                     &map_resp);
            resp_hdr->payload_len = sizeof(map_resp);
            memcpy(resp + sizeof(*resp_hdr), &map_resp, sizeof(map_resp));
            resp_hdr->status = map_resp.status;
        }
        break;

    case SIM_DEC_OP_GVA_MAP:
        min_len = sizeof(*hdr) + sizeof(SimDecGvaMapReq);
        if (len < min_len) {
            resp_hdr->status = SIM_DEC_STATUS_INVALID_PARAM;
            break;
        }
        {
            SimDecMapResp map_resp = {0};
            (void)sim_dec_handle_gva_map(
                (const SimDecGvaMapReq *)(data + sizeof(*hdr)),
                &map_resp);
            resp_hdr->payload_len = sizeof(map_resp);
            memcpy(resp + sizeof(*resp_hdr), &map_resp, sizeof(map_resp));
            resp_hdr->status = map_resp.status;
        }
        break;

    case SIM_DEC_OP_UNMAP:
        min_len = sizeof(*hdr) + sizeof(SimDecUnmapReq);
        if (len < min_len) {
            resp_hdr->status = SIM_DEC_STATUS_INVALID_PARAM;
            break;
        }
        resp_hdr->status = sim_dec_handle_unmap(
            (const SimDecUnmapReq *)(data + sizeof(*hdr)));
        resp_hdr->payload_len = 0;
        ret = 0;
        break;

    case SIM_DEC_OP_SYNC:
        min_len = sizeof(*hdr) + sizeof(SimDecSyncReq);
        if (len < min_len) {
            resp_hdr->status = SIM_DEC_STATUS_INVALID_PARAM;
            break;
        }
        resp_hdr->status = sim_dec_handle_sync(
            (const SimDecSyncReq *)(data + sizeof(*hdr)));
        resp_hdr->payload_len = 0;
        ret = 0;
        break;

    case SIM_DEC_OP_QUERY:
        min_len = sizeof(*hdr) + sizeof(SimDecQueryReq);
        if (len < min_len) {
            resp_hdr->status = SIM_DEC_STATUS_INVALID_PARAM;
            break;
        }
        {
            SimDecQueryResp query_resp = {0};
            (void)sim_dec_handle_query(
                (const SimDecQueryReq *)(data + sizeof(*hdr)), &query_resp);
            resp_hdr->payload_len = sizeof(query_resp);
            memcpy(resp + sizeof(*resp_hdr), &query_resp, sizeof(query_resp));
            resp_hdr->status = query_resp.status ? SIM_DEC_STATUS_INVALID_PARAM
                                                  : SIM_DEC_STATUS_SUCCESS;
        }
        break;

    case SIM_DEC_OP_COH_FENCE:
        min_len = sizeof(*hdr) + sizeof(SimDecSyncReq);
        if (len < min_len) {
            resp_hdr->status = SIM_DEC_STATUS_INVALID_PARAM;
            break;
        }
        resp_hdr->status = sim_dec_handle_sync(
            (const SimDecSyncReq *)(data + sizeof(*hdr)));
        resp_hdr->payload_len = 0;
        ret = 0;
        break;

    case SIM_DEC_OP_OBMM_BOOTSTRAP_PUBLISH:
        min_len = sizeof(*hdr) + sizeof(SimDecObmmBootstrapPublishReq);
        if (len < min_len) {
            resp_hdr->status = SIM_DEC_STATUS_INVALID_PARAM;
            break;
        }
        resp_hdr->status = sim_dec_handle_obmm_bootstrap_publish(
            (const SimDecObmmBootstrapPublishReq *)(data + sizeof(*hdr)));
        resp_hdr->payload_len = 0;
        break;

    case SIM_DEC_OP_OBMM_BOOTSTRAP_LOOKUP:
        min_len = sizeof(*hdr) + sizeof(SimDecObmmBootstrapLookupReq);
        if (len < min_len) {
            resp_hdr->status = SIM_DEC_STATUS_INVALID_PARAM;
            break;
        }
        {
            SimDecObmmBootstrapLookupResp lookup_resp = {0};
            resp_hdr->status = sim_dec_handle_obmm_bootstrap_lookup(
                (const SimDecObmmBootstrapLookupReq *)(data + sizeof(*hdr)),
                &lookup_resp);
            resp_hdr->payload_len = sizeof(lookup_resp);
            memcpy(resp + sizeof(*resp_hdr), &lookup_resp, sizeof(lookup_resp));
        }
        break;

    case SIM_DEC_OP_GSVA_QUERY_V1:
        min_len = sizeof(*hdr) + sizeof(SimDecGsvaQueryReq);
        if (len < min_len) {
            resp_hdr->status = SIM_DEC_STATUS_INVALID_PARAM;
            break;
        }
        {
            SimDecGsvaQueryResp gsva_resp = {0};
            if (g_sim_decoder && g_sim_decoder->bcs &&
                g_sim_decoder->bcs->ubc_dev) {
                obmm_coh_poll_rx_links(g_sim_decoder->bcs->ubc_dev);
                ubc_sim_dec_process_wait_links(g_sim_decoder->bcs,
                                               g_sim_decoder->bcs->ubc_dev,
                                               NULL);
            }
            (void)sim_dec_handle_gsva_query(
                (const SimDecGsvaQueryReq *)(data + sizeof(*hdr)),
                &gsva_resp);
            resp_hdr->payload_len = sizeof(gsva_resp);
            memcpy(resp + sizeof(*resp_hdr), &gsva_resp, sizeof(gsva_resp));
            resp_hdr->status = SIM_DEC_STATUS_SUCCESS;
        }
        break;

    case SIM_DEC_OP_GSVA_MAP_V1:
        min_len = sizeof(*hdr) + sizeof(SimDecGsvaMapReq);
        if (len < min_len) {
            resp_hdr->status = SIM_DEC_STATUS_INVALID_PARAM;
            break;
        }
        {
            SimDecGsvaMapResp gsva_resp = {0};
            int gerr = sim_dec_handle_gsva_map(
                (const SimDecGsvaMapReq *)(data + sizeof(*hdr)),
                &gsva_resp);
            resp_hdr->payload_len = sizeof(gsva_resp);
            memcpy(resp + sizeof(*resp_hdr), &gsva_resp, sizeof(gsva_resp));
            resp_hdr->status = (gerr == 0) ? SIM_DEC_STATUS_SUCCESS
                                           : SIM_DEC_STATUS_BACKEND_ERROR;
        }
        break;

    case SIM_DEC_OP_GSVA_UNMAP_V1:
        min_len = sizeof(*hdr) + sizeof(SimDecGsvaUnmapReq);
        if (len < min_len) {
            resp_hdr->status = SIM_DEC_STATUS_INVALID_PARAM;
            break;
        }
        {
            SimDecGsvaUnmapResp gsva_resp = {0};
            int gerr = sim_dec_handle_gsva_unmap(
                (const SimDecGsvaUnmapReq *)(data + sizeof(*hdr)),
                &gsva_resp);
            resp_hdr->payload_len = sizeof(gsva_resp);
            memcpy(resp + sizeof(*resp_hdr), &gsva_resp, sizeof(gsva_resp));
            resp_hdr->status = (gerr == 0) ? SIM_DEC_STATUS_SUCCESS
                                           : SIM_DEC_STATUS_BACKEND_ERROR;
        }
        break;

    case SIM_DEC_OP_GSVA_EVENT_V1: {
        min_len = sizeof(*hdr) + sizeof(uint32_t) * 4 + sizeof(GsvaKeyV1);
        if (len < min_len) {
            resp_hdr->status = SIM_DEC_STATUS_INVALID_PARAM;
            break;
        }
        /* payload: [uint32_t sub_op] [uint32_t requester_cna]
         *          [uint32_t token_id] [uint32_t token_value]
         *          [GsvaKeyV1 key] */
        const uint32_t *ev_payload = (const uint32_t *)(data + sizeof(*hdr));
        uint32_t sub_op = ev_payload[0];
        uint32_t requester_cna = ev_payload[1];
        uint32_t token_id = ev_payload[2];
        uint32_t token_value = ev_payload[3];
        const GsvaKeyV1 *ev_key = (const GsvaKeyV1 *)(ev_payload + 4);

        gsva_tables_init();

        int ev_rc;
        switch (sub_op) {
        case 1: /* ReadAcquire */
            ev_rc = gsva_coh_read_acquire_tx(&g_gsva_coh, &g_gsva_routes,
                                             g_sim_decoder &&
                                             g_sim_decoder->bcs ?
                                             g_sim_decoder->bcs->ubc_dev : NULL,
                                             ev_key, requester_cna,
                                             token_id, token_value);
            gsva_stats_read_acquire(&g_gsva_stats, ev_rc == GSVA_OK);
            break;
        case 2: /* WriteAcquire */
            ev_rc = gsva_coh_write_acquire_tx(&g_gsva_coh, &g_gsva_routes,
                                              g_sim_decoder &&
                                              g_sim_decoder->bcs ?
                                              g_sim_decoder->bcs->ubc_dev : NULL,
                                              ev_key, requester_cna,
                                              token_id, token_value);
            gsva_stats_write_acquire(&g_gsva_stats, ev_rc == GSVA_OK);
            break;
        case 3: /* Retire */
        {
            GsvaRouteEntry *route =
                gsva_route_lookup_base(&g_gsva_routes, ev_key);
            if (!route) {
                ev_rc = gsva_route_lookup_tombstone(&g_gsva_routes, ev_key) ?
                        GSVA_ERR_SEGMENT_RETIRED : GSVA_ERR_ROUTE_MISSING;
            } else {
                SimDecGsvaUnmapReq unmap_req = {0};
                SimDecGsvaUnmapResp unmap_resp = {0};
                ev_rc = gsva_coh_retire_tx(&g_gsva_coh,
                                           g_sim_decoder &&
                                           g_sim_decoder->bcs ?
                                           g_sim_decoder->bcs->ubc_dev : NULL,
                                           ev_key, requester_cna);
                if (ev_rc == GSVA_ERR_COH_PENDING) {
                    break;
                }
                if (ev_rc != GSVA_OK &&
                    ev_rc != GSVA_ERR_SEGMENT_RETIRED) {
                    break;
                }
                unmap_req.version = 1;
                unmap_req.key = *ev_key;
                unmap_req.map_id = route->map_id;
                (void)sim_dec_handle_gsva_unmap(&unmap_req, &unmap_resp);
                ev_rc = unmap_resp.error;
            }
            gsva_stats_retire(&g_gsva_stats, ev_rc == GSVA_OK);
            break;
        }
        case 4: /* InvAck */
            if (token_value != 0) {
                ev_rc = gsva_route_ack_token_revoke(&g_gsva_routes, ev_key,
                                                    token_id, token_value,
                                                    requester_cna);
                if (ev_rc == GSVA_OK) {
                    gsva_tlb_stable_flush_key(ev_key, "token_revoke_ack");
                }
            } else {
                ev_rc = gsva_coh_inv_ack(&g_gsva_coh, ev_key, requester_cna,
                                         token_id /* reuse as seq */);
                if (ev_rc == GSVA_OK) {
                    gsva_tlb_stable_flush_key(ev_key, "coh_inv_ack");
                }
            }
            break;
        case 5: /* Retry */
            if (g_sim_decoder && g_sim_decoder->bcs &&
                g_sim_decoder->bcs->ubc_dev) {
                obmm_coh_poll_rx_links(g_sim_decoder->bcs->ubc_dev);
                ubc_sim_dec_process_wait_links(g_sim_decoder->bcs,
                                               g_sim_decoder->bcs->ubc_dev,
                                               NULL);
            }
            ev_rc = gsva_coh_retry(&g_gsva_coh, ev_key,
                                    token_id /* reuse as seq */);
            break;
        case 6: /* TokenChange */
            ev_rc = gsva_route_rotate_token(&g_gsva_routes, ev_key,
                                            token_id, token_value);
            if (ev_rc == GSVA_OK) {
                gsva_tlb_stable_flush_key(ev_key, "token_revoke_pending");
                (void)gsva_coh_token_revoke_tx(&g_gsva_coh,
                                               g_sim_decoder &&
                                               g_sim_decoder->bcs ?
                                               g_sim_decoder->bcs->ubc_dev : NULL,
                                               ev_key, requester_cna,
                                               token_id, token_value);
            }
            break;
        case 7: /* Fence */
            ev_rc = gsva_coh_fence_tx(&g_gsva_coh,
                                      g_sim_decoder &&
                                      g_sim_decoder->bcs ?
                                      g_sim_decoder->bcs->ubc_dev : NULL,
                                      ev_key, requester_cna);
            break;
        default:
            ev_rc = GSVA_ERR_BAD_VERSION;
            break;
        }

        if (ev_rc == GSVA_ERR_COH_TIMEOUT) {
            gsva_tlb_stable_flush_key(ev_key, "coh_timeout");
        }

        resp_hdr->status = SIM_DEC_STATUS_SUCCESS;
        resp_hdr->payload_len = sizeof(uint32_t);
        uint32_t *resp_payload32 = (uint32_t *)(resp + sizeof(*resp_hdr));
        resp_payload32[0] = (uint32_t)ev_rc;
        break;
    }

    default:
        qemu_log("SIM_DEC: unknown opcode %u\n", hdr->opcode);
        resp_hdr->status = SIM_DEC_STATUS_NOT_SUPPORTED;
        break;
    }

    *resp_len = sizeof(*resp_hdr) + resp_hdr->payload_len;
    return ret;
}

/* ------------------------------------------------------------------ */
/* Device-side GSVA access wrappers (used by NPU, SSD, and future     */
/* UB-attached devices). These wrappers encapsulate route lookup,      */
/* token validation, coherence acquire, PA computation, and            */
/* address_space read/write.                                           */
/* ------------------------------------------------------------------ */

static bool ubc_gsva_device_access_allows(uint32_t access_flags,
                                          uint32_t required_access)
{
    if (access_flags & ~UB_GSVA_DEVICE_ACCESS_READ_WRITE) {
        return false;
    }

    return (access_flags & required_access) == required_access;
}

static MemTxResult ubc_gsva_route_backing_read(BusControllerDev *ubc,
                                               GsvaRouteEntry *route,
                                               uint64_t gsva,
                                               uint64_t pa,
                                               void *dst,
                                               uint64_t len)
{
    ObmmExportEntry *exp;
    bool restore_cpu_window;
    MemTxResult ret;

    if (route && route->home_cna == ubc->parent.cna) {
        ret = ubc_dma_read_local_data_tid_strict(
            ubc, gsva, dst, len, ubc_tid_or_auto(route->token.token_id));
        if (ret == MEMTX_OK) {
            return ret;
        }
    } else if (route && route->home_cna != 0) {
        ret = ubc_sim_dec_remote_read(ubc, gsva, route->token.token_id,
                                      route->home_cna, dst, (uint32_t)len);
        if (ret == MEMTX_OK) {
            return ret;
        }
    }

    exp = obmm_export_lookup(gsva, len);
    if (exp && exp->backing_uba) {
        uint64_t offset = gsva - exp->remote_uba;
        uint64_t backing_addr = exp->backing_uba + offset;

        ret = ubc_dma_read_local_data_tid_strict(ubc, backing_addr, dst, len,
                                                 UBC_DMA_TID_AUTO);
        if (ret == MEMTX_OK) {
            return ret;
        }
        ret = address_space_read(&address_space_memory, backing_addr,
                                 MEMTXATTRS_UNSPECIFIED, dst, len);
        if (ret == MEMTX_OK) {
            return ret;
        }
    }

    restore_cpu_window = route && route->cpu_window_mapped &&
                         ubc_cpu_window_detach(&route->cpu_window);
    ret = address_space_read(&address_space_memory, pa,
                             MEMTXATTRS_UNSPECIFIED, dst, len);
    if (restore_cpu_window) {
        memory_region_add_subregion_overlap(get_system_memory(),
                                            route->local_pa,
                                            &route->cpu_window, 10);
    }
    return ret;
}

static MemTxResult ubc_gsva_route_backing_write(BusControllerDev *ubc,
                                                GsvaRouteEntry *route,
                                                uint64_t gsva,
                                                uint64_t pa,
                                                const void *src,
                                                uint64_t len)
{
    ObmmExportEntry *exp;
    bool restore_cpu_window;
    MemTxResult ret;

    if (route && route->home_cna == ubc->parent.cna) {
        ret = ubc_dma_write_local_data_tid_strict(
            ubc, gsva, src, len, ubc_tid_or_auto(route->token.token_id));
        if (ret == MEMTX_OK) {
            return ret;
        }
    } else if (route && route->home_cna != 0) {
        ret = ubc_sim_dec_remote_write(ubc, gsva, route->token.token_id,
                                       route->home_cna, src, (uint32_t)len);
        if (ret == MEMTX_OK) {
            return ret;
        }
    }

    exp = obmm_export_lookup(gsva, len);
    if (exp && exp->backing_uba) {
        uint64_t offset = gsva - exp->remote_uba;
        uint64_t backing_addr = exp->backing_uba + offset;

        ret = ubc_dma_write_local_data_tid_strict(ubc, backing_addr, src, len,
                                                  UBC_DMA_TID_AUTO);
        if (ret == MEMTX_OK) {
            return ret;
        }
        ret = address_space_write(&address_space_memory, backing_addr,
                                  MEMTXATTRS_UNSPECIFIED, src, len);
        if (ret == MEMTX_OK) {
            return ret;
        }
    }

    restore_cpu_window = route && route->cpu_window_mapped &&
                         ubc_cpu_window_detach(&route->cpu_window);
    ret = address_space_write(&address_space_memory, pa,
                              MEMTXATTRS_UNSPECIFIED, src, len);
    if (restore_cpu_window) {
        memory_region_add_subregion_overlap(get_system_memory(),
                                            route->local_pa,
                                            &route->cpu_window, 10);
    }
    return ret;
}

int ubc_gsva_device_read_acquire(BusControllerDev *ubc,
                                 const GsvaKeyV1 *key,
                                 uint32_t requester_cna,
                                 uint64_t access_va,
                                 uint64_t access_len,
                                 uint32_t access_flags,
                                 uint32_t cmd_token_id,
                                 uint32_t cmd_token_value,
                                 uint64_t *pending_seq)
{
    GsvaRouteEntry *route;
    GsvaCohObject *coh_obj;
    int rc;

    if (!ubc || !key) {
        return GSVA_ERR_ROUTE_MISSING;
    }
    if (!ubc_gsva_device_access_allows(access_flags,
                                       UB_GSVA_DEVICE_ACCESS_READ)) {
        qemu_log("UB_DEV_GSVA: ReadAcquire denied by access_flags=%#" PRIx32
                 " key.segment=%#" PRIx64 " cna=%#" PRIx32 "\n",
                 access_flags, key->segment_id, requester_cna);
        return GSVA_ERR_UNSUPPORTED_POLICY;
    }

    gsva_tables_init();

    route = gsva_route_lookup_base(&g_gsva_routes, key);
    if (!route) {
        if (gsva_route_lookup_tombstone(&g_gsva_routes, key)) {
            return GSVA_ERR_SEGMENT_RETIRED;
        }
        return GSVA_ERR_ROUTE_MISSING;
    }
    if (route->state == GSVA_ROUTE_RETIRED) {
        return GSVA_ERR_SEGMENT_RETIRED;
    }
    coh_obj = gsva_coh_lookup(&g_gsva_coh, key);
    if (coh_obj && (coh_obj->state == GSVA_COH_RETIRED ||
                    (coh_obj->pending && coh_obj->pending_op == 3))) {
        return GSVA_ERR_SEGMENT_RETIRED;
    }

    rc = gsva_route_validate_token(route, requester_cna,
                                   cmd_token_id,
                                   cmd_token_value,
                                   0 /* read */);
    if (rc != GSVA_OK) {
        return rc;
    }

    if (!gsva_key_contains(&route->key, access_va, access_len)) {
        return GSVA_ERR_KEY_MISMATCH;
    }

    rc = gsva_coh_read_acquire_tx(&g_gsva_coh, &g_gsva_routes,
                                  ubc, key, requester_cna,
                                  cmd_token_id,
                                  cmd_token_value);
    if (rc == GSVA_ERR_COH_PENDING && pending_seq) {
        GsvaCohObject *obj = gsva_coh_lookup(&g_gsva_coh, key);
        *pending_seq = obj ? obj->pending_seq : 0;
    }

    if (rc == GSVA_OK) {
        qemu_log("UB_DEV_GSVA: ReadAcquire ok key.segment=%#" PRIx64
                 " cna=%#" PRIx32 " va=%#" PRIx64 " len=%#" PRIx64
                 " access_flags=%#" PRIx32 "\n",
                 key->segment_id, requester_cna, access_va, access_len,
                 access_flags);
    }

    return rc;
}

int ubc_gsva_device_write_acquire(BusControllerDev *ubc,
                                  const GsvaKeyV1 *key,
                                  uint32_t requester_cna,
                                  uint64_t access_va,
                                  uint64_t access_len,
                                  uint32_t access_flags,
                                  uint32_t cmd_token_id,
                                  uint32_t cmd_token_value,
                                  uint64_t *pending_seq)
{
    GsvaRouteEntry *route;
    GsvaCohObject *coh_obj;
    int rc;

    if (!ubc || !key) {
        return GSVA_ERR_ROUTE_MISSING;
    }
    if (!ubc_gsva_device_access_allows(access_flags,
                                       UB_GSVA_DEVICE_ACCESS_WRITE)) {
        qemu_log("UB_DEV_GSVA: WriteAcquire denied by access_flags=%#" PRIx32
                 " key.segment=%#" PRIx64 " cna=%#" PRIx32 "\n",
                 access_flags, key->segment_id, requester_cna);
        return GSVA_ERR_UNSUPPORTED_POLICY;
    }

    gsva_tables_init();

    route = gsva_route_lookup_base(&g_gsva_routes, key);
    if (!route) {
        if (gsva_route_lookup_tombstone(&g_gsva_routes, key)) {
            return GSVA_ERR_SEGMENT_RETIRED;
        }
        return GSVA_ERR_ROUTE_MISSING;
    }
    if (route->state == GSVA_ROUTE_RETIRED) {
        return GSVA_ERR_SEGMENT_RETIRED;
    }
    coh_obj = gsva_coh_lookup(&g_gsva_coh, key);
    if (coh_obj && (coh_obj->state == GSVA_COH_RETIRED ||
                    (coh_obj->pending && coh_obj->pending_op == 3))) {
        return GSVA_ERR_SEGMENT_RETIRED;
    }

    rc = gsva_route_validate_token(route, requester_cna,
                                   cmd_token_id,
                                   cmd_token_value,
                                   1 /* write */);
    if (rc != GSVA_OK) {
        return rc;
    }

    if (!gsva_key_contains(&route->key, access_va, access_len)) {
        return GSVA_ERR_KEY_MISMATCH;
    }

    rc = gsva_coh_write_acquire_tx(&g_gsva_coh, &g_gsva_routes,
                                   ubc, key, requester_cna,
                                   cmd_token_id,
                                   cmd_token_value);
    if (rc == GSVA_ERR_COH_PENDING && pending_seq) {
        GsvaCohObject *obj = gsva_coh_lookup(&g_gsva_coh, key);
        *pending_seq = obj ? obj->pending_seq : 0;
    }

    if (rc == GSVA_OK) {
        qemu_log("UB_DEV_GSVA: WriteAcquire ok key.segment=%#" PRIx64
                 " cna=%#" PRIx32 " va=%#" PRIx64 " len=%#" PRIx64
                 " access_flags=%#" PRIx32 "\n",
                 key->segment_id, requester_cna, access_va, access_len,
                 access_flags);
    }

    return rc;
}

int ubc_gsva_device_read(BusControllerDev *ubc,
                         const GsvaKeyV1 *key,
                         uint32_t requester_cna,
                         uint64_t gsva,
                         void *dst,
                         uint64_t len)
{
    GsvaRouteEntry *route;
    uint64_t offset, pa;
    MemTxResult ret;

    if (!ubc || !key || !dst || len == 0) {
        return GSVA_ERR_ROUTE_MISSING;
    }

    gsva_tables_init();

    route = gsva_route_lookup_base(&g_gsva_routes, key);
    if (!route || route->local_va == 0 || route->local_pa == 0) {
        return GSVA_ERR_ROUTE_MISSING;
    }

    if (!gsva_key_contains(&route->key, gsva, len)) {
        return GSVA_ERR_KEY_MISMATCH;
    }

    offset = gsva - route->local_va;
    pa = route->local_pa + offset;

    ret = ubc_gsva_route_backing_read(ubc, route, gsva, pa, dst, len);
    if (ret != MEMTX_OK) {
        qemu_log("UB_DEV_GSVA: read failed pa=%#" PRIx64 " len=%#" PRIx64
                 " ret=%d\n", pa, len, ret);
        return GSVA_ERR_ROUTE_MISSING;
    }

    qemu_log("UB_DEV_GSVA: read ok pa=%#" PRIx64 " len=%#" PRIx64
             " cna=%#" PRIx32 "\n", pa, len, requester_cna);
    return GSVA_OK;
}

int ubc_gsva_device_write(BusControllerDev *ubc,
                          const GsvaKeyV1 *key,
                          uint32_t requester_cna,
                          uint64_t gsva,
                          const void *src,
                          uint64_t len)
{
    GsvaRouteEntry *route;
    uint64_t offset, pa;
    MemTxResult ret;

    if (!ubc || !key || !src || len == 0) {
        return GSVA_ERR_ROUTE_MISSING;
    }

    gsva_tables_init();

    route = gsva_route_lookup_base(&g_gsva_routes, key);
    if (!route || route->local_va == 0 || route->local_pa == 0) {
        return GSVA_ERR_ROUTE_MISSING;
    }

    if (!gsva_key_contains(&route->key, gsva, len)) {
        return GSVA_ERR_KEY_MISMATCH;
    }

    offset = gsva - route->local_va;
    pa = route->local_pa + offset;

    ret = ubc_gsva_route_backing_write(ubc, route, gsva, pa, src, len);
    if (ret != MEMTX_OK) {
        qemu_log("UB_DEV_GSVA: write failed pa=%#" PRIx64 " len=%#" PRIx64
                 " ret=%d\n", pa, len, ret);
        return GSVA_ERR_ROUTE_MISSING;
    }

    qemu_log("UB_DEV_GSVA: write ok pa=%#" PRIx64 " len=%#" PRIx64
             " cna=%#" PRIx32 "\n", pa, len, requester_cna);
    return GSVA_OK;
}

int ubc_gsva_device_fence(BusControllerDev *ubc,
                          const GsvaKeyV1 *key,
                          uint32_t requester_cna,
                          uint64_t gsva,
                          uint64_t len)
{
    if (!ubc || !key) {
        return GSVA_ERR_ROUTE_MISSING;
    }

    gsva_tables_init();

    int rc = gsva_coh_fence_tx(&g_gsva_coh, ubc, key, requester_cna);
    if (rc != GSVA_OK) {
        qemu_log("UB_DEV_GSVA: fence failed key.segment=%#" PRIx64
                 " rc=%d\n", key->segment_id, rc);
    } else {
        qemu_log("UB_DEV_GSVA: fence ok key.segment=%#" PRIx64
                 " cna=%#" PRIx32 "\n", key->segment_id, requester_cna);
    }

    return rc;
}

static void ub_bus_controller_register_types(void)
{
    type_register_static(&ub_bus_controller_type_info);
    type_register_static(&ub_bus_controller_dev_type_info);
}
type_init(ub_bus_controller_register_types)

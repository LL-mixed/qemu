/*
 * UB-Attached SSD simulation device.
 *
 * V1: Semantic block storage endpoint with local MMIO command submission,
 * bottom-half execution, GSVA-coherent data access, and in-memory backend.
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/ub/ub_ubc.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "exec/address-spaces.h"
#include "qapi/qmp/qdict.h"
#include "qapi/qmp/qlist.h"
#include "qapi/qmp/qnum.h"
#include "qapi/qmp/qjson.h"
#include "qapi/qmp/qstring.h"

/* ------------------------------------------------------------------ */
/* SSD status bits                                                     */
/* ------------------------------------------------------------------ */

#define SSD_STATUS_READY            (1u << 0)
#define SSD_STATUS_BUSY             (1u << 1)
#define SSD_STATUS_COMPLETION_VALID (1u << 2)
#define SSD_STATUS_ERROR            (1u << 3)

/* ------------------------------------------------------------------ */
/* SSD opcodes                                                         */
/* ------------------------------------------------------------------ */

#define SSD_OP_BLOCK_WRITE    1
#define SSD_OP_BLOCK_READ     2
#define SSD_OP_BLOCK_SEAL     3
#define SSD_OP_BLOCK_TOMBSTONE 4
#define SSD_OP_FLUSH          5
#define SSD_OP_STAT           6
#define SSD_OP_EXPORT_SNAPSHOT 7
#define SSD_OP_IMPORT_SNAPSHOT 8

/* ------------------------------------------------------------------ */
/* SSD completion status codes                                         */
/* ------------------------------------------------------------------ */

#define SSD_OK                    0
#define SSD_ERR_BAD_VERSION       (-1)
#define SSD_ERR_BAD_OPCODE        (-2)
#define SSD_ERR_BAD_BLOCK         (-3)
#define SSD_ERR_BAD_DESCRIPTOR    (-4)
#define SSD_ERR_TOKEN_DENIED      (-5)
#define SSD_ERR_STALE_EPOCH       (-6)
#define SSD_ERR_SEGMENT_RETIRED   (-7)
#define SSD_ERR_COH_TIMEOUT       (-8)
#define SSD_ERR_DEVICE_BUSY       (-9)
#define SSD_ERR_CHECKSUM          (-10)
#define SSD_ERR_VERSION_CONFLICT  (-11)
#define SSD_ERR_SEALED            (-12)
#define SSD_ERR_TOMBSTONED        (-13)
#define SSD_ERR_BACKEND_IO        (-14)
#define SSD_ERR_BAD_SNAPSHOT      (-15)

/*
 * Internal-only status between op helpers and the executor.
 * SSD ABI status values intentionally overlap with GSVA internal error
 * numbers, so pending must use a private code before MMIO completion.
 */
#define SSD_INTERNAL_COH_PENDING  (-1006)

/* ------------------------------------------------------------------ */
/* Durable state for block records                                     */
/* ------------------------------------------------------------------ */

typedef enum UbSsdDurableState {
    UB_SSD_DURABLE_COMMITTED  = 0,
    UB_SSD_DURABLE_SEALED     = 1,
    UB_SSD_DURABLE_TOMBSTONED = 2,
    UB_SSD_DURABLE_QUARANTINED = 3,
} UbSsdDurableState;

/* ------------------------------------------------------------------ */
/* MMIO layout (4 KiB page)                                           */
/* ------------------------------------------------------------------ */

#define SSD_MMIO_SIZE        0x1000
#define SSD_CMD_SLOT_OFF     0x000
#define SSD_CMD_SLOT_SIZE    0x400
#define SSD_CPL_SLOT_OFF     0x400
#define SSD_CPL_SLOT_SIZE    0x100
#define SSD_CNA_OFF          0x500
#define SSD_STATUS_OFF       0x508
#define SSD_ERROR_OFF        0x50c
#define SSD_DOORBELL_OFF     0x510
#define SSD_CLEAR_CPL_OFF    0x514
#define SSD_LAST_REQ_ID_OFF  0x518
#define SSD_STATS_OFF        0x520
#define SSD_STATS_SIZE       0x090
#define SSD_BACKEND_PROFILE_OFF 0x5a0

/* ------------------------------------------------------------------ */
/* SSD block ref (matches design doc ub_ssd_block_ref_v1)             */
/* ------------------------------------------------------------------ */

typedef struct QEMU_PACKED UbSsdBlockRefV1 {
    uint64_t block_hi;
    uint64_t block_lo;
    uint64_t version;
    uint64_t offset;
    uint64_t bytes;
    uint64_t checksum64;
} UbSsdBlockRefV1;

/* ------------------------------------------------------------------ */
/* SSD buffer descriptor (matches design doc ub_ssd_buffer_desc_v1)   */
/* ------------------------------------------------------------------ */

typedef struct QEMU_PACKED UbSsdBufferDescV1 {
    uint64_t gsva_base;
    uint64_t bytes;
    GsvaKeyV1 key;
    uint32_t token_id;
    uint32_t token_value;
} UbSsdBufferDescV1;

/* ------------------------------------------------------------------ */
/* SSD command (matches design doc ub_ssd_cmd_v1)                      */
/* ------------------------------------------------------------------ */

typedef struct QEMU_PACKED UbSsdCmdV1 {
    uint32_t version;
    uint32_t opcode;
    uint64_t req_id;
    uint32_t source_cna;
    uint32_t target_ssd_cna;
    uint32_t flags;
    UbSsdBlockRefV1 block_ref;
    UbSsdBufferDescV1 buffer;
} UbSsdCmdV1;

/* ------------------------------------------------------------------ */
/* SSD completion (matches design doc ub_ssd_cpl_v1)                   */
/* ------------------------------------------------------------------ */

typedef struct QEMU_PACKED UbSsdCplV1 {
    uint32_t version;
    uint32_t status;
    uint64_t req_id;
    UbSsdBlockRefV1 committed_ref;
    uint64_t bytes_read;
    uint64_t bytes_written;
    uint64_t checksum64;
    uint64_t error_detail;
} UbSsdCplV1;

/* Compile-time layout checks: must match guest UAPI struct sizes */
QEMU_BUILD_BUG_ON(sizeof(UbSsdBufferDescV1) != 96);
QEMU_BUILD_BUG_ON(sizeof(UbSsdBlockRefV1) != 48);
QEMU_BUILD_BUG_ON(sizeof(UbSsdCmdV1) != 172);
QEMU_BUILD_BUG_ON(sizeof(UbSsdCplV1) != 96);

/* ------------------------------------------------------------------ */
/* Memory backend structures                                           */
/* ------------------------------------------------------------------ */

typedef struct UbSsdBlockRecord {
    uint64_t version;
    UbSsdDurableState durable_state;
    uint8_t *bytes;
    uint64_t byte_count;
    uint64_t checksum64;
    uint32_t writer_cna;
    uint32_t metadata_flags;
} UbSsdBlockRecord;

typedef struct UbSsdBlockChain {
    uint64_t block_hi;
    uint64_t block_lo;
    UbSsdBlockRecord *versions;
    uint32_t version_count;
    uint32_t version_capacity;
} UbSsdBlockChain;

/* ------------------------------------------------------------------ */
/* SSD stats                                                           */
/* ------------------------------------------------------------------ */

#define SSD_STATS_COUNT 19

typedef struct UbSsdStats {
    uint64_t cmd_total;
    uint64_t cmd_completed;
    uint64_t cmd_failed;
    uint64_t block_write;
    uint64_t block_read;
    uint64_t block_seal;
    uint64_t block_tombstone;
    uint64_t flush;
    uint64_t stat;
    uint64_t bytes_read_from_gsva;
    uint64_t bytes_written_to_gsva;
    uint64_t bytes_written_to_backend;
    uint64_t bytes_read_from_backend;
    uint64_t token_denied;
    uint64_t stale_epoch;
    uint64_t retired_segment;
    uint64_t version_conflict;
    uint64_t coh_timeout;
    uint64_t checksum_error;
} UbSsdStats;

/* ------------------------------------------------------------------ */
/* Execution state machine                                              */
/* ------------------------------------------------------------------ */

enum ub_ssd_exec_phase {
    SSD_PHASE_IDLE = 0,
    SSD_PHASE_EXECUTING,
    SSD_PHASE_PENDING_RETRY,
};

/* ------------------------------------------------------------------ */
/* Backend profile                                                     */
/* ------------------------------------------------------------------ */

#define UB_SSD_BACKEND_PROFILE_MEMORY "memory"

static uint32_t ub_ssd_parse_backend_profile(const char *profile)
{
    if (!profile || profile[0] == '\0' ||
        g_strcmp0(profile, UB_SSD_BACKEND_PROFILE_MEMORY) == 0) {
        return 0;
    }

    if (g_ascii_strcasecmp(profile, UB_SSD_BACKEND_PROFILE_MEMORY) == 0) {
        return 0;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Device state                                                        */
/* ------------------------------------------------------------------ */

#define TYPE_UB_SSD "ub-ssd"
OBJECT_DECLARE_SIMPLE_TYPE(UbSsdState, UB_SSD)

struct UbSsdState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    BusControllerDev *ubc;

    uint32_t device_cna;
    uint32_t node_id;
    uint32_t instance_id;

    /* Command / completion slots */
    UbSsdCmdV1 cmd;
    UbSsdCplV1 cpl;

    /* Status register */
    uint32_t status;
    uint32_t error_reg;
    uint64_t last_req_id;

    /* Execution */
    QEMUBH *bh;
    QEMUTimer *poll_timer;
    bool poll_timer_active;
    uint64_t pending_seq;
    int pending_acquire_rc;
    enum ub_ssd_exec_phase exec_phase;

    /* Memory backend */
    GHashTable *backend;
    uint32_t backend_profile;
    char *backend_profile_name;

    /* Stats */
    UbSsdStats stats;
};

/* ------------------------------------------------------------------ */
/* Forward declarations                                                */
/* ------------------------------------------------------------------ */

static void ub_ssd_bh(void *opaque);
static void ub_ssd_poll_timer_cb(void *opaque);

/* ------------------------------------------------------------------ */
/* Memory backend helpers                                              */
/* ------------------------------------------------------------------ */

static guint ub_ssd_block_key_hash(gconstpointer ptr)
{
    const UbSsdBlockRefV1 *ref = ptr;
    return (guint)(ref->block_hi ^ (ref->block_lo >> 32) ^ ref->block_lo);
}

static gboolean ub_ssd_block_key_equal(gconstpointer a, gconstpointer b)
{
    const UbSsdBlockRefV1 *ra = a;
    const UbSsdBlockRefV1 *rb = b;
    return ra->block_hi == rb->block_hi && ra->block_lo == rb->block_lo;
}

static UbSsdBlockChain *ub_ssd_find_chain(UbSsdState *s,
                                           uint64_t block_hi,
                                           uint64_t block_lo)
{
    UbSsdBlockRefV1 key = { .block_hi = block_hi, .block_lo = block_lo };
    return g_hash_table_lookup(s->backend, &key);
}

static UbSsdBlockRecord *ub_ssd_latest_record(UbSsdBlockChain *chain)
{
    if (!chain || chain->version_count == 0) {
        return NULL;
    }
    return &chain->versions[chain->version_count - 1];
}

static uint64_t ub_ssd_checksum64(const uint8_t *data, uint64_t len)
{
    uint64_t sum = 0;
    uint64_t i;
    for (i = 0; i + 8 <= len; i += 8) {
        uint64_t val;
        memcpy(&val, data + i, 8);
        sum += val;
    }
    return sum;
}

static UbSsdBlockChain *ub_ssd_create_chain(UbSsdState *s,
                                             uint64_t block_hi,
                                             uint64_t block_lo)
{
    UbSsdBlockRefV1 *key = g_new(UbSsdBlockRefV1, 1);
    UbSsdBlockChain *chain = g_new0(UbSsdBlockChain, 1);

    key->block_hi = block_hi;
    key->block_lo = block_lo;
    chain->block_hi = block_hi;
    chain->block_lo = block_lo;
    chain->version_capacity = 4;
    chain->versions = g_new0(UbSsdBlockRecord, chain->version_capacity);
    chain->version_count = 0;

    g_hash_table_insert(s->backend, key, chain);
    return chain;
}

static int ub_ssd_append_version(UbSsdBlockChain *chain,
                                  const uint8_t *data, uint64_t len,
                                  uint64_t version,
                                  UbSsdDurableState state,
                                  uint32_t writer_cna,
                                  uint32_t metadata_flags)
{
    if (chain->version_count >= chain->version_capacity) {
        chain->version_capacity *= 2;
        chain->versions = g_renew(UbSsdBlockRecord, chain->versions,
                                  chain->version_capacity);
    }

    UbSsdBlockRecord *rec = &chain->versions[chain->version_count];
    rec->version = version;
    rec->durable_state = state;
    rec->byte_count = len;
    rec->checksum64 = ub_ssd_checksum64(data, len);
    rec->writer_cna = writer_cna;
    rec->metadata_flags = metadata_flags;

    if (len > 0) {
        rec->bytes = g_malloc(len);
        memcpy(rec->bytes, data, len);
    } else {
        rec->bytes = NULL;
    }

    chain->version_count++;
    return 0;
}

static void ub_ssd_block_record_free(UbSsdBlockRecord *rec)
{
    if (rec->bytes) {
        g_free(rec->bytes);
        rec->bytes = NULL;
    }
}

static void ub_ssd_block_chain_free(void *ptr)
{
    UbSsdBlockChain *chain = ptr;
    if (!chain) {
        return;
    }
    for (uint32_t i = 0; i < chain->version_count; i++) {
        ub_ssd_block_record_free(&chain->versions[i]);
    }
    g_free(chain->versions);
    g_free(chain);
}

static void ub_ssd_block_key_free(void *ptr)
{
    g_free(ptr);
}

static char *ub_ssd_bytes_to_hex(const uint8_t *data, uint64_t len)
{
    GString *hex;
    uint64_t i;

    hex = g_string_new(NULL);
    for (i = 0; i < len; i++) {
        g_string_append_printf(hex, "%02x", data[i]);
    }
    return g_string_free(hex, false);
}

static int ub_ssd_hex_to_nibble(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + ch - 'a';
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + ch - 'A';
    }
    return -1;
}

static bool ub_ssd_hex_to_bytes(const char *hex, uint8_t *out, uint64_t out_len)
{
    uint64_t i;
    size_t hex_len;

    if (!hex) {
        return false;
    }

    hex_len = strlen(hex);
    if (hex_len != out_len * 2) {
        return false;
    }

    for (i = 0; i < out_len; i++) {
        int hi = ub_ssd_hex_to_nibble(hex[2 * i]);
        int lo = ub_ssd_hex_to_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static bool ub_ssd_get_u64_from_dict(const QDict *dict, const char *key,
                                    uint64_t *value)
{
    QObject *obj = qdict_get(dict, key);
    QNum *num = qobject_to(QNum, obj);
    if (!num) {
        return false;
    }
    return qnum_get_try_uint(num, value);
}

static bool ub_ssd_get_u32_from_dict(const QDict *dict, const char *key,
                                    uint32_t *value)
{
    uint64_t tmp;
    if (!ub_ssd_get_u64_from_dict(dict, key, &tmp)) {
        return false;
    }
    if (tmp > UINT32_MAX) {
        return false;
    }
    *value = (uint32_t)tmp;
    return true;
}

static UbSsdBlockChain *ub_ssd_create_chain_in_table(GHashTable *backend,
                                                    uint64_t block_hi,
                                                    uint64_t block_lo)
{
    UbSsdBlockRefV1 *key = g_new(UbSsdBlockRefV1, 1);
    UbSsdBlockChain *chain = g_new0(UbSsdBlockChain, 1);

    key->block_hi = block_hi;
    key->block_lo = block_lo;
    chain->block_hi = block_hi;
    chain->block_lo = block_lo;
    chain->version_capacity = 4;
    chain->versions = g_new0(UbSsdBlockRecord, chain->version_capacity);
    chain->version_count = 0;

    g_hash_table_insert(backend, key, chain);
    return chain;
}

static QDict *ub_ssd_record_to_dict(const UbSsdBlockRecord *rec)
{
    QDict *obj = qdict_new();
    char *bytes_hex = ub_ssd_bytes_to_hex(rec->bytes ? rec->bytes : (const uint8_t *)"",
                                          rec->byte_count);

    qdict_put_obj(obj, "version",
                  QOBJECT(qnum_from_uint(rec->version)));
    qdict_put_obj(obj, "durable_state",
                  QOBJECT(qnum_from_uint(rec->durable_state)));
    qdict_put_obj(obj, "byte_count",
                  QOBJECT(qnum_from_uint(rec->byte_count)));
    qdict_put_obj(obj, "checksum64",
                  QOBJECT(qnum_from_uint(rec->checksum64)));
    qdict_put_obj(obj, "writer_cna",
                  QOBJECT(qnum_from_uint(rec->writer_cna)));
    qdict_put_obj(obj, "metadata_flags",
                  QOBJECT(qnum_from_uint(rec->metadata_flags)));
    qdict_put_str(obj, "bytes", bytes_hex ? bytes_hex : "");
    g_free(bytes_hex);
    return obj;
}

static GString *ub_ssd_export_snapshot_json(UbSsdState *s)
{
    QDict *root = qdict_new();
    GHashTableIter iter;
    gpointer key;
    gpointer value;
    QList *blocks = qlist_new();
    GString *json;

    g_hash_table_iter_init(&iter, s->backend);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        UbSsdBlockRefV1 *chain_key = key;
        UbSsdBlockChain *chain = value;
        QList *versions = qlist_new();
        QDict *block = qdict_new();
        uint32_t idx;

        qdict_put_obj(block, "block_hi",
                      QOBJECT(qnum_from_uint(chain_key->block_hi)));
        qdict_put_obj(block, "block_lo",
                      QOBJECT(qnum_from_uint(chain_key->block_lo)));

        for (idx = 0; idx < chain->version_count; idx++) {
            UbSsdBlockRecord *rec = &chain->versions[idx];
            QDict *ver_obj = ub_ssd_record_to_dict(rec);
            qlist_append_obj(versions, QOBJECT(ver_obj));
        }

        qdict_put_obj(block, "versions", QOBJECT(versions));
        qlist_append_obj(blocks, QOBJECT(block));
    }

    qdict_put_obj(root, "snapshot_version",
                  QOBJECT(qnum_from_uint(1)));
    qdict_put_obj(root, "blocks", QOBJECT(blocks));
    json = qobject_to_json(QOBJECT(root));
    qobject_unref(root);
    return json;
}

static GHashTable *ub_ssd_parse_snapshot_json_to_backend(const uint8_t *data,
                                                        uint64_t len,
                                                        int *err_rc)
{
    const QListEntry *block_entry;
    GHashTable *backend = NULL;
    QList *blocks;
    QDict *root;
    QObject *obj;
    Error *err = NULL;
    char *text = NULL;
    uint64_t snapshot_version = 0;
    if (!data || len == 0) {
        *err_rc = SSD_ERR_BAD_SNAPSHOT;
        return NULL;
    }

    if (len > SSIZE_MAX) {
        *err_rc = SSD_ERR_BAD_SNAPSHOT;
        return NULL;
    }

    text = g_strndup((const char *)data, (size_t)len);
    obj = qobject_from_json(text, &err);
    g_free(text);
    if (!obj) {
        error_free(err);
        *err_rc = SSD_ERR_BAD_SNAPSHOT;
        return NULL;
    }

    root = qobject_to(QDict, obj);
    if (!root) {
        qobject_unref(obj);
        *err_rc = SSD_ERR_BAD_SNAPSHOT;
        return NULL;
    }

    if (!ub_ssd_get_u64_from_dict(root, "snapshot_version", &snapshot_version) ||
        snapshot_version != 1) {
        qobject_unref(obj);
        *err_rc = SSD_ERR_BAD_SNAPSHOT;
        return NULL;
    }

    blocks = qdict_get_qlist(root, "blocks");
    if (!blocks) {
        qobject_unref(obj);
        *err_rc = SSD_ERR_BAD_SNAPSHOT;
        return NULL;
    }

    backend = g_hash_table_new_full(ub_ssd_block_key_hash,
                                   ub_ssd_block_key_equal,
                                   ub_ssd_block_key_free,
                                   ub_ssd_block_chain_free);

    for (block_entry = qlist_first(blocks); block_entry;
         block_entry = qlist_next(block_entry)) {
        const QListEntry *version_entry;
        QDict *block_dict = qobject_to(QDict,
                                       qlist_entry_obj(block_entry));
        QList *versions;
        UbSsdBlockChain *chain = NULL;
        uint64_t block_hi = 0;
        uint64_t block_lo = 0;
        uint64_t expected_version = 0;
        bool first_version = true;

        if (!block_dict) {
            goto parse_fail;
        }

        if (!ub_ssd_get_u64_from_dict(block_dict, "block_hi", &block_hi) ||
            !ub_ssd_get_u64_from_dict(block_dict, "block_lo", &block_lo)) {
            goto parse_fail;
        }

        if (g_hash_table_lookup(backend, &(UbSsdBlockRefV1){ .block_hi = block_hi,
                                                            .block_lo = block_lo})) {
            goto parse_fail;
        }

        versions = qdict_get_qlist(block_dict, "versions");
        if (!versions) {
            goto parse_fail;
        }

        chain = ub_ssd_create_chain_in_table(backend, block_hi, block_lo);
        for (version_entry = qlist_first(versions);
             version_entry;
             version_entry = qlist_next(version_entry)) {
            QDict *version_dict = qobject_to(QDict,
                                            qlist_entry_obj(version_entry));
            uint64_t rec_version = 0;
            uint64_t rec_state = 0;
            uint64_t rec_byte_count = 0;
            uint64_t rec_checksum = 0;
            uint32_t writer_cna = 0;
            uint32_t metadata_flags = 0;
            const char *bytes_hex = NULL;
            uint8_t *bytes = NULL;
            UbSsdDurableState durable_state;

            if (!version_dict) {
                goto parse_fail;
            }

            if (!ub_ssd_get_u64_from_dict(version_dict, "version", &rec_version) ||
                (first_version ? (rec_version != 1) :
                                 (rec_version != expected_version + 1)) ||
                !ub_ssd_get_u64_from_dict(version_dict, "durable_state", &rec_state) ||
                rec_state > UB_SSD_DURABLE_QUARANTINED ||
                !ub_ssd_get_u64_from_dict(version_dict, "byte_count", &rec_byte_count) ||
                !ub_ssd_get_u64_from_dict(version_dict, "checksum64", &rec_checksum) ||
                !ub_ssd_get_u32_from_dict(version_dict, "writer_cna", &writer_cna) ||
                !ub_ssd_get_u32_from_dict(version_dict, "metadata_flags",
                                          &metadata_flags)) {
                goto parse_fail;
            }

            if (rec_byte_count > 1024 * 1024 * 1024ULL) {
                goto parse_fail;
            }

            bytes_hex = qdict_get_try_str(version_dict, "bytes");
            if (!bytes_hex) {
                goto parse_fail;
            }

            bytes = g_malloc0((size_t)rec_byte_count);
            if (!ub_ssd_hex_to_bytes(bytes_hex, bytes, rec_byte_count)) {
                g_free(bytes);
                goto parse_fail;
            }

            if (ub_ssd_checksum64(bytes, rec_byte_count) != rec_checksum) {
                g_free(bytes);
                goto parse_fail;
            }

            durable_state = (UbSsdDurableState)rec_state;
            if (durable_state > UB_SSD_DURABLE_QUARANTINED) {
                g_free(bytes);
                goto parse_fail;
            }

            if (ub_ssd_append_version(chain, bytes, rec_byte_count, rec_version,
                                      durable_state, writer_cna,
                                      metadata_flags) < 0) {
                g_free(bytes);
                goto parse_fail;
            }
            expected_version = rec_version;
            first_version = false;
            g_free(bytes);
        }
        if (chain->version_count == 0) {
            g_hash_table_remove(backend, &(UbSsdBlockRefV1){ .block_hi = block_hi,
                                                             .block_lo = block_lo});
            goto parse_fail;
        }
    }

    qobject_unref(obj);
    *err_rc = SSD_OK;
    return backend;

parse_fail:
    if (backend) {
        g_hash_table_destroy(backend);
    }
    qobject_unref(obj);
    *err_rc = SSD_ERR_BAD_SNAPSHOT;
    return NULL;
}

static int ub_ssd_apply_snapshot_import(UbSsdState *s, const uint8_t *data,
                                       uint64_t len)
{
    int rc = SSD_OK;
    GHashTable *new_backend;

    new_backend = ub_ssd_parse_snapshot_json_to_backend(data, len, &rc);
    if (!new_backend) {
        return rc;
    }

    g_hash_table_destroy(s->backend);
    s->backend = new_backend;
    return SSD_OK;
}

static int ub_ssd_load_u8_buffer_via_gsva(UbSsdState *s, const UbSsdBufferDescV1 *buf,
                                          void *out, uint64_t len)
{
    int rc;
    rc = ubc_gsva_device_read_acquire(s->ubc, &buf->key, s->device_cna,
                                      buf->gsva_base, buf->bytes,
                                      UB_GSVA_DEVICE_ACCESS_READ,
                                      buf->token_id, buf->token_value,
                                      &s->pending_seq);
    if (rc == GSVA_ERR_TOKEN_DENIED) {
        return SSD_ERR_TOKEN_DENIED;
    }
    if (rc == GSVA_ERR_STALE_EPOCH) {
        s->stats.stale_epoch++;
        return SSD_ERR_STALE_EPOCH;
    }
    if (rc == GSVA_ERR_SEGMENT_RETIRED) {
        s->stats.retired_segment++;
        return SSD_ERR_SEGMENT_RETIRED;
    }
    if (rc == GSVA_ERR_COH_PENDING) {
        return SSD_INTERNAL_COH_PENDING;
    }
    if (rc != GSVA_OK) {
        return SSD_ERR_BAD_DESCRIPTOR;
    }

    rc = ubc_gsva_device_read(s->ubc, &buf->key, s->device_cna,
                              buf->gsva_base, out, len);
    if (rc != GSVA_OK) {
        return SSD_ERR_BAD_DESCRIPTOR;
    }
    return SSD_OK;
}

static int ub_ssd_store_u8_buffer_via_gsva(UbSsdState *s, const UbSsdBufferDescV1 *buf,
                                           const void *in, uint64_t len)
{
    int rc;
    rc = ubc_gsva_device_write_acquire(s->ubc, &buf->key, s->device_cna,
                                       buf->gsva_base, buf->bytes,
                                       UB_GSVA_DEVICE_ACCESS_WRITE,
                                       buf->token_id, buf->token_value,
                                       &s->pending_seq);
    if (rc == GSVA_ERR_TOKEN_DENIED) {
        return SSD_ERR_TOKEN_DENIED;
    }
    if (rc == GSVA_ERR_STALE_EPOCH) {
        s->stats.stale_epoch++;
        return SSD_ERR_STALE_EPOCH;
    }
    if (rc == GSVA_ERR_SEGMENT_RETIRED) {
        s->stats.retired_segment++;
        return SSD_ERR_SEGMENT_RETIRED;
    }
    if (rc == GSVA_ERR_COH_PENDING) {
        return SSD_INTERNAL_COH_PENDING;
    }
    if (rc != GSVA_OK) {
        return SSD_ERR_BAD_DESCRIPTOR;
    }

    rc = ubc_gsva_device_write(s->ubc, &buf->key, s->device_cna,
                               buf->gsva_base, in, len);
    if (rc != GSVA_OK) {
        return SSD_ERR_BAD_DESCRIPTOR;
    }

    ubc_gsva_device_fence(s->ubc, &buf->key, s->device_cna,
                          buf->gsva_base, len);
    return SSD_OK;
}

static int ub_ssd_op_export_snapshot(UbSsdState *s, UbSsdCmdV1 *cmd)
{
    const UbSsdBufferDescV1 *buf = &cmd->buffer;
    GString *json;
    uint64_t snapshot_len;
    int rc;

    if (buf->bytes == 0) {
        return SSD_ERR_BAD_SNAPSHOT;
    }
    if (buf->bytes > SSIZE_MAX) {
        return SSD_ERR_BAD_SNAPSHOT;
    }

    json = ub_ssd_export_snapshot_json(s);
    if (!json) {
        return SSD_ERR_BAD_DESCRIPTOR;
    }

    snapshot_len = (uint64_t)json->len;
    if (snapshot_len > buf->bytes) {
        g_string_free(json, true);
        return SSD_ERR_BAD_SNAPSHOT;
    }

    rc = ub_ssd_store_u8_buffer_via_gsva(s, buf, json->str, snapshot_len);
    g_string_free(json, true);
    if (rc != SSD_OK) {
        return rc;
    }

    s->cpl.bytes_written = snapshot_len;
    return SSD_OK;
}

static int ub_ssd_op_import_snapshot(UbSsdState *s, UbSsdCmdV1 *cmd)
{
    const UbSsdBufferDescV1 *buf = &cmd->buffer;
    void *snapshot = NULL;
    int rc;

    if (buf->bytes == 0) {
        return SSD_ERR_BAD_SNAPSHOT;
    }
    if (buf->bytes > SSIZE_MAX) {
        return SSD_ERR_BAD_SNAPSHOT;
    }

    snapshot = g_malloc0((size_t)buf->bytes);
    if (!snapshot) {
        return SSD_ERR_BACKEND_IO;
    }

    rc = ub_ssd_load_u8_buffer_via_gsva(s, buf, snapshot, buf->bytes);
    if (rc != SSD_OK) {
        g_free(snapshot);
        return rc;
    }

    rc = ub_ssd_apply_snapshot_import(s, snapshot, buf->bytes);
    g_free(snapshot);
    if (rc != SSD_OK) {
        return rc;
    }

    s->cpl.bytes_read = buf->bytes;
    return SSD_OK;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static const char *ssd_opcode_name(uint32_t opcode)
{
    switch (opcode) {
    case SSD_OP_BLOCK_WRITE:    return "BLOCK_WRITE";
    case SSD_OP_BLOCK_READ:     return "BLOCK_READ";
    case SSD_OP_BLOCK_SEAL:     return "BLOCK_SEAL";
    case SSD_OP_BLOCK_TOMBSTONE: return "BLOCK_TOMBSTONE";
    case SSD_OP_FLUSH:          return "FLUSH";
    case SSD_OP_STAT:           return "STAT";
    case SSD_OP_EXPORT_SNAPSHOT: return "EXPORT_SNAPSHOT";
    case SSD_OP_IMPORT_SNAPSHOT: return "IMPORT_SNAPSHOT";
    default:                    return "UNKNOWN";
    }
}

static void ub_ssd_complete_command(UbSsdState *s, uint32_t status)
{
    s->cpl.version = 1;
    s->cpl.status = status;
    s->last_req_id = s->cmd.req_id;

    s->status &= ~SSD_STATUS_BUSY;
    if (status == SSD_OK) {
        s->stats.cmd_completed++;
    } else {
        s->stats.cmd_failed++;
        if (status == SSD_ERR_VERSION_CONFLICT) {
            s->stats.version_conflict++;
        }
        s->status |= SSD_STATUS_ERROR;
        s->error_reg = (uint32_t)(-status);
    }
    s->status |= SSD_STATUS_COMPLETION_VALID;

    qemu_log("UB_SSD_CPL: req_id=%#" PRIx64 " status=%" PRId32
             " opcode=%s bytes_read=%#" PRIx64
             " bytes_written=%#" PRIx64 "\n",
             s->cpl.req_id, status,
             ssd_opcode_name(s->cmd.opcode),
             s->cpl.bytes_read, s->cpl.bytes_written);
}

/* ------------------------------------------------------------------ */
/* Opcode implementations                                              */
/* ------------------------------------------------------------------ */

static int ub_ssd_op_block_write(UbSsdState *s, UbSsdCmdV1 *cmd)
{
    const UbSsdBufferDescV1 *buf = &cmd->buffer;
    const UbSsdBlockRefV1 *ref = &cmd->block_ref;
    UbSsdBlockChain *chain;
    UbSsdBlockRecord *latest;
    uint8_t *data;
    uint64_t data_len = buf->bytes;
    uint64_t data_csum;
    int rc;

    if (ref->block_hi == 0 && ref->block_lo == 0) {
        return SSD_ERR_BAD_BLOCK;
    }

    rc = ubc_gsva_device_read_acquire(s->ubc, &buf->key, s->device_cna,
                                       buf->gsva_base, buf->bytes,
                                       UB_GSVA_DEVICE_ACCESS_READ,
                                       buf->token_id, buf->token_value,
                                       &s->pending_seq);
    if (rc == GSVA_ERR_TOKEN_DENIED) return SSD_ERR_TOKEN_DENIED;
    if (rc == GSVA_ERR_STALE_EPOCH) {
        s->stats.stale_epoch++;
        return SSD_ERR_STALE_EPOCH;
    }
    if (rc == GSVA_ERR_SEGMENT_RETIRED) {
        s->stats.retired_segment++;
        return SSD_ERR_SEGMENT_RETIRED;
    }
    if (rc == GSVA_ERR_COH_TIMEOUT) {
        s->stats.coh_timeout++;
        return SSD_ERR_COH_TIMEOUT;
    }
    if (rc == GSVA_ERR_COH_PENDING) return SSD_INTERNAL_COH_PENDING;
    if (rc != GSVA_OK) return SSD_ERR_BAD_DESCRIPTOR;

    data = g_malloc(data_len);
    rc = ubc_gsva_device_read(s->ubc, &buf->key, s->device_cna,
                               buf->gsva_base, data, data_len);
    if (rc != GSVA_OK) {
        g_free(data);
        return SSD_ERR_BAD_DESCRIPTOR;
    }
    s->stats.bytes_read_from_gsva += data_len;
    data_csum = ub_ssd_checksum64(data, data_len);

    chain = ub_ssd_find_chain(s, ref->block_hi, ref->block_lo);
    latest = ub_ssd_latest_record(chain);

    if (ref->version == 0) {
        if (latest) {
            g_free(data);
            return SSD_ERR_VERSION_CONFLICT;
        }
        chain = ub_ssd_create_chain(s, ref->block_hi, ref->block_lo);
        ub_ssd_append_version(chain, data, data_len, 1,
                              UB_SSD_DURABLE_COMMITTED, cmd->source_cna, 0);
        s->cpl.committed_ref = *ref;
        s->cpl.committed_ref.version = 1;
        s->cpl.committed_ref.offset = 0;
        s->cpl.committed_ref.bytes = data_len;
        s->cpl.committed_ref.checksum64 = data_csum;
    } else {
        if (!chain || !latest) {
            g_free(data);
            return SSD_ERR_BAD_BLOCK;
        }
        if (latest->durable_state == UB_SSD_DURABLE_SEALED) {
            g_free(data);
            return SSD_ERR_SEALED;
        }
        if (latest->durable_state == UB_SSD_DURABLE_TOMBSTONED) {
            g_free(data);
            return SSD_ERR_TOMBSTONED;
        }
        if (latest->version != ref->version) {
            g_free(data);
            return SSD_ERR_VERSION_CONFLICT;
        }
        uint64_t new_version = latest->version + 1;
        ub_ssd_append_version(chain, data, data_len, new_version,
                              UB_SSD_DURABLE_COMMITTED, cmd->source_cna, 0);
        s->cpl.committed_ref = *ref;
        s->cpl.committed_ref.version = new_version;
        s->cpl.committed_ref.offset = 0;
        s->cpl.committed_ref.bytes = data_len;
        s->cpl.committed_ref.checksum64 = data_csum;
    }

    s->stats.bytes_written_to_backend += data_len;
    s->stats.block_write++;
    s->cpl.bytes_written = data_len;
    s->cpl.checksum64 = data_csum;
    g_free(data);

    return SSD_OK;
}

static int ub_ssd_op_block_read(UbSsdState *s, UbSsdCmdV1 *cmd)
{
    const UbSsdBufferDescV1 *buf = &cmd->buffer;
    const UbSsdBlockRefV1 *ref = &cmd->block_ref;
    UbSsdBlockChain *chain;
    UbSsdBlockRecord *target = NULL;
    int rc;

    if (ref->block_hi == 0 && ref->block_lo == 0) {
        return SSD_ERR_BAD_BLOCK;
    }

    chain = ub_ssd_find_chain(s, ref->block_hi, ref->block_lo);
    if (!chain || chain->version_count == 0) {
        return SSD_ERR_BAD_BLOCK;
    }

    if (ref->version == 0) {
        target = ub_ssd_latest_record(chain);
    } else {
        for (uint32_t i = 0; i < chain->version_count; i++) {
            if (chain->versions[i].version == ref->version) {
                target = &chain->versions[i];
                break;
            }
        }
    }

    if (!target) {
        return SSD_ERR_BAD_BLOCK;
    }
    if (target->durable_state == UB_SSD_DURABLE_TOMBSTONED) {
        return SSD_ERR_TOMBSTONED;
    }

    uint64_t read_len = MIN(target->byte_count, buf->bytes);
    if (ref->offset + read_len > target->byte_count) {
        return SSD_ERR_BAD_BLOCK;
    }

    /* Validate checksum */
    uint64_t actual_csum = ub_ssd_checksum64(target->bytes, target->byte_count);
    if (actual_csum != target->checksum64) {
        s->stats.checksum_error++;
        return SSD_ERR_CHECKSUM;
    }
    if (ref->checksum64 != 0 && ref->checksum64 != actual_csum) {
        s->stats.checksum_error++;
        return SSD_ERR_CHECKSUM;
    }

    rc = ubc_gsva_device_write_acquire(s->ubc, &buf->key, s->device_cna,
                                        buf->gsva_base, buf->bytes,
                                        UB_GSVA_DEVICE_ACCESS_WRITE,
                                        buf->token_id, buf->token_value,
                                        &s->pending_seq);
    if (rc == GSVA_ERR_TOKEN_DENIED) return SSD_ERR_TOKEN_DENIED;
    if (rc == GSVA_ERR_STALE_EPOCH) {
        s->stats.stale_epoch++;
        return SSD_ERR_STALE_EPOCH;
    }
    if (rc == GSVA_ERR_SEGMENT_RETIRED) {
        s->stats.retired_segment++;
        return SSD_ERR_SEGMENT_RETIRED;
    }
    if (rc == GSVA_ERR_COH_TIMEOUT) {
        s->stats.coh_timeout++;
        return SSD_ERR_COH_TIMEOUT;
    }
    if (rc == GSVA_ERR_COH_PENDING) return SSD_INTERNAL_COH_PENDING;
    if (rc != GSVA_OK) return SSD_ERR_BAD_DESCRIPTOR;

    rc = ubc_gsva_device_write(s->ubc, &buf->key, s->device_cna,
                                buf->gsva_base, target->bytes + ref->offset,
                                read_len);
    if (rc != GSVA_OK) {
        return SSD_ERR_BAD_DESCRIPTOR;
    }

    ubc_gsva_device_fence(s->ubc, &buf->key, s->device_cna,
                          buf->gsva_base, read_len);

    s->stats.bytes_read_from_backend += read_len;
    s->stats.bytes_written_to_gsva += read_len;
    s->stats.block_read++;
    s->cpl.bytes_read = read_len;
    s->cpl.checksum64 = actual_csum;
    s->cpl.committed_ref = *ref;
    s->cpl.committed_ref.version = target->version;
    s->cpl.committed_ref.bytes = read_len;
    s->cpl.committed_ref.checksum64 = actual_csum;

    return SSD_OK;
}

static int ub_ssd_op_block_seal(UbSsdState *s, UbSsdCmdV1 *cmd)
{
    const UbSsdBlockRefV1 *ref = &cmd->block_ref;
    UbSsdBlockChain *chain;
    UbSsdBlockRecord *latest;

    chain = ub_ssd_find_chain(s, ref->block_hi, ref->block_lo);
    if (!chain || chain->version_count == 0) {
        return SSD_ERR_BAD_BLOCK;
    }

    latest = ub_ssd_latest_record(chain);
    if (latest->durable_state == UB_SSD_DURABLE_SEALED) {
        return SSD_ERR_SEALED;
    }
    if (latest->durable_state == UB_SSD_DURABLE_TOMBSTONED) {
        return SSD_ERR_TOMBSTONED;
    }
    if (ref->version != 0 && latest->version != ref->version) {
        return SSD_ERR_VERSION_CONFLICT;
    }

    latest->durable_state = UB_SSD_DURABLE_SEALED;
    s->stats.block_seal++;
    s->cpl.committed_ref.block_hi = ref->block_hi;
    s->cpl.committed_ref.block_lo = ref->block_lo;
    s->cpl.committed_ref.version = latest->version;
    s->cpl.committed_ref.bytes = latest->byte_count;
    s->cpl.committed_ref.checksum64 = latest->checksum64;
    return SSD_OK;
}

static int ub_ssd_op_block_tombstone(UbSsdState *s, UbSsdCmdV1 *cmd)
{
    const UbSsdBlockRefV1 *ref = &cmd->block_ref;
    UbSsdBlockChain *chain;
    UbSsdBlockRecord *latest;

    chain = ub_ssd_find_chain(s, ref->block_hi, ref->block_lo);
    if (!chain || chain->version_count == 0) {
        return SSD_ERR_BAD_BLOCK;
    }

    latest = ub_ssd_latest_record(chain);
    if (latest->durable_state == UB_SSD_DURABLE_TOMBSTONED) {
        return SSD_ERR_TOMBSTONED;
    }
    if (ref->version != 0 && latest->version != ref->version) {
        return SSD_ERR_VERSION_CONFLICT;
    }

    latest->durable_state = UB_SSD_DURABLE_TOMBSTONED;
    s->stats.block_tombstone++;
    s->cpl.committed_ref.block_hi = ref->block_hi;
    s->cpl.committed_ref.block_lo = ref->block_lo;
    s->cpl.committed_ref.version = latest->version;
    s->cpl.committed_ref.bytes = latest->byte_count;
    s->cpl.committed_ref.checksum64 = latest->checksum64;
    return SSD_OK;
}

/* ------------------------------------------------------------------ */
/* Command execution                                                   */
/* ------------------------------------------------------------------ */

static void ub_ssd_execute_command(UbSsdState *s)
{
    UbSsdCmdV1 *cmd = &s->cmd;
    uint32_t opcode = cmd->opcode;
    int rc = SSD_OK;
    bool is_retry = (s->exec_phase == SSD_PHASE_PENDING_RETRY);

    if (!is_retry) {
        qemu_log("UB_SSD_CMD: req_id=%#" PRIx64 " opcode=%s(%" PRIu32 ")"
                 " source_cna=%#" PRIx32
                 " block=%#" PRIx64 ":%#" PRIx64 ":%#" PRIx64 "\n",
                 cmd->req_id, ssd_opcode_name(opcode), opcode,
                 cmd->source_cna,
                 cmd->block_ref.block_hi, cmd->block_ref.block_lo,
                 cmd->block_ref.version);
        s->stats.cmd_total++;

        if (cmd->version != 1) {
            ub_ssd_complete_command(s, SSD_ERR_BAD_VERSION);
            return;
        }

        s->exec_phase = SSD_PHASE_EXECUTING;
    } else {
        qemu_log("UB_SSD_CMD: retry req_id=%#" PRIx64 " opcode=%s(%" PRIu32 ")\n",
                 cmd->req_id, ssd_opcode_name(opcode), opcode);
    }

    switch (opcode) {
    case SSD_OP_BLOCK_WRITE:
        rc = ub_ssd_op_block_write(s, cmd);
        break;
    case SSD_OP_BLOCK_READ:
        rc = ub_ssd_op_block_read(s, cmd);
        break;
    case SSD_OP_BLOCK_SEAL:
        rc = ub_ssd_op_block_seal(s, cmd);
        break;
    case SSD_OP_BLOCK_TOMBSTONE:
        rc = ub_ssd_op_block_tombstone(s, cmd);
        break;
    case SSD_OP_FLUSH:
        if (!is_retry) s->stats.flush++;
        rc = SSD_OK;
        break;
    case SSD_OP_STAT:
        if (!is_retry) s->stats.stat++;
        rc = SSD_OK;
        break;
    case SSD_OP_EXPORT_SNAPSHOT:
        rc = ub_ssd_op_export_snapshot(s, cmd);
        break;
    case SSD_OP_IMPORT_SNAPSHOT:
        rc = ub_ssd_op_import_snapshot(s, cmd);
        break;
    default:
        ub_ssd_complete_command(s, SSD_ERR_BAD_OPCODE);
        return;
    }

    if (rc == SSD_INTERNAL_COH_PENDING) {
        s->pending_acquire_rc = SSD_INTERNAL_COH_PENDING;
        s->exec_phase = SSD_PHASE_PENDING_RETRY;
        timer_mod(s->poll_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000);
        s->poll_timer_active = true;
        return;
    }

    s->exec_phase = SSD_PHASE_IDLE;
    ub_ssd_complete_command(s, rc);
}

/* ------------------------------------------------------------------ */
/* Bottom half                                                         */
/* ------------------------------------------------------------------ */

static void ub_ssd_bh(void *opaque)
{
    UbSsdState *s = UB_SSD(opaque);

    if (s->pending_acquire_rc == SSD_INTERNAL_COH_PENDING && s->ubc) {
        obmm_coh_poll_rx_links(s->ubc);
        s->pending_acquire_rc = 0;
    }

    ub_ssd_execute_command(s);
}

static void ub_ssd_poll_timer_cb(void *opaque)
{
    UbSsdState *s = UB_SSD(opaque);
    s->poll_timer_active = false;
    qemu_bh_schedule(s->bh);
}

/* ------------------------------------------------------------------ */
/* MMIO read                                                           */
/* ------------------------------------------------------------------ */

static uint64_t ub_ssd_mmio_read(void *opaque, hwaddr offset, unsigned size)
{
    UbSsdState *s = UB_SSD(opaque);

    switch (offset) {
    case SSD_CNA_OFF:
        return s->device_cna;
    case SSD_STATUS_OFF:
        return s->status;
    case SSD_ERROR_OFF:
        return s->error_reg;
    case SSD_LAST_REQ_ID_OFF:
        return s->last_req_id;
    case SSD_BACKEND_PROFILE_OFF:
        return s->backend_profile;
    default:
        if (offset >= SSD_CPL_SLOT_OFF &&
            offset < SSD_CPL_SLOT_OFF + SSD_CPL_SLOT_SIZE) {
            uint64_t off = offset - SSD_CPL_SLOT_OFF;
            if (off + size > sizeof(s->cpl)) {
                return 0;
            }
            const uint8_t *p = (const uint8_t *)&s->cpl + off;
            uint64_t val = 0;
            memcpy(&val, p, MIN(size, (unsigned)sizeof(val)));
            return val;
        }
        if (offset >= SSD_STATS_OFF &&
            offset < SSD_STATS_OFF + SSD_STATS_SIZE) {
            uint64_t off = offset - SSD_STATS_OFF;
            if (off + size > sizeof(s->stats)) {
                return 0;
            }
            const uint8_t *p = (const uint8_t *)&s->stats + off;
            uint64_t val = 0;
            memcpy(&val, p, MIN(size, (unsigned)sizeof(val)));
            return val;
        }
        break;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* MMIO write                                                          */
/* ------------------------------------------------------------------ */

static void ub_ssd_mmio_write(void *opaque, hwaddr offset,
                               uint64_t val, unsigned size)
{
    UbSsdState *s = UB_SSD(opaque);

    switch (offset) {
    case SSD_DOORBELL_OFF:
        if (val != 1) {
            return;
        }
        if (s->status & SSD_STATUS_BUSY) {
            qemu_log("UB_SSD: doorbell while BUSY\n");
            if (!(s->status & SSD_STATUS_COMPLETION_VALID)) {
                s->cpl.version = 1;
                s->cpl.status = SSD_ERR_DEVICE_BUSY;
                s->cpl.req_id = s->cmd.req_id;
            }
            s->status |= SSD_STATUS_ERROR;
            s->error_reg = (uint32_t)(-SSD_ERR_DEVICE_BUSY);
            return;
        }
        s->status |= SSD_STATUS_BUSY;
        s->status &= ~(SSD_STATUS_COMPLETION_VALID | SSD_STATUS_ERROR);
        s->error_reg = 0;
        memset(&s->cpl, 0, sizeof(s->cpl));
        s->cpl.req_id = s->cmd.req_id;
        qemu_bh_schedule(s->bh);
        break;

    case SSD_CLEAR_CPL_OFF:
        if (val != 1) {
            return;
        }
        s->status &= ~SSD_STATUS_COMPLETION_VALID;
        break;

    default:
        if (offset >= SSD_CMD_SLOT_OFF &&
            offset < SSD_CMD_SLOT_OFF + SSD_CMD_SLOT_SIZE) {
            if (s->status & (SSD_STATUS_BUSY | SSD_STATUS_COMPLETION_VALID)) {
                qemu_log("UB_SSD: cmd slot write rejected while busy\n");
                return;
            }
            uint64_t off = offset - SSD_CMD_SLOT_OFF;
            if (off + size <= sizeof(s->cmd)) {
                uint8_t *p = (uint8_t *)&s->cmd + off;
                memcpy(p, &val, MIN(size, (unsigned)sizeof(val)));
            }
        }
        break;
    }
}

static const MemoryRegionOps ub_ssd_mmio_ops = {
    .read = ub_ssd_mmio_read,
    .write = ub_ssd_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 8,
};

/* ------------------------------------------------------------------ */
/* Device lifecycle                                                    */
/* ------------------------------------------------------------------ */

static void ub_ssd_realize(DeviceState *dev, Error **errp)
{
    UbSsdState *s = UB_SSD(dev);

    if (!s->ubc) {
        error_setg(errp, "ub-ssd: missing 'ubc' link property");
        return;
    }

    if (s->device_cna == 0) {
        s->device_cna = (s->node_id << 16) | (0x20 << 8) | s->instance_id;
    }

    s->backend = g_hash_table_new_full(ub_ssd_block_key_hash,
                                        ub_ssd_block_key_equal,
                                        ub_ssd_block_key_free,
                                        ub_ssd_block_chain_free);
    s->backend_profile = ub_ssd_parse_backend_profile(s->backend_profile_name);
    if (!s->backend_profile_name) {
        s->backend_profile_name = g_strdup(UB_SSD_BACKEND_PROFILE_MEMORY);
    }

    s->status = SSD_STATUS_READY;
    memset(&s->cmd, 0, sizeof(s->cmd));
    memset(&s->cpl, 0, sizeof(s->cpl));
    memset(&s->stats, 0, sizeof(s->stats));

    qemu_log("UB_SSD: realized cna=%#" PRIx32 " node_id=%" PRIu32
             " instance=%" PRIu32 " backend=memory\n",
             s->device_cna, s->node_id, s->instance_id);
}

static void ub_ssd_init(Object *obj)
{
    UbSsdState *s = UB_SSD(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->mmio, obj, &ub_ssd_mmio_ops,
                          s, "ub-ssd-mmio", SSD_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);

    s->bh = qemu_bh_new(ub_ssd_bh, s);
    s->poll_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                  ub_ssd_poll_timer_cb, s);
}

static void ub_ssd_finalize(Object *obj)
{
    UbSsdState *s = UB_SSD(obj);

    if (s->poll_timer) {
        timer_free(s->poll_timer);
        s->poll_timer = NULL;
    }
    if (s->bh) {
        qemu_bh_delete(s->bh);
        s->bh = NULL;
    }
    if (s->backend) {
        g_hash_table_destroy(s->backend);
        s->backend = NULL;
    }
}

static Property ub_ssd_properties[] = {
    DEFINE_PROP_UINT32("node-id", UbSsdState, node_id, 0),
    DEFINE_PROP_UINT32("instance-id", UbSsdState, instance_id, 0),
    DEFINE_PROP_UINT32("cna", UbSsdState, device_cna, 0),
    DEFINE_PROP_STRING("backend-profile", UbSsdState, backend_profile_name),
    DEFINE_PROP_LINK("ubc", UbSsdState, ubc,
                     TYPE_BUS_CONTROLLER_DEV, BusControllerDev *),
    DEFINE_PROP_END_OF_LIST(),
};

static void ub_ssd_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_props(dc, ub_ssd_properties);
    dc->realize = ub_ssd_realize;
}

static const TypeInfo ub_ssd_type_info = {
    .name = TYPE_UB_SSD,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(UbSsdState),
    .instance_init = ub_ssd_init,
    .instance_finalize = ub_ssd_finalize,
    .class_init = ub_ssd_class_init,
};

static void ub_ssd_register_types(void)
{
    type_register_static(&ub_ssd_type_info);
}
type_init(ub_ssd_register_types)

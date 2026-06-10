/*
 * UB-Attached NPU simulation device.
 *
 * V1: Semantic NPU endpoint with local MMIO command submission,
 * bottom-half execution, and GSVA-coherent data access.
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

/* ------------------------------------------------------------------ */
/* NPU status bits                                                     */
/* ------------------------------------------------------------------ */

#define NPU_STATUS_READY            (1u << 0)
#define NPU_STATUS_BUSY             (1u << 1)
#define NPU_STATUS_COMPLETION_VALID (1u << 2)
#define NPU_STATUS_ERROR            (1u << 3)

/* ------------------------------------------------------------------ */
/* NPU opcodes                                                         */
/* ------------------------------------------------------------------ */

#define NPU_OP_NOOP           0
#define NPU_OP_MEMCOPY        1
#define NPU_OP_FILL           2
#define NPU_OP_VECTOR_ADD_U32 3
#define NPU_OP_CHECKSUM64     4

/* ------------------------------------------------------------------ */
/* NPU completion status codes                                         */
/* ------------------------------------------------------------------ */

#define NPU_OK                    0
#define NPU_ERR_BAD_VERSION       (-1)
#define NPU_ERR_BAD_OPCODE        (-2)
#define NPU_ERR_BAD_DESCRIPTOR    (-3)
#define NPU_ERR_TOKEN_DENIED      (-4)
#define NPU_ERR_STALE_EPOCH       (-5)
#define NPU_ERR_SEGMENT_RETIRED   (-6)
#define NPU_ERR_COH_TIMEOUT       (-7)
#define NPU_ERR_DEVICE_BUSY       (-8)

/*
 * Internal-only status between op helpers and the executor.
 * This must not be exposed in MMIO completions because NPU ABI status
 * values intentionally overlap with GSVA internal error numbers.
 */
#define NPU_INTERNAL_COH_PENDING  (-1006)

/* ------------------------------------------------------------------ */
/* NPU buffer descriptor roles and access                              */
/* ------------------------------------------------------------------ */

#define NPU_BUF_INPUT       0
#define NPU_BUF_WEIGHT      1
#define NPU_BUF_OUTPUT      2
#define NPU_BUF_SCRATCH     3

#define NPU_ACCESS_READ       UB_GSVA_DEVICE_ACCESS_READ
#define NPU_ACCESS_WRITE      UB_GSVA_DEVICE_ACCESS_WRITE
#define NPU_ACCESS_READ_WRITE UB_GSVA_DEVICE_ACCESS_READ_WRITE

/* ------------------------------------------------------------------ */
/* MMIO layout (4 KiB page)                                           */
/* ------------------------------------------------------------------ */

#define NPU_MMIO_SIZE        0x1000
#define NPU_CMD_SLOT_OFF     0x000
#define NPU_CMD_SLOT_SIZE    0x400
#define NPU_CPL_SLOT_OFF     0x400
#define NPU_CPL_SLOT_SIZE    0x100
#define NPU_CNA_OFF          0x500
#define NPU_STATUS_OFF       0x508
#define NPU_ERROR_OFF        0x50c
#define NPU_DOORBELL_OFF     0x510
#define NPU_CLEAR_CPL_OFF    0x514
#define NPU_LAST_REQ_ID_OFF  0x518
#define NPU_STATS_OFF        0x520
#define NPU_STATS_SIZE       0x080

/* ------------------------------------------------------------------ */
/* NPU buffer descriptor (matches design doc ub_npu_buffer_desc_v1)    */
/* ------------------------------------------------------------------ */

typedef struct QEMU_PACKED UbNpuBufferDescV1 {
    uint32_t role;
    uint32_t access;
    uint64_t gsva_base;
    uint64_t bytes;
    GsvaKeyV1 key;
    uint32_t token_id;
    uint32_t token_value;
} UbNpuBufferDescV1;

/* ------------------------------------------------------------------ */
/* NPU command (matches design doc ub_npu_cmd_v1)                      */
/* ------------------------------------------------------------------ */

#define NPU_MAX_DESCS 4

typedef struct QEMU_PACKED UbNpuCmdV1 {
    uint32_t version;
    uint32_t opcode;
    uint64_t req_id;
    uint32_t source_cna;
    uint32_t target_npu_cna;
    uint32_t flags;
    uint32_t desc_count;
    UbNpuBufferDescV1 descs[NPU_MAX_DESCS];
    uint64_t scalar0;
    uint64_t scalar1;
} UbNpuCmdV1;

/* ------------------------------------------------------------------ */
/* NPU completion (matches design doc ub_npu_cpl_v1)                   */
/* ------------------------------------------------------------------ */

typedef struct QEMU_PACKED UbNpuCplV1 {
    uint32_t version;
    uint32_t status;
    uint64_t req_id;
    uint64_t bytes_read;
    uint64_t bytes_written;
    uint64_t checksum64;
    uint64_t error_detail;
} UbNpuCplV1;

/* Compile-time layout checks: must match guest UAPI struct sizes */
QEMU_BUILD_BUG_ON(sizeof(UbNpuBufferDescV1) != 104);
QEMU_BUILD_BUG_ON(sizeof(UbNpuCmdV1) != 464);
QEMU_BUILD_BUG_ON(sizeof(UbNpuCplV1) != 48);

/* ------------------------------------------------------------------ */
/* NPU stats                                                           */
/* ------------------------------------------------------------------ */

#define NPU_STATS_COUNT 13

typedef struct UbNpuStats {
    uint64_t cmd_total;
    uint64_t cmd_completed;
    uint64_t cmd_failed;
    uint64_t opcode_memcopy;
    uint64_t opcode_fill;
    uint64_t opcode_vector_add_u32;
    uint64_t opcode_checksum64;
    uint64_t bytes_read;
    uint64_t bytes_written;
    uint64_t token_denied;
    uint64_t stale_epoch;
    uint64_t retired_segment;
    uint64_t coh_timeout;
} UbNpuStats;

/* ------------------------------------------------------------------ */
/* Execution state machine                                              */
/* ------------------------------------------------------------------ */

enum ub_npu_exec_phase {
    NPU_PHASE_IDLE = 0,
    NPU_PHASE_EXECUTING,
    NPU_PHASE_PENDING_RETRY,
};

/* ------------------------------------------------------------------ */
/* Device state                                                        */
/* ------------------------------------------------------------------ */

#define TYPE_UB_NPU "ub-npu"
OBJECT_DECLARE_SIMPLE_TYPE(UbNpuState, UB_NPU)

struct UbNpuState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    BusControllerDev *ubc;

    uint32_t device_cna;
    uint32_t node_id;
    uint32_t instance_id;

    /* Command / completion slots */
    UbNpuCmdV1 cmd;
    UbNpuCplV1 cpl;

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
    enum ub_npu_exec_phase exec_phase;

    /* Stats */
    UbNpuStats stats;
};

/* ------------------------------------------------------------------ */
/* Forward declarations                                                */
/* ------------------------------------------------------------------ */

static void ub_npu_bh(void *opaque);
static void ub_npu_poll_timer_cb(void *opaque);

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static const char *npu_opcode_name(uint32_t opcode)
{
    switch (opcode) {
    case NPU_OP_NOOP:            return "NOOP";
    case NPU_OP_MEMCOPY:        return "MEMCOPY";
    case NPU_OP_FILL:           return "FILL";
    case NPU_OP_VECTOR_ADD_U32: return "VECTOR_ADD_U32";
    case NPU_OP_CHECKSUM64:     return "CHECKSUM64";
    default:                    return "UNKNOWN";
    }
}

static void ub_npu_complete_command(UbNpuState *s, uint32_t status)
{
    s->cpl.version = 1;
    s->cpl.status = status;
    s->last_req_id = s->cmd.req_id;

    s->status &= ~NPU_STATUS_BUSY;
    if (status == NPU_OK) {
        s->stats.cmd_completed++;
    } else {
        s->stats.cmd_failed++;
        s->status |= NPU_STATUS_ERROR;
        s->error_reg = (uint32_t)(-status);
    }
    s->status |= NPU_STATUS_COMPLETION_VALID;

    qemu_log("UB_NPU_CPL: req_id=%#" PRIx64 " status=%" PRId32
             " opcode=%s bytes_read=%#" PRIx64
             " bytes_written=%#" PRIx64 "\n",
             s->cpl.req_id, status,
             npu_opcode_name(s->cmd.opcode),
             s->cpl.bytes_read, s->cpl.bytes_written);
}

/* ------------------------------------------------------------------ */
/* Descriptor validation helpers                                       */
/* ------------------------------------------------------------------ */

static int ub_npu_validate_desc(const UbNpuBufferDescV1 *desc, uint32_t access)
{
    int key_rc = gsva_key_validate(&desc->key);
    if (key_rc != GSVA_OK) {
        qemu_log("UB_NPU_DESC: invalid key rc=%d segment_id=%#" PRIx64
                 " home_va=%#" PRIx64 " size=%#" PRIx64
                 " flags=%#" PRIx32 " version=%" PRIu32 "\n",
                 key_rc, desc->key.segment_id, desc->key.home_va,
                 desc->key.size, desc->key.flags, desc->key.version);
        return NPU_ERR_BAD_DESCRIPTOR;
    }
    if (!gsva_key_contains(&desc->key, desc->gsva_base, desc->bytes)) {
        qemu_log("UB_NPU_DESC: range outside key segment_id=%#" PRIx64
                 " gsva=%#" PRIx64 " bytes=%#" PRIx64
                 " key.home_va=%#" PRIx64 " key.size=%#" PRIx64 "\n",
                 desc->key.segment_id, desc->gsva_base, desc->bytes,
                 desc->key.home_va, desc->key.size);
        return NPU_ERR_BAD_DESCRIPTOR;
    }
    if (desc->bytes == 0) {
        qemu_log("UB_NPU_DESC: zero bytes segment_id=%#" PRIx64 "\n",
                 desc->key.segment_id);
        return NPU_ERR_BAD_DESCRIPTOR;
    }
    if (desc->access & ~NPU_ACCESS_READ_WRITE) {
        qemu_log("UB_NPU_DESC: invalid access=%#" PRIx32
                 " segment_id=%#" PRIx64 "\n",
                 desc->access, desc->key.segment_id);
        return NPU_ERR_BAD_DESCRIPTOR;
    }
    if ((desc->access & access) == 0) {
        qemu_log("UB_NPU_DESC: access denied access=%#" PRIx32
                 " required=%#" PRIx32 " segment_id=%#" PRIx64 "\n",
                 desc->access, access, desc->key.segment_id);
        return NPU_ERR_BAD_DESCRIPTOR;
    }
    return NPU_OK;
}

static int ub_npu_acquire_read(UbNpuState *s, const UbNpuBufferDescV1 *desc)
{
    int rc = ubc_gsva_device_read_acquire(s->ubc, &desc->key,
                                           s->device_cna,
                                           desc->gsva_base, desc->bytes,
                                           desc->access,
                                           desc->token_id, desc->token_value,
                                           &s->pending_seq);
    if (rc != GSVA_OK) {
        qemu_log("UB_NPU_ACQUIRE: read rc=%d req_id=%#" PRIx64
                 " segment_id=%#" PRIx64 " gsva=%#" PRIx64
                 " len=%#" PRIx64 " token_id=%" PRIu32 "\n",
                 rc, s->cmd.req_id, desc->key.segment_id,
                 desc->gsva_base, desc->bytes, desc->token_id);
    }
    if (rc == GSVA_ERR_TOKEN_DENIED) {
        s->stats.token_denied++;
        return NPU_ERR_TOKEN_DENIED;
    }
    if (rc == GSVA_ERR_STALE_EPOCH) {
        s->stats.stale_epoch++;
        return NPU_ERR_STALE_EPOCH;
    }
    if (rc == GSVA_ERR_SEGMENT_RETIRED) {
        s->stats.retired_segment++;
        return NPU_ERR_SEGMENT_RETIRED;
    }
    if (rc == GSVA_ERR_COH_TIMEOUT) {
        s->stats.coh_timeout++;
        return NPU_ERR_COH_TIMEOUT;
    }
    if (rc == GSVA_ERR_COH_PENDING) {
        return NPU_INTERNAL_COH_PENDING;
    }
    if (rc != GSVA_OK) {
        return NPU_ERR_BAD_DESCRIPTOR;
    }
    return NPU_OK;
}

static int ub_npu_acquire_write(UbNpuState *s, const UbNpuBufferDescV1 *desc)
{
    int rc = ubc_gsva_device_write_acquire(s->ubc, &desc->key,
                                            s->device_cna,
                                            desc->gsva_base, desc->bytes,
                                            desc->access,
                                            desc->token_id, desc->token_value,
                                            &s->pending_seq);
    if (rc != GSVA_OK) {
        qemu_log("UB_NPU_ACQUIRE: write rc=%d req_id=%#" PRIx64
                 " segment_id=%#" PRIx64 " gsva=%#" PRIx64
                 " len=%#" PRIx64 " token_id=%" PRIu32 "\n",
                 rc, s->cmd.req_id, desc->key.segment_id,
                 desc->gsva_base, desc->bytes, desc->token_id);
    }
    if (rc == GSVA_ERR_TOKEN_DENIED) {
        s->stats.token_denied++;
        return NPU_ERR_TOKEN_DENIED;
    }
    if (rc == GSVA_ERR_STALE_EPOCH) {
        s->stats.stale_epoch++;
        return NPU_ERR_STALE_EPOCH;
    }
    if (rc == GSVA_ERR_SEGMENT_RETIRED) {
        s->stats.retired_segment++;
        return NPU_ERR_SEGMENT_RETIRED;
    }
    if (rc == GSVA_ERR_COH_TIMEOUT) {
        s->stats.coh_timeout++;
        return NPU_ERR_COH_TIMEOUT;
    }
    if (rc == GSVA_ERR_COH_PENDING) {
        return NPU_INTERNAL_COH_PENDING;
    }
    if (rc != GSVA_OK) {
        return NPU_ERR_BAD_DESCRIPTOR;
    }
    return NPU_OK;
}

/* ------------------------------------------------------------------ */
/* Opcode implementations                                              */
/* ------------------------------------------------------------------ */

static int ub_npu_op_memcopy(UbNpuState *s, UbNpuCmdV1 *cmd)
{
    const UbNpuBufferDescV1 *input = &cmd->descs[0];
    const UbNpuBufferDescV1 *output = &cmd->descs[1];
    uint64_t copy_len;
    void *tmp;
    int rc;

    if (cmd->desc_count < 2) {
        qemu_log("UB_NPU_DESC: MEMCOPY desc_count=%" PRIu32 " < 2\n",
                 cmd->desc_count);
        return NPU_ERR_BAD_DESCRIPTOR;
    }
    if (input->role != NPU_BUF_INPUT || output->role != NPU_BUF_OUTPUT) {
        qemu_log("UB_NPU_DESC: MEMCOPY role mismatch input=%" PRIu32
                 " output=%" PRIu32 "\n",
                 input->role, output->role);
        return NPU_ERR_BAD_DESCRIPTOR;
    }

    rc = ub_npu_validate_desc(input, NPU_ACCESS_READ);
    if (rc != NPU_OK) return rc;
    rc = ub_npu_validate_desc(output, NPU_ACCESS_WRITE);
    if (rc != NPU_OK) return rc;

    copy_len = MIN(input->bytes, output->bytes);

    rc = ub_npu_acquire_read(s, input);
    if (rc != NPU_OK) return rc;

    tmp = g_malloc(copy_len);
    rc = ubc_gsva_device_read(s->ubc, &input->key, s->device_cna,
                               input->gsva_base, tmp, copy_len);
    if (rc != GSVA_OK) {
        g_free(tmp);
        return NPU_ERR_BAD_DESCRIPTOR;
    }
    s->stats.bytes_read += copy_len;
    s->cpl.bytes_read = copy_len;

    rc = ub_npu_acquire_write(s, output);
    if (rc != NPU_OK) {
        g_free(tmp);
        return rc;
    }

    rc = ubc_gsva_device_write(s->ubc, &output->key, s->device_cna,
                                output->gsva_base, tmp, copy_len);
    g_free(tmp);
    if (rc != GSVA_OK) {
        return NPU_ERR_BAD_DESCRIPTOR;
    }
    s->stats.bytes_written += copy_len;
    s->cpl.bytes_written = copy_len;

    ubc_gsva_device_fence(s->ubc, &output->key, s->device_cna,
                          output->gsva_base, copy_len);

    return NPU_OK;
}

static int ub_npu_op_fill(UbNpuState *s, UbNpuCmdV1 *cmd)
{
    const UbNpuBufferDescV1 *output = &cmd->descs[0];
    uint64_t fill_val = cmd->scalar0;
    uint64_t fill_bytes = output->bytes;
    uint8_t *buf;
    int rc;

    if (cmd->desc_count < 1) {
        qemu_log("UB_NPU_DESC: FILL desc_count=%" PRIu32 " < 1\n",
                 cmd->desc_count);
        return NPU_ERR_BAD_DESCRIPTOR;
    }
    if (output->role != NPU_BUF_OUTPUT) {
        qemu_log("UB_NPU_DESC: FILL role mismatch output=%" PRIu32 "\n",
                 output->role);
        return NPU_ERR_BAD_DESCRIPTOR;
    }

    rc = ub_npu_validate_desc(output, NPU_ACCESS_WRITE);
    if (rc != NPU_OK) return rc;

    rc = ub_npu_acquire_write(s, output);
    if (rc != NPU_OK) return rc;

    buf = g_malloc(fill_bytes);
    for (uint64_t off = 0; off + 8 <= fill_bytes; off += 8) {
        memcpy(buf + off, &fill_val, 8);
    }
    for (uint64_t off = fill_bytes & ~7ULL; off < fill_bytes; off++) {
        buf[off] = (uint8_t)(fill_val & 0xFF);
    }

    rc = ubc_gsva_device_write(s->ubc, &output->key, s->device_cna,
                                output->gsva_base, buf, fill_bytes);
    g_free(buf);
    if (rc != GSVA_OK) {
        return NPU_ERR_BAD_DESCRIPTOR;
    }
    s->stats.bytes_written += fill_bytes;
    s->cpl.bytes_written = fill_bytes;

    ubc_gsva_device_fence(s->ubc, &output->key, s->device_cna,
                          output->gsva_base, fill_bytes);

    return NPU_OK;
}

static int ub_npu_op_vector_add_u32(UbNpuState *s, UbNpuCmdV1 *cmd)
{
    const UbNpuBufferDescV1 *input0 = &cmd->descs[0];
    const UbNpuBufferDescV1 *input1 = &cmd->descs[1];
    const UbNpuBufferDescV1 *output = &cmd->descs[2];
    uint32_t element_count = (uint32_t)cmd->scalar0;
    uint64_t byte_len = (uint64_t)element_count * 4;
    uint32_t *a, *b, *c;
    int rc;

    if (cmd->desc_count < 3) {
        qemu_log("UB_NPU_DESC: VECTOR_ADD desc_count=%" PRIu32 " < 3\n",
                 cmd->desc_count);
        return NPU_ERR_BAD_DESCRIPTOR;
    }
    if (input0->role != NPU_BUF_INPUT || input1->role != NPU_BUF_INPUT ||
        output->role != NPU_BUF_OUTPUT) {
        qemu_log("UB_NPU_DESC: VECTOR_ADD role mismatch input0=%" PRIu32
                 " input1=%" PRIu32 " output=%" PRIu32 "\n",
                 input0->role, input1->role, output->role);
        return NPU_ERR_BAD_DESCRIPTOR;
    }
    if (element_count == 0 || byte_len > input0->bytes ||
        byte_len > input1->bytes || byte_len > output->bytes) {
        qemu_log("UB_NPU_DESC: VECTOR_ADD size mismatch elements=%" PRIu32
                 " byte_len=%#" PRIx64 " in0=%#" PRIx64
                 " in1=%#" PRIx64 " out=%#" PRIx64 "\n",
                 element_count, byte_len, input0->bytes,
                 input1->bytes, output->bytes);
        return NPU_ERR_BAD_DESCRIPTOR;
    }

    rc = ub_npu_validate_desc(input0, NPU_ACCESS_READ);
    if (rc != NPU_OK) return rc;
    rc = ub_npu_validate_desc(input1, NPU_ACCESS_READ);
    if (rc != NPU_OK) return rc;
    rc = ub_npu_validate_desc(output, NPU_ACCESS_WRITE);
    if (rc != NPU_OK) return rc;

    rc = ub_npu_acquire_read(s, input0);
    if (rc != NPU_OK) return rc;
    rc = ub_npu_acquire_read(s, input1);
    if (rc != NPU_OK) return rc;

    a = g_malloc(byte_len);
    b = g_malloc(byte_len);
    c = g_malloc(byte_len);

    rc = ubc_gsva_device_read(s->ubc, &input0->key, s->device_cna,
                               input0->gsva_base, a, byte_len);
    if (rc != GSVA_OK) {
        g_free(a); g_free(b); g_free(c);
        return NPU_ERR_BAD_DESCRIPTOR;
    }
    rc = ubc_gsva_device_read(s->ubc, &input1->key, s->device_cna,
                               input1->gsva_base, b, byte_len);
    if (rc != GSVA_OK) {
        g_free(a); g_free(b); g_free(c);
        return NPU_ERR_BAD_DESCRIPTOR;
    }
    s->stats.bytes_read += byte_len * 2;
    s->cpl.bytes_read = byte_len * 2;

    for (uint32_t i = 0; i < element_count; i++) {
        c[i] = a[i] + b[i];
    }
    g_free(a);
    g_free(b);

    rc = ub_npu_acquire_write(s, output);
    if (rc != NPU_OK) {
        g_free(c);
        return rc;
    }

    rc = ubc_gsva_device_write(s->ubc, &output->key, s->device_cna,
                                output->gsva_base, c, byte_len);
    g_free(c);
    if (rc != GSVA_OK) {
        return NPU_ERR_BAD_DESCRIPTOR;
    }
    s->stats.bytes_written += byte_len;
    s->cpl.bytes_written = byte_len;

    ubc_gsva_device_fence(s->ubc, &output->key, s->device_cna,
                          output->gsva_base, byte_len);

    return NPU_OK;
}

static int ub_npu_op_checksum64(UbNpuState *s, UbNpuCmdV1 *cmd)
{
    const UbNpuBufferDescV1 *input = &cmd->descs[0];
    uint64_t checksum = 0;
    uint8_t *buf;
    int rc;

    if (cmd->desc_count < 1) {
        qemu_log("UB_NPU_DESC: CHECKSUM desc_count=%" PRIu32 " < 1\n",
                 cmd->desc_count);
        return NPU_ERR_BAD_DESCRIPTOR;
    }
    if (input->role != NPU_BUF_INPUT) {
        qemu_log("UB_NPU_DESC: CHECKSUM role mismatch input=%" PRIu32 "\n",
                 input->role);
        return NPU_ERR_BAD_DESCRIPTOR;
    }

    rc = ub_npu_validate_desc(input, NPU_ACCESS_READ);
    if (rc != NPU_OK) return rc;

    rc = ub_npu_acquire_read(s, input);
    if (rc != NPU_OK) return rc;

    buf = g_malloc(input->bytes);
    rc = ubc_gsva_device_read(s->ubc, &input->key, s->device_cna,
                               input->gsva_base, buf, input->bytes);
    if (rc != GSVA_OK) {
        g_free(buf);
        return NPU_ERR_BAD_DESCRIPTOR;
    }
    s->stats.bytes_read += input->bytes;
    s->cpl.bytes_read = input->bytes;

    for (uint64_t i = 0; i + 8 <= input->bytes; i += 8) {
        uint64_t val;
        memcpy(&val, buf + i, 8);
        checksum += val;
    }
    g_free(buf);

    s->cpl.checksum64 = checksum;
    return NPU_OK;
}

/* ------------------------------------------------------------------ */
/* Command execution                                                   */
/* ------------------------------------------------------------------ */

static void ub_npu_execute_command(UbNpuState *s)
{
    UbNpuCmdV1 *cmd = &s->cmd;
    uint32_t opcode = cmd->opcode;
    int rc = NPU_OK;
    bool is_retry = (s->exec_phase == NPU_PHASE_PENDING_RETRY);

    if (!is_retry) {
        qemu_log("UB_NPU_CMD: req_id=%#" PRIx64 " opcode=%s(%" PRIu32 ")"
                 " source_cna=%#" PRIx32 " desc_count=%" PRIu32 "\n",
                 cmd->req_id, npu_opcode_name(opcode), opcode,
                 cmd->source_cna, cmd->desc_count);
        s->stats.cmd_total++;

        if (cmd->version != 1) {
            ub_npu_complete_command(s, NPU_ERR_BAD_VERSION);
            return;
        }

        s->exec_phase = NPU_PHASE_EXECUTING;
    } else {
        qemu_log("UB_NPU_CMD: retry req_id=%#" PRIx64 " opcode=%s(%" PRIu32 ")\n",
                 cmd->req_id, npu_opcode_name(opcode), opcode);
    }

    switch (opcode) {
    case NPU_OP_NOOP:
        rc = NPU_OK;
        break;
    case NPU_OP_MEMCOPY:
        if (!is_retry) s->stats.opcode_memcopy++;
        rc = ub_npu_op_memcopy(s, cmd);
        break;
    case NPU_OP_FILL:
        if (!is_retry) s->stats.opcode_fill++;
        rc = ub_npu_op_fill(s, cmd);
        break;
    case NPU_OP_VECTOR_ADD_U32:
        if (!is_retry) s->stats.opcode_vector_add_u32++;
        rc = ub_npu_op_vector_add_u32(s, cmd);
        break;
    case NPU_OP_CHECKSUM64:
        if (!is_retry) s->stats.opcode_checksum64++;
        rc = ub_npu_op_checksum64(s, cmd);
        break;
    default:
        ub_npu_complete_command(s, NPU_ERR_BAD_OPCODE);
        return;
    }

    if (rc == NPU_INTERNAL_COH_PENDING) {
        s->pending_acquire_rc = NPU_INTERNAL_COH_PENDING;
        s->exec_phase = NPU_PHASE_PENDING_RETRY;
        timer_mod(s->poll_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000);
        s->poll_timer_active = true;
        return;
    }

    s->exec_phase = NPU_PHASE_IDLE;
    ub_npu_complete_command(s, rc);
}

/* ------------------------------------------------------------------ */
/* Bottom half                                                         */
/* ------------------------------------------------------------------ */

static void ub_npu_bh(void *opaque)
{
    UbNpuState *s = UB_NPU(opaque);

    if (s->pending_acquire_rc == NPU_INTERNAL_COH_PENDING && s->ubc) {
        obmm_coh_poll_rx_links(s->ubc);
        s->pending_acquire_rc = 0;
    }

    ub_npu_execute_command(s);
}

static void ub_npu_poll_timer_cb(void *opaque)
{
    UbNpuState *s = UB_NPU(opaque);
    s->poll_timer_active = false;
    qemu_bh_schedule(s->bh);
}

/* ------------------------------------------------------------------ */
/* MMIO read                                                           */
/* ------------------------------------------------------------------ */

static uint64_t ub_npu_mmio_read(void *opaque, hwaddr offset, unsigned size)
{
    UbNpuState *s = UB_NPU(opaque);

    switch (offset) {
    case NPU_CNA_OFF:
        return s->device_cna;
    case NPU_STATUS_OFF:
        return s->status;
    case NPU_ERROR_OFF:
        return s->error_reg;
    case NPU_LAST_REQ_ID_OFF:
        return s->last_req_id;
    default:
        if (offset >= NPU_CPL_SLOT_OFF &&
            offset < NPU_CPL_SLOT_OFF + NPU_CPL_SLOT_SIZE) {
            uint64_t off = offset - NPU_CPL_SLOT_OFF;
            if (off + size > sizeof(s->cpl)) {
                return 0;
            }
            const uint8_t *p = (const uint8_t *)&s->cpl + off;
            uint64_t val = 0;
            memcpy(&val, p, MIN(size, (unsigned)sizeof(val)));
            return val;
        }
        if (offset >= NPU_STATS_OFF &&
            offset < NPU_STATS_OFF + NPU_STATS_SIZE) {
            uint64_t off = offset - NPU_STATS_OFF;
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

static void ub_npu_mmio_write(void *opaque, hwaddr offset,
                               uint64_t val, unsigned size)
{
    UbNpuState *s = UB_NPU(opaque);

    switch (offset) {
    case NPU_DOORBELL_OFF:
        if (val != 1) {
            return;
        }
        if (s->status & NPU_STATUS_BUSY) {
            qemu_log("UB_NPU: doorbell while BUSY\n");
            if (!(s->status & NPU_STATUS_COMPLETION_VALID)) {
                s->cpl.version = 1;
                s->cpl.status = NPU_ERR_DEVICE_BUSY;
                s->cpl.req_id = s->cmd.req_id;
            }
            s->status |= NPU_STATUS_ERROR;
            s->error_reg = (uint32_t)(-NPU_ERR_DEVICE_BUSY);
            return;
        }
        s->status |= NPU_STATUS_BUSY;
        s->status &= ~(NPU_STATUS_COMPLETION_VALID | NPU_STATUS_ERROR);
        s->error_reg = 0;
        memset(&s->cpl, 0, sizeof(s->cpl));
        s->cpl.req_id = s->cmd.req_id;
        qemu_bh_schedule(s->bh);
        break;

    case NPU_CLEAR_CPL_OFF:
        if (val != 1) {
            return;
        }
        s->status &= ~NPU_STATUS_COMPLETION_VALID;
        break;

    default:
        if (offset >= NPU_CMD_SLOT_OFF &&
            offset < NPU_CMD_SLOT_OFF + NPU_CMD_SLOT_SIZE) {
            if (s->status & (NPU_STATUS_BUSY | NPU_STATUS_COMPLETION_VALID)) {
                qemu_log("UB_NPU: cmd slot write rejected while busy\n");
                return;
            }
            uint64_t off = offset - NPU_CMD_SLOT_OFF;
            if (off + size <= sizeof(s->cmd)) {
                uint8_t *p = (uint8_t *)&s->cmd + off;
                memcpy(p, &val, MIN(size, (unsigned)sizeof(val)));
            }
        }
        break;
    }
}

static const MemoryRegionOps ub_npu_mmio_ops = {
    .read = ub_npu_mmio_read,
    .write = ub_npu_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 8,
};

/* ------------------------------------------------------------------ */
/* Device lifecycle                                                    */
/* ------------------------------------------------------------------ */

static void ub_npu_realize(DeviceState *dev, Error **errp)
{
    UbNpuState *s = UB_NPU(dev);

    if (!s->ubc) {
        error_setg(errp, "ub-npu: missing 'ubc' link property");
        return;
    }

    if (s->device_cna == 0) {
        s->device_cna = (s->node_id << 16) | (0x10 << 8) | s->instance_id;
    }

    s->status = NPU_STATUS_READY;
    memset(&s->cmd, 0, sizeof(s->cmd));
    memset(&s->cpl, 0, sizeof(s->cpl));
    memset(&s->stats, 0, sizeof(s->stats));

    qemu_log("UB_NPU: realized cna=%#" PRIx32 " node_id=%" PRIu32
             " instance=%" PRIu32 "\n",
             s->device_cna, s->node_id, s->instance_id);
}

static void ub_npu_init(Object *obj)
{
    UbNpuState *s = UB_NPU(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->mmio, obj, &ub_npu_mmio_ops,
                          s, "ub-npu-mmio", NPU_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);

    s->bh = qemu_bh_new(ub_npu_bh, s);
    s->poll_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                  ub_npu_poll_timer_cb, s);
}

static void ub_npu_finalize(Object *obj)
{
    UbNpuState *s = UB_NPU(obj);

    if (s->poll_timer) {
        timer_free(s->poll_timer);
        s->poll_timer = NULL;
    }
    if (s->bh) {
        qemu_bh_delete(s->bh);
        s->bh = NULL;
    }
}

static Property ub_npu_properties[] = {
    DEFINE_PROP_UINT32("node-id", UbNpuState, node_id, 0),
    DEFINE_PROP_UINT32("instance-id", UbNpuState, instance_id, 0),
    DEFINE_PROP_UINT32("cna", UbNpuState, device_cna, 0),
    DEFINE_PROP_LINK("ubc", UbNpuState, ubc,
                     TYPE_BUS_CONTROLLER_DEV, BusControllerDev *),
    DEFINE_PROP_END_OF_LIST(),
};

static void ub_npu_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_props(dc, ub_npu_properties);
    dc->realize = ub_npu_realize;
}

static const TypeInfo ub_npu_type_info = {
    .name = TYPE_UB_NPU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(UbNpuState),
    .instance_init = ub_npu_init,
    .instance_finalize = ub_npu_finalize,
    .class_init = ub_npu_class_init,
};

static void ub_npu_register_types(void)
{
    type_register_static(&ub_npu_type_info);
}
type_init(ub_npu_register_types)

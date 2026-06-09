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

/* ------------------------------------------------------------------ */
/* NPU buffer descriptor roles and access                              */
/* ------------------------------------------------------------------ */

#define NPU_BUF_INPUT       0
#define NPU_BUF_WEIGHT      1
#define NPU_BUF_OUTPUT      2
#define NPU_BUF_SCRATCH     3

#define NPU_ACCESS_READ      1
#define NPU_ACCESS_WRITE     2
#define NPU_ACCESS_READ_WRITE 3

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

typedef struct UbNpuBufferDescV1 {
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

typedef struct UbNpuCmdV1 {
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

typedef struct UbNpuCplV1 {
    uint32_t version;
    uint32_t status;
    uint64_t req_id;
    uint64_t bytes_read;
    uint64_t bytes_written;
    uint64_t checksum64;
    uint64_t error_detail;
} UbNpuCplV1;

/* ------------------------------------------------------------------ */
/* NPU stats                                                           */
/* ------------------------------------------------------------------ */

#define NPU_STATS_COUNT 12

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
    uint64_t coh_timeout;
} UbNpuStats;

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
/* Command execution (V1: echo only, GSVA ops in Step 4)              */
/* ------------------------------------------------------------------ */

static void ub_npu_execute_command(UbNpuState *s)
{
    UbNpuCmdV1 *cmd = &s->cmd;
    uint32_t opcode = cmd->opcode;

    qemu_log("UB_NPU_CMD: req_id=%#" PRIx64 " opcode=%s(%" PRIu32 ")"
             " source_cna=%#" PRIx32 " desc_count=%" PRIu32 "\n",
             cmd->req_id, npu_opcode_name(opcode), opcode,
             cmd->source_cna, cmd->desc_count);

    s->stats.cmd_total++;

    if (cmd->version != 1) {
        ub_npu_complete_command(s, NPU_ERR_BAD_VERSION);
        return;
    }

    switch (opcode) {
    case NPU_OP_MEMCOPY:
    case NPU_OP_FILL:
    case NPU_OP_VECTOR_ADD_U32:
    case NPU_OP_CHECKSUM64:
        /* V1 echo: complete with OK, actual GSVA ops in Step 4 */
        ub_npu_complete_command(s, NPU_OK);
        break;
    default:
        ub_npu_complete_command(s, NPU_ERR_BAD_OPCODE);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Bottom half                                                         */
/* ------------------------------------------------------------------ */

static void ub_npu_bh(void *opaque)
{
    UbNpuState *s = UB_NPU(opaque);
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

/*
 * OBMM asynchronous queue endpoint for the P2A software path.
 */

#ifndef HW_UB_OBMM_ASYNC_H
#define HW_UB_OBMM_ASYNC_H

#include "exec/hwaddr.h"
#include "qemu/osdep.h"

#define UB_OBMM_ASYNC_ENDPOINT_BASE 0x2000
#define UB_OBMM_ASYNC_ENDPOINT_BYTES 0x1000
#define UB_OBMM_ASYNC_ABI_VERSION 1
#define UB_OBMM_ASYNC_QUEUE_DEPTH 64
#define UB_OBMM_ASYNC_SLOT_BYTES 64

typedef struct BusControllerDev BusControllerDev;
typedef struct UbObmmAsyncState UbObmmAsyncState;

typedef struct QEMU_PACKED UbObmmAsyncSqEntryV1 {
    uint16_t abi_version;
    uint8_t opcode;
    uint8_t flags;
    uint32_t length;
    uint64_t token;
    uint64_t map_id;
    uint64_t map_generation;
    uint64_t remote_offset;
    uint32_t dst_buffer_id;
    uint32_t dst_offset;
    uint64_t deadline_ns;
    uint64_t user_data;
} UbObmmAsyncSqEntryV1;

typedef struct QEMU_PACKED UbObmmAsyncCqEntryV1 {
    uint16_t abi_version;
    uint8_t opcode;
    uint8_t flags;
    int32_t status;
    uint64_t token;
    uint64_t user_data;
    uint32_t bytes_done;
    uint32_t provider_status;
    uint64_t checksum64;
    uint64_t completed_ns;
    uint64_t map_generation;
    uint64_t reserved;
} UbObmmAsyncCqEntryV1;

UbObmmAsyncState *ub_obmm_async_new(BusControllerDev *ubc_dev);
void ub_obmm_async_free(UbObmmAsyncState *state);
bool ub_obmm_async_decode(hwaddr address, hwaddr *reg);
uint64_t ub_obmm_async_read(UbObmmAsyncState *state, hwaddr reg,
                            unsigned int size);
bool ub_obmm_async_write(UbObmmAsyncState *state, hwaddr reg,
                         uint64_t value, unsigned int size);

#endif

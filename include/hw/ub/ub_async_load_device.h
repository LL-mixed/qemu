/*
 * QEMU adapter for the OBMM direct-to-EL0 upcall mechanism.
 */

#ifndef HW_UB_ASYNC_LOAD_DEVICE_H
#define HW_UB_ASYNC_LOAD_DEVICE_H

#include "hw/ub/ub_async_load.h"
#include "hw/core/cpu.h"
#include "qapi/error.h"
#include "qemu/osdep.h"

#define UB_ASYNC_LOAD_ENDPOINT_BASE 0x3000
#define UB_ASYNC_LOAD_ENDPOINT_BYTES 0x1000
#define UB_ASYNC_LOAD_ABI_VERSION 2
#define UB_ASYNC_LOAD_RESUME_IMM 0x5343
#define UB_ASYNC_LOAD_CAP_REPLAY_RETIRE (1ULL << 8)
#define UB_ASYNC_LOAD_START_REPLAY_RETIRE (1ULL << 0)

typedef struct BusControllerDev BusControllerDev;
typedef struct UbAsyncLoadDeviceState UbAsyncLoadDeviceState;

typedef enum UbAsyncLoadTryResult {
    UB_ASYNC_LOAD_TRY_NOT_REMOTE,
    UB_ASYNC_LOAD_TRY_SYNC_STALL,
    UB_ASYNC_LOAD_TRY_PENDING,
    UB_ASYNC_LOAD_TRY_REPLAYED,
    UB_ASYNC_LOAD_TRY_FAIL_STOP,
} UbAsyncLoadTryResult;

UbAsyncLoadDeviceState *ub_async_load_device_new(BusControllerDev *ubc_dev,
                                    const char *model_spec,
                                    Error **errp);
void ub_async_load_device_free(UbAsyncLoadDeviceState *state);
bool ub_async_load_device_decode(hwaddr address, hwaddr *reg);
uint64_t ub_async_load_device_read(UbAsyncLoadDeviceState *state, hwaddr reg,
                            unsigned int size);
bool ub_async_load_device_write(UbAsyncLoadDeviceState *state, hwaddr reg,
                         uint64_t value, unsigned int size);

bool ub_async_load_cpu_enabled(CPUState *cpu);
bool ub_async_load_cpu_owner_matches(CPUState *cpu, uint64_t ttbr0_el1);
uint64_t ub_async_load_cpu_cycle(CPUState *cpu);
bool ub_async_load_cpu_address_is_remote(CPUState *cpu, uint64_t va,
                                  uint8_t bytes);
bool ub_async_load_cpu_replay_expected(CPUState *cpu);
bool ub_async_load_cpu_take_upcall(CPUState *cpu, uint64_t interrupted_pc,
                            uint64_t *upcall_entry);
bool ub_async_load_cpu_resume(CPUState *cpu, uint64_t context_id);
void ub_async_load_cpu_fail_stop(CPUState *cpu);
UbAsyncLoadTryResult ub_async_load_cpu_remote_load(
    CPUState *cpu, const UbAsyncLoadDesc *load, uint64_t *replay_value);

/* Implemented by target/arm: controls TB specialization and invalidation. */
void arm_async_load_set_active(CPUState *cpu, bool active);

#endif

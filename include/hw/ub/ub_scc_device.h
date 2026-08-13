/*
 * QEMU adapter for the OBMM direct-to-EL0 upcall mechanism.
 */

#ifndef HW_UB_SCC_DEVICE_H
#define HW_UB_SCC_DEVICE_H

#include "hw/ub/ub_scc.h"
#include "hw/core/cpu.h"
#include "qapi/error.h"
#include "qemu/osdep.h"

#define UB_SCC_ENDPOINT_BASE 0x3000
#define UB_SCC_ENDPOINT_BYTES 0x1000
#define UB_SCC_ABI_VERSION 2
#define UB_SCC_RESUME_IMM 0x5343

typedef struct BusControllerDev BusControllerDev;
typedef struct UbSccDeviceState UbSccDeviceState;

typedef enum UbSccLoadTryResult {
    UB_SCC_LOAD_NOT_REMOTE,
    UB_SCC_LOAD_SYNC_STALL,
    UB_SCC_LOAD_PENDING,
    UB_SCC_LOAD_FAIL_STOP,
} UbSccLoadTryResult;

UbSccDeviceState *ub_scc_device_new(BusControllerDev *ubc_dev,
                                    const char *model_spec,
                                    Error **errp);
void ub_scc_device_free(UbSccDeviceState *state);
bool ub_scc_device_decode(hwaddr address, hwaddr *reg);
uint64_t ub_scc_device_read(UbSccDeviceState *state, hwaddr reg,
                            unsigned int size);
bool ub_scc_device_write(UbSccDeviceState *state, hwaddr reg,
                         uint64_t value, unsigned int size);

bool ub_scc_cpu_enabled(CPUState *cpu);
bool ub_scc_cpu_owner_matches(CPUState *cpu, uint64_t ttbr0_el1);
uint64_t ub_scc_cpu_cycle(CPUState *cpu);
bool ub_scc_cpu_address_is_remote(CPUState *cpu, uint64_t va,
                                  uint8_t bytes);
bool ub_scc_cpu_take_upcall(CPUState *cpu, uint64_t interrupted_pc,
                            uint64_t *upcall_entry);
bool ub_scc_cpu_resume(CPUState *cpu, uint64_t context_id);
void ub_scc_cpu_fail_stop(CPUState *cpu);
UbSccLoadTryResult ub_scc_cpu_remote_load(
    CPUState *cpu, const ObmmSccLoadDesc *load);

/* Implemented by target/arm: controls TB specialization and invalidation. */
void arm_obmm_scc_set_active(CPUState *cpu, bool active);

#endif

/* Minimal AArch64 UAPI shim for QEMU Darwin builds. */
#ifndef QEMU_DARWIN_ASM_PTRACE_H
#define QEMU_DARWIN_ASM_PTRACE_H

#include <stdint.h>

struct user_pt_regs {
    uint64_t regs[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
};

struct user_fpsimd_state {
    uint64_t vregs[64];
    uint32_t fpsr;
    uint32_t fpcr;
    uint8_t __reserved[8];
};

#endif

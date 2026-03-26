#include "qemu/osdep.h"

#ifdef CONFIG_DARWIN

#include "exec/gdbstub.h"
#include "gdbstub/syscalls.h"
#include "qapi/error.h"
#include "qemu/coroutine_int.h"
#include "sysemu/iommufd.h"

typedef struct ARMCPU ARMCPU;

void gdb_register_coprocessor(CPUState *cpu,
                              gdb_get_reg_cb get_reg, gdb_set_reg_cb set_reg,
                              int num_regs, const char *xml, int g_pos)
{
}

void gdb_unregister_coprocessor_all(CPUState *cpu)
{
}

int gdbserver_start(const char *port_or_device)
{
    return -1;
}

void gdb_set_stop_cpu(CPUState *cpu)
{
}

void gdb_do_syscall(gdb_syscall_complete_cb cb, const char *fmt, ...)
{
    if (cb) {
        cb(NULL, (uint64_t)-1, GDB_EUNKNOWN);
    }
}

int use_gdb_syscalls(void)
{
    return 0;
}

void gdb_exit(int code)
{
}

bool iommufd_backend_alloc_hwpt(IOMMUFDBackend *be, uint32_t dev_id,
                                uint32_t pt_id, uint32_t flags,
                                uint32_t data_type, uint32_t data_len,
                                void *data_ptr, uint32_t *out_hwpt,
                                uint32_t *out_fault_fd, Error **errp)
{
    if (out_hwpt) {
        *out_hwpt = 0;
    }
    if (out_fault_fd) {
        *out_fault_fd = 0;
    }
    if (errp) {
        error_setg(errp, "iommufd is not supported on Darwin");
    }
    return false;
}

struct IOMMUFDVdev *iommufd_backend_alloc_vdev(HostIOMMUDeviceIOMMUFD *idev,
                                               IOMMUFDViommu *viommu,
                                               uint64_t virt_id)
{
    return NULL;
}

struct IOMMUFDViommu *iommufd_backend_alloc_viommu(IOMMUFDBackend *be,
                                                   uint32_t dev_id,
                                                   uint32_t viommu_type,
                                                   uint32_t hwpt_id)
{
    return NULL;
}

void iommufd_backend_free_id(IOMMUFDBackend *be, uint32_t id)
{
}

bool iommufd_backend_get_device_info(IOMMUFDBackend *be, uint32_t devid,
                                     uint32_t *type, void *data, uint32_t len,
                                     uint64_t *caps, uint8_t *max_pasid_log2,
                                     Error **errp)
{
    if (type) {
        *type = IOMMU_HW_INFO_TYPE_NONE;
    }
    if (caps) {
        *caps = 0;
    }
    if (max_pasid_log2) {
        *max_pasid_log2 = 0;
    }
    if (errp) {
        error_setg(errp, "iommufd is not supported on Darwin");
    }
    return false;
}

int iommufd_backend_invalidate_cache(IOMMUFDBackend *be, uint32_t hwpt_id,
                                     uint32_t data_type, uint32_t entry_len,
                                     uint32_t *entry_num, void *data_ptr)
{
    if (entry_num) {
        *entry_num = 0;
    }
    return -1;
}

int iommufd_viommu_invalidate_cache(IOMMUFDBackend *be, uint32_t viommu_id,
                                    uint32_t data_type, uint32_t entry_len,
                                    uint32_t *entry_num, void *data_ptr)
{
    if (entry_num) {
        *entry_num = 0;
    }
    return -1;
}

bool host_iommu_device_iommufd_attach_hwpt(HostIOMMUDeviceIOMMUFD *idev,
                                           uint32_t hwpt_id, Error **errp)
{
    if (errp) {
        error_setg(errp, "iommufd is not supported on Darwin");
    }
    return false;
}

int iommufd_device_get_info(HostIOMMUDeviceIOMMUFD *idev,
                            enum iommu_hw_info_type *type,
                            uint32_t len, void *data)
{
    if (type) {
        *type = IOMMU_HW_INFO_TYPE_NONE;
    }
    return -1;
}

void kvm_arm_reset_vcpu(ARMCPU *cpu)
{
}

void qemu_coroutine_info_add(const Coroutine *co_)
{
}

void qemu_coroutine_info_delete(const Coroutine *co_)
{
}

#endif

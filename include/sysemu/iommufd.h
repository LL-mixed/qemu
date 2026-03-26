/*
 * iommufd container backend declaration
 *
 * Copyright (C) 2024 Intel Corporation.
 * Copyright Red Hat, Inc. 2024
 *
 * Authors: Yi Liu <yi.l.liu@intel.com>
 *          Eric Auger <eric.auger@redhat.com>
 *          Zhenzhong Duan <zhenzhong.duan@intel.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef SYSEMU_IOMMUFD_H
#define SYSEMU_IOMMUFD_H

#include "qom/object.h"
#include "exec/hwaddr.h"
#include "exec/cpu-common.h"
#include "sysemu/host_iommu_device.h"
#ifdef __linux__
#include <linux/iommufd.h>
#else
enum iommu_hw_info_type {
    IOMMU_HW_INFO_TYPE_NONE = 0,
    IOMMU_HW_INFO_TYPE_ARM_SMMUV3 = 1,
    IOMMU_HW_INFO_TYPE_UMMU = 2,
};

struct iommu_hwpt_pgfault {
    uint32_t argsz;
    uint32_t flags;
    uint32_t pasid;
    uint32_t grpid;
    uint32_t perm;
    uint32_t cookie;
    uint64_t addr;
};

struct iommu_hwpt_page_response {
    uint32_t argsz;
    uint32_t cookie;
    uint32_t code;
};

struct iommu_hw_info_arm_smmuv3 {
    uint32_t argsz;
    uint32_t idr[8];
};

struct iommu_hw_info_ummu {
    uint32_t argsz;
    uint32_t flags;
    uint32_t iidr;
    uint32_t aidr;
};

struct iommu_hwpt_arm_smmuv3 {
    uint64_t ste[2];
};

struct iommu_hwpt_ummu {
    uint64_t tecte[2];
};

struct io_uring {
    int dummy;
};

struct io_uring_sqe {
    int dummy;
};

struct io_uring_cqe {
    int res;
    void *user_data;
};

struct __kernel_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

#define IOMMU_HWPT_ALLOC_NEST_PARENT 1U
#define IOMMU_HWPT_DATA_NONE 0U
#define IOMMU_HWPT_DATA_ARM_SMMUV3 1U
#define IOMMU_HWPT_DATA_UMMU 2U
#define IOMMU_VIOMMU_TYPE_ARM_SMMUV3 1U
#define IOMMU_VIOMMU_TYPE_UMMU 2U
#define IOMMU_VIOMMU_INVALIDATE_DATA_ARM_SMMUV3 2U
#define IOMMU_VIOMMU_INVALIDATE_DATA_UMMU 1U
#define IOMMU_HWPT_FAULT_ID_VALID (1U << 0)
#define IOMMU_PGFAULT_FLAGS_PASID_VALID (1U << 0)
#define IOMMU_PGFAULT_PERM_READ (1U << 0)
#define IOMMU_PGFAULT_PERM_PRIV (1U << 1)
#define IOMMU_PGFAULT_PERM_EXEC (1U << 2)
#define IOMMUFD_PAGE_RESP_INVALID 0U
#define IOMMUFD_PAGE_RESP_SUCCESS 1U

static inline struct io_uring_sqe *io_uring_get_sqe(struct io_uring *ring)
{
    return NULL;
}

static inline void io_uring_prep_timeout(struct io_uring_sqe *sqe,
                                         const struct __kernel_timespec *ts,
                                         unsigned count, unsigned flags)
{
}

static inline int io_uring_submit(struct io_uring *ring)
{
    return 0;
}

static inline void io_uring_prep_read(struct io_uring_sqe *sqe, int fd,
                                      void *buf, unsigned nbytes,
                                      uint64_t offset)
{
}

static inline void io_uring_sqe_set_data(struct io_uring_sqe *sqe, void *data)
{
}

static inline int io_uring_wait_cqe(struct io_uring *ring,
                                    struct io_uring_cqe **cqe_ptr)
{
    return -1;
}

static inline void *io_uring_cqe_get_data(const struct io_uring_cqe *cqe)
{
    return cqe ? cqe->user_data : NULL;
}

static inline void io_uring_cqe_seen(struct io_uring *ring,
                                     struct io_uring_cqe *cqe)
{
}

static inline int io_uring_queue_init(unsigned entries, struct io_uring *ring,
                                      unsigned flags)
{
    return 0;
}

static inline void io_uring_queue_exit(struct io_uring *ring)
{
}
#endif

#define TYPE_IOMMUFD_BACKEND "iommufd"
OBJECT_DECLARE_TYPE(IOMMUFDBackend, IOMMUFDBackendClass, IOMMUFD_BACKEND)

struct IOMMUFDBackendClass {
    ObjectClass parent_class;
};

struct IOMMUFDBackend {
    Object parent;

    /*< protected >*/
    int fd;            /* /dev/iommu file descriptor */
    bool owned;        /* is the /dev/iommu opened internally */
    uint32_t users;

    /*< public >*/
};

typedef struct IOMMUFDViommu {
    IOMMUFDBackend *iommufd;
    uint32_t s2_hwpt_id;
    uint32_t viommu_id;
} IOMMUFDViommu;

int iommufd_backend_connect(IOMMUFDBackend *be, Error **errp);
void iommufd_backend_disconnect(IOMMUFDBackend *be);

int iommufd_backend_alloc_ioas(IOMMUFDBackend *be, uint32_t *ioas_id,
                               Error **errp);
void iommufd_backend_free_id(IOMMUFDBackend *be, uint32_t id);
int iommufd_backend_map_dma(IOMMUFDBackend *be, uint32_t ioas_id, hwaddr iova,
                            ram_addr_t size, void *vaddr, bool readonly);
int iommufd_backend_unmap_dma(IOMMUFDBackend *be, uint32_t ioas_id,
                              hwaddr iova, ram_addr_t size);
bool iommufd_backend_get_device_info(IOMMUFDBackend *be, uint32_t devid,
                                     uint32_t *type, void *data, uint32_t len,
                                     uint64_t *caps, uint8_t *max_pasid_log2,
                                     Error **errp);
bool iommufd_backend_alloc_hwpt(IOMMUFDBackend *be, uint32_t dev_id,
                                uint32_t pt_id, uint32_t flags,
                                uint32_t data_type, uint32_t data_len,
                                void *data_ptr, uint32_t *out_hwpt,
                                uint32_t *out_fault_fd, Error **errp);
bool iommufd_backend_set_dirty_tracking(IOMMUFDBackend *be, uint32_t hwpt_id,
                                        bool start, Error **errp);
bool iommufd_backend_get_dirty_bitmap(IOMMUFDBackend *be, uint32_t hwpt_id,
                                      uint64_t iova, ram_addr_t size,
                                      uint64_t page_size, uint64_t *data,
                                      Error **errp);
int iommufd_backend_invalidate_cache(IOMMUFDBackend *be, uint32_t hwpt_id,
                                     uint32_t data_type, uint32_t entry_len,
                                     uint32_t *entry_num, void *data_ptr);
struct IOMMUFDViommu *iommufd_backend_alloc_viommu(IOMMUFDBackend *be,
                                                   uint32_t dev_id,
                                                   uint32_t viommu_type,
                                                   uint32_t hwpt_id);
int iommufd_viommu_invalidate_cache(IOMMUFDBackend *be, uint32_t viommu_id,
                                    uint32_t data_type, uint32_t entry_len,
                                    uint32_t *entry_num, void *data_ptr);

#define TYPE_HOST_IOMMU_DEVICE_IOMMUFD TYPE_HOST_IOMMU_DEVICE "-iommufd"
OBJECT_DECLARE_TYPE(HostIOMMUDeviceIOMMUFD, HostIOMMUDeviceIOMMUFDClass,
                    HOST_IOMMU_DEVICE_IOMMUFD)

/* Abstract of host IOMMU device with iommufd backend */
struct HostIOMMUDeviceIOMMUFD {
    HostIOMMUDevice parent_obj;

    IOMMUFDBackend *iommufd;
    uint32_t devid;
    uint32_t ioas_id;
};

struct HostIOMMUDeviceIOMMUFDClass {
    HostIOMMUDeviceClass parent_class;

    /**
     * @attach_hwpt: attach host IOMMU device to IOMMUFD hardware page table.
     * VFIO and VDPA device can have different implementation.
     *
     * Mandatory callback.
     *
     * @idev: host IOMMU device backed by IOMMUFD backend.
     *
     * @hwpt_id: ID of IOMMUFD hardware page table.
     *
     * @errp: pass an Error out when attachment fails.
     *
     * Returns: true on success, false on failure.
     */
    bool (*attach_hwpt)(HostIOMMUDeviceIOMMUFD *idev, uint32_t hwpt_id,
                        Error **errp);
    /**
     * @detach_hwpt: detach host IOMMU device from IOMMUFD hardware page table.
     * VFIO and VDPA device can have different implementation.
     *
     * Mandatory callback.
     *
     * @idev: host IOMMU device backed by IOMMUFD backend.
     *
     * @errp: pass an Error out when attachment fails.
     *
     * Returns: true on success, false on failure.
     */
    bool (*detach_hwpt)(HostIOMMUDeviceIOMMUFD *idev, Error **errp);
};

bool host_iommu_device_iommufd_attach_hwpt(HostIOMMUDeviceIOMMUFD *idev,
                                           uint32_t hwpt_id, Error **errp);
bool host_iommu_device_iommufd_detach_hwpt(HostIOMMUDeviceIOMMUFD *idev,
                                           Error **errp);

typedef struct IOMMUFDVdev {
    HostIOMMUDeviceIOMMUFD *idev;
    IOMMUFDViommu *viommu;
    uint32_t vdev_id;
    uint64_t virt_id;
} IOMMUFDVdev;

struct IOMMUFDVdev *iommufd_backend_alloc_vdev(HostIOMMUDeviceIOMMUFD *idev,
                                               IOMMUFDViommu *viommu,
                                               uint64_t virt_id);

int iommufd_device_get_info(HostIOMMUDeviceIOMMUFD *idev,
                            enum iommu_hw_info_type *type,
                            uint32_t len, void *data);
#endif

/*
 * Minimal UB switch device for guest-side topology enumeration.
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/qdev-core.h"
#include "hw/ub/ub.h"
#include "hw/ub/ub_config.h"

#define TYPE_UB_SWITCH_DEV "ub-switch-dev"
#define UB_SWITCH_CLASS_CODE 0x0003

static void ub_switch_space_cfg0_init(UBDevice *ub_dev)
{
    UbCfg0Basic *cfg0_basic;
    Cfg0SupportFeature *support_feature;
    uint64_t emulated_offset;

    emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG0_BASIC_START, true);
    cfg0_basic = (UbCfg0Basic *)(ub_dev->config + emulated_offset);
    cfg0_basic->header.slice_version = UB_SLICE_VERSION;
    cfg0_basic->header.slice_used_size = UB_CFG0_BASIC_SLICE_USED_SIZE;
    cfg0_basic->total_num_of_port = ub_dev->port.port_num & UINT16_MASK;
    cfg0_basic->total_num_of_ue = 1;
    support_feature = &cfg0_basic->support_feature;
    support_feature->bits.entity_available = 1;
    support_feature->bits.mtu_supported = 1;
    support_feature->bits.route_table_supported = SUPPORTED;
    support_feature->bits.upi_supported = SUPPORTED;
    support_feature->bits.switch_supported = SUPPORTED;
    support_feature->bits.cc_supported = NOT_SUPPORTED;
}

static void ub_switch_space_cfg1_init(UBDevice *ub_dev)
{
    UbCfg1Basic *cfg1_basic;
    uint64_t emulated_offset;
    uint8_t *cfg1_raw;

    emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG1_BASIC_START, true);
    cfg1_basic = (UbCfg1Basic *)(ub_dev->config + emulated_offset);
    cfg1_raw = ub_dev->config + emulated_offset;
    cfg1_basic->header.slice_version = UB_SLICE_VERSION;
    cfg1_basic->header.slice_used_size = UB_CFG1_BASIC_SLICE_USED_SIZE;
    cfg1_basic->class_code = UB_SWITCH_CLASS_CODE;
    *(uint32_t *)(cfg1_raw + 0xa4) = UB_SWITCH_CLASS_CODE;
}

static void ub_switch_dev_realize(UBDevice *dev, Error **errp)
{
    (void)errp;
    dev->dev_type = UB_TYPE_SWITCH;
    ub_switch_space_cfg0_init(dev);
    ub_switch_space_cfg1_init(dev);
}

static void ub_switch_dev_class_init(ObjectClass *klass, void *data)
{
    UBDeviceClass *uc = UB_DEVICE_CLASS(klass);
    uc->realize = ub_switch_dev_realize;
}

static const TypeInfo ub_switch_dev_type_info = {
    .name = TYPE_UB_SWITCH_DEV,
    .parent = TYPE_UB_DEVICE,
    .instance_size = sizeof(UBDevice),
    .class_size = sizeof(UBDeviceClass),
    .class_init = ub_switch_dev_class_init,
};

static void ub_switch_dev_register_types(void)
{
    type_register_static(&ub_switch_dev_type_info);
}

type_init(ub_switch_dev_register_types)

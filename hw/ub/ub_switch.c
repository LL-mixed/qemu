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
#define UB_SWITCH_ERS0_SPACE_SIZE 0x20
#define UB_SWITCH_ERS0_SPACE_ADDR 0x3c00000000ULL

static void ub_switch_space_cfg0_init(UBDevice *ub_dev)
{
    UbCfg0Basic *cfg0_basic;
    Cfg0SupportFeature *support_feature;
    UbCfg0ShpCap *shp_cap;
    UbSlotInfo *slot_info;
    uint64_t emulated_offset;

    emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG0_BASIC_START, true);
    cfg0_basic = (UbCfg0Basic *)(ub_dev->config + emulated_offset);
    cfg0_basic->header.slice_version = UB_SLICE_VERSION;
    cfg0_basic->header.slice_used_size = UB_CFG0_BASIC_SLICE_USED_SIZE;
    cfg0_basic->total_num_of_port = ub_dev->port.port_num & UINT16_MASK;
    cfg0_basic->total_num_of_ue = 1;
    cfg0_basic->cap_bitmap[CFG0_CAP2_SHP_INDEX / BITS_PER_BYTE] =
        1 << (CFG0_CAP2_SHP_INDEX % BITS_PER_BYTE);
    support_feature = &cfg0_basic->support_feature;
    support_feature->bits.entity_available = 1;
    support_feature->bits.mtu_supported = 1;
    support_feature->bits.route_table_supported = SUPPORTED;
    support_feature->bits.upi_supported = SUPPORTED;
    support_feature->bits.switch_supported = SUPPORTED;
    support_feature->bits.cc_supported = NOT_SUPPORTED;

    emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG0_CAP2_SHP_START, true);
    shp_cap = (UbCfg0ShpCap *)(ub_dev->config + emulated_offset);
    shp_cap->slot_num = 1;
    shp_cap->header.slice_version = UB_SLICE_VERSION;
    shp_cap->header.slice_used_size =
        (shp_cap->slot_num * sizeof(UbSlotInfo) + sizeof(UbCfg0ShpCap)) / DWORD_SIZE;
    slot_info = (UbSlotInfo *)shp_cap->slot_info;
    slot_info->pps = 1;
    slot_info->wlps = 1;
    slot_info->plps = 1;
    slot_info->pdss = 1;
    slot_info->pwcs = 1;
    slot_info->start_port_idx = 0;
    slot_info->end_port_idx = cfg0_basic->total_num_of_port - 1;
    slot_info->pp_ctrl = 1;
    slot_info->ms_ctrl = 1;
    slot_info->pd_ctrl = 1;
    slot_info->pds_ctrl = 1;
}

static void ub_switch_space_cfg1_init(UBDevice *ub_dev)
{
    UbCfg1Basic *cfg1_basic;
    Cfg1SupportFeature *support_feature;
    uint64_t emulated_offset;
    uint8_t *cfg1_raw;
    uint32_t support_feature_l = 0;

    emulated_offset = ub_cfg_offset_to_emulated_offset(UB_CFG1_BASIC_START, true);
    cfg1_basic = (UbCfg1Basic *)(ub_dev->config + emulated_offset);
    cfg1_raw = ub_dev->config + emulated_offset;
    cfg1_basic->header.slice_version = UB_SLICE_VERSION;
    cfg1_basic->header.slice_used_size = UB_CFG1_BASIC_SLICE_USED_SIZE;
    support_feature = &cfg1_basic->support_feature;
    support_feature->bits.ers0s = SUPPORTED;
    support_feature_l |= BIT(6); /* ERS0S */
    cfg1_basic->ers_space_size[0] = UB_SWITCH_ERS0_SPACE_SIZE;
    cfg1_basic->ers_start_addr[0] = UB_SWITCH_ERS0_SPACE_ADDR;
    cfg1_basic->class_code = UB_SWITCH_CLASS_CODE;
    *(uint32_t *)(cfg1_raw + 0x24) = support_feature_l;
    *(uint32_t *)(cfg1_raw + 0x34) = UB_SWITCH_ERS0_SPACE_SIZE;
    *(uint32_t *)(cfg1_raw + 0x40) = (uint32_t)(UB_SWITCH_ERS0_SPACE_ADDR & UINT32_MAX);
    *(uint32_t *)(cfg1_raw + 0x44) = (uint32_t)(UB_SWITCH_ERS0_SPACE_ADDR >> 32);
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

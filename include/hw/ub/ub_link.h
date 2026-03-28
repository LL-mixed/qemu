/*
 * Minimal standalone UB point-to-point link object.
 *
 * One UBLink instance models exactly one point-to-point port-to-port
 * connection. It exists as a topology object, separate from the UBC model,
 * so future multi-instance UBC<->UBC links can be represented without
 * embedding interconnect behavior inside a controller implementation.
 */

#ifndef UB_LINK_H
#define UB_LINK_H

#include "qom/object.h"

typedef struct UBDevice UBDevice;

#define TYPE_UB_LINK "ub-link"
OBJECT_DECLARE_SIMPLE_TYPE(UBLinkState, UB_LINK)

typedef struct UBLinkEndpointDesc {
    char *device_id;
    uint32_t port_idx;
    UBDevice *device;
} UBLinkEndpointDesc;

struct UBLinkState {
    Object parent_obj;

    UBLinkEndpointDesc a;
    UBLinkEndpointDesc b;
    bool link_up;
    bool attached;
    bool applied;
    bool pending;
    bool remote_applied;
};

void ub_link_configure(UBLinkState *s, const char *a_device_id, uint32_t a_port_idx,
                       const char *b_device_id, uint32_t b_port_idx, bool link_up);
int ub_link_attach_endpoints(UBLinkState *s, Error **errp);
int ub_link_apply(UBLinkState *s, Error **errp);
int ub_link_deactivate(UBLinkState *s, Error **errp);
void ub_link_detach_endpoints(UBLinkState *s);
bool ub_link_is_pending(UBLinkState *s);

#endif

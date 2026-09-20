#ifndef GSVA_TOKEN_ROTATION_H
#define GSVA_TOKEN_ROTATION_H

#include "hw/ub/gsva_home.h"
#include "hw/ub/gsva_route.h"

/* Apply one token update at the authoritative home. The caller reports
 * whether its local route is already in the pending state. */
int gsva_token_rotation_apply_home(GsvaHomeTable *home,
                                   uint32_t local_cna,
                                   GsvaRouteTable *routes,
                                   const GsvaKeyV1 *key,
                                   uint32_t requester_cna,
                                   uint32_t token_id,
                                   uint32_t new_token_value,
                                   bool route_already_pending);

#endif

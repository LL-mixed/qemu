#include "qemu/osdep.h"
#include "hw/ub/gsva_token_rotation.h"

static GsvaRouteEntry *exact_route(GsvaRouteTable *routes,
                                   const GsvaKeyV1 *key,
                                   int *error)
{
    GsvaRouteEntry *route;

    *error = GSVA_OK;
    if (!routes) {
        return NULL;
    }
    route = gsva_route_lookup_base(routes, key);
    if (route && memcmp(&route->key, key, sizeof(*key)) != 0) {
        *error = GSVA_ERR_STALE_EPOCH;
        return NULL;
    }
    return route;
}

int gsva_token_rotation_apply_home(GsvaHomeTable *home,
                                   uint32_t local_cna,
                                   GsvaRouteTable *routes,
                                   const GsvaKeyV1 *key,
                                   uint32_t requester_cna,
                                   uint32_t token_id,
                                   uint32_t new_token_value,
                                   bool route_already_pending)
{
    GsvaRouteEntry *route;
    uint32_t old_token_value = 0;
    bool home_rotated = false;
    bool route_pending = route_already_pending;
    int error;

    if (!home || !local_cna || !key || !token_id || !new_token_value) {
        return GSVA_ERR_BAD_VERSION;
    }
    route = exact_route(routes, key, &error);
    if (error != GSVA_OK || (route_already_pending && !route)) {
        return error != GSVA_OK ? error : GSVA_ERR_ROUTE_MISSING;
    }
    error = gsva_home_token_value(home, local_cna, key, token_id,
                                  &old_token_value);
    if (error != GSVA_OK) {
        if (route_already_pending) {
            (void)gsva_route_abort_token_revoke(
                routes, key, token_id, new_token_value);
        }
        return error;
    }
    if (route && !route_pending) {
        error = gsva_route_rotate_token(routes, key, token_id,
                                        new_token_value);
        if (error != GSVA_OK) {
            return error;
        }
        route_pending = true;
    }
    error = gsva_home_rotate_token(home, local_cna, key, token_id,
                                   new_token_value);
    home_rotated = error == GSVA_OK;
    if (error == GSVA_OK && route) {
        error = gsva_route_ack_token_revoke(routes, key, token_id,
                                            new_token_value,
                                            requester_cna);
    }
    if (error == GSVA_OK) {
        return GSVA_OK;
    }
    if (route_pending) {
        (void)gsva_route_abort_token_revoke(routes, key, token_id,
                                            new_token_value);
    }
    if (home_rotated) {
        (void)gsva_home_rotate_token(home, local_cna, key, token_id,
                                     old_token_value);
    }
    return error;
}

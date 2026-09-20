#include "qemu/osdep.h"
#include "hw/ub/gsva_token_rotation.h"

void memory_region_init_io(MemoryRegion *mr, Object *owner,
                           const MemoryRegionOps *ops, void *opaque,
                           const char *name, uint64_t size)
{
    g_assert_not_reached();
}

void memory_region_del_subregion(MemoryRegion *parent, MemoryRegion *mr)
{
    g_assert_not_reached();
}

static const uint32_t home_cna = 0xc4c2;
static const GsvaKeyV1 key = {
    .version = 1,
    .segment_id = 0xc4c2000000000001,
    .home_va = 0x700000000000,
    .size = 0x200000,
    .epoch = 1,
};

static void bind_home(GsvaHomeTable *home)
{
    GsvaHomeRequest request = {
        .version = 1,
        .operation = GSVA_HOME_BIND,
        .identity = {
            .key = key,
            .home_cna = home_cna,
            .token_id = 2,
            .token_value = 3,
            .backing_token_id = 17,
        },
        .export_mem_id = 9,
        .access_flags = GSVA_HOME_ACCESS_READ | GSVA_HOME_ACCESS_WRITE,
    };

    g_assert_cmpint(gsva_home_update(home, home_cna, &request), ==, GSVA_OK);
}

static void add_route(GsvaRouteTable *routes, GsvaRouteEntry *route)
{
    memset(route, 0, sizeof(*route));
    route->key = key;
    route->state = GSVA_ROUTE_ACTIVE;
    route->home_cna = home_cna;
    route->token.token_id = 2;
    route->token.token_value = 3;
    route->token.state = GSVA_TOKEN_ACTIVE;
    route->token.active = true;
    QTAILQ_INSERT_TAIL(&routes->routes, route, next);
    routes->route_count++;
}

static void init_tables(GsvaHomeTable *home, GsvaRouteTable *routes)
{
    memset(home, 0, sizeof(*home));
    gsva_route_table_init(routes);
    bind_home(home);
}

static void destroy_tables(GsvaHomeTable *home, GsvaRouteTable *routes,
                           GsvaRouteEntry *route)
{
    if (route) {
        QTAILQ_REMOVE(&routes->routes, route, next);
        routes->route_count--;
    }
    gsva_home_destroy(home);
}

static void assert_home_token(GsvaHomeTable *home, uint32_t value)
{
    uint32_t actual = 0;

    g_assert_cmpint(gsva_home_token_value(home, home_cna, &key, 2, &actual),
                    ==, GSVA_OK);
    g_assert_cmpuint(actual, ==, value);
}

static void test_home_without_import_route(void)
{
    GsvaHomeTable home;
    GsvaRouteTable routes;

    init_tables(&home, &routes);
    g_assert_cmpint(gsva_token_rotation_apply_home(
                        &home, home_cna, &routes, &key, 0xc4d2, 2, 7, false),
                    ==, GSVA_OK);
    assert_home_token(&home, 7);
    destroy_tables(&home, &routes, NULL);
}

static void test_local_pending_route_commits(void)
{
    GsvaHomeTable home;
    GsvaRouteTable routes;
    GsvaRouteEntry route;

    init_tables(&home, &routes);
    add_route(&routes, &route);
    g_assert_cmpint(gsva_route_rotate_token(&routes, &key, 2, 7), ==,
                    GSVA_OK);
    g_assert_cmpint(gsva_token_rotation_apply_home(
                        &home, home_cna, &routes, &key, home_cna,
                        2, 7, true),
                    ==, GSVA_OK);
    assert_home_token(&home, 7);
    g_assert_true(route.token.active);
    g_assert_cmpint(route.token.state, ==, GSVA_TOKEN_ACTIVE);
    g_assert_cmpuint(route.token.token_value, ==, 7);
    g_assert_cmpuint(route.token.pending_token_value, ==, 0);
    destroy_tables(&home, &routes, &route);
}

static void test_remote_route_commits(void)
{
    GsvaHomeTable home;
    GsvaRouteTable routes;
    GsvaRouteEntry route;

    init_tables(&home, &routes);
    add_route(&routes, &route);
    g_assert_cmpint(gsva_token_rotation_apply_home(
                        &home, home_cna, &routes, &key, 0xc4d2,
                        2, 7, false),
                    ==, GSVA_OK);
    assert_home_token(&home, 7);
    g_assert_true(route.token.active);
    g_assert_cmpuint(route.token.token_value, ==, 7);
    destroy_tables(&home, &routes, &route);
}

static void test_pinned_home_aborts_pending_route(void)
{
    GsvaHomeTable home;
    GsvaRouteTable routes;
    GsvaRouteEntry route;
    GsvaHomeIdentity identity = {
        .key = key,
        .home_cna = home_cna,
        .token_id = 2,
        .token_value = 3,
        .backing_token_id = 17,
    };
    GsvaHomeBinding *pin = NULL;

    init_tables(&home, &routes);
    add_route(&routes, &route);
    g_assert_cmpint(gsva_home_acquire(&home, home_cna, &identity,
                                     key.home_va, 8, false, &pin), ==,
                    GSVA_OK);
    g_assert_cmpint(gsva_route_rotate_token(&routes, &key, 2, 7), ==,
                    GSVA_OK);
    g_assert_cmpint(gsva_token_rotation_apply_home(
                        &home, home_cna, &routes, &key, home_cna,
                        2, 7, true),
                    ==, GSVA_ERR_COH_PENDING);
    assert_home_token(&home, 3);
    g_assert_true(route.token.active);
    g_assert_cmpint(route.token.state, ==, GSVA_TOKEN_ACTIVE);
    g_assert_cmpuint(route.token.token_value, ==, 3);
    g_assert_cmpuint(route.token.pending_token_value, ==, 0);
    gsva_home_release(pin);
    destroy_tables(&home, &routes, &route);
}

static void test_stale_route_rejects_without_home_change(void)
{
    GsvaHomeTable home;
    GsvaRouteTable routes;
    GsvaRouteEntry route;

    init_tables(&home, &routes);
    add_route(&routes, &route);
    route.key.epoch++;
    g_assert_cmpint(gsva_token_rotation_apply_home(
                        &home, home_cna, &routes, &key, 0xc4d2,
                        2, 7, false),
                    ==, GSVA_ERR_STALE_EPOCH);
    assert_home_token(&home, 3);
    g_assert_cmpuint(route.token.token_value, ==, 3);
    destroy_tables(&home, &routes, &route);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/gsva-token-rotation/home-only",
                    test_home_without_import_route);
    g_test_add_func("/gsva-token-rotation/local-pending",
                    test_local_pending_route_commits);
    g_test_add_func("/gsva-token-rotation/remote-route",
                    test_remote_route_commits);
    g_test_add_func("/gsva-token-rotation/pinned-home",
                    test_pinned_home_aborts_pending_route);
    g_test_add_func("/gsva-token-rotation/stale-route",
                    test_stale_route_rejects_without_home_change);
    return g_test_run();
}

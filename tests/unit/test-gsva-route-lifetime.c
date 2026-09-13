/*
 * Real QOM and production route lifecycle, with a minimal memory API model.
 * Held owner references model delayed FlatView/DMA release deterministically.
 * Actual address-space/RCU integration is covered by the dual-guest ASan run.
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/ub/gsva_route.h"

static unsigned windows_destroyed;
static MemoryRegion container;

static void window_finalize(Object *obj)
{
    windows_destroyed++;
}

static const TypeInfo window_type = {
    .name = TYPE_MEMORY_REGION,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(MemoryRegion),
    .instance_finalize = window_finalize,
};

void memory_region_init_io(MemoryRegion *mr, Object *owner,
                           const MemoryRegionOps *ops, void *opaque,
                           const char *name, uint64_t size)
{
    object_initialize(mr, sizeof(*mr), TYPE_MEMORY_REGION);
    mr->owner = owner;
    mr->ops = ops;
    mr->opaque = opaque;
    object_property_add_child(owner, name, OBJECT(mr));
    object_unref(OBJECT(mr));
}

void memory_region_del_subregion(MemoryRegion *parent, MemoryRegion *mr)
{
    g_assert_true(parent == &container && mr->container == parent);
    mr->container = NULL;
    object_unref(mr->owner);
}

static GsvaKeyV1 key = {
    .version = 1, .segment_id = 1, .home_va = 0x700000000000,
    .size = 4096, .epoch = 1,
};

static GsvaRouteEntry *add_route(GsvaRouteTable *table, bool window)
{
    uint64_t id;
    GsvaRouteEntry *route;

    g_assert_cmpint(gsva_route_map(table, &key, 0x40000000000,
        key.home_va, key.home_va, 1, GSVA_ADDRESS_PROFILE_STRICT_GSVA,
        7, 2, 3, 3, &id), ==, GSVA_OK);
    route = gsva_route_lookup_base(table, &key);
    g_assert_nonnull(route);
    if (window) {
        gsva_route_init_cpu_window(route, NULL);
        g_assert_true(route->cpu_window.owner == OBJECT(route));
        g_assert_true(route->cpu_window.opaque == route);
        route->cpu_window.container = &container;
        route->cpu_window_mapped = true;
        object_ref(OBJECT(route)); /* Address-space subregion ownership. */
    }
    return route;
}

static void test_delayed_release(void)
{
    GsvaRouteTable table;
    GsvaRouteEntry *route;
    MemoryRegion *window;
    unsigned before = windows_destroyed;

    gsva_route_table_init(&table);
    route = add_route(&table, true);
    window = &route->cpu_window;
    object_ref(window->owner); /* Old FlatView. */
    object_ref(window->owner); /* Outstanding DMA reference. */
    g_assert_cmpint(gsva_route_unmap(&table, route->map_id, false), ==, GSVA_OK);
    g_assert_cmpint(table.route_count, ==, 0);
    g_assert_null(gsva_route_lookup_base(&table, &key));
    g_assert_cmpint(route->state, ==, GSVA_ROUTE_RETIRED);
    g_assert_false(route->cpu_window_mapped);
    g_assert_null(window->container);
    g_assert_true(window->owner == OBJECT(route));
    g_assert_cmpuint(windows_destroyed, ==, before);
    object_unref(window->owner);
    g_assert_cmpuint(windows_destroyed, ==, before);
    object_unref(window->owner);
    g_assert_cmpuint(windows_destroyed, ==, before + 1);
    gsva_route_table_destroy(&table);
}

static void test_tombstone_replacement(void)
{
    GsvaRouteTable table;
    GsvaRouteEntry *route, *replacement;
    unsigned before = windows_destroyed;
    uint64_t id;

    gsva_route_table_init(&table);
    route = add_route(&table, true);
    object_ref(OBJECT(route));
    g_assert_cmpint(gsva_route_unmap(&table, route->map_id, true), ==, GSVA_OK);
    g_assert_cmpint(gsva_route_unmap(&table, route->map_id, true), ==, GSVA_OK);
    g_assert_cmpint(table.tombstone_count, ==, 1);
    g_assert_cmpint(gsva_route_map(&table, &key, 0, key.home_va, key.home_va,
        1, GSVA_ADDRESS_PROFILE_STRICT_GSVA, 7, 2, 3, 3, &id), ==,
        GSVA_ERR_STALE_EPOCH);
    key.epoch++;
    replacement = add_route(&table, true);
    g_assert_true(replacement != route);
    g_assert_cmpuint(route->key.epoch + 1, ==, replacement->key.epoch);
    g_assert_cmpuint(windows_destroyed, ==, before);
    object_unref(OBJECT(route));
    g_assert_cmpuint(windows_destroyed, ==, before + 1);
    gsva_route_table_destroy(&table);
    g_assert_cmpuint(windows_destroyed, ==, before + 2);
    key.epoch--;
}

static void test_table_destroy_with_reference(void)
{
    GsvaRouteTable table;
    GsvaRouteEntry *route;
    unsigned before = windows_destroyed;

    gsva_route_table_init(&table);
    route = add_route(&table, true);
    object_ref(OBJECT(route));
    gsva_route_table_destroy(&table);
    g_assert_cmpint(table.route_count, ==, 0);
    g_assert_false(route->cpu_window_mapped);
    g_assert_cmpuint(windows_destroyed, ==, before);
    object_unref(OBJECT(route));
    g_assert_cmpuint(windows_destroyed, ==, before + 1);
}

static void test_route_without_window(void)
{
    GsvaRouteTable table;
    GsvaRouteEntry *route;
    unsigned before = windows_destroyed;
    uint64_t id;

    gsva_route_table_init(&table);
    route = add_route(&table, false);
    id = route->map_id;
    g_assert_cmpint(gsva_route_unmap(&table, id, false), ==, GSVA_OK);
    g_assert_cmpint(gsva_route_unmap(&table, id, false), ==, GSVA_ERR_ROUTE_MISSING);
    route = add_route(&table, false);
    g_assert_cmpint(gsva_route_unmap(&table, route->map_id, true), ==, GSVA_OK);
    gsva_route_table_destroy(&table);
    g_assert_cmpuint(windows_destroyed, ==, before);
}

int main(int argc, char **argv)
{
    module_call_init(MODULE_INIT_QOM);
    type_register_static(&window_type);
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/gsva-route/delayed-release", test_delayed_release);
    g_test_add_func("/gsva-route/tombstone-replacement", test_tombstone_replacement);
    g_test_add_func("/gsva-route/table-destroy", test_table_destroy_with_reference);
    g_test_add_func("/gsva-route/no-window", test_route_without_window);
    return g_test_run();
}

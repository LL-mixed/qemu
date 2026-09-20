/*
 * Real QOM and production route lifecycle, with a minimal memory API model.
 * Held owner references model delayed FlatView/DMA release deterministically.
 * Actual address-space/RCU integration is covered by the dual-guest ASan run.
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/ub/gsva_route.h"
#include "hw/ub/ub_ubc.h"

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

static void test_pto_range_and_permissions(void)
{
    GsvaRouteTable table;
    GsvaRouteEntry *route;
    GsvaRouteAccess access;
    uint64_t pa = 0x40000000000;

    gsva_route_table_init(&table);
    route = add_route(&table, true);
    g_assert_cmpint(gsva_route_resolve_pto(&table, pa, 4096, 7, &access), ==,
                    GSVA_ERR_FEATURE_MISSING);
    route->backing_token_id = 11;
    g_assert_cmpint(gsva_route_resolve_pto(&table, pa, 4096, 7, &access), ==, 0);
    g_assert_cmpuint(access.backing_token_id, ==, 11);
    g_assert_cmpuint(access.token_id, ==, 2);
    g_assert_cmpuint(access.key.home_va, ==, key.home_va);
    g_assert_cmpint(gsva_route_resolve_pto(&table, pa + 4095, 1, 7, &access), ==, 0);
    g_assert_cmpuint(access.local_pa, ==, pa);
    g_assert_cmpint(gsva_route_resolve_pto(&table, pa + 4095, 2, 7, &access), ==,
                    GSVA_ERR_KEY_MISMATCH);
    g_assert_cmpint(gsva_route_resolve_pto(&table, pa, 0, 7, &access), ==,
                    GSVA_ERR_KEY_MISMATCH);
    g_assert_cmpint(gsva_route_resolve_pto(&table, UINT64_MAX, 2, 7, &access), ==,
                    GSVA_ERR_KEY_MISMATCH);
    g_assert_cmpint(gsva_route_resolve_pto(&table, pa + 4096, 1, 7, &access), ==,
                    GSVA_ERR_ROUTE_MISSING);
    route->token.access_flags = 1;
    g_assert_cmpint(gsva_route_resolve_pto(&table, pa, 4096, 7, &access), ==, 0);
    g_assert_cmpuint(access.access_flags, ==, 1);
    route->token.allowed_cna_bitmap = 1ULL << 7;
    g_assert_cmpint(gsva_route_resolve_pto(&table, pa, 1, 7, &access), ==, 0);
    g_assert_cmpint(gsva_route_resolve_pto(&table, pa, 1, 6, &access), ==,
                    GSVA_ERR_TOKEN_DENIED);
    g_assert_cmpint(gsva_route_resolve_pto(&table, pa, 1, 71, &access), ==,
                    GSVA_ERR_TOKEN_DENIED);
    route->token.access_flags = 0;
    g_assert_cmpint(gsva_route_resolve_pto(&table, pa, 1, 7, &access), ==,
                    GSVA_ERR_TOKEN_DENIED);
    route->token.access_flags = 4;
    g_assert_cmpint(gsva_route_resolve_pto(&table, pa, 1, 7, &access), ==,
                    GSVA_ERR_TOKEN_DENIED);
    gsva_route_table_destroy(&table);
}

static void test_pto_direct_home_backing(void)
{
    GsvaRouteTable table;
    GsvaRouteEntry *route;
    GsvaRouteAccess access;

    gsva_route_table_init(&table);
    route = add_route(&table, false);
    route->backing_token_id = 11;
    g_assert_cmpint(gsva_route_resolve_pto(
        &table, route->local_pa, 4096, 7, &access), ==,
        GSVA_ERR_FEATURE_MISSING);
    route->direct_home_backing = true;
    g_assert_cmpint(gsva_route_resolve_pto(
        &table, route->local_pa, 4096, 7, &access), ==, GSVA_OK);
    g_assert_true(access.direct_home_backing);

    gsva_route_init_cpu_window(route, NULL);
    route->cpu_window.container = &container;
    route->cpu_window_mapped = true;
    object_ref(OBJECT(route));
    g_assert_cmpint(gsva_route_resolve_pto(
        &table, route->local_pa, 4096, 7, &access), ==,
        GSVA_ERR_FEATURE_MISSING);
    gsva_route_table_destroy(&table);
}

static void test_pto_identity_snapshot(void)
{
    GsvaRouteTable table;
    GsvaRouteEntry *route;
    GsvaRouteAccess original, copy;

    gsva_route_table_init(&table);
    route = add_route(&table, true);
    route->backing_token_id = 11;
    g_assert_cmpint(gsva_route_resolve_pto(&table, route->local_pa, 4096, 7,
                                         &original), ==, 0);
    copy = original;
    g_assert_true(gsva_route_access_equal(&original, &copy));
#define CHECK_IDENTITY_FIELD(field) do { \
    copy = original; \
    copy.field ^= 1; \
    g_assert_false(gsva_route_access_equal(&original, &copy)); \
} while (0)
    CHECK_IDENTITY_FIELD(key.version);
    CHECK_IDENTITY_FIELD(key.flags);
    CHECK_IDENTITY_FIELD(key.segment_id);
    CHECK_IDENTITY_FIELD(key.home_va);
    CHECK_IDENTITY_FIELD(key.size);
    CHECK_IDENTITY_FIELD(key.vmid);
    CHECK_IDENTITY_FIELD(key.asid);
    CHECK_IDENTITY_FIELD(key.pte_offset);
    CHECK_IDENTITY_FIELD(key.p_tag);
    CHECK_IDENTITY_FIELD(key.cache_policy);
    CHECK_IDENTITY_FIELD(key.epoch);
    CHECK_IDENTITY_FIELD(map_id);
    CHECK_IDENTITY_FIELD(local_pa);
    CHECK_IDENTITY_FIELD(lease_epoch);
    CHECK_IDENTITY_FIELD(allowed_cna_bitmap);
    CHECK_IDENTITY_FIELD(home_cna);
    CHECK_IDENTITY_FIELD(owner_cna);
    CHECK_IDENTITY_FIELD(source);
    CHECK_IDENTITY_FIELD(token_id);
    CHECK_IDENTITY_FIELD(token_value);
    CHECK_IDENTITY_FIELD(access_flags);
    CHECK_IDENTITY_FIELD(token_flags);
    CHECK_IDENTITY_FIELD(backing_token_id);
    CHECK_IDENTITY_FIELD(direct_home_backing);
#undef CHECK_IDENTITY_FIELD
    g_assert_false(gsva_route_access_equal(NULL, &original));
    gsva_route_table_destroy(&table);
}

static void test_pto_live_revalidation(void)
{
    GsvaRouteTable table;
    GsvaRouteEntry *route;
    GsvaRouteAccess original, current;
    uint64_t pa = 0x40000000000;

    gsva_route_table_init(&table);
    route = add_route(&table, true);
    route->backing_token_id = 11;
    g_assert_cmpint(gsva_route_resolve_pto(&table, pa, 4096, 7, &original), ==, 0);
    g_assert_cmpint(gsva_route_rotate_token(&table, &key, 2, 4), ==, 0);
    g_assert_cmpint(gsva_route_resolve_pto(&table, pa, 4096, 7, &current), ==,
                    GSVA_ERR_TOKEN_DENIED);
    g_assert_cmpint(gsva_route_ack_token_revoke(&table, &key, 2, 4, 7), ==, 0);
    g_assert_cmpint(gsva_route_ack_token_revoke(&table, &key, 2, 4, 7), ==, 0);
    g_assert_cmpint(gsva_route_resolve_pto(&table, pa, 4096, 7, &current), ==, 0);
    g_assert_false(gsva_route_access_equal(&original, &current));
    route->backing_token_id++;
    g_assert_cmpint(gsva_route_resolve_pto(&table, pa, 4096, 7, &current), ==, 0);
    g_assert_false(gsva_route_access_equal(&original, &current));
    route->state = GSVA_ROUTE_STALE;
    g_assert_cmpint(gsva_route_resolve_pto(&table, pa, 1, 7, &current), ==,
                    GSVA_ERR_TOKEN_DENIED);
    route->state = GSVA_ROUTE_ACTIVE;
    g_assert_cmpint(gsva_route_unmap(&table, route->map_id, true), ==, 0);
    g_assert_cmpint(gsva_route_resolve_pto(&table, pa, 1, 7, &current), ==,
                    GSVA_ERR_ROUTE_MISSING);
    key.epoch++;
    route = add_route(&table, true);
    route->backing_token_id = 11;
    g_assert_cmpint(gsva_route_resolve_pto(&table, pa, 4096, 7, &current), ==, 0);
    g_assert_false(gsva_route_access_equal(&original, &current));
    gsva_route_table_destroy(&table);
    key.epoch--;
}

static void test_pto_ambiguous_pa(void)
{
    GsvaRouteTable table;
    GsvaRouteEntry *route;
    GsvaRouteAccess access;
    uint64_t pa = 0x40000000000;

    gsva_route_table_init(&table);
    route = add_route(&table, true);
    route->backing_token_id = 11;
    key.home_va += 4096;
    route = add_route(&table, true);
    route->backing_token_id = 12;
    g_assert_cmpint(gsva_route_resolve_pto(&table, pa, 1, 7, &access), ==,
                    GSVA_ERR_KEY_MISMATCH);
    gsva_route_table_destroy(&table);
    key.home_va -= 4096;
}

static void test_pto_vma_access_intersection(void)
{
    UbcObmmResolvedMap map = { .strict_gsva = true, .pto_access = 3 };
    uint32_t access = 0;

    g_assert_true(ubc_obmm_strict_pto_access(&map, 1, &access));
    g_assert_cmpuint(access, ==, 1);
    g_assert_true(ubc_obmm_strict_pto_access(&map, 2, &access));
    g_assert_cmpuint(access, ==, 2);
    g_assert_true(ubc_obmm_strict_pto_access(&map, 3, &access));
    g_assert_cmpuint(access, ==, 3);
    g_assert_false(ubc_obmm_strict_pto_access(&map, 0, &access));
    g_assert_false(ubc_obmm_strict_pto_access(&map, 4, &access));
    g_assert_false(ubc_obmm_strict_pto_access(&map, UINT64_MAX, &access));
    map.pto_access = 1;
    g_assert_true(ubc_obmm_strict_pto_access(&map, 3, &access));
    g_assert_cmpuint(access, ==, 1);
    g_assert_false(ubc_obmm_strict_pto_access(&map, 2, &access));
    map.strict_gsva = false;
    g_assert_false(ubc_obmm_strict_pto_access(&map, 3, &access));
}

static void test_quarantine_owns_interval(void)
{
    GsvaRouteTable table;
    GsvaRouteEntry *route;
    GsvaRouteAccess access;
    GsvaKeyV1 replacement = key;
    uint64_t id = 0;

    gsva_route_table_init(&table);
    route = add_route(&table, true);
    route->backing_token_id = 97;
    route->state = GSVA_ROUTE_STALE;
    g_assert_cmpint(gsva_route_validate_token(route, 7, 2, 3, 1), !=, GSVA_OK);
    g_assert_cmpint(gsva_route_resolve_pto(&table, route->local_pa, 8, 7,
                                          &access), !=, GSVA_OK);
    replacement.segment_id++;
    g_assert_cmpint(gsva_route_map(&table, &replacement, 0x40000000000,
        key.home_va, key.home_va, 1, GSVA_ADDRESS_PROFILE_STRICT_GSVA,
        7, 4, 5, 3, &id), ==, GSVA_ERR_KEY_MISMATCH);
    g_assert_true(route->cpu_window_mapped);
    g_assert_cmpint(table.route_count, ==, 1);
    g_assert_cmpint(gsva_route_unmap(&table, route->map_id, false), ==, GSVA_OK);
    g_assert_cmpint(gsva_route_map(&table, &replacement, 0x40000000000,
        key.home_va, key.home_va, 1, GSVA_ADDRESS_PROFILE_STRICT_GSVA,
        7, 4, 5, 3, &id), ==, GSVA_OK);
    gsva_route_table_destroy(&table);
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
    g_test_add_func("/gsva-route/pto-range-permissions", test_pto_range_and_permissions);
    g_test_add_func("/gsva-route/pto-direct-home", test_pto_direct_home_backing);
    g_test_add_func("/gsva-route/pto-identity", test_pto_identity_snapshot);
    g_test_add_func("/gsva-route/pto-live-revalidation", test_pto_live_revalidation);
    g_test_add_func("/gsva-route/pto-ambiguous-pa", test_pto_ambiguous_pa);
    g_test_add_func("/gsva-route/pto-vma-access", test_pto_vma_access_intersection);
    g_test_add_func("/gsva-route/quarantine-interval", test_quarantine_owns_interval);
    return g_test_run();
}

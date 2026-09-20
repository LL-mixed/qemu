#include "qemu/osdep.h"
#include "hw/ub/gsva_home.h"

static const uint32_t home_cna = 0xc4c2;
static const GsvaKeyV1 key = {
    .version = 1,
    .segment_id = 0xc4c2000000000001,
    .home_va = 0x700000000000,
    .size = 0x200000,
    .epoch = 1,
};

static GsvaHomeRequest request_with_token(uint32_t token_value)
{
    GsvaHomeRequest request = {
        .version = 1,
        .operation = GSVA_HOME_BIND,
        .identity = {
            .key = key,
            .home_cna = home_cna,
            .token_id = 2,
            .token_value = token_value,
            .backing_token_id = 17,
        },
        .export_mem_id = 9,
        .access_flags = GSVA_HOME_ACCESS_READ | GSVA_HOME_ACCESS_WRITE,
    };

    return request;
}

static void test_token_rotation_updates_authoritative_identity(void)
{
    GsvaHomeTable table = {0};
    GsvaHomeRequest request = request_with_token(3);
    GsvaHomeIdentity old_identity = request.identity;
    GsvaHomeIdentity new_identity = request.identity;
    GsvaHomeBinding *pin = NULL;

    g_assert_cmpint(gsva_home_update(&table, home_cna, &request), ==, GSVA_OK);
    g_assert_cmpint(gsva_home_acquire(&table, home_cna, &old_identity,
                                     key.home_va, 8, true, &pin), ==, GSVA_OK);
    gsva_home_release(pin);

    g_assert_cmpint(gsva_home_rotate_token(&table, home_cna, &key, 2, 7),
                    ==, GSVA_OK);
    g_assert_cmpint(gsva_home_acquire(&table, home_cna, &old_identity,
                                     key.home_va, 8, true, &pin),
                    ==, GSVA_ERR_ROUTE_MISSING);
    new_identity.token_value = 7;
    g_assert_cmpint(gsva_home_acquire(&table, home_cna, &new_identity,
                                     key.home_va, 8, true, &pin), ==, GSVA_OK);
    gsva_home_release(pin);
    gsva_home_destroy(&table);
}

static void test_token_rotation_fails_while_pinned(void)
{
    GsvaHomeTable table = {0};
    GsvaHomeRequest request = request_with_token(3);
    GsvaHomeBinding *pin = NULL;

    g_assert_cmpint(gsva_home_update(&table, home_cna, &request), ==, GSVA_OK);
    g_assert_cmpint(gsva_home_acquire(&table, home_cna, &request.identity,
                                     key.home_va, 8, false, &pin), ==, GSVA_OK);
    g_assert_cmpint(gsva_home_rotate_token(&table, home_cna, &key, 2, 7),
                    ==, GSVA_ERR_COH_PENDING);
    gsva_home_release(pin);
    gsva_home_destroy(&table);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/gsva-home/token-rotation",
                    test_token_rotation_updates_authoritative_identity);
    g_test_add_func("/gsva-home/token-rotation-pinned",
                    test_token_rotation_fails_while_pinned);
    return g_test_run();
}

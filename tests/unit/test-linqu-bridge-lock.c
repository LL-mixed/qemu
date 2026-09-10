/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/ub/linqu_bridge_lock.h"

static QemuMutex bql;
static __thread bool owns_bql;
static LinquBridgeLock bridge;
static QemuSemaphore waiting;
static unsigned counter;

bool qemu_mutex_iothread_locked(void)
{
    return owns_bql;
}

void qemu_mutex_lock_iothread_impl(const char *file, int line)
{
    g_assert_false(owns_bql);
    qemu_mutex_lock(&bql);
    owns_bql = true;
}

void qemu_mutex_unlock_iothread(void)
{
    g_assert_true(owns_bql);
    owns_bql = false;
    qemu_mutex_unlock(&bql);
}

static void *increment(void *opaque)
{
    unsigned i;

    for (i = 0; i < 10000; i++) {
        g_autoptr(LinquBridgeLock) guard = linqu_bridge_lock(&bridge);
        g_autoptr(LinquBridgeLock) nested = linqu_bridge_lock(&bridge);
        unsigned previous = counter;

        if (i % 100 == 0) {
            g_usleep(1);
        }
        counter = previous + 1;
    }
    return NULL;
}

static void test_exclusive_nested_ownership(void)
{
    QemuThread threads[8];
    unsigned i;

    counter = 0;
    for (i = 0; i < G_N_ELEMENTS(threads); i++) {
        qemu_thread_create(&threads[i], "bridge-owner", increment, NULL,
                           QEMU_THREAD_JOINABLE);
    }
    for (i = 0; i < G_N_ELEMENTS(threads); i++) {
        qemu_thread_join(&threads[i]);
    }
    g_assert_cmpuint(counter, ==, 80000);
}

static void *contend_with_bql(void *opaque)
{
    qemu_mutex_lock_iothread();
    qemu_sem_post(&waiting);
    {
        g_autoptr(LinquBridgeLock) guard = linqu_bridge_lock(&bridge);

        g_assert_true(qemu_mutex_iothread_locked());
        counter++;
    }
    qemu_mutex_unlock_iothread();
    return NULL;
}

static void test_callback_progress(void)
{
    QemuThread contender;
    LinquBridgeLock *guard;

    counter = 0;
    qemu_mutex_lock_iothread();
    guard = linqu_bridge_lock(&bridge);
    qemu_mutex_unlock_iothread();
    qemu_thread_create(&contender, "bridge-waiter", contend_with_bql, NULL,
                       QEMU_THREAD_JOINABLE);
    g_assert_cmpint(qemu_sem_timedwait(&waiting, 5000), ==, 0);
    /* The waiting caller owns the BQL. It must drop it while waiting for
     * the bridge, allowing a PTO callback and the original owner to finish.
     * The test runner timeout catches a lock-order regression here.
     */
    qemu_mutex_lock_iothread();
    g_assert_cmpuint(counter, ==, 0);
    g_clear_pointer(&guard, qemu_rec_mutex_unlock);
    qemu_mutex_unlock_iothread();
    qemu_thread_join(&contender);
    g_assert_cmpuint(counter, ==, 1);
}

int main(int argc, char **argv)
{
    int result;

    g_test_init(&argc, &argv, NULL);
    qemu_mutex_init(&bql);
    qemu_rec_mutex_init(&bridge);
    qemu_sem_init(&waiting, 0);
    g_test_add_func("/linqu-bridge/exclusive-nested",
                    test_exclusive_nested_ownership);
    g_test_add_func("/linqu-bridge/bql-callback", test_callback_progress);
    result = g_test_run();
    qemu_sem_destroy(&waiting);
    qemu_rec_mutex_destroy(&bridge);
    qemu_mutex_destroy(&bql);
    return result;
}

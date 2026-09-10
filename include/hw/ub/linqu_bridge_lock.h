/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LINQU_BRIDGE_LOCK_H
#define LINQU_BRIDGE_LOCK_H

#include "qemu/thread.h"
#include "qemu/main-loop.h"

typedef QemuRecMutex LinquBridgeLock;

/* Keep bridge ownership across BQL handoffs to synchronous PTO workers.
 * A waiter must leave the BQL available for the owner's worker callbacks.
 */
static inline LinquBridgeLock *linqu_bridge_lock(LinquBridgeLock *lock)
{
    bool had_bql;

    if (qemu_rec_mutex_trylock(lock) == 0) {
        return lock;
    }
    had_bql = qemu_mutex_iothread_locked();
    if (had_bql) {
        qemu_mutex_unlock_iothread();
    }
    qemu_rec_mutex_lock(lock);
    if (had_bql) {
        qemu_mutex_lock_iothread();
    }
    return lock;
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(LinquBridgeLock, qemu_rec_mutex_unlock)

#endif

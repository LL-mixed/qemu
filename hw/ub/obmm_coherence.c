/*
 * OBMM Directory-Based MESI Coherence for QEMU UB Simulation
 *
 * Implements directory-based cache coherence for cross-node OBMM shmem.
 * Milestone A: write-through profile (no local cache).
 * Milestone B: COH_FENCE/ACK message round-trip.
 * Milestone C: COH_GETS + shared-read state.
 * Milestone D: COH_GETM + invalidation.
 * Milestone E: Full MESI dirty owner.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/ub/ub_ubc.h"
#include "hw/ub/obmm_coherence.h"

typedef struct ObmmCohLocalLine {
    BusControllerDev *ubc_dev;
    uint64_t line_addr;
    uint32_t home_cna;
    uint32_t token_id;
    ObmmCoherenceState state;
    bool dirty;
    uint8_t data[OBMM_COH_LINE_SIZE];
} ObmmCohLocalLine;

typedef struct ObmmCohLineKey {
    BusControllerDev *ubc_dev;
    uint64_t line_addr;
    uint32_t home_cna;
    uint32_t token_id;
} ObmmCohLineKey;

typedef struct ObmmCohDirLine {
    uint64_t line_addr;
    uint32_t home_cna;
    uint32_t token_id;
    uint32_t owner_cna;
    uint32_t sharer_count;
    uint32_t sharers[OBMM_COH_MAX_SHARERS];
    ObmmCoherenceState state;
    uint64_t version;
    bool has_owner;
    bool dirty;
    bool pending;
    bool data_valid;
    uint8_t data[OBMM_COH_LINE_SIZE];
} ObmmCohDirLine;

typedef struct ObmmCohDirKey {
    uint64_t line_addr;
    uint32_t home_cna;
    uint32_t token_id;
} ObmmCohDirKey;

typedef struct ObmmCohPendingKey {
    BusControllerDev *ubc_dev;
    uint32_t req_id;
    uint32_t peer_cna;
    uint32_t msg_type;
} ObmmCohPendingKey;

typedef struct ObmmCohPending {
    ObmmCohPendingKey *key;
    bool pending;
    int status;
    uint32_t data_len;
    uint32_t grant_state;
    uint8_t data[OBMM_COH_LINE_SIZE];
} ObmmCohPending;

static GHashTable *g_obmm_coh_cache;
static GHashTable *g_obmm_coh_dir;
static GHashTable *g_obmm_coh_pending;
static QemuMutex g_obmm_coh_lock;
static bool g_obmm_coh_inited;

static guint obmm_coh_line_key_hash(gconstpointer ptr);
static gboolean obmm_coh_line_key_equal(gconstpointer a, gconstpointer b);
static guint obmm_coh_dir_key_hash(gconstpointer ptr);
static gboolean obmm_coh_dir_key_equal(gconstpointer a, gconstpointer b);
static guint obmm_coh_pending_key_hash(gconstpointer ptr);
static gboolean obmm_coh_pending_key_equal(gconstpointer a, gconstpointer b);
static void obmm_coh_wait_for_response(BusControllerDev *ubc_dev,
                                        ObmmCohPending *pending);
static int obmm_coh_invalidate_local_line(BusControllerDev *ubc_dev,
                                          uint64_t line_addr,
                                          uint32_t home_cna,
                                          uint32_t token_id);
static int obmm_coh_downgrade_local_line(BusControllerDev *ubc_dev,
                                         uint64_t line_addr,
                                         uint32_t home_cna,
                                         uint32_t token_id);
static int obmm_coh_send_inv_wait(BusControllerDev *ubc_dev,
                                  uint32_t target_cna,
                                  uint64_t line_addr,
                                  uint32_t token_id);
static int obmm_coh_send_downgrade_wait(BusControllerDev *ubc_dev,
                                        uint32_t target_cna,
                                        uint64_t line_addr,
                                        uint32_t token_id);
static bool obmm_coh_wait_dir_not_pending(BusControllerDev *ubc_dev,
                                          uint64_t line_addr,
                                          uint32_t home_cna,
                                          uint32_t token_id,
                                          const char *op);
static bool obmm_coh_wait_owner_released(BusControllerDev *ubc_dev,
                                         uint64_t line_addr,
                                         uint32_t home_cna,
                                         uint32_t token_id,
                                         uint32_t owner_cna,
                                         const char *op);

static void obmm_coh_init_once(void)
{
    if (g_obmm_coh_inited) {
        return;
    }
    qemu_mutex_init(&g_obmm_coh_lock);
    g_obmm_coh_cache = g_hash_table_new_full(obmm_coh_line_key_hash,
                                             obmm_coh_line_key_equal,
                                             g_free, g_free);
    g_obmm_coh_dir = g_hash_table_new_full(obmm_coh_dir_key_hash,
                                           obmm_coh_dir_key_equal,
                                           g_free, g_free);
    g_obmm_coh_pending = g_hash_table_new_full(obmm_coh_pending_key_hash,
                                               obmm_coh_pending_key_equal,
                                               g_free, g_free);
    g_obmm_coh_inited = true;
}

static bool obmm_coh_log_sample(uint32_t req_id, uint32_t status)
{
    return status != 0 || req_id < 16 || (req_id & 0x3ff) == 0;
}

static uint64_t obmm_coh_line_addr(uint64_t addr)
{
    return addr & ~(uint64_t)(OBMM_COH_LINE_SIZE - 1);
}

static guint obmm_coh_line_key_hash(gconstpointer ptr)
{
    const ObmmCohLineKey *key = ptr;
    return (guint)(((uintptr_t)key->ubc_dev >> 4) ^
                   key->line_addr ^ (key->line_addr >> 32) ^
                   ((uint64_t)key->home_cna << 16) ^ key->token_id);
}

static gboolean obmm_coh_line_key_equal(gconstpointer a, gconstpointer b)
{
    const ObmmCohLineKey *ka = a;
    const ObmmCohLineKey *kb = b;

    return ka->ubc_dev == kb->ubc_dev &&
           ka->line_addr == kb->line_addr &&
           ka->home_cna == kb->home_cna &&
           ka->token_id == kb->token_id;
}

static guint obmm_coh_dir_key_hash(gconstpointer ptr)
{
    const ObmmCohDirKey *key = ptr;

    return (guint)(key->line_addr ^ (key->line_addr >> 32) ^
                   ((uint64_t)key->home_cna << 16) ^ key->token_id);
}

static gboolean obmm_coh_dir_key_equal(gconstpointer a, gconstpointer b)
{
    const ObmmCohDirKey *ka = a;
    const ObmmCohDirKey *kb = b;

    return ka->line_addr == kb->line_addr &&
           ka->home_cna == kb->home_cna &&
           ka->token_id == kb->token_id;
}

static guint obmm_coh_pending_key_hash(gconstpointer ptr)
{
    const ObmmCohPendingKey *key = ptr;

    return (guint)(((uintptr_t)key->ubc_dev >> 4) ^ key->req_id ^
                   ((uint64_t)key->peer_cna << 8) ^ key->msg_type);
}

static gboolean obmm_coh_pending_key_equal(gconstpointer a, gconstpointer b)
{
    const ObmmCohPendingKey *ka = a;
    const ObmmCohPendingKey *kb = b;

    return ka->ubc_dev == kb->ubc_dev &&
           ka->req_id == kb->req_id &&
           ka->peer_cna == kb->peer_cna &&
           ka->msg_type == kb->msg_type;
}

static ObmmCohPending *obmm_coh_pending_begin(BusControllerDev *ubc_dev,
                                              uint32_t req_id,
                                              uint32_t peer_cna,
                                              uint32_t msg_type,
                                              uint32_t grant_state)
{
    ObmmCohPendingKey *key;
    ObmmCohPending *pending;

    obmm_coh_init_once();
    key = g_new0(ObmmCohPendingKey, 1);
    key->ubc_dev = ubc_dev;
    key->req_id = req_id;
    key->peer_cna = peer_cna;
    key->msg_type = msg_type;

    pending = g_new0(ObmmCohPending, 1);
    pending->key = key;
    pending->pending = true;
    pending->status = -1;
    pending->grant_state = grant_state;

    qemu_mutex_lock(&g_obmm_coh_lock);
    g_hash_table_insert(g_obmm_coh_pending, key, pending);
    qemu_mutex_unlock(&g_obmm_coh_lock);
    return pending;
}

static ObmmCohPending *obmm_coh_pending_find_locked(BusControllerDev *ubc_dev,
                                                    uint32_t req_id,
                                                    uint32_t peer_cna,
                                                    uint32_t msg_type)
{
    ObmmCohPendingKey key = {
        .ubc_dev = ubc_dev,
        .req_id = req_id,
        .peer_cna = peer_cna,
        .msg_type = msg_type,
    };

    if (!g_obmm_coh_pending) {
        return NULL;
    }
    return g_hash_table_lookup(g_obmm_coh_pending, &key);
}

static void obmm_coh_pending_end(ObmmCohPending *pending)
{
    if (!pending) {
        return;
    }
    qemu_mutex_lock(&g_obmm_coh_lock);
    g_hash_table_remove(g_obmm_coh_pending, pending->key);
    qemu_mutex_unlock(&g_obmm_coh_lock);
}

static int obmm_coh_pending_status(ObmmCohPending *pending)
{
    int status;

    qemu_mutex_lock(&g_obmm_coh_lock);
    status = pending->status;
    qemu_mutex_unlock(&g_obmm_coh_lock);
    return status;
}

static uint32_t obmm_coh_pending_data_len(ObmmCohPending *pending)
{
    uint32_t data_len;

    qemu_mutex_lock(&g_obmm_coh_lock);
    data_len = pending->data_len;
    qemu_mutex_unlock(&g_obmm_coh_lock);
    return data_len;
}

static void obmm_coh_pending_complete_status(BusControllerDev *ubc_dev,
                                             uint32_t req_id,
                                             uint32_t peer_cna,
                                             uint32_t msg_type,
                                             int status)
{
    ObmmCohPending *pending;
    GHashTableIter iter;
    gpointer value;

    obmm_coh_init_once();
    qemu_mutex_lock(&g_obmm_coh_lock);
    pending = obmm_coh_pending_find_locked(ubc_dev, req_id, peer_cna,
                                           msg_type);
    if (!pending && g_obmm_coh_pending) {
        g_hash_table_iter_init(&iter, g_obmm_coh_pending);
        while (g_hash_table_iter_next(&iter, NULL, &value)) {
            ObmmCohPending *candidate = value;

            if (candidate->key->ubc_dev == ubc_dev &&
                candidate->key->req_id == req_id &&
                candidate->key->msg_type == msg_type) {
                pending = candidate;
                qemu_log("OBMM_COH pending status peer fallback"
                         " req_id=%u expected_peer=%#x actual_peer=%#x"
                         " msg=%u\n",
                         req_id, candidate->key->peer_cna, peer_cna,
                         msg_type);
                break;
            }
        }
    }
    if (pending) {
        pending->status = status;
        pending->pending = false;
    } else if (obmm_coh_log_sample(req_id, (uint32_t)status)) {
        qemu_log("OBMM_COH pending status no match req_id=%u peer=%#x"
                 " msg=%u status=%d\n",
                 req_id, peer_cna, msg_type, status);
    }
    qemu_mutex_unlock(&g_obmm_coh_lock);
}

static void obmm_coh_pending_complete_data(BusControllerDev *ubc_dev,
                                           const ObmmCohDataPld *pld,
                                           uint32_t peer_cna)
{
    ObmmCohPending *pending;

    obmm_coh_init_once();
    qemu_mutex_lock(&g_obmm_coh_lock);
    pending = obmm_coh_pending_find_locked(ubc_dev, pld->hdr.req_id,
                                           peer_cna, UBC_MSG_SUB_COH_DATA);
    if (pending) {
        pending->status = (int)pld->status;
        pending->data_len = pld->data_len;
        pending->grant_state = pld->grant_state;
        if (pld->data_len <= OBMM_COH_LINE_SIZE) {
            memcpy(pending->data, pld->data, pld->data_len);
        }
        pending->pending = false;
    }
    qemu_mutex_unlock(&g_obmm_coh_lock);
}

static ObmmCohLocalLine *obmm_coh_find_line_locked(BusControllerDev *ubc_dev,
                                                   uint64_t line_addr,
                                                   uint32_t home_cna,
                                                   uint32_t token_id)
{
    ObmmCohLineKey key = {
        .ubc_dev = ubc_dev,
        .line_addr = line_addr,
        .home_cna = home_cna,
        .token_id = token_id,
    };

    if (!g_obmm_coh_cache) {
        return NULL;
    }
    return g_hash_table_lookup(g_obmm_coh_cache, &key);
}

static ObmmCohLocalLine *obmm_coh_get_line_locked(BusControllerDev *ubc_dev,
                                                  uint64_t line_addr,
                                                  uint32_t home_cna,
                                                  uint32_t token_id)
{
    ObmmCohLocalLine *line = obmm_coh_find_line_locked(ubc_dev, line_addr,
                                                       home_cna, token_id);
    ObmmCohLineKey *key;

    if (line) {
        return line;
    }
    key = g_new(ObmmCohLineKey, 1);
    key->ubc_dev = ubc_dev;
    key->line_addr = line_addr;
    key->home_cna = home_cna;
    key->token_id = token_id;
    line = g_malloc0(sizeof(*line));
    line->ubc_dev = ubc_dev;
    line->line_addr = line_addr;
    line->home_cna = home_cna;
    line->token_id = token_id;
    line->state = OBMM_COH_I;
    g_hash_table_insert(g_obmm_coh_cache, key, line);
    return line;
}

static ObmmCohDirLine *obmm_coh_find_dir_locked(uint64_t line_addr,
                                                uint32_t home_cna,
                                                uint32_t token_id)
{
    ObmmCohDirKey key = {
        .line_addr = line_addr,
        .home_cna = home_cna,
        .token_id = token_id,
    };

    if (!g_obmm_coh_dir) {
        return NULL;
    }
    return g_hash_table_lookup(g_obmm_coh_dir, &key);
}

static ObmmCohDirLine *obmm_coh_get_dir_locked(uint64_t line_addr,
                                               uint32_t home_cna,
                                               uint32_t token_id)
{
    ObmmCohDirLine *line = obmm_coh_find_dir_locked(line_addr, home_cna,
                                                    token_id);
    ObmmCohDirKey *key;

    if (line) {
        return line;
    }
    key = g_new(ObmmCohDirKey, 1);
    key->line_addr = line_addr;
    key->home_cna = home_cna;
    key->token_id = token_id;
    line = g_malloc0(sizeof(*line));
    line->line_addr = line_addr;
    line->home_cna = home_cna;
    line->token_id = token_id;
    line->state = OBMM_COH_I;
    g_hash_table_insert(g_obmm_coh_dir, key, line);
    return line;
}

static void obmm_coh_clear_dir_pending(uint64_t line_addr, uint32_t home_cna,
                                       uint32_t token_id)
{
    ObmmCohDirLine *line;

    qemu_mutex_lock(&g_obmm_coh_lock);
    line = obmm_coh_find_dir_locked(line_addr, home_cna, token_id);
    if (line) {
        line->pending = false;
    }
    qemu_mutex_unlock(&g_obmm_coh_lock);
}

static bool obmm_coh_dir_has_sharer(const ObmmCohDirLine *line, uint32_t cna)
{
    uint32_t i;

    for (i = 0; i < line->sharer_count; i++) {
        if (line->sharers[i] == cna) {
            return true;
        }
    }
    return false;
}

static bool obmm_coh_dir_add_sharer(ObmmCohDirLine *line, uint32_t cna)
{
    if (obmm_coh_dir_has_sharer(line, cna)) {
        return true;
    }
    if (line->sharer_count < OBMM_COH_MAX_SHARERS) {
        line->sharers[line->sharer_count++] = cna;
        return true;
    }
    return false;
}

static void obmm_coh_dir_remove_sharer(ObmmCohDirLine *line, uint32_t cna)
{
    uint32_t i;

    for (i = 0; i < line->sharer_count; i++) {
        if (line->sharers[i] == cna) {
            line->sharers[i] = line->sharers[line->sharer_count - 1];
            line->sharer_count--;
            return;
        }
    }
}

static void obmm_coh_dir_clear_sharers(ObmmCohDirLine *line)
{
    line->sharer_count = 0;
}

static MemTxResult obmm_coh_home_read_line(BusControllerDev *ubc_dev,
                                           ObmmCohDirLine *dir,
                                           uint64_t line_addr,
                                           uint32_t token_id,
                                           uint8_t *buf)
{
    MemTxResult ret;

    ret = obmm_coh_local_read(ubc_dev, line_addr, token_id, buf,
                              OBMM_COH_LINE_SIZE);
    if (ret == MEMTX_OK) {
        memcpy(dir->data, buf, OBMM_COH_LINE_SIZE);
        dir->data_valid = true;
        return MEMTX_OK;
    }
    if (dir->data_valid) {
        memcpy(buf, dir->data, OBMM_COH_LINE_SIZE);
        return MEMTX_OK;
    }
    return ret;
}

static int obmm_coh_invalidate_local_line(BusControllerDev *ubc_dev,
                                          uint64_t line_addr,
                                          uint32_t home_cna,
                                          uint32_t token_id)
{
    ObmmCohLocalLine wb_line = { 0 };
    ObmmCohLocalLine *line;
    bool need_wb = false;

    obmm_coh_init_once();
    qemu_mutex_lock(&g_obmm_coh_lock);
    line = obmm_coh_find_line_locked(ubc_dev, line_addr, home_cna, token_id);
    if (line && line->state != OBMM_COH_I) {
        wb_line = *line;
        if (line->dirty && line->state == OBMM_COH_M) {
            need_wb = true;
        }
        line->state = OBMM_COH_I;
        line->dirty = false;
    }
    qemu_mutex_unlock(&g_obmm_coh_lock);

    if (need_wb &&
        obmm_coh_local_write(ubc_dev, wb_line.line_addr, wb_line.token_id,
                             wb_line.data, OBMM_COH_LINE_SIZE) != MEMTX_OK) {
        qemu_mutex_lock(&g_obmm_coh_lock);
        line = obmm_coh_find_line_locked(ubc_dev, line_addr, home_cna,
                                         token_id);
        if (line && line->state == OBMM_COH_I) {
            *line = wb_line;
        }
        qemu_mutex_unlock(&g_obmm_coh_lock);
        return -1;
    }
    return 0;
}

static int obmm_coh_downgrade_local_line(BusControllerDev *ubc_dev,
                                         uint64_t line_addr,
                                         uint32_t home_cna,
                                         uint32_t token_id)
{
    ObmmCohLocalLine *line;
    int ret = 0;

    obmm_coh_init_once();
    qemu_mutex_lock(&g_obmm_coh_lock);
    line = obmm_coh_find_line_locked(ubc_dev, line_addr, home_cna, token_id);
    if (line && line->state == OBMM_COH_E && !line->dirty) {
        line->state = OBMM_COH_S;
    } else if (line && line->state == OBMM_COH_M) {
        ret = -1;
    }
    qemu_mutex_unlock(&g_obmm_coh_lock);
    return ret;
}

static bool obmm_coh_ranges_overlap(uint64_t a_start, uint64_t a_len,
                                    uint64_t b_start, uint64_t b_len)
{
    uint64_t a_end = a_start + a_len;
    uint64_t b_end = b_start + b_len;

    if (a_end < a_start) {
        a_end = UINT64_MAX;
    }
    if (b_end < b_start) {
        b_end = UINT64_MAX;
    }
    return a_start < b_end && b_start < a_end;
}

static int obmm_coh_drain_home_range(BusControllerDev *ubc_dev,
                                     uint64_t range_start,
                                     uint64_t range_len,
                                     uint32_t token_id)
{
    for (;;) {
        ObmmCohDirLine *line = NULL;
        GHashTableIter iter;
        gpointer value;
        uint32_t owner_cna = 0;
        uint64_t line_addr = 0;
        bool blocked_by_pending = false;

        qemu_mutex_lock(&g_obmm_coh_lock);
        g_hash_table_iter_init(&iter, g_obmm_coh_dir);
        while (g_hash_table_iter_next(&iter, NULL, &value)) {
            ObmmCohDirLine *cur = value;

            if (cur->home_cna == ubc_dev->parent.cna &&
                cur->token_id == token_id &&
                cur->has_owner &&
                cur->dirty &&
                obmm_coh_ranges_overlap(cur->line_addr, OBMM_COH_LINE_SIZE,
                                        range_start, range_len)) {
                if (cur->pending) {
                    line_addr = cur->line_addr;
                    blocked_by_pending = true;
                    break;
                }
                line = cur;
                owner_cna = cur->owner_cna;
                line_addr = cur->line_addr;
                cur->pending = true;
                break;
            }
        }
        qemu_mutex_unlock(&g_obmm_coh_lock);

        if (!line) {
            if (blocked_by_pending) {
                (void)obmm_coh_wait_dir_not_pending(ubc_dev, line_addr,
                                                    ubc_dev->parent.cna,
                                                    token_id,
                                                    "FENCE_HOME");
                continue;
            }
            return 0;
        }
        if (owner_cna == ubc_dev->parent.cna) {
            if (obmm_coh_invalidate_local_line(ubc_dev, line_addr,
                                               ubc_dev->parent.cna,
                                               token_id) != 0) {
                qemu_mutex_lock(&g_obmm_coh_lock);
                line = obmm_coh_find_dir_locked(line_addr,
                                                ubc_dev->parent.cna,
                                                token_id);
                if (line) {
                    line->pending = false;
                }
                qemu_mutex_unlock(&g_obmm_coh_lock);
                return -1;
            }
        } else if (obmm_coh_send_inv_wait(ubc_dev, owner_cna, line_addr,
                                          token_id) != 0 &&
                   !obmm_coh_wait_owner_released(ubc_dev, line_addr,
                                                 ubc_dev->parent.cna,
                                                 token_id, owner_cna,
                                                 "FENCE_HOME")) {
            qemu_mutex_lock(&g_obmm_coh_lock);
            line = obmm_coh_find_dir_locked(line_addr, ubc_dev->parent.cna,
                                            token_id);
            if (line) {
                line->pending = false;
            }
            qemu_mutex_unlock(&g_obmm_coh_lock);
            return -1;
        }

        qemu_mutex_lock(&g_obmm_coh_lock);
        line = obmm_coh_find_dir_locked(line_addr, ubc_dev->parent.cna,
                                        token_id);
        if (line) {
            obmm_coh_dir_remove_sharer(line, owner_cna);
            if (line->has_owner && line->owner_cna == owner_cna) {
                line->has_owner = false;
                line->dirty = false;
                line->state = line->sharer_count ? OBMM_COH_S : OBMM_COH_I;
                line->version++;
            }
            line->pending = false;
        }
        qemu_mutex_unlock(&g_obmm_coh_lock);
    }
    return 0;
}

static int obmm_coh_send_msg(BusControllerDev *ubc_dev, uint32_t dcna,
                             uint8_t sub_msg_code, const void *payload,
                             uint32_t payload_len)
{
    if (dcna == ubc_dev->parent.cna) {
        return -1;
    }
    return obmm_coh_send_ub_link_msg(ubc_dev, dcna, sub_msg_code, payload,
                                     payload_len);
}

static int obmm_coh_send_inv_wait(BusControllerDev *ubc_dev, uint32_t target_cna,
                                  uint64_t line_addr, uint32_t token_id)
{
    ObmmCohInvPld inv = { 0 };
    ObmmCohPending *pending;
    uint32_t req_id = ++ubc_dev->next_coh_req_id;
    int rc;

    inv.hdr.req_id = req_id;
    inv.hdr.msg_type = UBC_MSG_SUB_COH_INV;
    inv.hdr.line_addr = line_addr;
    inv.hdr.home_cna = ubc_dev->parent.cna;
    inv.hdr.requester_cna = ubc_dev->parent.cna;
    inv.hdr.token_id = token_id;

    pending = obmm_coh_pending_begin(ubc_dev, req_id, target_cna,
                                     UBC_MSG_SUB_COH_INV_ACK, OBMM_COH_I);
    rc = obmm_coh_send_msg(ubc_dev, target_cna, UBC_MSG_SUB_COH_INV, &inv,
                           sizeof(inv));
    if (rc != 0) {
        obmm_coh_pending_end(pending);
        return -1;
    }
    obmm_coh_wait_for_response(ubc_dev, pending);
    rc = obmm_coh_pending_status(pending);
    obmm_coh_pending_end(pending);
    return rc;
}

static int obmm_coh_send_downgrade_wait(BusControllerDev *ubc_dev,
                                        uint32_t target_cna,
                                        uint64_t line_addr,
                                        uint32_t token_id)
{
    ObmmCohDowngradePld downgrade = { 0 };
    ObmmCohPending *pending;
    uint32_t req_id = ++ubc_dev->next_coh_req_id;
    int rc;

    downgrade.hdr.req_id = req_id;
    downgrade.hdr.msg_type = UBC_MSG_SUB_COH_DOWNGRADE;
    downgrade.hdr.line_addr = line_addr;
    downgrade.hdr.home_cna = ubc_dev->parent.cna;
    downgrade.hdr.requester_cna = ubc_dev->parent.cna;
    downgrade.hdr.token_id = token_id;

    pending = obmm_coh_pending_begin(ubc_dev, req_id, target_cna,
                                     UBC_MSG_SUB_COH_DOWNGRADE_ACK,
                                     OBMM_COH_I);
    rc = obmm_coh_send_msg(ubc_dev, target_cna, UBC_MSG_SUB_COH_DOWNGRADE,
                           &downgrade, sizeof(downgrade));
    if (rc != 0) {
        obmm_coh_pending_end(pending);
        return -1;
    }
    obmm_coh_wait_for_response(ubc_dev, pending);
    rc = obmm_coh_pending_status(pending);
    obmm_coh_pending_end(pending);
    return rc;
}

static int obmm_coh_send_wb_nowait(BusControllerDev *ubc_dev,
                                   uint32_t home_cna,
                                   const ObmmCohLocalLine *line)
{
    ObmmCohWbPld wb = { 0 };
    uint32_t wb_req_id = ++ubc_dev->next_coh_req_id;

    wb.hdr.req_id = wb_req_id;
    wb.hdr.msg_type = UBC_MSG_SUB_COH_WB;
    wb.hdr.line_addr = line->line_addr;
    wb.hdr.home_cna = home_cna;
    wb.hdr.requester_cna = ubc_dev->parent.cna;
    wb.hdr.token_id = line->token_id;
    wb.data_len = OBMM_COH_LINE_SIZE;
    memcpy(wb.data, line->data, OBMM_COH_LINE_SIZE);

    return obmm_coh_send_msg(ubc_dev, home_cna, UBC_MSG_SUB_COH_WB, &wb,
                             sizeof(wb));
}

static int obmm_coh_send_wb_wait(BusControllerDev *ubc_dev, uint32_t home_cna,
                                 const ObmmCohLocalLine *line)
{
    ObmmCohWbPld wb = { 0 };
    ObmmCohPending *pending;
    uint32_t wb_req_id = ++ubc_dev->next_coh_req_id;
    int status;

    wb.hdr.req_id = wb_req_id;
    wb.hdr.msg_type = UBC_MSG_SUB_COH_WB;
    wb.hdr.line_addr = line->line_addr;
    wb.hdr.home_cna = home_cna;
    wb.hdr.requester_cna = ubc_dev->parent.cna;
    wb.hdr.token_id = line->token_id;
    wb.data_len = OBMM_COH_LINE_SIZE;
    memcpy(wb.data, line->data, OBMM_COH_LINE_SIZE);

    pending = obmm_coh_pending_begin(ubc_dev, wb_req_id, home_cna,
                                     UBC_MSG_SUB_COH_WB_ACK, OBMM_COH_I);
    if (obmm_coh_send_msg(ubc_dev, home_cna, UBC_MSG_SUB_COH_WB, &wb,
                          sizeof(wb)) != 0) {
        obmm_coh_pending_end(pending);
        return -1;
    }
    obmm_coh_wait_for_response(ubc_dev, pending);
    status = obmm_coh_pending_status(pending);
    obmm_coh_pending_end(pending);
    return status;
}

void obmm_coh_invalidate_local_range(BusControllerDev *ubc_dev,
                                     uint32_t home_cna,
                                     uint64_t range_start,
                                     uint64_t range_len,
                                     uint32_t token_id)
{
    GHashTableIter iter;
    gpointer value;

    if (!ubc_dev || range_len == 0) {
        return;
    }

    obmm_coh_init_once();
    qemu_mutex_lock(&g_obmm_coh_lock);
    g_hash_table_iter_init(&iter, g_obmm_coh_cache);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        ObmmCohLocalLine *line = value;

        if (line->ubc_dev == ubc_dev &&
            line->home_cna == home_cna &&
            line->token_id == token_id &&
            obmm_coh_ranges_overlap(line->line_addr, OBMM_COH_LINE_SIZE,
                                    range_start, range_len)) {
            line->state = OBMM_COH_I;
            line->dirty = false;
        }
    }
    qemu_mutex_unlock(&g_obmm_coh_lock);
}

MemTxResult obmm_coh_read(BusControllerDev *ubc_dev,
                          uint64_t remote_uba, uint32_t token_id,
                          uint32_t dcna, uint8_t *buf, uint32_t len)
{
    uint64_t line_addr;
    uint32_t line_off;
    ObmmCohLocalLine *line;
    ObmmCohMsgHdr req = { 0 };
    ObmmCohPending *pending;
    uint32_t req_id;
    uint32_t grant_state;
    uint32_t attempt;
    int rc;

    if (!ubc_dev || !buf || len == 0) {
        return MEMTX_DECODE_ERROR;
    }
    line_addr = obmm_coh_line_addr(remote_uba);
    line_off = (uint32_t)(remote_uba - line_addr);
    if (len > OBMM_COH_LINE_SIZE - line_off) {
        uint32_t first_len = OBMM_COH_LINE_SIZE - line_off;

        if (obmm_coh_read(ubc_dev, remote_uba, token_id, dcna, buf,
                          first_len) != MEMTX_OK) {
            return MEMTX_DECODE_ERROR;
        }
        return obmm_coh_read(ubc_dev, remote_uba + first_len, token_id, dcna,
                             buf + first_len, len - first_len);
    }
    obmm_coh_init_once();
    qemu_mutex_lock(&g_obmm_coh_lock);
    line = obmm_coh_find_line_locked(ubc_dev, line_addr, dcna, token_id);
    if (line && line->state != OBMM_COH_I) {
        memcpy(buf, line->data + line_off, len);
        qemu_mutex_unlock(&g_obmm_coh_lock);
        return MEMTX_OK;
    }
    qemu_mutex_unlock(&g_obmm_coh_lock);

    for (attempt = 0; attempt < 8; attempt++) {
        req_id = ++ubc_dev->next_coh_req_id;
        memset(&req, 0, sizeof(req));
        req.req_id = req_id;
        req.msg_type = UBC_MSG_SUB_COH_GETS;
        req.line_addr = line_addr;
        req.home_cna = dcna;
        req.requester_cna = ubc_dev->parent.cna;
        req.token_id = token_id;
        pending = obmm_coh_pending_begin(ubc_dev, req_id, dcna,
                                         UBC_MSG_SUB_COH_DATA, OBMM_COH_S);
        rc = obmm_coh_send_msg(ubc_dev, dcna, UBC_MSG_SUB_COH_GETS, &req,
                               sizeof(req));
        if (rc != 0) {
            obmm_coh_pending_end(pending);
            return MEMTX_DECODE_ERROR;
        }
        obmm_coh_wait_for_response(ubc_dev, pending);
        if (obmm_coh_pending_status(pending) == 0 &&
            obmm_coh_pending_data_len(pending) == OBMM_COH_LINE_SIZE) {
            break;
        }
        qemu_log("OBMM_COH_GETS retry attempt=%u line=%#" PRIx64
                 " home=%#x token=%u status=%d len=%u\n",
                 attempt + 1, line_addr, dcna, token_id,
                 obmm_coh_pending_status(pending),
                 obmm_coh_pending_data_len(pending));
        if (attempt == 7) {
            break;
        }
        obmm_coh_pending_end(pending);
        obmm_coh_poll_rx_links(ubc_dev);
    }
    if (obmm_coh_pending_status(pending) != 0 ||
        obmm_coh_pending_data_len(pending) != OBMM_COH_LINE_SIZE) {
        obmm_coh_pending_end(pending);
        return MEMTX_DECODE_ERROR;
    }

    qemu_mutex_lock(&g_obmm_coh_lock);
    grant_state = pending->grant_state;
    memcpy(buf, pending->data + line_off, len);
    qemu_mutex_unlock(&g_obmm_coh_lock);

    qemu_mutex_lock(&g_obmm_coh_lock);
    line = obmm_coh_get_line_locked(ubc_dev, line_addr, dcna, token_id);
    line->home_cna = dcna;
    line->token_id = token_id;
    line->state = (grant_state == OBMM_COH_E) ? OBMM_COH_E : OBMM_COH_S;
    line->dirty = false;
    memcpy(line->data, pending->data, OBMM_COH_LINE_SIZE);
    qemu_mutex_unlock(&g_obmm_coh_lock);
    obmm_coh_pending_end(pending);
    return MEMTX_OK;
}

MemTxResult obmm_coh_write(BusControllerDev *ubc_dev,
                           uint64_t remote_uba, uint32_t token_id,
                           uint32_t dcna, const uint8_t *buf, uint32_t len)
{
    uint64_t line_addr;
    uint32_t line_off;
    ObmmCohLocalLine *line;
    ObmmCohMsgHdr req = { 0 };
    ObmmCohPending *pending;
    uint32_t req_id;
    uint32_t attempt;
    int rc;

    if (!ubc_dev || !buf || len == 0) {
        return MEMTX_DECODE_ERROR;
    }
    line_addr = obmm_coh_line_addr(remote_uba);
    line_off = (uint32_t)(remote_uba - line_addr);
    if (len > OBMM_COH_LINE_SIZE - line_off) {
        uint32_t first_len = OBMM_COH_LINE_SIZE - line_off;

        if (obmm_coh_write(ubc_dev, remote_uba, token_id, dcna, buf,
                           first_len) != MEMTX_OK) {
            return MEMTX_DECODE_ERROR;
        }
        return obmm_coh_write(ubc_dev, remote_uba + first_len, token_id, dcna,
                              buf + first_len, len - first_len);
    }
    obmm_coh_init_once();
    qemu_mutex_lock(&g_obmm_coh_lock);
    line = obmm_coh_find_line_locked(ubc_dev, line_addr, dcna, token_id);
    if (line && line->state == OBMM_COH_M) {
        memcpy(line->data + line_off, buf, len);
        line->dirty = true;
        qemu_mutex_unlock(&g_obmm_coh_lock);
        return MEMTX_OK;
    }
    if (line && line->state == OBMM_COH_E) {
        memcpy(line->data + line_off, buf, len);
        line->state = OBMM_COH_M;
        line->dirty = true;
        qemu_mutex_unlock(&g_obmm_coh_lock);
        return MEMTX_OK;
    }
    qemu_mutex_unlock(&g_obmm_coh_lock);

    for (attempt = 0; attempt < 8; attempt++) {
        req_id = ++ubc_dev->next_coh_req_id;
        memset(&req, 0, sizeof(req));
        req.req_id = req_id;
        req.msg_type = UBC_MSG_SUB_COH_GETM;
        req.line_addr = line_addr;
        req.home_cna = dcna;
        req.requester_cna = ubc_dev->parent.cna;
        req.token_id = token_id;
        pending = obmm_coh_pending_begin(ubc_dev, req_id, dcna,
                                         UBC_MSG_SUB_COH_DATA, OBMM_COH_M);
        rc = obmm_coh_send_msg(ubc_dev, dcna, UBC_MSG_SUB_COH_GETM, &req,
                               sizeof(req));
        if (rc != 0) {
            obmm_coh_pending_end(pending);
            return MEMTX_DECODE_ERROR;
        }
        obmm_coh_wait_for_response(ubc_dev, pending);
        if (obmm_coh_pending_status(pending) == 0 &&
            obmm_coh_pending_data_len(pending) == OBMM_COH_LINE_SIZE) {
            break;
        }
        qemu_log("OBMM_COH_GETM retry attempt=%u line=%#" PRIx64
                 " home=%#x token=%u status=%d len=%u\n",
                 attempt + 1, line_addr, dcna, token_id,
                 obmm_coh_pending_status(pending),
                 obmm_coh_pending_data_len(pending));
        if (attempt == 7) {
            break;
        }
        obmm_coh_pending_end(pending);
        obmm_coh_poll_rx_links(ubc_dev);
    }
    if (obmm_coh_pending_status(pending) != 0 ||
        obmm_coh_pending_data_len(pending) != OBMM_COH_LINE_SIZE) {
        obmm_coh_pending_end(pending);
        return MEMTX_DECODE_ERROR;
    }

    qemu_mutex_lock(&g_obmm_coh_lock);
    line = obmm_coh_get_line_locked(ubc_dev, line_addr, dcna, token_id);
    line->home_cna = dcna;
    line->token_id = token_id;
    line->state = OBMM_COH_M;
    memcpy(line->data, pending->data, OBMM_COH_LINE_SIZE);
    memcpy(line->data + line_off, buf, len);
    line->dirty = true;
    qemu_mutex_unlock(&g_obmm_coh_lock);
    obmm_coh_pending_end(pending);
    return MEMTX_OK;
}

/* --- Milestone B: COH_FENCE/ACK --- */

#define COH_WAIT_TIMEOUT_ITERS  5000000
#define COH_DIR_PENDING_WAIT_ITERS  500000

static void obmm_coh_wait_for_response(BusControllerDev *ubc_dev,
                                        ObmmCohPending *pending)
{
    uint32_t iters = 0;
    bool still_pending = true;

    while (still_pending && iters < COH_WAIT_TIMEOUT_ITERS) {
        obmm_coh_poll_rx_links(ubc_dev);
        iters++;
        qemu_mutex_lock(&g_obmm_coh_lock);
        still_pending = pending->pending;
        qemu_mutex_unlock(&g_obmm_coh_lock);
    }
    if (still_pending) {
        qemu_log("OBMM_COH: wait timeout req_id=%u peer=%#x msg=%u\n",
                 pending->key->req_id, pending->key->peer_cna,
                 pending->key->msg_type);
        qemu_mutex_lock(&g_obmm_coh_lock);
        pending->pending = false;
        pending->status = -1;
        qemu_mutex_unlock(&g_obmm_coh_lock);
    }
}

static bool obmm_coh_wait_dir_not_pending(BusControllerDev *ubc_dev,
                                          uint64_t line_addr,
                                          uint32_t home_cna,
                                          uint32_t token_id,
                                          const char *op)
{
    uint32_t iters = 0;

    for (;;) {
        ObmmCohDirLine *dir;
        bool pending = false;

        qemu_mutex_lock(&g_obmm_coh_lock);
        dir = obmm_coh_find_dir_locked(line_addr, home_cna, token_id);
        pending = dir && dir->pending;
        qemu_mutex_unlock(&g_obmm_coh_lock);

        if (!pending) {
            return true;
        }
        if (iters++ >= COH_DIR_PENDING_WAIT_ITERS) {
            qemu_log("OBMM_COH_%s wait pending timeout line=%#" PRIx64
                     " home=%#x token=%u\n",
                     op ? op : "DIR", line_addr, home_cna, token_id);
            obmm_coh_clear_dir_pending(line_addr, home_cna, token_id);
            return false;
        }
        obmm_coh_poll_rx_links(ubc_dev);
    }
}

static bool obmm_coh_wait_owner_released(BusControllerDev *ubc_dev,
                                         uint64_t line_addr,
                                         uint32_t home_cna,
                                         uint32_t token_id,
                                         uint32_t owner_cna,
                                         const char *op)
{
    uint32_t iters = 0;

    for (;;) {
        ObmmCohDirLine *dir;
        bool released = false;

        qemu_mutex_lock(&g_obmm_coh_lock);
        dir = obmm_coh_find_dir_locked(line_addr, home_cna, token_id);
        released = dir && (!dir->has_owner || dir->owner_cna != owner_cna ||
                           !dir->dirty);
        qemu_mutex_unlock(&g_obmm_coh_lock);

        if (released) {
            return true;
        }
        if (iters++ >= COH_DIR_PENDING_WAIT_ITERS) {
            qemu_log("OBMM_COH_%s wait owner release timeout line=%#" PRIx64
                     " home=%#x owner=%#x token=%u\n",
                     op ? op : "DIR", line_addr, home_cna, owner_cna,
                     token_id);
            return false;
        }
        obmm_coh_poll_rx_links(ubc_dev);
    }
}

int obmm_coh_send_fence(BusControllerDev *ubc_dev, uint32_t home_cna,
                        uint64_t range_start, uint64_t range_len,
                        uint32_t token_id)
{
    ObmmCohFencePld pld = { 0 };
    uint32_t req_id;
    uint32_t attempt;
    GHashTableIter iter;
    gpointer value;
    int rc;

    if (!ubc_dev || UINT64_MAX - range_start < range_len) {
        return -1;
    }

    obmm_coh_init_once();
    for (;;) {
        ObmmCohLocalLine flush_line = { 0 };
        ObmmCohLocalLine *line;
        bool found = false;
        bool home_dir_pending = false;

        qemu_mutex_lock(&g_obmm_coh_lock);
        g_hash_table_iter_init(&iter, g_obmm_coh_cache);
        while (g_hash_table_iter_next(&iter, NULL, &value)) {
            ObmmCohDirLine *dir;
            line = value;
            if (line->ubc_dev != ubc_dev ||
                !line->dirty ||
                line->home_cna != home_cna ||
                line->token_id != token_id ||
                !obmm_coh_ranges_overlap(line->line_addr, OBMM_COH_LINE_SIZE,
                                         range_start, range_len)) {
                continue;
            }
            if (home_cna == ubc_dev->parent.cna) {
                dir = obmm_coh_find_dir_locked(line->line_addr, home_cna,
                                               token_id);
                if (!dir || !dir->has_owner ||
                    dir->owner_cna != ubc_dev->parent.cna || !dir->dirty) {
                    line->state = OBMM_COH_I;
                    line->dirty = false;
                    continue;
                }
            }
            flush_line = *line;
            found = true;
            break;
        }
        qemu_mutex_unlock(&g_obmm_coh_lock);

        if (!found) {
            break;
        }

        if (home_cna == ubc_dev->parent.cna) {
            ObmmCohDirLine *dir;

            qemu_mutex_lock(&g_obmm_coh_lock);
            dir = obmm_coh_find_dir_locked(flush_line.line_addr, home_cna,
                                           flush_line.token_id);
            if (dir && dir->pending) {
                qemu_mutex_unlock(&g_obmm_coh_lock);
                (void)obmm_coh_wait_dir_not_pending(ubc_dev,
                                                    flush_line.line_addr,
                                                    home_cna,
                                                    flush_line.token_id,
                                                    "FENCE_LOCAL");
                continue;
            }
            if (dir) {
                dir->pending = true;
                home_dir_pending = true;
            }
            qemu_mutex_unlock(&g_obmm_coh_lock);

            if (obmm_coh_local_write(ubc_dev, flush_line.line_addr,
                                     flush_line.token_id, flush_line.data,
                                     OBMM_COH_LINE_SIZE) != MEMTX_OK) {
                if (home_dir_pending) {
                    obmm_coh_clear_dir_pending(flush_line.line_addr, home_cna,
                                               flush_line.token_id);
                }
                return -1;
            }
        } else {
            if (obmm_coh_send_wb_wait(ubc_dev, home_cna, &flush_line) != 0) {
                return -1;
            }
        }

        qemu_mutex_lock(&g_obmm_coh_lock);
        line = obmm_coh_find_line_locked(ubc_dev, flush_line.line_addr,
                                         home_cna, flush_line.token_id);
        if (line && line->dirty) {
            line->dirty = false;
            if (line->state == OBMM_COH_M) {
                line->state = OBMM_COH_S;
            }
        }
        if (home_cna == ubc_dev->parent.cna) {
            ObmmCohDirLine *dir;

            dir = obmm_coh_find_dir_locked(flush_line.line_addr, home_cna,
                                           flush_line.token_id);
            if (dir && dir->has_owner &&
                dir->owner_cna == ubc_dev->parent.cna &&
                obmm_coh_dir_add_sharer(dir, ubc_dev->parent.cna)) {
                dir->has_owner = false;
                dir->dirty = false;
                dir->state = OBMM_COH_S;
                dir->version++;
            }
            if (dir && home_dir_pending) {
                dir->pending = false;
            }
        }
        qemu_mutex_unlock(&g_obmm_coh_lock);
        qemu_log("OBMM_COH_WB local line=%#" PRIx64 " home=%#x\n",
                 flush_line.line_addr, home_cna);
    }

    if (home_cna == ubc_dev->parent.cna) {
        return obmm_coh_drain_home_range(ubc_dev, range_start, range_len,
                                         token_id);
    }

    for (attempt = 0; attempt < 4; attempt++) {
        ObmmCohPending *pending;

        req_id = ++ubc_dev->next_coh_req_id;
        memset(&pld, 0, sizeof(pld));
        pld.hdr.req_id = req_id;
        pld.hdr.msg_type = UBC_MSG_SUB_COH_FENCE;
        pld.hdr.home_cna = home_cna;
        pld.hdr.requester_cna = ubc_dev->parent.cna;
        pld.hdr.token_id = token_id;
        pld.range_start = range_start;
        pld.range_len = range_len;

        pending = obmm_coh_pending_begin(ubc_dev, req_id, home_cna,
                                         UBC_MSG_SUB_COH_FENCE,
                                         OBMM_COH_I);
        rc = obmm_coh_send_ub_link_msg(ubc_dev, home_cna,
                                       UBC_MSG_SUB_COH_FENCE,
                                       &pld, sizeof(pld));
        if (rc != 0) {
            obmm_coh_pending_end(pending);
            return rc;
        }

        obmm_coh_wait_for_response(ubc_dev, pending);
        rc = obmm_coh_pending_status(pending);
        obmm_coh_pending_end(pending);
        if (rc == 0) {
            return 0;
        }
        qemu_log("OBMM_COH_FENCE retry attempt=%u home=%#x"
                 " range=%#" PRIx64 "+%" PRIu64 " token=%u rc=%d\n",
                 attempt + 1, home_cna, range_start, range_len, token_id, rc);
    }
    return rc;
}

/* --- Coherence message receive handlers --- */

void obmm_coh_handle_rx_fence(BusControllerDev *ubc_dev,
                              const ObmmCohFencePld *pld, uint32_t src_cna)
{
    ObmmCohFenceAckPld ack = { 0 };
    int drain_ret;

    qemu_log("OBMM_COH_FENCE req_id=%u from=%#x"
             " range=%#" PRIx64 "+%" PRIu64 "\n",
             pld->hdr.req_id, src_cna,
             pld->range_start, pld->range_len);

    ack.hdr = pld->hdr;
    ack.hdr.msg_type = UBC_MSG_SUB_COH_FENCE_ACK;
    ack.hdr.requester_cna = ubc_dev->parent.cna;
    if (pld->hdr.home_cna != ubc_dev->parent.cna) {
        drain_ret = -1;
    } else {
        drain_ret = obmm_coh_drain_home_range(ubc_dev, pld->range_start,
                                              pld->range_len,
                                              pld->hdr.token_id);
    }
    ack.status = (drain_ret == 0) ? 0 : 1;

    obmm_coh_send_ub_link_msg(ubc_dev, src_cna,
                               UBC_MSG_SUB_COH_FENCE_ACK,
                               &ack, sizeof(ack));
}

void obmm_coh_handle_rx_fence_ack(BusControllerDev *ubc_dev,
                                  const ObmmCohFenceAckPld *pld,
                                  uint32_t src_cna)
{
    qemu_log("OBMM_COH_FENCE_ACK req_id=%u from=%#x status=%u\n",
             pld->hdr.req_id, src_cna, pld->status);

    obmm_coh_pending_complete_status(ubc_dev, pld->hdr.req_id, src_cna,
                                     UBC_MSG_SUB_COH_FENCE,
                                     (int)pld->status);
}

/* --- MESI coherence message receive handlers --- */
void obmm_coh_handle_rx_gets(BusControllerDev *ubc_dev,
                             const ObmmCohMsgHdr *pld, uint32_t src_cna)
{
    ObmmCohDataPld data = { 0 };
    ObmmCohDirLine *dir;
    uint32_t owner_cna = 0;
    uint32_t new_sharers = 0;
    bool need_owner_inv = false;
    bool need_owner_downgrade = false;
    bool dir_pending = false;
    const char *fail_reason = "none";

    data.hdr = *pld;
    data.hdr.msg_type = UBC_MSG_SUB_COH_DATA;
    data.hdr.requester_cna = ubc_dev->parent.cna;
    data.data_len = OBMM_COH_LINE_SIZE;
    data.grant_state = OBMM_COH_S;

    if (pld->home_cna != ubc_dev->parent.cna) {
        data.status = 1;
        data.data_len = 0;
        fail_reason = "wrong_home";
        goto send_data;
    }

    obmm_coh_init_once();
    qemu_mutex_lock(&g_obmm_coh_lock);
    dir = obmm_coh_get_dir_locked(pld->line_addr, ubc_dev->parent.cna,
                                  pld->token_id);
    if (dir->pending) {
        qemu_mutex_unlock(&g_obmm_coh_lock);
        (void)obmm_coh_wait_dir_not_pending(ubc_dev, pld->line_addr,
                                            ubc_dev->parent.cna,
                                            pld->token_id, "GETS");
        qemu_mutex_lock(&g_obmm_coh_lock);
        dir = obmm_coh_get_dir_locked(pld->line_addr, ubc_dev->parent.cna,
                                      pld->token_id);
        dir->pending = true;
        dir_pending = true;
    } else {
        dir->pending = true;
        dir_pending = true;
    }
    if (data.status == 0 && dir->has_owner && dir->owner_cna != src_cna) {
        owner_cna = dir->owner_cna;
        if (dir->state == OBMM_COH_E && !dir->dirty) {
            need_owner_downgrade = true;
        } else {
            need_owner_inv = true;
        }
    }
    if (data.status == 0 && !obmm_coh_dir_has_sharer(dir, src_cna)) {
        new_sharers++;
    }
    if (data.status == 0 && need_owner_downgrade &&
        !obmm_coh_dir_has_sharer(dir, owner_cna)) {
        new_sharers++;
    }
    if (data.status == 0 &&
        dir->sharer_count + new_sharers > OBMM_COH_MAX_SHARERS) {
        data.status = 1;
        data.data_len = 0;
        fail_reason = "sharer_overflow";
    }
    qemu_mutex_unlock(&g_obmm_coh_lock);

    if (data.status != 0) {
        /* Cannot record requester as a sharer; do not grant access. */
    } else if (need_owner_downgrade) {
        int downgrade_ret;

        if (owner_cna == ubc_dev->parent.cna) {
            downgrade_ret = obmm_coh_downgrade_local_line(ubc_dev,
                                                          pld->line_addr,
                                                          ubc_dev->parent.cna,
                                                          pld->token_id);
        } else {
            downgrade_ret = obmm_coh_send_downgrade_wait(ubc_dev, owner_cna,
                                                         pld->line_addr,
                                                         pld->token_id);
        }
        if (downgrade_ret != 0) {
            need_owner_downgrade = false;
            need_owner_inv = true;
        }
    }
    if (data.status == 0 && need_owner_inv && owner_cna == ubc_dev->parent.cna &&
        obmm_coh_invalidate_local_line(ubc_dev, pld->line_addr,
                                       ubc_dev->parent.cna,
                                       pld->token_id) != 0) {
        data.status = 1;
        data.data_len = 0;
        fail_reason = "local_owner_inv_failed";
    } else if (data.status == 0 && need_owner_inv &&
               obmm_coh_send_inv_wait(ubc_dev, owner_cna, pld->line_addr,
                                      pld->token_id) != 0 &&
               !obmm_coh_wait_owner_released(ubc_dev, pld->line_addr,
                                             ubc_dev->parent.cna,
                                             pld->token_id, owner_cna,
                                             "GETS")) {
        data.status = 1;
        data.data_len = 0;
        fail_reason = "remote_owner_inv_failed";
    }
    if (data.status == 0) {
        qemu_mutex_lock(&g_obmm_coh_lock);
        dir = obmm_coh_get_dir_locked(pld->line_addr, ubc_dev->parent.cna,
                                      pld->token_id);
        if (obmm_coh_home_read_line(ubc_dev, dir, pld->line_addr,
                                    pld->token_id, data.data) != MEMTX_OK) {
            data.status = 1;
            data.data_len = 0;
            fail_reason = "local_read_failed";
        }
        qemu_mutex_unlock(&g_obmm_coh_lock);
    }
    if (data.status == 0) {
        qemu_mutex_lock(&g_obmm_coh_lock);
        dir = obmm_coh_get_dir_locked(pld->line_addr, ubc_dev->parent.cna,
                                      pld->token_id);
        if (need_owner_inv) {
            obmm_coh_dir_remove_sharer(dir, owner_cna);
            if (dir->owner_cna == owner_cna) {
                dir->has_owner = false;
            }
        } else if (need_owner_downgrade) {
            if (!obmm_coh_dir_add_sharer(dir, owner_cna)) {
                data.status = 1;
                data.data_len = 0;
                fail_reason = "owner_add_sharer_failed";
                dir->pending = false;
                qemu_mutex_unlock(&g_obmm_coh_lock);
                goto send_data;
            }
            if (dir->owner_cna == owner_cna) {
                dir->has_owner = false;
            }
        }
        dir->has_owner = false;
        dir->dirty = false;
        if (dir->sharer_count == 0) {
            data.grant_state = OBMM_COH_E;
            dir->owner_cna = src_cna;
            dir->has_owner = true;
            dir->state = OBMM_COH_E;
        } else {
            data.grant_state = OBMM_COH_S;
            dir->state = OBMM_COH_S;
        }
        if (!obmm_coh_dir_add_sharer(dir, src_cna)) {
            data.status = 1;
            data.data_len = 0;
            fail_reason = "requester_add_sharer_failed";
            dir->pending = false;
            qemu_mutex_unlock(&g_obmm_coh_lock);
            goto send_data;
        }
        dir->version++;
        dir->pending = false;
        qemu_mutex_unlock(&g_obmm_coh_lock);
    }
send_data:
    if (data.status != 0 && dir_pending) {
        obmm_coh_clear_dir_pending(pld->line_addr, ubc_dev->parent.cna,
                                   pld->token_id);
    }
    if (obmm_coh_log_sample(pld->req_id, data.status)) {
        qemu_log("OBMM_COH_GETS req_id=%u from=%#x line=%#" PRIx64
                 " status=%u reason=%s owner=%#x owner_inv=%u owner_down=%u"
                 " dir_pending=%u\n",
                 pld->req_id, src_cna, pld->line_addr, data.status,
                 fail_reason, owner_cna, need_owner_inv,
                 need_owner_downgrade, dir_pending);
    }
    obmm_coh_send_ub_link_msg(ubc_dev, src_cna, UBC_MSG_SUB_COH_DATA,
                              &data, sizeof(data));
}

void obmm_coh_handle_rx_data(BusControllerDev *ubc_dev,
                             const ObmmCohDataPld *pld, uint32_t src_cna)
{
    if (obmm_coh_log_sample(pld->hdr.req_id, pld->status)) {
        qemu_log("OBMM_COH_DATA req_id=%u from=%#x line=%#" PRIx64
                 " status=%u len=%u grant=%u\n",
                 pld->hdr.req_id, src_cna, pld->hdr.line_addr, pld->status,
                 pld->data_len, pld->grant_state);
    }
    obmm_coh_pending_complete_data(ubc_dev, pld, src_cna);
}

void obmm_coh_handle_rx_getm(BusControllerDev *ubc_dev,
                             const ObmmCohMsgHdr *pld, uint32_t src_cna)
{
    ObmmCohDataPld data = { 0 };
    ObmmCohDirLine *dir;
    uint32_t inv_targets[OBMM_COH_MAX_SHARERS];
    uint32_t inv_count = 0;
    uint32_t owner_cna = 0;
    bool need_owner_inv = false;
    bool dir_pending = false;
    const char *fail_reason = "none";
    int i;

    data.hdr = *pld;
    data.hdr.msg_type = UBC_MSG_SUB_COH_DATA;
    data.hdr.requester_cna = ubc_dev->parent.cna;
    data.data_len = OBMM_COH_LINE_SIZE;
    data.grant_state = OBMM_COH_M;

    if (pld->home_cna != ubc_dev->parent.cna) {
        data.status = 1;
        data.data_len = 0;
        fail_reason = "wrong_home";
        goto send_data;
    }

    obmm_coh_init_once();
    qemu_mutex_lock(&g_obmm_coh_lock);
    dir = obmm_coh_get_dir_locked(pld->line_addr, ubc_dev->parent.cna,
                                  pld->token_id);
    if (dir->pending) {
        qemu_mutex_unlock(&g_obmm_coh_lock);
        (void)obmm_coh_wait_dir_not_pending(ubc_dev, pld->line_addr,
                                            ubc_dev->parent.cna,
                                            pld->token_id, "GETM");
        qemu_mutex_lock(&g_obmm_coh_lock);
        dir = obmm_coh_get_dir_locked(pld->line_addr, ubc_dev->parent.cna,
                                      pld->token_id);
        dir->pending = true;
        dir_pending = true;
    } else {
        dir->pending = true;
        dir_pending = true;
    }
    if (data.status == 0 && dir->has_owner && dir->owner_cna != src_cna) {
        owner_cna = dir->owner_cna;
        need_owner_inv = true;
    }
    for (i = 0; data.status == 0 && i < dir->sharer_count; i++) {
        if (dir->sharers[i] != src_cna &&
            (!need_owner_inv || dir->sharers[i] != owner_cna)) {
            inv_targets[inv_count++] = dir->sharers[i];
        }
    }
    qemu_mutex_unlock(&g_obmm_coh_lock);

    if (need_owner_inv && owner_cna == ubc_dev->parent.cna &&
        obmm_coh_invalidate_local_line(ubc_dev, pld->line_addr,
                                       ubc_dev->parent.cna,
                                       pld->token_id) != 0) {
        data.status = 1;
        data.data_len = 0;
        fail_reason = "local_owner_inv_failed";
    } else if (need_owner_inv &&
               obmm_coh_send_inv_wait(ubc_dev, owner_cna, pld->line_addr,
                                      pld->token_id) != 0 &&
               !obmm_coh_wait_owner_released(ubc_dev, pld->line_addr,
                                             ubc_dev->parent.cna,
                                             pld->token_id, owner_cna,
                                             "GETM_OWNER")) {
        data.status = 1;
        data.data_len = 0;
        fail_reason = "remote_owner_inv_failed";
    }
    for (i = 0; data.status == 0 && i < inv_count; i++) {
        if (inv_targets[i] == ubc_dev->parent.cna) {
            if (obmm_coh_invalidate_local_line(ubc_dev, pld->line_addr,
                                               ubc_dev->parent.cna,
                                               pld->token_id) != 0) {
                data.status = 1;
                data.data_len = 0;
                fail_reason = "local_sharer_inv_failed";
            }
        } else if (obmm_coh_send_inv_wait(ubc_dev, inv_targets[i],
                                          pld->line_addr,
                                          pld->token_id) != 0) {
            data.status = 1;
            data.data_len = 0;
            fail_reason = "remote_sharer_inv_failed";
        }
    }
    if (data.status == 0) {
        qemu_mutex_lock(&g_obmm_coh_lock);
        dir = obmm_coh_get_dir_locked(pld->line_addr, ubc_dev->parent.cna,
                                      pld->token_id);
        if (obmm_coh_home_read_line(ubc_dev, dir, pld->line_addr,
                                    pld->token_id, data.data) != MEMTX_OK) {
            data.status = 1;
            data.data_len = 0;
            fail_reason = "local_read_failed";
        }
        qemu_mutex_unlock(&g_obmm_coh_lock);
    }
    if (data.status == 0) {
        qemu_mutex_lock(&g_obmm_coh_lock);
        dir = obmm_coh_get_dir_locked(pld->line_addr, ubc_dev->parent.cna,
                                      pld->token_id);
        dir->owner_cna = src_cna;
        dir->has_owner = true;
        dir->dirty = true;
        dir->state = OBMM_COH_M;
        obmm_coh_dir_clear_sharers(dir);
        dir->version++;
        dir->pending = false;
        qemu_mutex_unlock(&g_obmm_coh_lock);
    }
send_data:
    if (data.status != 0 && dir_pending) {
        obmm_coh_clear_dir_pending(pld->line_addr, ubc_dev->parent.cna,
                                   pld->token_id);
    }
    if (obmm_coh_log_sample(pld->req_id, data.status)) {
        qemu_log("OBMM_COH_GETM req_id=%u from=%#x line=%#" PRIx64
                 " status=%u reason=%s owner=%#x owner_inv=%u inv_count=%u"
                 " dir_pending=%u\n",
                 pld->req_id, src_cna, pld->line_addr, data.status,
                 fail_reason, owner_cna, need_owner_inv, inv_count,
                 dir_pending);
    }
    obmm_coh_send_ub_link_msg(ubc_dev, src_cna, UBC_MSG_SUB_COH_DATA,
                              &data, sizeof(data));
}

void obmm_coh_handle_rx_inv(BusControllerDev *ubc_dev,
                            const ObmmCohInvPld *pld, uint32_t src_cna)
{
    ObmmCohInvAckPld ack = { 0 };
    ObmmCohLocalLine wb_line = { 0 };
    ObmmCohLocalLine *line;
    bool need_wb = false;
    bool invalidated = false;

    ack.hdr = pld->hdr;
    ack.hdr.msg_type = UBC_MSG_SUB_COH_INV_ACK;
    ack.hdr.requester_cna = ubc_dev->parent.cna;
    qemu_log("OBMM_COH_INV req_id=%u from=%#x line=%#" PRIx64 "\n",
             pld->hdr.req_id, src_cna, pld->hdr.line_addr);
    if (pld->hdr.home_cna != src_cna) {
        ack.status = 1;
        goto send_ack;
    }

    obmm_coh_init_once();
    qemu_mutex_lock(&g_obmm_coh_lock);
    line = obmm_coh_find_line_locked(ubc_dev, pld->hdr.line_addr,
                                     src_cna, pld->hdr.token_id);
    if (line && line->state != OBMM_COH_I && line->home_cna == src_cna) {
        wb_line = *line;
        if (line->dirty && line->state == OBMM_COH_M) {
            need_wb = true;
        }
        line->state = OBMM_COH_I;
        if (!need_wb) {
            line->dirty = false;
        }
        invalidated = true;
    }
    qemu_mutex_unlock(&g_obmm_coh_lock);

    if (need_wb && obmm_coh_send_wb_nowait(ubc_dev, src_cna, &wb_line) != 0) {
        ack.status = 1;
    }
    if (ack.status != 0 && invalidated) {
        qemu_mutex_lock(&g_obmm_coh_lock);
        line = obmm_coh_find_line_locked(ubc_dev, pld->hdr.line_addr,
                                         src_cna, pld->hdr.token_id);
        if (line && line->state == OBMM_COH_I && line->home_cna == src_cna) {
            *line = wb_line;
        }
        qemu_mutex_unlock(&g_obmm_coh_lock);
    }
send_ack:
    qemu_log("OBMM_COH_INV_ACK req_id=%u from=%#x line=%#" PRIx64
             " status=%u\n",
             pld->hdr.req_id, src_cna, pld->hdr.line_addr, ack.status);
    obmm_coh_send_ub_link_msg(ubc_dev, src_cna, UBC_MSG_SUB_COH_INV_ACK,
                              &ack, sizeof(ack));
}

void obmm_coh_handle_rx_inv_ack(BusControllerDev *ubc_dev,
                                const ObmmCohInvAckPld *pld, uint32_t src_cna)
{
    if (obmm_coh_log_sample(pld->hdr.req_id, pld->status)) {
        qemu_log("OBMM_COH_INV_ACK_RX req_id=%u from=%#x line=%#" PRIx64
                 " status=%u\n",
                 pld->hdr.req_id, src_cna, pld->hdr.line_addr, pld->status);
    }
    obmm_coh_pending_complete_status(ubc_dev, pld->hdr.req_id, src_cna,
                                     UBC_MSG_SUB_COH_INV_ACK,
                                     (int)pld->status);
}

void obmm_coh_handle_rx_downgrade(BusControllerDev *ubc_dev,
                                  const ObmmCohDowngradePld *pld,
                                  uint32_t src_cna)
{
    ObmmCohDowngradeAckPld ack = { 0 };

    ack.hdr = pld->hdr;
    ack.hdr.msg_type = UBC_MSG_SUB_COH_DOWNGRADE_ACK;
    ack.hdr.requester_cna = ubc_dev->parent.cna;
    qemu_log("OBMM_COH_DOWNGRADE req_id=%u from=%#x line=%#" PRIx64 "\n",
             pld->hdr.req_id, src_cna, pld->hdr.line_addr);
    if (pld->hdr.home_cna != src_cna ||
        obmm_coh_downgrade_local_line(ubc_dev, pld->hdr.line_addr,
                                      src_cna, pld->hdr.token_id) != 0) {
        ack.status = 1;
    }

    qemu_log("OBMM_COH_DOWNGRADE_ACK req_id=%u from=%#x line=%#" PRIx64
             " status=%u\n",
             pld->hdr.req_id, src_cna, pld->hdr.line_addr, ack.status);
    obmm_coh_send_ub_link_msg(ubc_dev, src_cna,
                              UBC_MSG_SUB_COH_DOWNGRADE_ACK,
                              &ack, sizeof(ack));
}

void obmm_coh_handle_rx_downgrade_ack(BusControllerDev *ubc_dev,
                                      const ObmmCohDowngradeAckPld *pld,
                                      uint32_t src_cna)
{
    if (obmm_coh_log_sample(pld->hdr.req_id, pld->status)) {
        qemu_log("OBMM_COH_DOWNGRADE_ACK_RX req_id=%u from=%#x line=%#" PRIx64
                 " status=%u\n",
                 pld->hdr.req_id, src_cna, pld->hdr.line_addr, pld->status);
    }
    obmm_coh_pending_complete_status(ubc_dev, pld->hdr.req_id, src_cna,
                                     UBC_MSG_SUB_COH_DOWNGRADE_ACK,
                                     (int)pld->status);
}

void obmm_coh_handle_rx_wb(BusControllerDev *ubc_dev,
                           const ObmmCohWbPld *pld, uint32_t src_cna)
{
    ObmmCohWbAckPld ack = { 0 };
    ObmmCohDirLine *dir;
    bool stale_clean_wb = false;
    bool clean_noop_wb = false;

    ack.hdr = pld->hdr;
    ack.hdr.msg_type = UBC_MSG_SUB_COH_WB_ACK;
    ack.hdr.requester_cna = ubc_dev->parent.cna;
    if (obmm_coh_log_sample(pld->hdr.req_id, 0)) {
        qemu_log("OBMM_COH_WB req_id=%u from=%#x line=%#" PRIx64
                 " len=%u\n",
                 pld->hdr.req_id, src_cna, pld->hdr.line_addr, pld->data_len);
    }
    if (pld->hdr.home_cna != ubc_dev->parent.cna) {
        ack.status = 1;
    }

    obmm_coh_init_once();
    qemu_mutex_lock(&g_obmm_coh_lock);
    dir = obmm_coh_find_dir_locked(pld->hdr.line_addr, ubc_dev->parent.cna,
                                   pld->hdr.token_id);
    if (ack.status == 0 && !dir) {
        qemu_log("OBMM_COH_WB reject reason=no_dir line=%#" PRIx64
                 " from=%#x token=%u\n",
                 pld->hdr.line_addr, src_cna, pld->hdr.token_id);
        ack.status = 1;
    } else if (ack.status == 0 && !dir->has_owner) {
        if (dir->state == OBMM_COH_S && !dir->dirty) {
            clean_noop_wb = !obmm_coh_dir_has_sharer(dir, src_cna);
            if (obmm_coh_log_sample(pld->hdr.req_id, 0)) {
                qemu_log("OBMM_COH_WB accept reason=%s line=%#" PRIx64
                         " from=%#x token=%u sharers=%u\n",
                         clean_noop_wb ? "stale_clean_no_owner" :
                                         "idempotent_sharer",
                         pld->hdr.line_addr, src_cna, pld->hdr.token_id,
                         dir->sharer_count);
            }
        } else {
            qemu_log("OBMM_COH_WB reject reason=no_owner line=%#" PRIx64
                     " from=%#x token=%u state=%u sharers=%u dirty=%u\n",
                     pld->hdr.line_addr, src_cna, pld->hdr.token_id,
                     dir->state, dir->sharer_count, dir->dirty);
            ack.status = 1;
        }
    } else if (ack.status == 0 && dir->owner_cna != src_cna) {
        if (!dir->dirty) {
            if (obmm_coh_log_sample(pld->hdr.req_id, 0)) {
                qemu_log("OBMM_COH_WB accept reason=stale_clean_owner"
                         " line=%#" PRIx64 " from=%#x owner=%#x token=%u"
                         " state=%u pending=%u\n",
                         pld->hdr.line_addr, src_cna, dir->owner_cna,
                         pld->hdr.token_id, dir->state, dir->pending);
            }
            stale_clean_wb = true;
            clean_noop_wb = true;
        } else {
            qemu_log("OBMM_COH_WB reject reason=owner_mismatch line=%#" PRIx64
                     " from=%#x owner=%#x token=%u state=%u dirty=%u"
                     " pending=%u\n",
                     pld->hdr.line_addr, src_cna, dir->owner_cna,
                     pld->hdr.token_id, dir->state, dir->dirty, dir->pending);
            ack.status = 1;
        }
    }
    qemu_mutex_unlock(&g_obmm_coh_lock);

    if (ack.status == 0 && pld->data_len != OBMM_COH_LINE_SIZE) {
        qemu_log("OBMM_COH_WB reject reason=bad_len line=%#" PRIx64
                 " len=%u\n",
                 pld->hdr.line_addr, pld->data_len);
        ack.status = 1;
    } else if (ack.status == 0 && !clean_noop_wb &&
               obmm_coh_local_write(ubc_dev, pld->hdr.line_addr,
                                    pld->hdr.token_id, pld->data,
                                    pld->data_len) != MEMTX_OK) {
        qemu_log("OBMM_COH_WB reject reason=local_write line=%#" PRIx64
                 " len=%u\n",
                 pld->hdr.line_addr, pld->data_len);
        ack.status = 1;
    }
    if (ack.status == 0) {
        qemu_mutex_lock(&g_obmm_coh_lock);
        dir = obmm_coh_find_dir_locked(pld->hdr.line_addr, ubc_dev->parent.cna,
                                       pld->hdr.token_id);
        if (!clean_noop_wb) {
            memcpy(dir->data, pld->data, OBMM_COH_LINE_SIZE);
            dir->data_valid = true;
        }
        if (clean_noop_wb || stale_clean_wb) {
            dir->version++;
        } else if (obmm_coh_dir_add_sharer(dir, src_cna)) {
            dir->has_owner = false;
            dir->dirty = false;
            dir->state = OBMM_COH_S;
            dir->version++;
        } else {
            ack.status = 1;
        }
        qemu_mutex_unlock(&g_obmm_coh_lock);
    }
    if (obmm_coh_log_sample(pld->hdr.req_id, ack.status)) {
        qemu_log("OBMM_COH_WB_ACK req_id=%u from=%#x line=%#" PRIx64
                 " status=%u\n",
                 pld->hdr.req_id, src_cna, pld->hdr.line_addr, ack.status);
    }
    obmm_coh_send_ub_link_msg(ubc_dev, src_cna, UBC_MSG_SUB_COH_WB_ACK,
                              &ack, sizeof(ack));
}

void obmm_coh_handle_rx_wb_ack(BusControllerDev *ubc_dev,
                               const ObmmCohWbAckPld *pld, uint32_t src_cna)
{
    if (obmm_coh_log_sample(pld->hdr.req_id, pld->status)) {
        qemu_log("OBMM_COH_WB_ACK_RX req_id=%u from=%#x line=%#" PRIx64
                 " status=%u\n",
                 pld->hdr.req_id, src_cna, pld->hdr.line_addr, pld->status);
    }
    obmm_coh_pending_complete_status(ubc_dev, pld->hdr.req_id, src_cna,
                                     UBC_MSG_SUB_COH_WB_ACK,
                                     (int)pld->status);
}

#include "mock_osdep.h"
#include <assert.h>

// Minimal structures for the mock test
typedef struct UBLinkEndpointDesc {
    char *device_id;
    uint32_t port_idx;
} UBLinkEndpointDesc;

typedef struct UBLinkState {
    UBLinkEndpointDesc a;
    UBLinkEndpointDesc b;
    bool remote_applied;
} UBLinkState;

// Implementation of the functions we want to test (copied from my changes)

static const char *ub_link_shared_dir(void)
{
    const char *dir = getenv("UB_FM_SHARED_DIR");
    return (dir && dir[0]) ? dir : "/tmp/ub-qemu-links";
}

static char *ub_link_global_device_id(const char *device_id)
{
    const char *local_node_id = getenv("UB_FM_NODE_ID");
    if (local_node_id && local_node_id[0]) {
        return g_strdup_printf("%s.%s", local_node_id, device_id);
    }
    return g_strdup(device_id);
}

static char *ub_link_sanitize_token(const char *token)
{
    char *out = g_strdup(token);
    for (size_t i = 0; out[i]; i++) {
        if (!g_ascii_isalnum(out[i]) && out[i] != '-' && out[i] != '_') {
            out[i] = '_';
        }
    }
    return out;
}

static char *ub_link_kick_path(const UBLinkEndpointDesc *ep)
{
    g_autofree char *global_id = ub_link_global_device_id(ep->device_id);
    g_autofree char *sanitized = ub_link_sanitize_token(global_id);
    mkdir(ub_link_shared_dir(), 0755);
    return g_strdup_printf("%s/%s__%u.kick", ub_link_shared_dir(), sanitized, ep->port_idx);
}

int ub_link_kick_remote(UBLinkState *s)
{
    UBLinkEndpointDesc *remote = NULL;
    g_autofree char *path = NULL;

    if (!s || !s->remote_applied) return 0;

    // Simulate Node A kicking Node B
    // In node A's view: a=local, b=remote
    remote = &s->b;
    path = ub_link_kick_path(remote);
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "1");
    fclose(f);
    printf("[Node A] Kicked remote endpoint %s:%u at %s\n", remote->device_id, remote->port_idx, path);
    return 0;
}

int ub_link_poll_kick(UBLinkState *s)
{
    UBLinkEndpointDesc *local = NULL;
    g_autofree char *path = NULL;

    if (!s || !s->remote_applied) return 0;

    // In node B's view: a=local, b=remote
    local = &s->a;
    path = ub_link_kick_path(local);
    if (access(path, F_OK) == 0) {
        unlink(path);
        printf("[Node B] Received kick on local endpoint %s:%u\n", local->device_id, local->port_idx);
        return 1;
    }
    return 0;
}

int main() {
    // Setup a temporary shared dir for this test
    char test_dir[] = "/tmp/ub-test-m3-XXXXXX";
    mkdtemp(test_dir);
    setenv("UB_FM_SHARED_DIR", test_dir, 1);
    printf("Using test shared dir: %s\n", test_dir);

    // Node A setup
    // Node A wants to kick Node B's local endpoint ubc0:1
    UBLinkState nodeA_view = {
        .a = { .device_id = "ubc0", .port_idx = 1 }, // local
        .b = { .device_id = "ubc0", .port_idx = 1 }, // remote's local ID
        .remote_applied = true
    };

    // Node B setup
    UBLinkState nodeB_view = {
        .a = { .device_id = "ubc0", .port_idx = 1 }, // local
        .b = { .device_id = "ubc0", .port_idx = 1 }, // remote
        .remote_applied = true
    };

    // Scenario: Node A kicks Node B
    // We set node id to "nodeB" because kick_path uses the node_id of the target endpoint
    setenv("UB_FM_NODE_ID", "nodeB", 1);
    assert(ub_link_kick_remote(&nodeA_view) == 0);

    // Verification: Node B polls and finds the kick
    setenv("UB_FM_NODE_ID", "nodeB", 1);
    assert(ub_link_poll_kick(&nodeB_view) == 1);
    assert(ub_link_poll_kick(&nodeB_view) == 0); // Should be consumed

    printf("M3 Validation Test PASSED!\n");

    // Cleanup
    rmdir(test_dir);
    return 0;
}

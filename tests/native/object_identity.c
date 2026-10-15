/// object_identity — stable identities under compaction, resets and adversarial tokens
#include "object_identity.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int payload; uint64_t foreign_id; } Item;
static ObsIdentityView view(Item *items, size_t n, uint64_t rev, ObsIdentityKind kind) {
    return (ObsIdentityView){ items, sizeof(Item), n, offsetof(Item, foreign_id), rev, kind };
}
static void must_borrow(ObsIdentityRegistry *r, ObsIdentityView v, size_t n,
                        const void **token, uint32_t *gen) {
    assert(obs_identity_borrow(r, v, n, token, gen));
    assert(*token && *gen);
    size_t idx = SIZE_MAX;
    assert(obs_identity_resolve(r, v, *token, *gen, &idx));
    assert(idx == n);
}
int main(void) {
    ObsIdentityRegistry r = {0};
    Item *meshes = calloc(1024, sizeof(*meshes));
    Item nodes[128] = {0};
    assert(meshes);
    uint64_t mr = 1, nr = 1;
    const void *a, *b, *c, *root;
    uint32_t ga, gb, gc, gr;
    for (int i = 0; i < 16; ++i) meshes[i].payload = i + 10;
    must_borrow(&r, view(meshes, 16, mr, OBS_ID_MESH), 5, &a, &ga);
    must_borrow(&r, view(meshes, 16, mr, OBS_ID_MESH), 9, &b, &gb);
    must_borrow(&r, view(nodes, 1, nr, OBS_ID_NODE), 0, &root, &gr);
    assert(a != b && a != root);
    /* Unrelated append must not invalidate existing handles. */
    meshes[16] = (Item){100, 0};
    ++mr;
    assert(obs_identity_resolve(&r, view(meshes, 17, mr, OBS_ID_MESH), a, ga, &(size_t){0}));
    /* Array relocation, then swap-last deletion of unrelated object. */
    Item *moved = malloc(1024 * sizeof(*moved));
    assert(moved);
    memcpy(moved, meshes, 17 * sizeof(*meshes));
    free(meshes); meshes = moved;
    meshes[2] = meshes[16];
    ++mr;
    size_t index = SIZE_MAX;
    assert(obs_identity_resolve(&r, view(meshes, 16, mr, OBS_ID_MESH), a, ga, &index) && index == 5);
    assert(obs_identity_resolve(&r, view(meshes, 16, mr, OBS_ID_MESH), b, gb, &index) && index == 9);
    /* Delete target 5 via swap last. Old target must retire. */
    meshes[5] = meshes[15];
    ++mr;
    assert(!obs_identity_resolve(&r, view(meshes, 15, mr, OBS_ID_MESH), a, ga, &index));
    assert(obs_identity_resolve(&r, view(meshes, 15, mr, OBS_ID_MESH), b, gb, &index) && index == 9);
    must_borrow(&r, view(meshes, 15, mr, OBS_ID_MESH), 5, &c, &gc);
    assert(!(a == c && ga == gc));
    assert(!obs_identity_resolve(&r, view(meshes, 15, mr, OBS_ID_MESH), a, ga, &index));
    /* Node compaction preserves logical identity even when slot moves. */
    nodes[1] = (Item){20, 0};
    nodes[2] = (Item){30, 0};
    ++nr;
    const void *node2; uint32_t gn2;
    must_borrow(&r, view(nodes, 3, nr, OBS_ID_NODE), 2, &node2, &gn2);
    nodes[1] = nodes[2];
    ++nr;
    assert(obs_identity_resolve(&r, view(nodes, 2, nr, OBS_ID_NODE), node2, gn2, &index) && index == 1);
    /* Same token must not act as the wrong nominal kind. */
    assert(!obs_identity_resolve(&r, view(nodes, 2, nr, OBS_ID_NODE), b, gb, &index));
    /* Never dereference arbitrary pointers or misaligned tokens. */
    assert(!obs_identity_resolve(&r, view(meshes, 15, mr, OBS_ID_MESH),
                                 (const char *)c + 1, gc, &index));
    assert(!obs_identity_resolve(&r, view(meshes, 15, mr, OBS_ID_MESH), (void *)(uintptr_t)1, gc, &index));
    assert(!obs_identity_resolve(&r, view(meshes, 15, mr, OBS_ID_MESH), c, gc + 1, &index));
    /* Repeated compaction / append and cached reference resolution. */
    for (int step = 0; step < 50000; ++step) {
        size_t n = 15 + (size_t)(step & 1);
        if (n == 16) meshes[15] = (Item){step, 0};
        ++mr;
        assert(obs_identity_resolve(&r, view(meshes, n, mr, OBS_ID_MESH), b, gb, &index));
        assert(index == 9);
        assert(!obs_identity_resolve(&r, view(meshes, n, mr, OBS_ID_MESH), a, ga, &index));
        const void *temp; uint32_t gt;
        must_borrow(&r, view(meshes, n, mr, OBS_ID_MESH), n - 1, &temp, &gt);
    }
    /* Full reset: previous generations are no longer valid. */
    ++mr; ++nr;
    assert(obs_identity_sync(&r, view(NULL, 0, mr, OBS_ID_MESH)));
    assert(obs_identity_sync(&r, view(NULL, 0, nr, OBS_ID_NODE)));
    meshes[0] = (Item){999, 0};
    ++mr;
    must_borrow(&r, view(meshes, 1, mr, OBS_ID_MESH), 0, &c, &gc);
    assert(!obs_identity_resolve(&r, view(meshes, 1, mr, OBS_ID_MESH), b, gb, &index));
    assert(!obs_identity_resolve(&r, view(meshes, 1, mr, OBS_ID_MESH), root, gr, &index));
    assert(!obs_identity_resolve(&r, view(meshes, 1, mr, OBS_ID_MESH), c, gc + 1, &index));
    free(meshes);
    obs_identity_destroy(&r);
    puts("object_identity: 50,000 stress cycles + relocation/compaction/reset/ABA checks OK");
    return 0;
}

/// mesh_storage tests — allocation failure atomicity, ownership, append safety
#include "mesh_storage.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

typedef struct { uint64_t id; unsigned char payload[40]; } Item;
typedef struct { int calls, fail_at, frees; } AllocCtx;
static void *fault_alloc(size_t n, void *ctx) {
    AllocCtx *a = ctx;
    ++a->calls;
    if (a->calls == a->fail_at) return NULL;
    return malloc(n);
}
static void fault_free(void *p, void *ctx) {
    AllocCtx *a = ctx;
    if (p) ++a->frees;
    free(p);
}

/// Deterministic failures
static void test_failure_atomicity(void) {
    for (int failed_at = 1; failed_at <= 2; ++failed_at) {
        Item *values = malloc(sizeof(Item) * 2);
        uint32_t *order = malloc(sizeof(uint32_t) * 2);
        assert(values && order);
        values[0].id = 0xcafe;
        values[1].id = 0xbeef;
        order[0] = 1; order[1] = 0;
        Item *original = values;
        uint32_t *original_order = order;
        size_t cap = 2;
        AllocCtx ctx = {.fail_at = failed_at};
        bool ok = obs_mesh_storage_reserve((void **)&values, &order, &cap, 2, 10,
                                            sizeof(Item), fault_alloc, fault_free, &ctx);
        assert(!ok && values == original && order == original_order && cap == 2);
        assert(values[0].id == 0xcafe && values[1].id == 0xbeef);
        assert(order[0] == 1 && order[1] == 0);
        free(values); free(order);
    }
}

/// Append stresses
static void test_many(void) {
    Item *items = NULL;
    uint32_t *order = NULL;
    size_t capacity = 0, live = 0;
    for (size_t i = 0; i < 50000; ++i) {
        Item item = {.id = i + 1};
        if (live == capacity)
            assert(obs_mesh_storage_reserve((void **)&items, &order, &capacity,
                                             live, live+1, sizeof(Item), NULL, NULL, NULL));
        assert(obs_mesh_storage_append(items, order, &live, capacity, &item,
                                        1, sizeof(Item)));
        assert(items[live-1].id == live && order[live-1] == live-1);
    }
    assert(live == 50000);
    Item extra = {.id = 999};
    Item before = items[live-1];
    assert(!obs_mesh_storage_append(items, order, &live, capacity,
                                     &extra, capacity + 1, sizeof(Item)));
    assert(items[live-1].id == before.id && live == 50000);
    free(items); free(order);
}

/// Malformed requests must have no side effects
static void test_invalid(void) {
    Item *items = NULL;
    uint32_t *order = NULL;
    size_t capacity = 0, live = 0;
    assert(!obs_mesh_storage_reserve((void **)&items, &order, &capacity,
                                      1, 10, sizeof(Item), NULL, NULL, NULL));
    assert(!obs_mesh_storage_reserve((void **)&items, &order, &capacity,
                                      0, SIZE_MAX, sizeof(Item), NULL, NULL, NULL));
    assert(!obs_mesh_storage_reserve((void **)&items, &order, &capacity,
                                      0, 5, 0, NULL, NULL, NULL));
    assert(!obs_mesh_storage_append(items, order, &live, 0, &(Item){0}, 1, sizeof(Item)));
    assert(items == NULL && order == NULL && capacity == 0 && live == 0);
}
/// Cache-hit prepare/commit contract with simulated GPU acceptance
static void test_cache_candidate_visibility(void) {
    Item *live_items = NULL;
    uint32_t *live_order = NULL;
    size_t live = 0, capacity = 0;
    Item prepared[3] = {{.id=11}, {.id=22}, {.id=33}};
    assert(obs_mesh_storage_reserve((void **)&live_items, &live_order,
        &capacity, live, 3, sizeof(Item), NULL, NULL, NULL));
    /* Source, hierarchy and texture preparation cannot expose the meshes. */
    assert(live == 0);
    /* A rejected GPU upload discards only the private array. */
    assert(live == 0 && capacity >= 3);
    /* The next successful batch commits in one allocation-free step. */
    assert(obs_mesh_storage_append(live_items, live_order, &live,
                                   capacity, prepared, 3, sizeof(Item)));
    assert(live == 3 && live_items[0].id == 11 && live_items[2].id == 33);
    uint64_t tail = live_items[2].id;
    assert(!obs_mesh_storage_append(live_items, live_order, &live,
                                    capacity, prepared, 3, sizeof(Item)));
    assert(live == 3 && live_items[2].id == tail);
    free(live_items); free(live_order);
}

int main(void) {
    test_failure_atomicity();
    test_invalid();
    test_many();
    test_cache_candidate_visibility();
    puts("mesh-storage: allocation/append atomicity PASS");
    return 0;
}

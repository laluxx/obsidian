#include "mesh_storage.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/// Helpers
static void *storage_malloc(size_t n, void *ctx) { (void)ctx; return malloc(n); }
static void storage_free(void *p, void *ctx) { (void)ctx; free(p); }

/// Reservation
bool obs_mesh_storage_reserve(void **items, uint32_t **order,
                              size_t *capacity, size_t live, size_t minimum,
                              size_t item_size, ObsStorageAlloc alloc,
                              ObsStorageFree release, void *ctx) {
    if (!items || !order || !capacity || !item_size ||
        (!!alloc != !!release) || live > *capacity ||
        (live && (!*items || !*order)) ||
        (*capacity && (!*items || !*order))) return false;
    if (minimum <= *capacity) return true;
    if (minimum > UINT32_MAX || minimum > SIZE_MAX / item_size ||
        minimum > SIZE_MAX / sizeof(uint32_t)) return false;
    size_t next = *capacity ? *capacity : 4;
    while (next < minimum) {
        if (next > SIZE_MAX / 2) { next = minimum; break; }
        next *= 2;
    }
    if (next > UINT32_MAX || next > SIZE_MAX / item_size ||
        next > SIZE_MAX / sizeof(uint32_t)) next = minimum;
    if (next > UINT32_MAX || next > SIZE_MAX / item_size ||
        next > SIZE_MAX / sizeof(uint32_t)) return false;
    if (!alloc) { alloc = storage_malloc; release = storage_free; }
    void *fresh_items = alloc(next * item_size, ctx);
    if (!fresh_items) return false;
    uint32_t *fresh_order = alloc(next * sizeof(uint32_t), ctx);
    if (!fresh_order) { release(fresh_items, ctx); return false; }
    if (live) {
        memcpy(fresh_items, *items, live * item_size);
        memcpy(fresh_order, *order, live * sizeof(uint32_t));
    }
    release(*items, ctx);
    release(*order, ctx);
    *items = fresh_items;
    *order = fresh_order;
    *capacity = next;
    return true;
}

/// Allocation-free append
bool obs_mesh_storage_append(void *items, uint32_t *order,
                             size_t *live, size_t capacity,
                             const void *prepared, size_t count,
                             size_t item_size) {
    if (!live || !item_size || *live > capacity || count > capacity - *live ||
        (count && (!items || !order || !prepared)) ||
        count > UINT32_MAX || *live > UINT32_MAX - count ||
        *live > SIZE_MAX / item_size || count > (SIZE_MAX / item_size) - *live)
        return false;
    if (!count) return true;
    memcpy((unsigned char *)items + *live * item_size, prepared, count * item_size);
    for (size_t i = 0; i < count; ++i) order[*live + i] = (uint32_t)(*live + i);
    *live += count;
    return true;
}

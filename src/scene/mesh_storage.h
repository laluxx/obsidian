/// mesh_storage — failure-atomic storage reservation and allocation-free scene commit
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void *(*ObsStorageAlloc)(size_t bytes, void *ctx);
typedef void (*ObsStorageFree)(void *pointer, void *ctx);

/// Reserve
/* Allocates both paired arrays BEFORE publishing either pointer. A failed
 * allocation leaves the previous storage, count and capacity untouched.
 * Tests supply fault-injectable allocators; NULL alloc/free selects libc. */
bool obs_mesh_storage_reserve(void **items, uint32_t **order,
                              size_t *capacity, size_t live, size_t minimum,
                              size_t item_size, ObsStorageAlloc alloc,
                              ObsStorageFree release, void *ctx);

/// Commit
/* Caller has already reserved capacity and verified all resource owners.
 * No allocation, no callback and no side effects on rejection. */
bool obs_mesh_storage_append(void *items, uint32_t *order,
                             size_t *live, size_t capacity,
                             const void *prepared, size_t count,
                             size_t item_size);

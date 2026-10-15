/// node_topology.h — deterministic parent-first ordering for native scene graphs
#ifndef OBSIDIAN_SCENE_NODE_TOPOLOGY_H
#define OBSIDIAN_SCENE_NODE_TOPOLOGY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// Validation contract
/* Build a stable parent-first ordering from an array of strided node records.
 * The signed int32_t parent is copied with memcpy; no aliased type-punning.
 * -1 denotes a root. Every other parent must be in [0, count).
 * Cycles, self-parents, missing storage, and bad layouts are rejected.
 * On failure the order is INCOMPLETE and must not be used by the renderer.
 * order, stack and states are caller-owned arrays of count elements.
 * O(count) time and O(count) scratch; no recursion, heap use, or globals. */
bool obs_node_topological_order(const void *nodes, uint32_t count,
                                size_t stride, size_t parent_offset,
                                uint32_t *order, uint32_t *stack,
                                uint8_t *states);
#endif

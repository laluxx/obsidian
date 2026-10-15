/// node_topology.c — linear-time, cycle-detecting hierarchy preparation
#include "node_topology.h"
#include <string.h>

/// Parent decoding
static int32_t parent_at(const unsigned char *nodes, size_t stride,
                         size_t offset, uint32_t i) {
    int32_t parent;
    memcpy(&parent, nodes + (size_t)i * stride + offset, sizeof(parent));
    return parent;
}

/// Iterative path resolution
bool obs_node_topological_order(const void *nodes, uint32_t count,
                                size_t stride, size_t parent_offset,
                                uint32_t *order, uint32_t *stack,
                                uint8_t *states) {
    if (!count) return true;
    if (!nodes || !order || !stack || !states ||
        stride < sizeof(int32_t) || parent_offset > stride - sizeof(int32_t) ||
        (size_t)count > SIZE_MAX / stride || order == stack) return false;

    memset(states, 0, (size_t)count);
    const unsigned char *base = nodes;
    uint32_t written = 0;
    for (uint32_t start = 0; start < count; ++start) {
        uint32_t depth = 0;
        uint32_t curr = start;
        while (states[curr] != 2) {
            if (states[curr] == 1) return false; /* cycle */
            states[curr] = 1;
            stack[depth++] = curr;
            int32_t parent = parent_at(base, stride, parent_offset, curr);
            if (parent == -1) break;
            if (parent < 0 || (uint32_t)parent >= count) return false;
            curr = (uint32_t)parent;
        }
        while (depth) {
            uint32_t node = stack[--depth];
            states[node] = 2;
            order[written++] = node;
        }
    }
    return written == count;
}

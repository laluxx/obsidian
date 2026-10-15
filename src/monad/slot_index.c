/// slot_index.c — Checked, constant-time foreign slot membership
#include "slot_index.h"
#include <stdint.h>

/// Address validation
bool obs_slot_index(const void *base, size_t element_size, size_t count,
                    const void *candidate, size_t *index) {
    if (index) *index = 0;
    if (!base || !candidate || !element_size || !count) return false;
    /* uintptr_t is provided by the supported native platforms. Unsigned
     * arithmetic avoids UB from subtracting unrelated C pointers. This
     * representation assumes ordinary flat-address CPU engine allocations;
     * it is not a general capability-pointer operation. */
    uintptr_t first = (uintptr_t)base;
    uintptr_t current = (uintptr_t)candidate;
    if (current < first) return false;
    uintptr_t displacement = current - first;
    if (displacement % element_size) return false;
    uintptr_t slot = displacement / element_size;
    if (slot >= count || slot > SIZE_MAX) return false;
    if (index) *index = (size_t)slot;
    return true;
}

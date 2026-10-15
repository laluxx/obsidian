#include "arena_growth.h"

#include <stdint.h>

bool obs_arena_growth_capacity(uint64_t current, uint64_t minimum,
                               uint64_t required, uint64_t *out_capacity) {
    if (!out_capacity || minimum == 0) return false;

    uint64_t capacity = current < minimum ? minimum : current;
    while (capacity < required) {
        if (capacity > UINT64_MAX / 2) {
            capacity = required;
            break;
        }
        capacity *= 2;
    }

    *out_capacity = capacity;
    return true;
}

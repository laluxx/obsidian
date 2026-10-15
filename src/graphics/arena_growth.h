#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Return the smallest geometric capacity >= required, starting at minimum.
 * On overflow the final step uses the exact required size. */
bool obs_arena_growth_capacity(uint64_t current, uint64_t minimum,
                               uint64_t required, uint64_t *out_capacity);

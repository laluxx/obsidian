#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Size pages from the request, not page count: small requests use geometric
 * power-of-two classes; larger-than-maximum requests round to Vulkan alignment. */
bool obs_texture_page_capacity(uint64_t required, uint64_t alignment,
                               uint64_t *capacity);

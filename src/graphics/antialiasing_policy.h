#pragma once

#include <stdbool.h>
#include <stdint.h>

/* A zero request is Monad nil/off; enabled requests are exact Vulkan sample
 * counts. The support mask uses VkSampleCountFlagBits numeric values. */
bool obs_antialiasing_resolve_request(int64_t requested,
                                      uint32_t supported_counts,
                                      uint32_t *resolved_count);

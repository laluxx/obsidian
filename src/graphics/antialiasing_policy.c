#include "antialiasing_policy.h"

bool obs_antialiasing_resolve_request(int64_t requested,
                                      uint32_t supported_counts,
                                      uint32_t *resolved_count) {
  if (!resolved_count)
    return false;
  if (requested == 0) {
    *resolved_count = 1;
    return true;
  }
  if (requested != 2 && requested != 4 && requested != 8)
    return false;
  const uint32_t sample_bit = (uint32_t)requested;
  if (!(supported_counts & sample_bit))
    return false;
  *resolved_count = sample_bit;
  return true;
}

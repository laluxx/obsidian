#include "texture_page_policy.h"

#define TEXTURE_PAGE_INITIAL_SIZE (UINT64_C(256) * 1024)
#define TEXTURE_PAGE_MAX_SIZE (UINT64_C(256) * 1024 * 1024)

bool obs_texture_page_capacity(uint64_t required, uint64_t alignment,
                               uint64_t *capacity) {
  if (!required || !alignment || !capacity ||
      (alignment & (alignment - 1)) != 0)
    return false;

  uint64_t selected = TEXTURE_PAGE_INITIAL_SIZE;
  while (selected < required && selected < TEXTURE_PAGE_MAX_SIZE) {
    selected = selected > TEXTURE_PAGE_MAX_SIZE / 2
                   ? TEXTURE_PAGE_MAX_SIZE
                   : selected * 2;
  }

  if (required > selected) {
    if (required > UINT64_MAX - (alignment - 1))
      return false;
    selected = (required + alignment - 1) & ~(alignment - 1);
  }
  if (selected < required) return false;

  *capacity = selected;
  return true;
}

#include "texture_page_policy.h"

#include <assert.h>
#include <stdint.h>

static uint64_t capacity_for(uint64_t required) {
  uint64_t capacity = 0;
  assert(obs_texture_page_capacity(required, 256, &capacity));
  return capacity;
}

int main(void) {
  const uint64_t kib = UINT64_C(1024);
  const uint64_t mib = kib * kib;

  assert(capacity_for(1) == 256 * kib);
  assert(capacity_for(256 * kib) == 256 * kib);
  assert(capacity_for(256 * kib + 1) == 512 * kib);
  assert(capacity_for(4 * mib) == 4 * mib);
  assert(capacity_for(256 * mib) == 256 * mib);
  assert(capacity_for(256 * mib + 1) == 256 * mib + 256);

  uint64_t capacity = UINT64_C(123);
  assert(!obs_texture_page_capacity(0, 256, &capacity));
  assert(!obs_texture_page_capacity(1, 0, &capacity));
  assert(!obs_texture_page_capacity(1, 3, &capacity));
  assert(!obs_texture_page_capacity(UINT64_MAX, 256, &capacity));
  assert(capacity == 123);
  return 0;
}

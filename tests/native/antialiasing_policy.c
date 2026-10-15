#include "antialiasing_policy.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
  const uint32_t supported = 1u | 2u | 4u | 8u;
  uint32_t samples = UINT32_MAX;

  assert(obs_antialiasing_resolve_request(0, supported, &samples));
  assert(samples == 1);
  assert(obs_antialiasing_resolve_request(2, supported, &samples));
  assert(samples == 2);
  assert(obs_antialiasing_resolve_request(4, supported, &samples));
  assert(samples == 4);
  assert(obs_antialiasing_resolve_request(8, supported, &samples));
  assert(samples == 8);

  samples = 99;
  assert(!obs_antialiasing_resolve_request(2, 1u | 4u, &samples));
  assert(samples == 99);
  assert(!obs_antialiasing_resolve_request(16, supported, &samples));
  assert(samples == 99);
  assert(!obs_antialiasing_resolve_request(-1, supported, &samples));
  assert(samples == 99);
  assert(!obs_antialiasing_resolve_request(4, supported, NULL));

  puts("antialiasing policy: nil/off, exact 2x/4x/8x and rejection PASS");
  return 0;
}

#include "gpu_profile.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

static void expect_delta(uint64_t begin, uint64_t end, uint32_t valid_bits,
                         double period, uint64_t expected_ns) {
    uint64_t actual = UINT64_MAX;
    assert(obs_gpu_profile_timestamp_delta_ns(
        begin, end, valid_bits, period, &actual));
    assert(actual == expected_ns);
}

static void expect_invalid(uint64_t begin, uint64_t end, uint32_t valid_bits,
                           double period) {
    uint64_t output = 123;
    assert(!obs_gpu_profile_timestamp_delta_ns(
        begin, end, valid_bits, period, &output));
    assert(output == 123);
}

int main(void) {
    expect_delta(100, 125, 64, 1.0, 25);
    expect_delta(100, 125, 64, 0.5, 12);
    expect_delta(0, 0, 64, 1.0, 0);

    const uint64_t mask36 = (UINT64_C(1) << 36) - 1;
    expect_delta(mask36 - 3, 3, 36, 1.0, 7);
    const uint64_t mask48 = (UINT64_C(1) << 48) - 1;
    expect_delta(mask48 - 4, 5, 48, 2.0, 20);
    expect_delta(UINT64_MAX - 4, 3, 64, 1.0, 8);
    expect_delta(1, 0, 1, 1.0, 1);

    expect_invalid(0, 1, 0, 1.0);
    expect_invalid(0, 1, 65, 1.0);
    expect_invalid(0, 1, 64, 0.0);
    expect_invalid(0, 1, 64, -1.0);
    expect_invalid(0, 1, 64, NAN);
    expect_invalid(0, 1, 64, INFINITY);
    expect_invalid(0, UINT64_MAX, 64, 2.0);
    assert(!obs_gpu_profile_timestamp_delta_ns(0, 1, 64, 1.0, NULL));

    puts("gpu_profile: units, valid widths, modular wrap, missing data and overflow PASS");
    return 0;
}

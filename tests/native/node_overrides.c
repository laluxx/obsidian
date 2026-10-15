/// node_overrides.c — script translation survives repeated animation evaluation
#include "node_overrides.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/// Validation and no-op behavior
static void transaction_test(void) {
    float offset[3] = {0, 0, 0};
    const double xyz[3] = {2, -3, 4};
    assert(obs_node_offset_set(offset, xyz) == OBS_PROPERTY_CHANGED);
    assert(obs_node_offset_set(offset, xyz) == OBS_PROPERTY_UNCHANGED);
    for (int axis = 0; axis < 3; ++axis) {
        double bad[3] = {2, -3, 4};
        float old[3]; memcpy(old, offset, sizeof old);
        bad[axis] = NAN;
        assert(obs_node_offset_set(offset, bad) == OBS_PROPERTY_INVALID);
        assert(memcmp(old, offset, sizeof old) == 0);
        bad[axis] = INFINITY;
        assert(obs_node_offset_set(offset, bad) == OBS_PROPERTY_INVALID);
        assert(memcmp(old, offset, sizeof old) == 0);
        bad[axis] = (double)FLT_MAX * 2;
        assert(obs_node_offset_set(offset, bad) == OBS_PROPERTY_INVALID);
        assert(memcmp(old, offset, sizeof old) == 0);
    }
    assert(obs_node_offset_set(NULL, xyz) == OBS_PROPERTY_INVALID);
    assert(obs_node_offset_set(offset, NULL) == OBS_PROPERTY_INVALID);
    const double reset[3] = {0,0,0};
    assert(obs_node_offset_set(offset, reset) == OBS_PROPERTY_CHANGED);
    assert(obs_node_offset_set(offset, reset) == OBS_PROPERTY_UNCHANGED);
}

/// Animation composition: local matrix reconstructed each tick
static void persistence_test(void) {
    float overlay[3] = {2, 0, 0};
    for (unsigned frame = 0; frame < 50000; ++frame) {
        /* A quarter-turn about Z: local X becomes world Y. Different
         * sampled animation translations must never overwrite script state. */
        float local[4][4] = {
            {0,1,0,0}, {-1,0,0,0}, {0,0,1,0},
            {(float)(frame%127),3,4,1}
        };
        obs_node_apply_local_translation(local, overlay);
        assert(local[3][0] == (float)(frame%127));
        assert(local[3][1] == 5 && local[3][2] == 4);
        assert(local[3][3] == 1);
        assert(overlay[0] == 2 && overlay[1] == 0 && overlay[2] == 0);
    }
    float identity[4][4] = {
        {1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}
    };
    float zero[3] = {0,0,0};
    float before[4][4]; memcpy(before, identity, sizeof identity);
    obs_node_apply_local_translation(identity, zero);
    assert(memcmp(before, identity, sizeof identity) == 0);
    obs_node_apply_local_translation(NULL, overlay);
    obs_node_apply_local_translation(identity, NULL);
}
int main(void) {
    transaction_test(); persistence_test();
    puts("node_overrides: 50,000 animation ticks and failure atomicity passed");
}

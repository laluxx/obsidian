/// mesh_properties.c — pure property validation and mutation contracts
#include "mesh_properties.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/// Deterministic tests
static void material_cases(void) {
    float rough = .5f, metal = 0;
    assert(obs_property_pbr(&rough, &metal, .5, 0.) == OBS_PROPERTY_UNCHANGED);
    assert(obs_property_pbr(&rough, &metal, .25, .75) == OBS_PROPERTY_CHANGED);
    assert(rough == .25f && metal == .75f);
    float original[2] = {rough, metal};
    double bad[] = {NAN, INFINITY, -INFINITY, -1., 1.01};
    for (size_t i = 0; i < sizeof bad/sizeof bad[0]; ++i) {
        assert(obs_property_pbr(&rough, &metal, bad[i], .75) == OBS_PROPERTY_INVALID);
        assert(obs_property_pbr(&rough, &metal, .25, bad[i]) == OBS_PROPERTY_INVALID);
        assert(rough == original[0] && metal == original[1]);
    }
    assert(obs_property_pbr(NULL, &metal, .2, .2) == OBS_PROPERTY_INVALID);
    assert(obs_property_pbr(&rough, NULL, .2, .2) == OBS_PROPERTY_INVALID);
}
static void color_cases(void) {
    float rgba[4] = {1, 1, 1, 1};
    double next[4] = {.1, .2, .3, .4};
    assert(obs_property_rgba(rgba, next) == OBS_PROPERTY_CHANGED);
    assert(obs_property_rgba(rgba, next) == OBS_PROPERTY_UNCHANGED);
    float snapshot[4]; memcpy(snapshot, rgba, sizeof rgba);
    for (int k = 0; k < 4; ++k) {
        double bad[4] = {.9, .9, .9, .9};
        bad[k] = NAN;
        assert(obs_property_rgba(rgba, bad) == OBS_PROPERTY_INVALID);
        assert(memcmp(snapshot, rgba, sizeof rgba) == 0);
        bad[k] = 2;
        assert(obs_property_rgba(rgba, bad) == OBS_PROPERTY_INVALID);
        assert(memcmp(snapshot, rgba, sizeof rgba) == 0);
    }
    assert(obs_property_rgba(NULL, next) == OBS_PROPERTY_INVALID);
    assert(obs_property_rgba(rgba, NULL) == OBS_PROPERTY_INVALID);
}
static void transform_cases(void) {
    float model[4][4] = {{1,0,0,0},{0,1,0,0},{0,0,1,0},{10,20,30,1}};
    double newxyz[3] = {10,20,30};
    assert(obs_property_world_position(model, newxyz) == OBS_PROPERTY_UNCHANGED);
    newxyz[0] = 5; newxyz[2] = -3;
    assert(obs_property_world_position(model, newxyz) == OBS_PROPERTY_CHANGED);
    assert(model[3][0] == 5 && model[3][1] == 20 && model[3][2] == -3 && model[3][3] == 1);
    assert(model[0][0] == 1 && model[1][1] == 1 && model[2][2] == 1);
    float backup[4][4]; memcpy(backup, model, sizeof model);
    double invalid[3] = {99, NAN, 99};
    assert(obs_property_world_position(model, invalid) == OBS_PROPERTY_INVALID);
    assert(memcmp(backup, model, sizeof model) == 0);
    invalid[1] = (double)FLT_MAX * 2;
    assert(obs_property_world_position(model, invalid) == OBS_PROPERTY_INVALID);
    assert(memcmp(backup, model, sizeof model) == 0);
    assert(obs_property_world_position(NULL, newxyz) == OBS_PROPERTY_INVALID);
    assert(obs_property_world_position(model, NULL) == OBS_PROPERTY_INVALID);

    const double z_axis[3] = {0, 0, 5};
    const double quarter_turn = 1.57079632679489661923;
    float rotated[4][4] = {
        {2,0,0,0}, {0,3,0,0}, {0,0,4,0}, {5,6,7,1}
    };
    assert(obs_property_world_rotate_axis(rotated, z_axis, 0.0) ==
           OBS_PROPERTY_UNCHANGED);
    assert(obs_property_world_rotate_axis(rotated, z_axis, quarter_turn) ==
           OBS_PROPERTY_CHANGED);
    assert(fabsf(rotated[0][0]) < 1e-6f && fabsf(rotated[0][1] - 2) < 1e-6f);
    assert(fabsf(rotated[1][0] + 3) < 1e-6f && fabsf(rotated[1][1]) < 1e-6f);
    assert(rotated[2][2] == 4 && rotated[3][0] == 5 &&
           rotated[3][1] == 6 && rotated[3][2] == 7 && rotated[3][3] == 1);
    assert(rotated[0][3] == 0 && rotated[1][3] == 0 && rotated[2][3] == 0);
    assert(obs_property_world_rotate_axis(rotated, z_axis, -quarter_turn) ==
           OBS_PROPERTY_CHANGED);
    assert(fabsf(rotated[0][0] - 2) < 1e-6f &&
           fabsf(rotated[1][1] - 3) < 1e-6f);
    memcpy(backup, rotated, sizeof rotated);
    const double zero_axis[3] = {0, 0, 0};
    assert(obs_property_world_rotate_axis(rotated, zero_axis, 1.0) ==
           OBS_PROPERTY_INVALID);
    assert(memcmp(backup, rotated, sizeof rotated) == 0);
    const double invalid_axis[3] = {0, NAN, 1};
    assert(obs_property_world_rotate_axis(rotated, invalid_axis, 1.0) ==
           OBS_PROPERTY_INVALID);
    assert(memcmp(backup, rotated, sizeof rotated) == 0);
    assert(obs_property_world_rotate_axis(rotated, z_axis, INFINITY) ==
           OBS_PROPERTY_INVALID);
    assert(memcmp(backup, rotated, sizeof rotated) == 0);
    assert(obs_property_world_rotate_axis(NULL, z_axis, 1.0) ==
           OBS_PROPERTY_INVALID);
    assert(obs_property_world_rotate_axis(rotated, NULL, 1.0) ==
           OBS_PROPERTY_INVALID);
}
static void fuzz_cases(void) {
    uint32_t state = 0x12345678;
    float rgba[4] = {1,1,1,1};
    float matrix[4][4] = {{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}};
    for (int i = 0; i < 50000; ++i) {
        double vec[4];
        for (int k = 0; k < 4; ++k) {
            state ^= state << 13; state ^= state >> 17; state ^= state << 5;
            vec[k] = (double)(state % 10001) / 10000.;
        }
        assert(obs_property_rgba(rgba, vec) != OBS_PROPERTY_INVALID);
        assert(obs_property_world_position(matrix, vec) != OBS_PROPERTY_INVALID);
        assert(isfinite(matrix[3][0]) && rgba[0] >= 0 && rgba[0] <= 1);
    }
}
int main(void) {
    material_cases(); color_cases(); transform_cases(); fuzz_cases();
    puts("mesh_properties: 50,000 deterministic valid/failure atomicity cases OK");
}

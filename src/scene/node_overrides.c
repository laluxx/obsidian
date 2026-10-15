/// node_overrides.c — pure animation-overlay primitive
#include "node_overrides.h"
#include <float.h>
#include <math.h>

/// Finite transaction
ObsPropertyResult obs_node_offset_set(float offset[3], const double xyz[3]) {
    if (!offset || !xyz) return OBS_PROPERTY_INVALID;
    for (unsigned i = 0; i < 3; ++i)
        if (!isfinite(xyz[i]) || xyz[i] > FLT_MAX || xyz[i] < -FLT_MAX)
            return OBS_PROPERTY_INVALID;
    const float next[3] = {(float)xyz[0], (float)xyz[1], (float)xyz[2]};
    if (offset[0] == next[0] && offset[1] == next[1] && offset[2] == next[2])
        return OBS_PROPERTY_UNCHANGED;
    for (unsigned i = 0; i < 3; ++i) offset[i] = next[i];
    return OBS_PROPERTY_CHANGED;
}

/// Composing with sampled animation
void obs_node_apply_local_translation(float matrix[4][4], const float offset[3]) {
    if (!matrix || !offset ||
        (offset[0] == 0 && offset[1] == 0 && offset[2] == 0)) return;
    for (unsigned row = 0; row < 4; ++row)
        matrix[3][row] += matrix[0][row] * offset[0] +
                          matrix[1][row] * offset[1] +
                          matrix[2][row] * offset[2];
}

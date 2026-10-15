/// mesh_properties.c — atomic validated PBR and world-transform mutations
#include "mesh_properties.h"
#include <float.h>
#include <math.h>

/// Validation
static bool unit(double x) { return isfinite(x) && x >= 0.0 && x <= 1.0; }
static bool position(double x) {
    /* Reject conversion overflow, NaNs and infinities. Avoid arbitrarily
     * clipping valid world coordinates. */
    return isfinite(x) && x >= -FLT_MAX && x <= FLT_MAX;
}

/// Atomic field updates
ObsPropertyResult obs_property_pbr(float *roughness, float *metallic,
                                   double new_roughness, double new_metallic) {
    if (!roughness || !metallic || !unit(new_roughness) || !unit(new_metallic))
        return OBS_PROPERTY_INVALID;
    const float r = (float)new_roughness, m = (float)new_metallic;
    if (*roughness == r && *metallic == m) return OBS_PROPERTY_UNCHANGED;
    *roughness = r;
    *metallic = m;
    return OBS_PROPERTY_CHANGED;
}
ObsPropertyResult obs_property_rgba(float rgba[4], const double values[4]) {
    if (!rgba || !values) return OBS_PROPERTY_INVALID;
    for (unsigned i = 0; i < 4; ++i)
        if (!unit(values[i])) return OBS_PROPERTY_INVALID;
    bool changed = false;
    for (unsigned i = 0; i < 4; ++i)
        changed |= rgba[i] != (float)values[i];
    if (!changed) return OBS_PROPERTY_UNCHANGED;
    for (unsigned i = 0; i < 4; ++i) rgba[i] = (float)values[i];
    return OBS_PROPERTY_CHANGED;
}
ObsPropertyResult obs_property_world_position(float model[4][4],
                                               const double xyz[3]) {
    if (!model || !xyz) return OBS_PROPERTY_INVALID;
    for (unsigned i = 0; i < 3; ++i)
        if (!position(xyz[i])) return OBS_PROPERTY_INVALID;
    bool changed = false;
    for (unsigned i = 0; i < 3; ++i)
        changed |= model[3][i] != (float)xyz[i];
    if (!changed) return OBS_PROPERTY_UNCHANGED;
    for (unsigned i = 0; i < 3; ++i) model[3][i] = (float)xyz[i];
    return OBS_PROPERTY_CHANGED;
}

ObsPropertyResult obs_property_world_rotate_axis(float model[4][4],
                                                  const double axis[3],
                                                  double radians) {
    if (!model || !axis || !isfinite(radians)) return OBS_PROPERTY_INVALID;
    for (unsigned column = 0; column < 4; ++column)
        for (unsigned row = 0; row < 4; ++row)
            if (!isfinite(model[column][row])) return OBS_PROPERTY_INVALID;
    for (unsigned i = 0; i < 3; ++i)
        if (!isfinite(axis[i])) return OBS_PROPERTY_INVALID;

    const double axis_length = hypot(hypot(axis[0], axis[1]), axis[2]);
    if (!isfinite(axis_length) || axis_length == 0.0)
        return OBS_PROPERTY_INVALID;

    const double x = axis[0] / axis_length;
    const double y = axis[1] / axis_length;
    const double z = axis[2] / axis_length;
    const double cosine = cos(radians);
    const double sine = sin(radians);
    const double one_minus_cosine = 1.0 - cosine;
    const double rotation[3][3] = {
        {cosine + x * x * one_minus_cosine,
         x * y * one_minus_cosine - z * sine,
         x * z * one_minus_cosine + y * sine},
        {y * x * one_minus_cosine + z * sine,
         cosine + y * y * one_minus_cosine,
         y * z * one_minus_cosine - x * sine},
        {z * x * one_minus_cosine - y * sine,
         z * y * one_minus_cosine + x * sine,
         cosine + z * z * one_minus_cosine},
    };

    float rotated[3][3];
    bool changed = false;
    for (unsigned column = 0; column < 3; ++column) {
        for (unsigned row = 0; row < 3; ++row) {
            double value = 0.0;
            for (unsigned inner = 0; inner < 3; ++inner)
                value += rotation[row][inner] * model[column][inner];
            if (!isfinite(value) || value < -FLT_MAX || value > FLT_MAX)
                return OBS_PROPERTY_INVALID;
            rotated[column][row] = (float)value;
            changed |= rotated[column][row] != model[column][row];
        }
    }
    if (!changed) return OBS_PROPERTY_UNCHANGED;
    for (unsigned column = 0; column < 3; ++column)
        for (unsigned row = 0; row < 3; ++row)
            model[column][row] = rotated[column][row];
    return OBS_PROPERTY_CHANGED;
}

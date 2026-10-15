/// mesh_properties.h — validated, allocation-free mesh property transactions
#ifndef OBSIDIAN_SCENE_MESH_PROPERTIES_H
#define OBSIDIAN_SCENE_MESH_PROPERTIES_H
#include <stdbool.h>

/// Public interface
/* Pure CPU helpers: do not perform GPU work, acquire foreign references, or
 * publish a partially updated value. The caller marks the mesh GPU slot dirty
 * only for OBS_PROPERTY_CHANGED. */
typedef enum ObsPropertyResult {
    OBS_PROPERTY_INVALID = -1,
    OBS_PROPERTY_UNCHANGED = 0,
    OBS_PROPERTY_CHANGED = 1,
} ObsPropertyResult;

ObsPropertyResult obs_property_pbr(float *roughness, float *metallic,
                                   double new_roughness, double new_metallic);
ObsPropertyResult obs_property_rgba(float rgba[4], const double values[4]);
/* World-model translation only, not a node's animated local transform.
 * This deliberately does not imply persistence across animation updates. */
ObsPropertyResult obs_property_world_position(float model[4][4],
                                               const double xyz[3]);
/* Rotate the world-model basis around a normalized world-space axis, in
 * radians, while preserving translation and the complete affine basis shape. */
ObsPropertyResult obs_property_world_rotate_axis(float model[4][4],
                                                  const double axis[3],
                                                  double radians);
#endif

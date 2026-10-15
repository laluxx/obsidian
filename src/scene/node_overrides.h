/// node_overrides.h — animation-safe script translation offsets
#ifndef OBSIDIAN_SCENE_NODE_OVERRIDES_H
#define OBSIDIAN_SCENE_NODE_OVERRIDES_H
#include "mesh_properties.h"

/// Public interface
/* These offsets are runtime-only; never stored in the OMDL geometry cache. */
ObsPropertyResult obs_node_offset_set(float offset[3], const double xyz[3]);
/* Column-major matrix post-translation: matrix := matrix * T(offset). */
void obs_node_apply_local_translation(float matrix[4][4], const float offset[3]);
#endif

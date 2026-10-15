/// sdf_sculpt — checked procedural scene writes shared with Monad
#pragma once
#include <stdbool.h>
#include <stdint.h>
/* Positive monotonically nonreused Int64 IDs, valid within scene lifetime. */
bool obs_sculpt_sphere(const double xyz[3], double radius,
                       const double rgb[3], uint64_t *ticket);
bool obs_sculpt_move(uint64_t ticket, const double xyz[3]);
bool obs_sculpt_tint(uint64_t ticket, const double rgb[3]);
bool obs_sculpt_x(uint64_t ticket, double *x);
/* Exact world translation of a checked, unrotated, unscaled ticketed sphere. */
bool obs_sculpt_position(uint64_t ticket, double out[3]);
/* Inspector identity/position includes sphere, box and terrain. CPU viewport
 * picking includes only analytic sphere/box; the sphere API stays strict. */
bool obs_sculpt_position_any(uint64_t ticket, double out[3]);
uint64_t obs_sculpt_ticket_at(uint32_t index);
uint64_t obs_sculpt_ticket_any_at(uint32_t index);

/* Shared capability namespace for SDF spheres created through Monad or the
 * external agent. Tickets are positive, never reused, and scene-epoch scoped. */
bool obs_sculpt_index(uint64_t ticket, uint32_t *index);
bool obs_sculpt_index_any(uint64_t ticket, uint32_t *index);
bool obs_sculpt_can_ticket(void);

typedef struct {
    uint64_t ticket;
    uint32_t index;
    double distance;
    double position[3];
    double normal[3];
} ObsSculptRayHit;
/* Nearest primitive sphere hit in [0, max_distance], not a shader-exact SDF
 * union surface query. Zero direction, nonfinite, nontranslation transforms,
 * invalid scene topology and exhausted ticket namespace fail closed. */
bool obs_sculpt_ray_cast(const double origin[3], const double direction[3],
                         double max_distance, ObsSculptRayHit *out);
/* Nearest analytic sphere or translation-only axis-aligned box boundary.
 * Terrain and CSG-composed shader surfaces are deliberately not approximated. */
bool obs_sculpt_ray_cast_primitive(const double origin[3],
                                   const double direction[3],
                                   double max_distance,
                                   ObsSculptRayHit *out);

/// sdf_sculpt — checked, bounded operations on Obsidian's actual SDF scene
#ifdef OBS_SDF_TEST
#include "agent_engine_contract.h"
#else
#include "scene.h"
#include "sdf.h"
#endif
#include "sdf_sculpt.h"
#include "sdf_tokens.h"
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
static ObsSdfTickets s_sculpt_tickets;
static bool coord(double x) { return isfinite(x) && x > -(double)FLT_MAX && x < (double)FLT_MAX; }
static bool rgb_valid(const double c[3]) {
    return c && isfinite(c[0]) && isfinite(c[1]) && isfinite(c[2]) &&
        c[0] >= 0 && c[0] <= 1 && c[1] >= 0 && c[1] <= 1 &&
        c[2] >= 0 && c[2] <= 1;
}
bool obs_sculpt_can_ticket(void) { return obs_sdf_tickets_can_create(); }
bool obs_sculpt_index_any(uint64_t id, uint32_t *index) {
    uint32_t i = 0;
    if (!index || !obs_sdf_tickets_resolve(&s_sculpt_tickets,
            scene->sdf_lifetime_epoch, id, scene->sdf_count, &i) ||
        !scene->sdfs || scene->sdf_count > scene->sdf_capacity ||
        (scene->sdfs[i].type != SDF_TYPE_SPHERE &&
         scene->sdfs[i].type != SDF_TYPE_BOX &&
         scene->sdfs[i].type != SDF_TYPE_TERRAIN)) return false;
    *index = i;
    return true;
}
bool obs_sculpt_index(uint64_t id, uint32_t *index) {
    uint32_t i;
    if (!obs_sculpt_index_any(id, &i) ||
        scene->sdfs[i].type != SDF_TYPE_SPHERE) return false;
    if (!index) return false;
    *index = i;
    return true;
}
static SdfPrimitive *resolve(uint64_t id) {
    uint32_t i = 0;
    return obs_sculpt_index(id, &i) ? &scene->sdfs[i] : NULL;
}
bool obs_sculpt_sphere(const double xyz[3], double radius,
                       const double rgb[3], uint64_t *ticket) {
    if (!ticket || !xyz || !rgb_valid(rgb) ||
        !coord(xyz[0]) || !coord(xyz[1]) || !coord(xyz[2]) ||
        !isfinite(radius) || radius <= 0 || radius > (double)FLT_MAX ||
        !obs_sdf_tickets_can_create() ||
        scene->sdf_count >= OBS_SDF_TICKET_LIMIT ||
        !scene->sdf_lifetime_epoch) return false;
    vec3 pos = {(float)xyz[0], (float)xyz[1], (float)xyz[2]};
    vec4 size = {(float)radius, 0, 0, 0};
    vec4 color = {(float)rgb[0], (float)rgb[1], (float)rgb[2], 1};
    int32_t idx = obs_sdf_add_checked(scene, "Monad sphere", SDF_TYPE_SPHERE,
                                      pos, size, color);
    if (idx < 0) return false;
    /* Preflight above guarantees a ticket; cannot fail after scene mutation. */
    uint64_t id = obs_sculpt_ticket_at((uint32_t)idx);
    if (!id) return false;
    *ticket = id;
    return true;
}
bool obs_sculpt_move(uint64_t id, const double xyz[3]) {
    if (!xyz || !coord(xyz[0]) || !coord(xyz[1]) || !coord(xyz[2])) return false;
    SdfPrimitive *sdf = resolve(id);
    if (!sdf) return false;
    bool changed = false;
    for (unsigned i = 0; i < 3; ++i)
        changed |= sdf->inverseTransform[3][i] != -(float)xyz[i];
    if (changed) {
        for (unsigned i = 0; i < 3; ++i)
            sdf->inverseTransform[3][i] = -(float)xyz[i];
        flushSdfSSBO(); /* deferred to fenced frame slot */
    }
    return true;
}
bool obs_sculpt_tint(uint64_t id, const double rgb[3]) {
    if (!rgb_valid(rgb)) return false;
    SdfPrimitive *sdf = resolve(id);
    if (!sdf) return false;
    bool changed = false;
    for (unsigned i = 0; i < 3; ++i)
        changed |= sdf->color[i] != (float)rgb[i];
    if (changed) {
        for (unsigned i = 0; i < 3; ++i) sdf->color[i] = (float)rgb[i];
        flushSdfSSBO();
    }
    return true;
}
bool obs_sculpt_x(uint64_t id, double *x) {
    SdfPrimitive *sdf = resolve(id);
    if (!sdf || !x) return false;
    *x = -(double)sdf->inverseTransform[3][0];
    return isfinite(*x);
}
/* Translation-only checks are shared by the sphere API and the typed
 * viewport; box editing must not weaken the public sphere-only contract. */
static bool translated_position(const SdfPrimitive *s, double out[3]) {
    if (!s || !out) return false;
    for (unsigned col = 0; col < 4; ++col)
        for (unsigned row = 0; row < 4; ++row)
            if (!(col == 3 && row < 3) &&
                s->inverseTransform[col][row] != (float)(col == row))
                return false;
    for (unsigned i = 0; i < 3; ++i) {
        out[i] = -(double)s->inverseTransform[3][i];
        if (!coord(out[i])) return false;
    }
    return true;
}
bool obs_sculpt_position_any(uint64_t ticket, double out[3]) {
    uint32_t index = 0;
    return out && obs_sculpt_index_any(ticket, &index) &&
           translated_position(&scene->sdfs[index], out);
}
bool obs_sculpt_position(uint64_t ticket, double out[3]) {
    SdfPrimitive *s = resolve(ticket);
    return s && translated_position(s, out);
}
uint64_t obs_sculpt_ticket_any_at(uint32_t index) {
    if (!scene->sdfs || index >= scene->sdf_count ||
        scene->sdf_count > scene->sdf_capacity ||
        scene->sdf_count > OBS_SDF_TICKET_LIMIT) return 0;
    if ((scene->sdfs[index].type != SDF_TYPE_SPHERE &&
         scene->sdfs[index].type != SDF_TYPE_BOX &&
         scene->sdfs[index].type != SDF_TYPE_TERRAIN) ||
        !scene->sdf_lifetime_epoch) return 0;
    uint64_t ticket = obs_sdf_tickets_at(&s_sculpt_tickets,
        scene->sdf_lifetime_epoch, scene->sdf_count, index);
    if (ticket) return ticket;
    return obs_sdf_tickets_create(&s_sculpt_tickets,
        scene->sdf_lifetime_epoch, index);
}

uint64_t obs_sculpt_ticket_at(uint32_t index) {
    if (!scene->sdfs || index >= scene->sdf_count ||
        scene->sdf_count > scene->sdf_capacity ||
        scene->sdfs[index].type != SDF_TYPE_SPHERE) return 0;
    return obs_sculpt_ticket_any_at(index);
}

/* CPU selection matches primitive sphere and box geometry in sdf.frag.
 * This is intentionally NOT a full CSG/smooth-union or terrain raymarch.
 * Only translation-only transforms are eligible; types remain nominal at
 * the original sphere API. Each emitted hit is checked before publishing. */
static bool primitive_hit(const SdfPrimitive *s, const double origin[3],
                          const double unit[3], bool boxes,
                          double *distance, double normal[3]) {
    double center[3];
    if (!translated_position(s, center)) return false;
    if (s->type == SDF_TYPE_SPHERE) {
        double radius = s->size[0];
        if (!isfinite(radius) || radius <= 0 || radius > 1000) return false;
        double offset[3];
        for (unsigned k = 0; k < 3; ++k) offset[k] = origin[k] - center[k];
        double projection = offset[0]*unit[0] + offset[1]*unit[1] + offset[2]*unit[2];
        double distance2 = offset[0]*offset[0] + offset[1]*offset[1] + offset[2]*offset[2];
        double discriminant = projection*projection - (distance2 - radius*radius);
        if (!(discriminant >= 0) || !isfinite(discriminant)) return false;
        double root = sqrt(discriminant);
        double t = -projection - root;
        if (t < 0) t = -projection + root;
        if (!isfinite(t) || t < 0) return false;
        *distance = t;
        for (unsigned k = 0; k < 3; ++k)
            normal[k] = (origin[k] + unit[k]*t - center[k]) / radius;
        return true;
    }
    if (!boxes || s->type != SDF_TYPE_BOX) return false;
    double enter = -INFINITY, leave = INFINITY;
    int enter_axis = -1, leave_axis = -1;
    double enter_sign = 0, leave_sign = 0;
    for (unsigned k = 0; k < 3; ++k) {
        const double half = s->size[k]; /* sdBox(localPos, size.xyz) */
        if (!isfinite(half) || half <= 0 || half > 1000) return false;
        const double local = origin[k] - center[k];
        if (!isfinite(local)) return false;
        if (unit[k] == 0) {
            if (local < -half || local > half) return false;
            continue;
        }
        double t0 = (-half - local) / unit[k];
        double t1 = ( half - local) / unit[k];
        double s0 = -1.0, s1 = 1.0;
        if (t0 > t1) {
            double tmp = t0; t0 = t1; t1 = tmp;
            s0 = 1.0; s1 = -1.0;
        }
        /* Strict inequalities yield a deterministic X/Y/Z face on edges. */
        if (t0 > enter) { enter = t0; enter_axis = (int)k; enter_sign = s0; }
        if (t1 < leave) { leave = t1; leave_axis = (int)k; leave_sign = s1; }
        if (enter > leave) return false;
    }
    bool inside = enter < 0;
    double t = inside ? leave : enter;
    int axis = inside ? leave_axis : enter_axis;
    if (!isfinite(t) || t < 0 || axis < 0) return false;
    *distance = t;
    normal[0] = normal[1] = normal[2] = 0;
    normal[axis] = inside ? leave_sign : enter_sign;
    return true;
}
static bool ray_cast(const double origin[3], const double direction[3],
                     double max_distance, ObsSculptRayHit *out, bool boxes) {
    if (!origin || !direction || !out || !isfinite(max_distance) ||
        max_distance <= 0 || !scene->sdf_lifetime_epoch || !scene->sdfs ||
        !scene->sdf_count || scene->sdf_count > scene->sdf_capacity ||
        scene->sdf_count > OBS_SDF_TICKET_LIMIT) return false;
    for (unsigned i = 0; i < 3; ++i)
        if (!coord(origin[i]) || !coord(direction[i])) return false;
    double magnitude = hypot(hypot(direction[0], direction[1]), direction[2]);
    if (!isfinite(magnitude) || magnitude <= 0) return false;
    double unit[3];
    for (unsigned k = 0; k < 3; ++k) unit[k] = direction[k] / magnitude;
    double nearest = max_distance, chosen_normal[3] = {0};
    uint32_t nearest_slot = UINT32_MAX;
    for (uint32_t i = 0; i < scene->sdf_count; ++i) {
        double t, normal[3];
        if (!primitive_hit(&scene->sdfs[i], origin, unit, boxes, &t, normal) ||
            t > nearest || (t == nearest && nearest_slot != UINT32_MAX))
            continue;
        nearest = t;
        nearest_slot = i;
        memcpy(chosen_normal, normal, sizeof(chosen_normal));
    }
    if (nearest_slot == UINT32_MAX) return false;
    uint64_t ticket = boxes ? obs_sculpt_ticket_any_at(nearest_slot) :
                              obs_sculpt_ticket_at(nearest_slot);
    if (!ticket) return false;
    ObsSculptRayHit result = {.ticket = ticket, .index = nearest_slot, .distance = nearest};
    for (unsigned k = 0; k < 3; ++k) {
        result.position[k] = origin[k] + unit[k] * nearest;
        result.normal[k] = chosen_normal[k];
        if (!isfinite(result.position[k]) || !isfinite(result.normal[k])) return false;
    }
    *out = result;
    return true;
}
bool obs_sculpt_ray_cast(const double origin[3], const double direction[3],
                         double max_distance, ObsSculptRayHit *out) {
    return ray_cast(origin, direction, max_distance, out, false);
}
bool obs_sculpt_ray_cast_primitive(const double origin[3], const double direction[3],
                                   double max_distance, ObsSculptRayHit *out) {
    return ray_cast(origin, direction, max_distance, out, true);
}

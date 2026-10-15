/// geometry_pending — disjoint, unpublished scene geometry reservations
#pragma once

#include "geometry_budget.h"
#include <stdbool.h>
#include <stdint.h>

/// Ownership and publication contracts
/* Pure owner-thread reservation journal. It never touches a VkDevice, and
 * cannot prove GPU completion. A caller must retain its actual staging,
 * fence, command buffer and scene until the corresponding ticket is ready.
 *
 * Reservations use a monotonic high-water cursor, not the published cursor.
 * This prevents two in-flight scenes from receiving overlapping geometry.
 * Gaps from failed/non-tail loads are deliberate until the journal drains.
 * Existing legacy append allocators MUST be excluded or made to use the
 * high-water cursor before this is connected to concurrent Vulkan loads.
 */
#define OBS_PENDING_SCENES_MAX 16u

typedef enum {
    OBS_SCENE_EMPTY = 0,
    OBS_SCENE_PREPARED,
    OBS_SCENE_SUBMITTED,
    OBS_SCENE_READY,
    OBS_SCENE_UNKNOWN
} ObsPendingSceneState;

typedef enum {
    OBS_SCENE_FENCE_PENDING = 0,
    OBS_SCENE_FENCE_SIGNALED,
    OBS_SCENE_FENCE_UNKNOWN
} ObsPendingSceneFence;

typedef struct {
    uint64_t ticket;             /* epoch-protected logical load handle */
    uint64_t scene_id;           /* nonzero host scene identity */
    ObsPendingSceneState state;
    ObsGeometryReservation geometry;
    /* Original high-water cursor, before any per-stream alignment padding.
     * Needed to undo a PREPARED tail without leaking triangle padding. */
    uint32_t prior[OBS_GEOM_POOL_COUNT];
} ObsPendingScene;

typedef struct {
    ObsGeometryBudget pool_layout; /* capacities, strides; used is initial cursor */
    uint32_t highwater[OBS_GEOM_POOL_COUNT];
    uint32_t published[OBS_GEOM_POOL_COUNT];
    ObsPendingScene slots[OBS_PENDING_SCENES_MAX];
    uint64_t serial;             /* never reset, even after slot reuse */
    uint32_t live;
    bool initialized;
} ObsPendingScenes;

/// Prepare and submit
bool obs_pending_scenes_init(ObsPendingScenes *book, const ObsGeometryBudget *initial);
/* All pool counts are explicit. Bounds and skin counts must match meshlets.
 * On failure, the journal and output are unchanged. */
bool obs_pending_scene_reserve(ObsPendingScenes *book, uint64_t scene_id,
                               const uint32_t counts[OBS_GEOM_POOL_COUNT],
                               uint64_t *ticket);
bool obs_pending_scene_submitted(ObsPendingScenes *book, uint64_t ticket);
bool obs_pending_scene_observe(ObsPendingScenes *book, uint64_t ticket,
                               ObsPendingSceneFence observation);

/// Commit or retire
/* Scene-visible geometry addresses only become available after a signaled
 * completion. Rejected calls preserve the caller's output and journal. */
bool obs_pending_scene_publish(ObsPendingScenes *book, uint64_t ticket,
                               ObsGeometryReservation *published_geometry);
/* Cancellation is only possible before submission. */
bool obs_pending_scene_cancel(ObsPendingScenes *book, uint64_t ticket);
/* Caller must supply independent proof that GPU can no longer read/write the
 * reservation. A timeout, failed wait or device error is NOT such proof. */
bool obs_pending_scene_retire_quiesced(ObsPendingScenes *book, uint64_t ticket,
                                       bool gpu_quiesced);
/* Return to the committed high-water mark after all pending reservations are
 * retired. Only call when the owning scene/renderer remains alive. */
bool obs_pending_scenes_drain(ObsPendingScenes *book);

/// Copy-only inspection; never hand a script live C allocation addresses
bool obs_pending_scene_snapshot(const ObsPendingScenes *book, uint64_t ticket,
                                 ObsPendingScene *out);
uint32_t obs_pending_scenes_live(const ObsPendingScenes *book);

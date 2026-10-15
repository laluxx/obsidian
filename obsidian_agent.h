/// obsidian_agent — Versioned, opaque *outer* headless engine ABI
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OBS_AGENT_ABI_VERSION 1u

/* One active instance per process, on one thread. No Vulkan, Scene, MonadC,
 * allocation or platform types leak into this header. The returned pointer
 * becomes invalid after obs_agent_close; never share it with other threads. */
typedef struct ObsAgentEngine ObsAgentEngine;

typedef enum {
    OBS_AGENT_OK = 0,
    OBS_AGENT_INVALID_ARGUMENT,
    OBS_AGENT_BUSY,
    OBS_AGENT_WRONG_THREAD,
    OBS_AGENT_INITIALIZATION_FAILED,
    OBS_AGENT_MODULE_FAILED,
    OBS_AGENT_FRAME_FAILED,
    OBS_AGENT_OUTPUT_FAILED,
    OBS_AGENT_SHUTDOWN_FAILED,
    OBS_AGENT_STALE_REFERENCE,
    OBS_AGENT_RESOURCE_LIMIT,
    OBS_AGENT_NO_HIT,
    OBS_AGENT_HISTORY_EMPTY,
    OBS_AGENT_EDIT_CONFLICT
} ObsAgentStatus;

typedef struct {
    uint32_t abi_version;       /* OBS_AGENT_ABI_VERSION */
    uint32_t width;             /* 1..8192 */
    uint32_t height;            /* 1..8192 */
    bool call_camera_policy;    /* require compiled Int -> Int policy per frame */
} ObsAgentConfig;

typedef struct {
    ObsAgentStatus code;
    char message[256];         /* always NUL-terminated, caller-owned */
} ObsAgentError;

/* A value capability, not a pointer into the SDF array or a GPU address.
 * Never reuse a ticket; retain only while the scene and engine are alive. */
typedef struct { uint64_t ticket; } ObsAgentSdfRef;
typedef struct {
    double position[3];
    double radius;
    double rgba[4];
    double roughness;
    double metallic;
} ObsAgentSphere;

/* CPU ray/primitive-sphere intersection, not screen-space GPU picking and
 * not the exact smooth-union distance field. Returned ref is shared with
 * compiled Monad SDF tickets and remains scene-epoch checked. */
typedef struct {
    ObsAgentSdfRef ref;
    uint32_t index;
    double distance;
    double position[3];
    double normal[3];
} ObsAgentRayHit;

typedef void (*ObsAgentDraw)(void *userdata);

/* Initialization may perform synchronous Vulkan resource allocation and
 * MonadC runtime startup. Returns NULL on failure, with error if supplied. */
ObsAgentEngine *obs_agent_open(const ObsAgentConfig *config,
                               ObsAgentError *error);

/* Authoritative MonadC native source publication; it is NOT an interpreter.
 * Reloading an existing name uses MonadC's actual publication semantics,
 * not an engine-fabricated rollback. Requires the owner thread. */
ObsAgentStatus obs_agent_load_file(ObsAgentEngine *engine, const char *path,
                                   ObsAgentError *error);

/* Source-unit text is bounded to 16 MiB, compiled/published on the owner
 * thread. No arbitrary REPL expression evaluation or host-side rollback. */
ObsAgentStatus obs_agent_load_source(ObsAgentEngine *engine, const char *name,
                                     const char *source, ObsAgentError *error);

/* Each step invokes one production headless Vulkan frame. An optional draw
 * callback runs on the same owning thread during the frame; NULL is valid.
 * The engine also invokes its regular compiled Monad on-frame callback.
 * The camera policy is checked when call_camera_policy was requested. */
ObsAgentStatus obs_agent_step(ObsAgentEngine *engine, ObsAgentDraw draw,
                              void *userdata, ObsAgentError *error);

/* Capture submits ONE further frame, waits for real GPU readback and writes a
 * tightly packed RGBA8 file atomically. Optional JSON receipt is written on
 * success (separately atomic). A session is limited to 10000 frame steps.
 * The draw callback must not recursively call this API. */
ObsAgentStatus obs_agent_capture(ObsAgentEngine *engine, const char *raw_path,
                                 const char *receipt_path, ObsAgentDraw draw,
                                 void *userdata, ObsAgentError *error);

/* A complete scene edit through the real scene tree and renderer SDF SSBO.
 * Finite world coordinates and positive radius; RGBA/PBR factors in [0,1].
 * All mutation is owner-thread, outside frame callbacks and publication.
 * There is no native engine/world reset function in this ABI. */
ObsAgentStatus obs_agent_sphere_create(ObsAgentEngine *engine,
    const ObsAgentSphere *sphere, ObsAgentSdfRef *out, ObsAgentError *error);
ObsAgentStatus obs_agent_sphere_query(ObsAgentEngine *engine,
    ObsAgentSdfRef ref, ObsAgentSphere *out, ObsAgentError *error);
ObsAgentStatus obs_agent_sphere_set_position(ObsAgentEngine *engine,
    ObsAgentSdfRef ref, const double xyz[3], ObsAgentError *error);
ObsAgentStatus obs_agent_sphere_set_material(ObsAgentEngine *engine,
    ObsAgentSdfRef ref, const double rgba[4], double roughness,
    double metallic, ObsAgentError *error);

/* Quiesces GPU, closes the one compiler runtime, tears down the engine, and
 * invalidates its handle. A failed close leaves the handle live for retry. */
/* Read-only scene inspection, including spheres created by compiled Monad.
 * Index is a snapshot query, NOT a persistent mutation capability. */
ObsAgentStatus obs_agent_scene_sphere_count(ObsAgentEngine *engine,
    uint32_t *out, ObsAgentError *error);
ObsAgentStatus obs_agent_scene_sphere_at(ObsAgentEngine *engine,
    uint32_t index, ObsAgentSphere *out, ObsAgentError *error);

/* Normalizes a nonzero direction; returns nearest intersection in
 * [0,max_distance]. A miss is OBS_AGENT_NO_HIT and leaves *out unchanged.
 * All input components must be finite and bounded to float world range.
 * Handles created by compiled Monad are accepted by sphere_query/setters. */
ObsAgentStatus obs_agent_scene_pick_ray(ObsAgentEngine *engine,
    const double origin[3], const double direction[3], double max_distance,
    ObsAgentRayHit *out, ObsAgentError *error);

/* Cycles 52–53: shared selection and 64-record Emacs-style undo log. The editing
 * surface changes XYZ and/or material only, never radius/topology. It acts on
 * tickets from either compiled Monad or this outer API. Undo refuses to
 * clobber out-of-band mutations; discard_history permits explicit recovery.
 * Each undo records its inverse; unlike a linear redo stack, a command after
 * undo does not erase the original history.
 * History is volatile, bounded, and automatically expired on scene reset. */
#define OBS_AGENT_EDIT_POSITION 1u
#define OBS_AGENT_EDIT_MATERIAL 2u
typedef struct {
    ObsAgentSdfRef selected;
    uint32_t undo_count, redo_count;
} ObsAgentEditHistory;
ObsAgentStatus obs_agent_scene_select(ObsAgentEngine *engine,
    ObsAgentSdfRef ref, ObsAgentError *error);
ObsAgentStatus obs_agent_scene_selected(ObsAgentEngine *engine,
    ObsAgentSdfRef *out, ObsAgentError *error);
ObsAgentStatus obs_agent_scene_edit_selected(ObsAgentEngine *engine,
    uint32_t fields, const ObsAgentSphere *values, ObsAgentError *error);
ObsAgentStatus obs_agent_scene_undo(ObsAgentEngine *engine, ObsAgentError *error);
/* Undo the latest active command touching the selected SDF object. The
 * original multi-object group stays atomic; newer unrelated edits survive. */
ObsAgentStatus obs_agent_scene_undo_selected(ObsAgentEngine *engine,
                                             ObsAgentError *error);
/* Emacs-compatible distinction: undo reverses consecutive older commands;
 * after a command boundary, undoing an undo re-applies it. undo_only does
 * not redo. Explicit redo only applies to a recent undo-generated record. */
ObsAgentStatus obs_agent_scene_undo_only(ObsAgentEngine *engine, ObsAgentError *error);
ObsAgentStatus obs_agent_scene_command_boundary(ObsAgentEngine *engine,
    ObsAgentError *error);
/* Transactional scene gesture: begin stages live preview edits outside the
 * committed undo log; end commits as one reversible command; abort restores
 * all previews without losing earlier history. Other writers can cause a
 * conflict on end/abort; failed cancellation leaves the session active.
 * Bounded at 64 actual changes; no-op previews do not use a slot. */
ObsAgentStatus obs_agent_scene_begin_group(ObsAgentEngine *engine, ObsAgentError *error);
ObsAgentStatus obs_agent_scene_end_group(ObsAgentEngine *engine, ObsAgentError *error);
ObsAgentStatus obs_agent_scene_abort_group(ObsAgentEngine *engine, ObsAgentError *error);
ObsAgentStatus obs_agent_scene_redo(ObsAgentEngine *engine, ObsAgentError *error);
ObsAgentStatus obs_agent_scene_history(ObsAgentEngine *engine,
    ObsAgentEditHistory *out, ObsAgentError *error);
ObsAgentStatus obs_agent_scene_discard_history(ObsAgentEngine *engine,
    ObsAgentError *error);

ObsAgentStatus obs_agent_close(ObsAgentEngine *engine, ObsAgentError *error);

#ifdef __cplusplus
}
#endif

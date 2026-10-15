#ifdef OBS_SCENE_LOAD_VK_CONTRACT_TEST
#include "scene_load_vk_contract.h"
#else
#include "scene_load_vk.h"
#endif
#include <stdint.h>
#include <stddef.h>

/// Input span validation
/* BeginCooked is count-based, so reject short or dangling-looking source
 * spans BEFORE handing it any pointers. Never trust caller byte lengths. */
static bool span_fits(ObsGeometrySourceSpan span, uint32_t count,
                      size_t element_size) {
    return element_size && (size_t)count <= SIZE_MAX / element_size &&
           (span.bytes >= (size_t)count * element_size) &&
           (!count || span.data != NULL);
}

/// GPU allocation authority: actual Vulkan append cursor ownership
static ObsSceneStartResult start_upload(void *user, uint64_t id,
                const uint32_t count[OBS_GEOM_POOL_COUNT],
                const ObsGeometrySourceSpan spans[OBS_GEOM_POOL_COUNT],
                uint64_t *ticket) {
    ObsVkSceneLoadBridge *bridge = user;
    if (!bridge || !bridge->context || bridge->active_ticket ||
        !count || !spans || !ticket || !id ||
        count[OBS_GEOM_MESHLET] != count[OBS_GEOM_BOUNDS] ||
        count[OBS_GEOM_MESHLET] != count[OBS_GEOM_SKIN])
        return OBS_SCENE_START_REJECTED;

    const size_t sizes[OBS_GEOM_POOL_COUNT] = {
        sizeof(Vertex), sizeof(uint32_t), sizeof(MorphDelta),
        sizeof(Meshlet), sizeof(MeshletBounds), sizeof(MeshletSkinData),
        sizeof(uint32_t), sizeof(uint8_t),
    };
    for (unsigned i = 0; i < OBS_GEOM_POOL_COUNT; ++i)
        if (!span_fits(spans[i], count[i], sizes[i]))
            return OBS_SCENE_START_REJECTED;

    MegaGeometryCounts needed = {
        .vertices = count[OBS_GEOM_VERTEX],
        .indices = count[OBS_GEOM_INDEX],
        .morphs = count[OBS_GEOM_MORPH],
        .meshlets = count[OBS_GEOM_MESHLET],
        .meshlet_vertices = count[OBS_GEOM_MESHLET_VERTEX],
        .meshlet_triangles = count[OBS_GEOM_MESHLET_TRIANGLE],
    };
    MegaCookedGeometry data = {
        .vertices = spans[OBS_GEOM_VERTEX].data,
        .indices = spans[OBS_GEOM_INDEX].data,
        .morphs = spans[OBS_GEOM_MORPH].data,
        .meshlets = spans[OBS_GEOM_MESHLET].data,
        .bounds = spans[OBS_GEOM_BOUNDS].data,
        .skins = spans[OBS_GEOM_SKIN].data,
        .meshlet_vertices = spans[OBS_GEOM_MESHLET_VERTEX].data,
        .meshlet_triangles = spans[OBS_GEOM_MESHLET_TRIANGLE].data,
    };
    uint64_t issued = 0;
    bool accepted = megaGeometryAsyncBeginCooked(bridge->context, id,
                                                   &needed, &data, &issued);
    if (!accepted) {
        /* BeginCooked returns a ticket when an uncertain GPU submission
         * must be quarantined. A clean rejection has already cancelled its
         * reservation, and MUST NOT be adopted as a live SceneLoad. */
        if (!issued || megaGeometryAsyncPoll(issued) != OBS_ASYNC_QUARANTINED)
            return OBS_SCENE_START_REJECTED;
        bridge->counts = needed;
        bridge->active_ticket = *ticket = issued;
        return OBS_SCENE_START_UNKNOWN;
    }
    if (!issued) return OBS_SCENE_START_REJECTED;
    bridge->counts = needed;
    bridge->active_ticket = *ticket = issued;
    return OBS_SCENE_START_ACCEPTED;
}

static ObsAsyncState poll_upload(void *user, uint64_t ticket) {
    ObsVkSceneLoadBridge *bridge = user;
    if (!bridge || !ticket || ticket != bridge->active_ticket)
        return OBS_ASYNC_QUARANTINED;
    return megaGeometryAsyncPoll(ticket);
}

static bool commit_upload(void *user, uint64_t ticket, ObsSceneGpuBases *out) {
    ObsVkSceneLoadBridge *bridge = user;
    if (!bridge || !out || !ticket || bridge->active_ticket != ticket)
        return false;
    MegaGeometryBases actual;
    if (!megaGeometryAsyncCommit(bridge->context, ticket,
                                 &bridge->counts, &actual)) return false;
    *out = (ObsSceneGpuBases){
        .vertices = actual.vertices, .indices = actual.indices,
        .morphs = actual.morphs, .meshlets = actual.meshlets,
        .meshlet_vertices = actual.meshlet_vertices,
        .meshlet_triangles = actual.meshlet_triangles,
    };
    bridge->active_ticket = 0;
    return true;
}

static bool discard_upload(void *user, uint64_t ticket) {
    ObsVkSceneLoadBridge *bridge = user;
    if (!bridge || !ticket || bridge->active_ticket != ticket ||
        !megaGeometryAsyncDiscardReady(ticket)) return false;
    bridge->active_ticket = 0;
    return true;
}

static bool recover_upload(void *user, uint64_t ticket, bool quiesced) {
    ObsVkSceneLoadBridge *bridge = user;
    if (!bridge || !quiesced || !ticket ||
        bridge->active_ticket != ticket ||
        !megaGeometryAsyncRecoverDeviceIdle(bridge->context, ticket))
        return false;
    bridge->active_ticket = 0;
    return true;
}

/// Public adapter initialization
bool obs_vk_scene_load_bridge_init(ObsVkSceneLoadBridge *bridge,
                                   VulkanContext *context) {
    if (!bridge || !context || bridge->context || bridge->active_ticket)
        return false;
    bridge->context = context;
    return true;
}

ObsSceneLoadBackend obs_vk_scene_load_backend(ObsVkSceneLoadBridge *bridge) {
    if (!bridge || !bridge->context) return (ObsSceneLoadBackend){0};
    return (ObsSceneLoadBackend){
        .start = start_upload,
        .poll = poll_upload,
        .commit = commit_upload,
        .discard_ready = discard_upload,
        .recover_quiesced = recover_upload,
        .user = bridge,
    };
}

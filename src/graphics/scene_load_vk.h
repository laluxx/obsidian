/// scene_load_vk — Vulkan mega-buffer adapter for pending SceneLoad owners
#pragma once

#include "pending_load.h"
#include "vulkan_setup.h"

/// Vulkan-specific bridge
/* One bridge per renderer/context. It does not own the scene graph: the
 * pending_load owner does. It calls megaGeometryAsyncCommit, NEVER the pure
 * reservation journal, so GPU append cursors are updated as one operation. */
typedef struct {
    VulkanContext *context;
    MegaGeometryCounts counts;
    uint64_t active_ticket;
} ObsVkSceneLoadBridge;

/* Zero-initialize before init (for example: ObsVkSceneLoadBridge bridge = {0}). */
bool obs_vk_scene_load_bridge_init(ObsVkSceneLoadBridge *bridge,
                                   VulkanContext *context);
ObsSceneLoadBackend obs_vk_scene_load_backend(ObsVkSceneLoadBridge *bridge);

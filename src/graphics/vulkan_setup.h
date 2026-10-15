/// vulkan_setup — graphics subsystem
#pragma once
#include "context.h"
#include "geometry_budget.h"
#include "renderer.h"
#include "vk_memory_stats.h"
#include <cglm/types.h>
#include <stdbool.h>
#include <stdio.h>

/// Public interface

/* Convenience accessor — context.h stores lighting data as raw bytes to
   avoid a circular include; cast it here where renderer.h is visible.   */
#define CTX_LIGHTING(ctx) ((LightingData *)((ctx)->lightingDataRaw))

#define MAX_FRAMES_IN_FLIGHT 2
extern bool ambientOcclusionEnabled;

typedef struct {
  mat4 vp;
  mat4 view; // needed for view-space ops and IBL
  mat4 proj;
  vec4 cameraPos; // xyz=world pos, w=unused
  float time;     // for animated effects
  float _pad[3];
} UniformBufferObject;

/* ── instance / device ─────────────────────────── */
bool checkExtensionSupport(const char **requiredExtensions,
                           uint32_t requiredCount);
bool checkValidationLayerSupport(void);
void createInstance(VulkanContext *context);
void pickPhysicalDevice(VulkanContext *context);
void createLogicalDevice(VulkanContext *context);
bool obs_vk_memory_budget_snapshot(VulkanContext *context, uint32_t *heap_count,
                                   VkDeviceSize usage[VK_MAX_MEMORY_HEAPS],
                                   VkDeviceSize budget[VK_MAX_MEMORY_HEAPS]);
void obs_vk_name_object(VulkanContext *context, VkObjectType type,
                        uint64_t handle, const char *name);

/* ── swapchain ─────────────────────────────────── */
void createSwapChain(VulkanContext *context, ObsWindow *w);
void createImageViews(VulkanContext *context, ObsWindow *w);
void recreateSwapChain(VulkanContext *context, ObsWindow *w);
void cleanupSwapChain(VulkanContext *context, ObsWindow *w);
void cleanupSwapChainResources(VulkanContext *context, ObsWindow *w,
                               VkSwapchainKHR oldSwapchain);

/* ── descriptor layouts / pools ───────────────── */
void createDescriptorSetLayout(VulkanContext *context);
void create2DDescriptorSetLayout(VulkanContext *context);
void createDescriptorPool(VulkanContext *context);
void create2DDescriptorPool(VulkanContext *context);
void createDescriptorSet(VulkanContext *context);
void createBindlessDescriptorLayout(VulkanContext *ctx);
void createBindlessDescriptorPool(VulkanContext *ctx);
void createBindlessDescriptorSet(VulkanContext *ctx);
void createLightingDescriptors(VulkanContext *ctx);
void updateLightingUBO(VulkanContext *ctx);
bool loadIBL(VulkanContext *ctx, const char *hdr_path);
void destroyIBL(VulkanContext *ctx);
void bindlessRegisterTexture(VulkanContext *ctx, uint32_t slot,
                             VkImageView view, VkSampler sampler);

/* All direct VkDeviceMemory allocations in engine C sources pass through the
 * profiler's low-frequency resource-lifecycle accounting wrappers. */
#define vkAllocateMemory(device, info, allocator, memory)                      \
  (obs_vk_memory_next_owner(__func__),                                         \
   obs_vk_allocate_memory_tracked(device, info, allocator, memory))
#define vkFreeMemory obs_vk_free_memory_tracked

/* ── SSBO + indirect ───────────────────────────────────────────── */
void createMeshSSBO(VulkanContext *ctx, uint32_t maxMeshes);
bool obs_mesh_ssbo_reserve(VulkanContext *ctx, uint32_t required_meshes);
void createIndirectBuffer(VulkanContext *ctx, uint32_t maxMeshes);
void updateMeshSSBOAndIndirect(VulkanContext *ctx, Meshes *meshes);
void markMeshesSSBODirty(VulkanContext *ctx);
void markMeshDirty(VulkanContext *ctx, uint32_t meshIndex);
void flushMeshSSBO(VulkanContext *ctx, Meshes *meshes);
void createComputeCullPipeline(VulkanContext *ctx);
void createComputeCompactPipeline(VulkanContext *ctx);
void dispatchFrustumCull(VulkanContext *ctx, VkCommandBuffer cmd);

/* ── pipelines (all created in one batch call) ─── */
void createAllPipelineLayouts(
    VulkanContext *context); /* call before createGraphicsPipelines */
void createIndirectPipelineLayout(
    VulkanContext *context); /* call after createMeshSSBO */
void createGraphicsPipelines(VulkanContext *context);
bool obs_vk_set_antialiasing_request(VulkanContext *context, int64_t requested);
/* handle for the indirect (SSBO-driven) pipeline variants */
extern VkPipeline pipelineIndirectSolid;
extern VkPipeline pipelineIndirectTextured;

/* Legacy stubs — kept for call-site compatibility; each is a no-op because
   createGraphicsPipelines() already handles all of them.                   */
void createGraphicsPipeline(VulkanContext *context);
void create2DGraphicsPipeline(VulkanContext *context);
void createTextured2DGraphicsPipeline(VulkanContext *context);
void create3DTexturedGraphicsPipeline(VulkanContext *context);
void createLineGraphicsPipeline(VulkanContext *context);

/* ── commands / sync ────────────── */
void createCommandPool(VulkanContext *context);
void createCommandBuffers(VulkanContext *context, ObsWindow *w);
void createFrameSyncObjects(VulkanContext *context);
void createWindowSyncObjects(VulkanContext *context, ObsWindow *w);

/* ── depth & shadows ───────────────────────────── */
void createDepthResources(VulkanContext *context, ObsWindow *w);
void createShadowResources(VulkanContext *context);

/* ── uniform buffer ────────────────────────────── */
void createUniformBuffer(VulkanContext *context);
void updateUniformBuffer(VulkanContext *context);

/* ── frame loop ────────────────────────────────── */
void drawFrame(VulkanContext *context);
void recordCommandBuffer(VulkanContext *context, ObsWindow *w,
                         uint32_t imageIndex);
void shadowCacheFrameSubmitted(VulkanContext *context);

/* ── misc ──────────────────────────────────────── */
void clear_background(Color color);
void toggle_ambient_occlusion(void);
extern bool skyboxEnabled;
extern bool iblLightingEnabled;
extern bool shadowsEnabled;
extern bool cullingFrozen;

void cleanup(VulkanContext *context);
uint32_t findMemoryType(VkPhysicalDevice physDev, uint32_t typeFilter,
                        VkMemoryPropertyFlags props);
VkCommandBuffer beginSingleTimeCommands(VkDevice device, VkCommandPool pool);
void endSingleTimeCommands(VkDevice device, VkCommandPool pool, VkQueue queue,
                           VkCommandBuffer cmd);
void copyBuffer(VkDevice device, VkCommandPool pool, VkQueue queue,
                VkBuffer src, VkBuffer dst, VkDeviceSize size,
                VkDeviceSize srcOffset, VkDeviceSize dstOffset);

/* One aggregate capacity gate for all glTF/OMDL geometry streams. It may grow
 * index/morph backing arenas (waiting for existing queue users only on growth),
 * but does not append geometry or submit the scene's upload. Call before any
 * per-pool upload. */
typedef struct {
  uint32_t vertices, indices, morphs, meshlets;
  uint32_t meshlet_vertices, meshlet_triangles;
} MegaGeometryCounts;
bool megaGeometryPreflight(VulkanContext *ctx,
                           const MegaGeometryCounts *counts);
/* Cache geometry is fully CPU-prepared before Vulkan submission. The returned
 * bases and global append offsets become visible only after fence completion.
 * This is a blocking, checked transfer, not asynchronous GPU streaming. */
typedef struct {
  uint32_t vertices, indices, morphs, meshlets;
  uint32_t meshlet_vertices, meshlet_triangles;
} MegaGeometryBases;
bool megaGeometryUploadCacheBatch(VulkanContext *ctx, FILE *file,
                                  const MegaGeometryCounts *counts,
                                  MegaGeometryBases *bases);

/* All eight freshly cooked streams, including the independent meshlet
 * bounds/skin payloads. Null is accepted only for an empty stream.
 * Borrowed until preparation completes; the Vulkan upload owns a CPU copy. */
typedef struct {
  const Vertex *vertices;
  const uint32_t *indices;
  const MorphDelta *morphs;
  const Meshlet *meshlets;
  const MeshletBounds *bounds;
  const MeshletSkinData *skins;
  const uint32_t *meshlet_vertices;
  const uint8_t *meshlet_triangles;
} MegaCookedGeometry;
bool megaGeometryUploadCookedBatch(VulkanContext *ctx,
                                   const MegaGeometryCounts *counts,
                                   const MegaCookedGeometry *sources,
                                   MegaGeometryBases *bases);

/// Opt-in nonblocking geometry transfer; scene publication is caller-owned
/* This API serializes legacy mega-buffer appenders while a transfer is
 * active. The main glTF loader remains synchronous. Poll never waits; callers
 * must keep pending scene state private until Commit succeeds. Begin returns
 * a valid ticket even for uncertain submission; inspect Poll and use explicit
 * device-idle recovery rather than dropping that ticket. */
#include "geometry_async.h"
bool megaGeometryAsyncBeginCooked(VulkanContext *ctx, uint64_t scene_id,
                                  const MegaGeometryCounts *counts,
                                  const MegaCookedGeometry *sources,
                                  uint64_t *ticket);
ObsAsyncState megaGeometryAsyncPoll(uint64_t ticket);
bool megaGeometryAsyncCommit(VulkanContext *ctx, uint64_t ticket,
                             const MegaGeometryCounts *counts,
                             MegaGeometryBases *bases);
bool megaGeometryAsyncDiscardReady(uint64_t ticket);
bool megaGeometryAsyncRecoverDeviceIdle(VulkanContext *ctx, uint64_t ticket);

/* ── mega-buffer / dynamic buffer helpers ─────── */
void createMegaVertexBuffer(VulkanContext *ctx, VkDeviceSize size);
void createMegaIndexBuffer(VulkanContext *ctx, VkDeviceSize size);
bool createDynamicBuffers(VulkanContext *ctx, VkDeviceSize size);
bool megaVertexBufferReserve(VulkanContext *ctx, uint64_t required_vertices);
bool megaIndexBufferReserve(VulkanContext *ctx, uint64_t required_indices);
void createUploadStagingBuffer(VulkanContext *ctx, VkDeviceSize size);
void flushUploadStagingBuffer(VulkanContext *ctx);
void destroyUploadStagingBuffer(VulkanContext *ctx);
uint32_t megaBufferAllocate(VulkanContext *ctx, Vertex *vertices,
                            uint32_t vertexCount);
uint32_t megaIndexBufferAllocate(VulkanContext *ctx, uint32_t *indices,
                                 uint32_t indexCount);
void dynamicBufferUploadAndCopy(VulkanContext *ctx, void *data,
                                VkDeviceSize size, VkDeviceSize dstOffset);

void gltf_load_enqueue(const char *filepath);

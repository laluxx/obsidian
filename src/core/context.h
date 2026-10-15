/// context — core subsystem
#pragma once
#define GLFW_INCLUDE_VULKAN
#include "common.h"
#include "gpu_profile.h"
#include "render_graph.h"
#include <GLFW/glfw3.h>
#include <stdbool.h>

/// Public interface

#define MAX_FRAMES_IN_FLIGHT 2

typedef struct ObsRetiredSwapchain ObsRetiredSwapchain;

/* ── Window ────────────────────────────────────────────────────────────────
 *
 * Everything that exists per OS window: the surface, its swapchain and
 * depth buffer, the command buffers that render into it, and the sync
 * objects tied to its images.  Device-global state (instance, queues,
 * pipelines, buffers) stays in VulkanContext and is shared by every window.
 *
 * The frame-slot counter (context.currentFrame) is GLOBAL and advances once
 * per window frame, so the shared per-slot mapped buffers (dynamic vertices,
 * UBO) keep their double-buffering contract no matter how many windows draw
 * in one tick. */
typedef struct ObsWindow {
  GLFWwindow *window;
  VkSurfaceKHR surface;
  bool headless;
  bool render2DOnly;
  bool pointerPositionKnown;
  double pointerX, pointerY;
  VkImage rawColorImage[MAX_FRAMES_IN_FLIGHT];
  VkDeviceMemory rawColorMemory[MAX_FRAMES_IN_FLIGHT];
  VkBuffer rawReadbackBuffer[MAX_FRAMES_IN_FLIGHT];
  VkDeviceMemory rawReadbackMemory[MAX_FRAMES_IN_FLIGHT];
  void *rawReadbackMapped[MAX_FRAMES_IN_FLIGHT];

  VkSwapchainKHR swapChain;
  VkImage *swapChainImages;
  uint32_t swapChainImageCount;
  VkFormat swapChainImageFormat;
  VkExtent2D swapChainExtent;
  VkImageView *swapChainImageViews;

  VkImage depthImage[MAX_FRAMES_IN_FLIGHT];
  VkDeviceMemory depthImageMemory[MAX_FRAMES_IN_FLIGHT];
  VkImageView depthImageView[MAX_FRAMES_IN_FLIGHT];
  VkFormat depthFormat;
  VkImage multisampleColorImage[MAX_FRAMES_IN_FLIGHT];
  VkDeviceMemory multisampleColorMemory[MAX_FRAMES_IN_FLIGHT];
  VkImageView multisampleColorView[MAX_FRAMES_IN_FLIGHT];
  VkImage multisampleDepthImage[MAX_FRAMES_IN_FLIGHT];
  VkDeviceMemory multisampleDepthMemory[MAX_FRAMES_IN_FLIGHT];
  VkImageView multisampleDepthView[MAX_FRAMES_IN_FLIGHT];

  // Per-window post-processing resources.  These used to live in global
  // arrays, which made the second window overwrite the first window's HZB
  // and transmission images (and eventually exhaust its descriptor pool).
  VkImage transmissionImage[MAX_FRAMES_IN_FLIGHT];
  VkDeviceMemory transmissionMemory[MAX_FRAMES_IN_FLIGHT];
  VkImageView transmissionView[MAX_FRAMES_IN_FLIGHT];
  VkImage hzbImage[MAX_FRAMES_IN_FLIGHT];
  VkDeviceMemory hzbMemory[MAX_FRAMES_IN_FLIGHT];
  VkImageView hzbView[MAX_FRAMES_IN_FLIGHT];
  VkImageView hzbMipViews[MAX_FRAMES_IN_FLIGHT][16];
  VkDescriptorSet hzbSets[MAX_FRAMES_IN_FLIGHT][16];
  VkDescriptorPool hzbDescriptorPool;
  uint32_t hzbMipCount;
  bool hzbValid[MAX_FRAMES_IN_FLIGHT];

  VkCommandBuffer *commandBuffers;       // per swapchain image
  VkSemaphore *renderFinishedSemaphores; // per swapchain image
  VkFence *imagesInFlight;               // per swapchain image
  VkFence *presentFences;                // maintenance1 completion fences
  bool *presentFencePending;
  bool framebufferResized;
  uint32_t resizeFramebufferWidth;
  uint32_t resizeFramebufferHeight;
  double resizeLastEventTime;
  bool suspended;
  bool swapchainOutOfDate;
  uint64_t swapchainGeneration;
  ObsRetiredSwapchain *retiredSwapchains;

  struct ObsWindow *next;
} ObsWindow;

struct ObsRetiredSwapchain {
  /* Snapshot of the old window-owned WSI resources. window/surface/next are
   * borrowed and are never destroyed by the snapshot cleanup path. */
  ObsWindow resources;
  ObsWindow *owner;
  uint64_t generation;
  struct ObsRetiredSwapchain *next;
};

typedef struct {
  // ── windows ──────────────────────────────────────────────────────
  GLFWwindow *window;    // primary window (legacy convenience)
  ObsWindow *primary;    // legacy single-window path
  ObsWindow *windows;    // registry of all live windows
  VkExtent2D drawExtent; // extent of the window being drawn

  // ── core ────────────────────────────────────────────────────────
  VkInstance instance;
  bool headless;
  bool headlessReadbackEnabled;
  bool profileWorkCountersEnabled;
  uint64_t profileDrawCalls;
  uint64_t profileIndirectDraws;
  uint64_t profileSubmittedVertices;
  uint64_t profileSubmittedIndices;
  uint64_t profileMeshTaskWorkgroups;
  uint64_t profileComputeDispatches;
  uint64_t profileComputeWorkgroups;
  uint32_t profileWorkStageDepth;
  ObsGpuProfileStage profileWorkStageStack[8];
  uint64_t profileDrawCallsByStage[OBS_GPU_PROFILE_STAGE_COUNT];
  uint64_t profileIndirectDrawsByStage[OBS_GPU_PROFILE_STAGE_COUNT];
  uint64_t profileVerticesByStage[OBS_GPU_PROFILE_STAGE_COUNT];
  uint64_t profileIndicesByStage[OBS_GPU_PROFILE_STAGE_COUNT];
  uint64_t profileMeshTaskWorkgroupsByStage[OBS_GPU_PROFILE_STAGE_COUNT];
  uint64_t profileDispatchesByStage[OBS_GPU_PROFILE_STAGE_COUNT];
  uint64_t profileComputeWorkgroupsByStage[OBS_GPU_PROFILE_STAGE_COUNT];
  bool debugLabelsEnabled;
  bool memoryBudgetEnabled;
  bool shaderStorageImageExtendedFormatsEnabled;
  PFN_vkCmdBeginDebugUtilsLabelEXT cmdBeginDebugUtilsLabel;
  PFN_vkCmdEndDebugUtilsLabelEXT cmdEndDebugUtilsLabel;
  PFN_vkSetDebugUtilsObjectNameEXT setDebugUtilsObjectName;
  VkPhysicalDevice physicalDevice;
  VkDevice device;
  uint32_t graphicsQueueFamily;
  VkQueue graphicsQueue;
  VkSampleCountFlagBits rasterizationSamples;
  VkSampleCountFlags supportedSampleCounts;
  VkResolveModeFlagBits depthResolveMode;

  // ── descriptor layouts ───────────────────────────────────────────
  VkDescriptorSetLayout descriptorSetLayout;   // set=0: UBO  (3D)
  VkDescriptorSetLayout descriptorSetLayout2D; // set=0/1: sampler

  // ── per-frame UBO resources (one per frame-in-flight) ───────────
  VkBuffer uniformBuffers[MAX_FRAMES_IN_FLIGHT];
  VkDeviceMemory uniformBuffersMemory[MAX_FRAMES_IN_FLIGHT];
  void *uboMapped[MAX_FRAMES_IN_FLIGHT];                // persistent map
  VkDescriptorPool descriptorPool;                      // UBO pool
  VkDescriptorSet descriptorSets[MAX_FRAMES_IN_FLIGHT]; // one per frame

  // ── texture descriptor pool (2D / 3D textured / SDF) ────────────
  VkDescriptorPool descriptorPool2D;

  // ── pipeline layouts ─────────────────────────────────────────────
  VkPipelineLayout pipelineLayout;           // 3D solid + line
  VkPipelineLayout pipelineLayoutTextured3D; // 3D textured + SDF3D
  VkPipelineLayout pipelineLayoutIndirect;
  VkPipeline computeCullPipeline;
  VkPipelineLayout computeCullPipelineLayout;
  VkDescriptorSetLayout computeCullSetLayout;
  VkDescriptorPool computeCullPool;
  VkDescriptorSet computeCullSets[MAX_FRAMES_IN_FLIGHT];

  VkPipeline computeCompactPipeline;
  VkPipelineLayout computeCompactPipelineLayout;
  VkDescriptorSetLayout computeCompactSetLayout;
  VkDescriptorPool computeCompactPool;
  VkDescriptorSet computeCompactSets[MAX_FRAMES_IN_FLIGHT];
  VkBuffer frustumUBOBuffer[4];
  VkDeviceMemory frustumUBOMemory[4];
  void *frustumUBOMapped[4];

  VkPipelineLayout pipelineLayout2D;         // 2D color
  VkPipelineLayout pipelineLayoutTextured2D; // 2D textured + SDF2D
  // aliases — point to the same handles, never destroyed separately
  VkPipelineLayout pipelineLayoutLine;
  VkPipelineLayout pipelineLayoutSDF2D;
  VkPipelineLayout pipelineLayoutSDF3D;

  // ── pipelines ────────────────────────────────────────────────────
  VkPipeline graphicsPipeline;           // 3D solid
  VkPipeline graphicsPipelineTextured3D; // 3D textured
  VkPipeline graphicsPipelineSDF3D;      // 3D SDF
  VkPipeline graphicsPipelineLine;       // 3D lines
  VkPipeline graphicsPipeline2D;         // 2D color
  VkPipeline graphicsPipelineTextured2D; // 2D textured
  VkPipeline graphicsPipelineSDF2D;      // 2D SDF

  // ── 2D vertex buffer ─────────────────────────────────────────────
  VkBuffer vertexBuffer2D[MAX_FRAMES_IN_FLIGHT];
  VkDeviceMemory vertexBufferMemory2D[MAX_FRAMES_IN_FLIGHT];
  void *vertexBuffer2DMapped[MAX_FRAMES_IN_FLIGHT];

  // ── commands ─────────────────────────────────────────────────────
  VkCommandPool commandPool;

  // ── sync ─────────────────────────────────────────────────────────
  // Global, per frame SLOT (not per window): frames are drawn sequentially
  // across windows, and the slot advances once per window frame, so a slot
  // is never in flight for two windows at once.
  VkSemaphore *imageAvailableSemaphores;
  VkFence *inFlightFences;
  uint32_t currentFrame;
  ObsWindow *frameSlotWindow[MAX_FRAMES_IN_FLIGHT];
  uint64_t frameSlotGeneration[MAX_FRAMES_IN_FLIGHT];
  bool swapchainMaintenanceEnabled;
  bool surfaceMaintenance1Enabled;
  VkQueryPool profileTimestampPool;
  uint32_t profileTimestampValidBits;
  double profileTimestampPeriod;
  uint64_t profileGeometryUploadGpuNs;
  bool profileGeometryUploadGpuSampled;
  uint64_t profileStageNs[MAX_FRAMES_IN_FLIGHT][OBS_GPU_PROFILE_STAGE_COUNT];
  uint32_t profileStageMask[MAX_FRAMES_IN_FLIGHT];
  uint64_t profileFrameSequence[MAX_FRAMES_IN_FLIGHT];
  bool profileQueryPending[MAX_FRAMES_IN_FLIGHT];

  Color clearColor;

  // ── mega vertex buffer (static mesh geometry, DEVICE_LOCAL) ──
  VkBuffer megaVertexBuffer;
  VkDeviceMemory megaVertexBufferMemory;
  VkDeviceSize megaVertexBufferSize;
  uint32_t megaVertexBufferOffset;
  VkBuffer megaIndexBuffer;
  VkDeviceMemory megaIndexBufferMemory;
  VkDeviceSize megaIndexBufferSize;
  uint32_t megaIndexBufferOffset;

  // ── mega morph buffer (static morph deltas, DEVICE_LOCAL) ──
  VkBuffer megaMorphBuffer;
  VkDeviceMemory megaMorphBufferMemory;
  VkDeviceSize megaMorphBufferSize;
  uint32_t megaMorphBufferOffset;

  // ── morph weight buffer (dynamic per-frame, HOST_VISIBLE) ──
  VkBuffer morphWeightBuffer[MAX_FRAMES_IN_FLIGHT];
  VkDeviceMemory morphWeightMemory[MAX_FRAMES_IN_FLIGHT];
  void *morphWeightMapped[MAX_FRAMES_IN_FLIGHT];
  uint32_t morphWeightOffset;

  // ── persistent upload staging buffer ─────────────────────────────
  // One large HOST_VISIBLE buffer used for all mesh uploads.
  // Batched regions are flushed in a single vkCmdCopyBuffer call.
  VkBuffer uploadStagingBuffer;
  VkDeviceMemory uploadStagingMemory;
  void *uploadStagingMapped;
  VkDeviceSize uploadStagingSize;
  VkDeviceSize uploadStagingOffset; /* current write head */

  // pending copy regions accumulated during load, flushed once at end
  VkBufferCopy *pendingVertexCopies;
  uint32_t pendingVertexCopyCount;
  uint32_t pendingVertexCopyCapacity;
  VkBufferCopy *pendingIndexCopies;
  uint32_t pendingIndexCopyCount;
  uint32_t pendingIndexCopyCapacity;
  VkBufferCopy *pendingMeshletCopies;
  uint32_t pendingMeshletCopyCount;
  uint32_t pendingMeshletCopyCapacity;
  VkBufferCopy *pendingMeshletBoundsCopies;
  uint32_t pendingMeshletSkinCopyCapacity;
  uint32_t pendingMeshletSkinCopyCount;
  VkBufferCopy *pendingMeshletSkinCopies;
  uint32_t pendingMeshletBoundsCopyCount;
  uint32_t pendingMeshletBoundsCopyCapacity;
  VkBufferCopy *pendingMeshletVertexCopies;
  uint32_t pendingMeshletVertexCopyCount;
  uint32_t pendingMeshletVertexCopyCapacity;
  VkBufferCopy *pendingMeshletTriangleCopies;
  uint32_t pendingMeshletTriangleCopyCount;
  uint32_t pendingMeshletTriangleCopyCapacity;

  // ── per-frame dynamic buffers (2D, lines, morph) ──
  // staging (HOST_VISIBLE|HOST_COHERENT, persistently mapped)
  VkBuffer dynamicStagingBuffer;
  VkDeviceMemory dynamicStagingMemory;
  void *dynamicStagingMapped;
  VkDeviceSize dynamicStagingSize;

  // ── shadow mapping ───────────────────────────────────────────────
  VkImage shadowImage[MAX_FRAMES_IN_FLIGHT];
  VkDeviceMemory shadowMemory[MAX_FRAMES_IN_FLIGHT];
  VkImageView shadowView[MAX_FRAMES_IN_FLIGHT];
  VkSampler shadowSampler;
  uint64_t shadowCacheKey[MAX_FRAMES_IN_FLIGHT];
  bool shadowCacheValid[MAX_FRAMES_IN_FLIGHT];
  uint64_t shadowCachePendingKey;
  uint32_t shadowCachePendingFrameSlot;
  bool shadowCachePending;
  bool shadowCachePendingInvalidate;
  bool shadowCacheReuse;

  // ── bindless texture array ───────────────────────────────────────
  VkDescriptorSetLayout bindlessSetLayout;
  VkDescriptorPool bindlessPool;
  VkDescriptorSet bindlessSet;
  uint32_t bindlessTextureCount;

  // ── IBL resources ────────────────────────────────────────────────
  // All three are registered into the bindless array at fixed slots:
  //   IBL_IRRADIANCE_SLOT      = MAX_TEXTURES - 3
  //   IBL_PREFILTER_SLOT       = MAX_TEXTURES - 2
  //   IBL_BRDF_LUT_SLOT        = MAX_TEXTURES - 1
  VkImage iblIrradianceImage;
  VkDeviceMemory iblIrradianceMemory;
  VkImageView iblIrradianceView;
  VkSampler iblIrradianceSampler;

  VkImage iblPrefilterImage;
  VkDeviceMemory iblPrefilterMemory;
  VkImageView iblPrefilterView;
  VkSampler iblPrefilterSampler;

  VkImage iblBrdfLutImage;
  VkDeviceMemory iblBrdfLutMemory;
  VkImageView iblBrdfLutView;
  VkSampler iblBrdfLutSampler;

  bool iblLoaded;

  // ── scene lighting UBO ───────────────────────────────────────────
  VkDescriptorSetLayout lightingSetLayout;
  VkDescriptorPool lightingPool;
  VkDescriptorSet lightingSets[MAX_FRAMES_IN_FLIGHT];
  VkBuffer lightingUBO[MAX_FRAMES_IN_FLIGHT];
  VkDeviceMemory lightingUBOMemory[MAX_FRAMES_IN_FLIGHT];
  void *lightingUBOMapped[MAX_FRAMES_IN_FLIGHT];
  /* LightingData is defined in renderer.h — use a raw byte buffer here
     to break the circular dependency. Cast to LightingData* at use sites. */
  _Alignas(16) uint8_t lightingDataRaw[1024]; // expanded for shadow matrices

  // ── SSBO: per-mesh data (model matrix, texture index, flags) ─────
  VkBuffer meshSSBO[MAX_FRAMES_IN_FLIGHT];
  VkDeviceMemory meshSSBOMemory[MAX_FRAMES_IN_FLIGHT];
  void *meshSSBOMapped[MAX_FRAMES_IN_FLIGHT];
  VkDescriptorSetLayout ssboSetLayout;
  VkDescriptorPool ssboPool;
  VkDescriptorSet ssboSets[MAX_FRAMES_IN_FLIGHT];

  // ── immediate-mode scratch buffer ────────────────────────────────
  // Used ONLY by the legacy vertex shader path: lines, debug prims,
  // emit_draw_with_slot. Mesh shader draws bypass this entirely.
  // One slot per (static mesh + dynamic draw), two frames in flight.
  VkBuffer indirectBuffer; /* alias: scratch for imm-mode draws      */
  VkDeviceMemory indirectBufferMemory;
  void *srcIndirectBufferMapped; /* persistently mapped write ptr          */
  uint32_t indirectDrawCount;    /* total draws dispatched this frame      */
  uint32_t ssboFramesDirty;
  uint64_t *meshDirtyBits;
  uint32_t meshDirtyCapacity;

  RgGraph *renderGraph;
} VulkanContext;

static inline void obs_profile_count_draw(VulkanContext *ctx, uint64_t draws,
                                          uint64_t vertices, uint64_t indices) {
  if (!ctx || !ctx->profileWorkCountersEnabled)
    return;
  ctx->profileDrawCalls += draws;
  ctx->profileSubmittedVertices += vertices;
  ctx->profileSubmittedIndices += indices;
  for (uint32_t i = 0; i < ctx->profileWorkStageDepth; ++i) {
    ObsGpuProfileStage stage = ctx->profileWorkStageStack[i];
    ctx->profileDrawCallsByStage[stage] += draws;
    ctx->profileVerticesByStage[stage] += vertices;
    ctx->profileIndicesByStage[stage] += indices;
  }
}

static inline void obs_profile_count_mesh_tasks(VulkanContext *ctx, uint32_t x,
                                                uint32_t y, uint32_t z) {
  if (!ctx || !ctx->profileWorkCountersEnabled)
    return;
  ctx->profileDrawCalls++;
  ctx->profileMeshTaskWorkgroups += (uint64_t)x * y * z;
  for (uint32_t i = 0; i < ctx->profileWorkStageDepth; ++i) {
    ObsGpuProfileStage stage = ctx->profileWorkStageStack[i];
    ctx->profileDrawCallsByStage[stage]++;
    ctx->profileMeshTaskWorkgroupsByStage[stage] += (uint64_t)x * y * z;
  }
}

static inline void obs_profile_count_indirect_draws(VulkanContext *ctx,
                                                    uint32_t draws) {
  if (!ctx || !ctx->profileWorkCountersEnabled)
    return;
  ctx->profileDrawCalls += draws;
  ctx->profileIndirectDraws += draws;
  for (uint32_t i = 0; i < ctx->profileWorkStageDepth; ++i) {
    ObsGpuProfileStage stage = ctx->profileWorkStageStack[i];
    ctx->profileDrawCallsByStage[stage] += draws;
    ctx->profileIndirectDrawsByStage[stage] += draws;
  }
}

static inline void obs_profile_count_dispatch(VulkanContext *ctx, uint32_t x,
                                              uint32_t y, uint32_t z) {
  if (!ctx || !ctx->profileWorkCountersEnabled)
    return;
  ctx->profileComputeDispatches++;
  uint64_t workgroups = (uint64_t)x * y * z;
  ctx->profileComputeWorkgroups += workgroups;
  for (uint32_t i = 0; i < ctx->profileWorkStageDepth; ++i) {
    ObsGpuProfileStage stage = ctx->profileWorkStageStack[i];
    ctx->profileDispatchesByStage[stage]++;
    ctx->profileComputeWorkgroupsByStage[stage] += workgroups;
  }
}

extern VulkanContext context;

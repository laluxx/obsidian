#pragma once
#include "context.h"
#include "renderer.h"
#include <stdbool.h>
#include <cglm/types.h>

#define MAX_FRAMES_IN_FLIGHT 2
extern bool ambientOcclusionEnabled;

typedef struct {
    mat4 vp;
} UniformBufferObject;

/* ── instance / device ─────────────────────────── */
bool checkExtensionSupport(const char** requiredExtensions, uint32_t requiredCount);
bool checkValidationLayerSupport(void);
void createInstance(VulkanContext* context);
void pickPhysicalDevice(VulkanContext* context);
void createLogicalDevice(VulkanContext* context);

/* ── swapchain ─────────────────────────────────── */
void createSwapChain(VulkanContext* context);
void createImageViews(VulkanContext* context);
void recreateSwapChain(VulkanContext* context);
void cleanupSwapChain(VulkanContext* context);
void cleanupSwapChainResources(VulkanContext* context, VkSwapchainKHR oldSwapchain);

/* ── render pass ───────────────────────────────── */
void createRenderPass(VulkanContext* context);

/* ── descriptor layouts / pools ───────────────── */
void createDescriptorSetLayout(VulkanContext* context);
void create2DDescriptorSetLayout(VulkanContext* context);
void createDescriptorPool(VulkanContext* context);
void create2DDescriptorPool(VulkanContext* context);
void createDescriptorSet(VulkanContext* context);
void createBindlessDescriptorLayout(VulkanContext* context);
void createBindlessDescriptorPool(VulkanContext* context);
void createBindlessDescriptorSet(VulkanContext* context);
void bindlessRegisterTexture(VulkanContext* ctx, uint32_t slot,
                             VkImageView view, VkSampler sampler);

/* ── SSBO + indirect ───────────────────────────────────────────── */
void createMeshSSBO(VulkanContext* ctx, uint32_t maxMeshes);
void createIndirectBuffer(VulkanContext* ctx, uint32_t maxMeshes);
void updateMeshSSBOAndIndirect(VulkanContext* ctx, Meshes* meshes);
void markMeshesSSBODirty(VulkanContext* ctx);
void markMeshDirty(VulkanContext* ctx, uint32_t meshIndex);
void flushMeshSSBO(VulkanContext* ctx, Meshes* meshes);
void createComputeCullPipeline(VulkanContext* ctx);
void dispatchFrustumCull(VulkanContext* ctx, VkCommandBuffer cmd);

/* ── pipelines (all created in one batch call) ─── */
void createAllPipelineLayouts(VulkanContext* context);   /* call before createGraphicsPipelines */
void createIndirectPipelineLayout(VulkanContext* context); /* call after createMeshSSBO */
void createGraphicsPipelines(VulkanContext* context);
/* handle for the indirect (SSBO-driven) pipeline variants */
extern VkPipeline pipelineIndirectSolid;
extern VkPipeline pipelineIndirectTextured;

/* Legacy stubs — kept for call-site compatibility; each is a no-op because
   createGraphicsPipelines() already handles all of them.                   */
void createGraphicsPipeline(VulkanContext* context);
void create2DGraphicsPipeline(VulkanContext* context);
void createTextured2DGraphicsPipeline(VulkanContext* context);
void create3DTexturedGraphicsPipeline(VulkanContext* context);
void createLineGraphicsPipeline(VulkanContext* context);
void createSDF2DGraphicsPipeline(VulkanContext* context);
void createSDF3DGraphicsPipeline(VulkanContext* context);

/* ── framebuffers / commands / sync ────────────── */
void createFramebuffers(VulkanContext* context);
void createCommandPool(VulkanContext* context);
void createCommandBuffers(VulkanContext* context);
void createSyncObjects(VulkanContext* context);

/* ── depth ─────────────────────────────────────── */
void createDepthResources(VulkanContext* context);

/* ── uniform buffer ────────────────────────────── */
void createUniformBuffer(VulkanContext* context);
void updateUniformBuffer(VulkanContext* context);

/* ── frame loop ────────────────────────────────── */
void drawFrame(VulkanContext* context);
void recordCommandBuffer(VulkanContext* context, uint32_t imageIndex);

/* ── misc ──────────────────────────────────────── */
void clear_background(Color color);
void cleanup(VulkanContext* context);
uint32_t findMemoryType(VkPhysicalDevice physDev, uint32_t typeFilter, VkMemoryPropertyFlags props);
VkCommandBuffer beginSingleTimeCommands(VkDevice device, VkCommandPool pool);
void endSingleTimeCommands(VkDevice device, VkCommandPool pool, VkQueue queue, VkCommandBuffer cmd);
void copyBuffer(VkDevice device, VkCommandPool pool, VkQueue queue,
                VkBuffer src, VkBuffer dst, VkDeviceSize size, VkDeviceSize srcOffset, VkDeviceSize dstOffset);
void toggle_ambient_occlusion(void);

/* ── mega-buffer / dynamic buffer helpers ─────── */
void createMegaVertexBuffer(VulkanContext* ctx, VkDeviceSize size);
void createMegaIndexBuffer(VulkanContext* ctx, VkDeviceSize size);
void createDynamicBuffers(VulkanContext* ctx, VkDeviceSize size);
uint32_t megaBufferAllocate(VulkanContext* ctx, Vertex* vertices, uint32_t vertexCount);
uint32_t megaIndexBufferAllocate(VulkanContext* ctx, uint32_t* indices, uint32_t indexCount);
void dynamicBufferUploadAndCopy(VulkanContext* ctx, void* data, VkDeviceSize size, VkDeviceSize dstOffset);

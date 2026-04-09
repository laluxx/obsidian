#pragma once
#include "context.h"
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

/* ── pipelines (all created in one batch call) ─── */
void createAllPipelineLayouts(VulkanContext* context);   /* call before createGraphicsPipelines */
void createGraphicsPipelines(VulkanContext* context);    /* creates all 7 pipelines at once     */

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
void toggle_ambient_occlusion(void);

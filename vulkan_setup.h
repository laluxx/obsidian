#pragma once

#include "context.h"
#include <stdbool.h>
#include <cglm/types.h>


#define MAX_FRAMES_IN_FLIGHT 2

extern VkDescriptorPool descriptorPool;
extern VkDescriptorSet descriptorSet;
extern VkBuffer uniformBuffer;
extern VkDeviceMemory uniformBufferMemory;

extern bool ambientOcclusionEnabled;


typedef struct {
    mat4 vp;
} UniformBufferObject;


void create2DDescriptorSetLayout(VulkanContext* context);
void create2DDescriptorPool(VulkanContext *context);
void createDescriptorSet(VulkanContext *context);
void createDescriptorPool(VulkanContext *context);
bool checkExtensionSupport(const char** requiredExtensions, uint32_t requiredCount);
bool checkValidationLayerSupport();
void createInstance(VulkanContext *context);
void pickPhysicalDevice(VulkanContext *context);
void createLogicalDevice(VulkanContext *context);
void createSwapChain(VulkanContext *context);
void createImageViews(VulkanContext *context);
void createRenderPass(VulkanContext *context);
void createTextured2DGraphicsPipeline(VulkanContext *context);
void create2DGraphicsPipeline(VulkanContext *context);
void create3DTexturedGraphicsPipeline(VulkanContext *context);
void createLineGraphicsPipeline(VulkanContext *context);
void createGraphicsPipeline(VulkanContext *context);
void createFramebuffers(VulkanContext *context);
void createCommandPool(VulkanContext *context);
void createCommandBuffers(VulkanContext *context);
void createSyncObjects(VulkanContext *context);
void createDepthResources(VulkanContext *context);
void drawFrame(VulkanContext *context);

void createUniformBuffer(VulkanContext* context);
void createDescriptorSetLayout(VulkanContext *context);
void clear_background(Color color);
void recordCommandBuffer(VulkanContext* context, uint32_t imageIndex);


void cleanup(VulkanContext* context);


void toggle_ambient_occlusion();





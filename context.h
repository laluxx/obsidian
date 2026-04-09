#pragma once
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include "common.h"
#include <stdbool.h>

#define MAX_FRAMES_IN_FLIGHT 2

typedef struct {
    // ── core ────────────────────────────────────────────────────────
    GLFWwindow*          window;
    VkInstance           instance;
    VkPhysicalDevice     physicalDevice;
    VkDevice             device;
    uint32_t             graphicsQueueFamily;
    VkQueue              graphicsQueue;
    VkSurfaceKHR         surface;

    // ── swapchain ────────────────────────────────────────────────────
    VkSwapchainKHR       swapChain;
    VkImage*             swapChainImages;
    uint32_t             swapChainImageCount;
    VkFormat             swapChainImageFormat;
    VkExtent2D           swapChainExtent;
    VkImageView*         swapChainImageViews;
    VkFramebuffer*       swapChainFramebuffers;

    // ── render pass ──────────────────────────────────────────────────
    VkRenderPass         renderPass;

    // ── depth ────────────────────────────────────────────────────────
    VkImage              depthImage;
    VkDeviceMemory       depthImageMemory;
    VkImageView          depthImageView;
    VkFormat             depthFormat;

    // ── descriptor layouts ───────────────────────────────────────────
    VkDescriptorSetLayout descriptorSetLayout;    // set=0: UBO  (3D)
    VkDescriptorSetLayout descriptorSetLayout2D;  // set=0/1: sampler

    // ── per-frame UBO resources (one per frame-in-flight) ───────────
    VkBuffer             uniformBuffers[MAX_FRAMES_IN_FLIGHT];
    VkDeviceMemory       uniformBuffersMemory[MAX_FRAMES_IN_FLIGHT];
    void*                uboMapped[MAX_FRAMES_IN_FLIGHT];        // persistent map
    VkDescriptorPool     descriptorPool;                         // UBO pool
    VkDescriptorSet      descriptorSets[MAX_FRAMES_IN_FLIGHT];   // one per frame

    // ── texture descriptor pool (2D / 3D textured / SDF) ────────────
    VkDescriptorPool     descriptorPool2D;

    // ── pipeline layouts ─────────────────────────────────────────────
    VkPipelineLayout     pipelineLayout;           // 3D solid + line
    VkPipelineLayout     pipelineLayoutTextured3D; // 3D textured + SDF3D
    VkPipelineLayout     pipelineLayoutIndirect;
    VkPipelineLayout     pipelineLayout2D;         // 2D color
    VkPipelineLayout     pipelineLayoutTextured2D; // 2D textured + SDF2D
    // aliases — point to the same handles, never destroyed separately
    VkPipelineLayout     pipelineLayoutLine;
    VkPipelineLayout     pipelineLayoutSDF2D;
    VkPipelineLayout     pipelineLayoutSDF3D;

    // ── pipelines ────────────────────────────────────────────────────
    VkPipeline           graphicsPipeline;           // 3D solid
    VkPipeline           graphicsPipelineTextured3D; // 3D textured
    VkPipeline           graphicsPipelineSDF3D;      // 3D SDF
    VkPipeline           graphicsPipelineLine;       // 3D lines
    VkPipeline           graphicsPipeline2D;         // 2D color
    VkPipeline           graphicsPipelineTextured2D; // 2D textured
    VkPipeline           graphicsPipelineSDF2D;      // 2D SDF

    // ── 2D vertex buffer ─────────────────────────────────────────────
    VkBuffer vertexBuffer2D;
    VkDeviceMemory vertexBufferMemory2D;
    void* vertexBuffer2DMapped;

    // ── commands ─────────────────────────────────────────────────────
    VkCommandPool        commandPool;
    VkCommandBuffer*     commandBuffers;

    // ── sync ─────────────────────────────────────────────────────────
    VkSemaphore*         imageAvailableSemaphores;
    VkSemaphore*         renderFinishedSemaphores;
    VkFence*             inFlightFences;
    VkFence*             imagesInFlight;
    uint32_t             currentFrame;

    // ── misc ─────────────────────────────────────────────────────────
    Color                clearColor;
    bool                 framebufferResized;

    // ── mega vertex buffer (static mesh geometry, DEVICE_LOCAL) ──
    VkBuffer         megaVertexBuffer;
    VkDeviceMemory   megaVertexBufferMemory;
    VkDeviceSize     megaVertexBufferSize;
    uint32_t         megaVertexBufferOffset;
    VkBuffer         megaIndexBuffer;
    VkDeviceMemory   megaIndexBufferMemory;
    VkDeviceSize     megaIndexBufferSize;
    uint32_t         megaIndexBufferOffset; /* next free vertex index */

    // ── per-frame dynamic buffers (2D, lines, morph) ──
    // staging (HOST_VISIBLE|HOST_COHERENT, persistently mapped)
    VkBuffer         dynamicStagingBuffer;
    VkDeviceMemory   dynamicStagingMemory;
    void*            dynamicStagingMapped;

    // device-local target (GPU reads from here)
    VkBuffer         dynamicDeviceBuffer;
    VkDeviceMemory   dynamicDeviceMemory;
    VkDeviceSize     dynamicBufferSize;

    // ── bindless texture array ───────────────────────────────────────
    VkDescriptorSetLayout bindlessSetLayout;
    VkDescriptorPool      bindlessPool;
    VkDescriptorSet       bindlessSet;          // one set, all textures
    uint32_t              bindlessTextureCount;

    // ── SSBO: per-mesh data (model matrix, texture index, flags) ─────
    VkBuffer              meshSSBO[MAX_FRAMES_IN_FLIGHT];
    VkDeviceMemory        meshSSBOMemory[MAX_FRAMES_IN_FLIGHT];
    void*                 meshSSBOMapped[MAX_FRAMES_IN_FLIGHT];
    VkDescriptorSetLayout ssboSetLayout;
    VkDescriptorPool      ssboPool;
    VkDescriptorSet       ssboSets[MAX_FRAMES_IN_FLIGHT];

    // ── indirect draw buffer ────────────────────────────────────────
    VkBuffer              indirectBuffer;
    VkDeviceMemory        indirectBufferMemory;
    void*                 indirectBufferMapped;
    uint32_t              indirectDrawCount;
    uint32_t              ssboFramesDirty;    /* counts down from MAX_FRAMES_IN_FLIGHT to 0 */
} VulkanContext;

extern VulkanContext context;


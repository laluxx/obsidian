#pragma once
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include "common.h"
#include "render_graph.h"
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
    VkSwapchainKHR        swapChain;
    VkImage* swapChainImages;
    uint32_t              swapChainImageCount;
    VkFormat              swapChainImageFormat;
    VkExtent2D            swapChainExtent;
    VkImageView* swapChainImageViews;

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
    VkPipelineLayout pipelineLayoutIndirect;
    VkPipeline       computeCullPipeline;
    VkPipelineLayout computeCullPipelineLayout;
    VkDescriptorSetLayout computeCullSetLayout;
    VkDescriptorPool      computeCullPool;
    VkDescriptorSet       computeCullSets[4];  /* 2 per frame (camera + sun) */
    VkBuffer         frustumUBOBuffer[4];
    VkDeviceMemory   frustumUBOMemory[4];
    void* frustumUBOMapped[4];

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
    VkBuffer         vertexBuffer2D[MAX_FRAMES_IN_FLIGHT];
    VkDeviceMemory   vertexBufferMemory2D[MAX_FRAMES_IN_FLIGHT];
    void*            vertexBuffer2DMapped[MAX_FRAMES_IN_FLIGHT];

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
    uint32_t         megaIndexBufferOffset;

    // ── persistent upload staging buffer ─────────────────────────────
    // One large HOST_VISIBLE buffer used for all mesh uploads.
    // Batched regions are flushed in a single vkCmdCopyBuffer call.
    VkBuffer         uploadStagingBuffer;
    VkDeviceMemory   uploadStagingMemory;
    void*            uploadStagingMapped;
    VkDeviceSize     uploadStagingSize;
    VkDeviceSize     uploadStagingOffset;  /* current write head */

    // pending copy regions accumulated during load, flushed once at end
    VkBufferCopy*    pendingVertexCopies;
    uint32_t         pendingVertexCopyCount;
    uint32_t         pendingVertexCopyCapacity;
    VkBufferCopy*    pendingIndexCopies;
    uint32_t         pendingIndexCopyCount;
    uint32_t         pendingIndexCopyCapacity;

    // ── per-frame dynamic buffers (2D, lines, morph) ──
    // staging (HOST_VISIBLE|HOST_COHERENT, persistently mapped)
    VkBuffer         dynamicStagingBuffer;
    VkDeviceMemory   dynamicStagingMemory;
    void*            dynamicStagingMapped;

    // device-local target (GPU reads from here)
    VkBuffer         dynamicDeviceBuffer;
    VkDeviceMemory   dynamicDeviceMemory;
    VkDeviceSize     dynamicBufferSize;

    // ── shadow mapping ───────────────────────────────────────────────
    VkImage          shadowImage;
    VkDeviceMemory   shadowMemory;
    VkImageView      shadowView;
    VkSampler        shadowSampler;

    // ── bindless texture array ───────────────────────────────────────
    VkDescriptorSetLayout bindlessSetLayout;
    VkDescriptorPool      bindlessPool;
    VkDescriptorSet       bindlessSet;
    uint32_t              bindlessTextureCount;

    // ── IBL resources ────────────────────────────────────────────────
    // All three are registered into the bindless array at fixed slots:
    //   IBL_IRRADIANCE_SLOT      = MAX_TEXTURES - 3
    //   IBL_PREFILTER_SLOT       = MAX_TEXTURES - 2
    //   IBL_BRDF_LUT_SLOT        = MAX_TEXTURES - 1
    VkImage              iblIrradianceImage;
    VkDeviceMemory       iblIrradianceMemory;
    VkImageView          iblIrradianceView;
    VkSampler            iblIrradianceSampler;

    VkImage              iblPrefilterImage;
    VkDeviceMemory       iblPrefilterMemory;
    VkImageView          iblPrefilterView;
    VkSampler            iblPrefilterSampler;

    VkImage              iblBrdfLutImage;
    VkDeviceMemory       iblBrdfLutMemory;
    VkImageView          iblBrdfLutView;
    VkSampler            iblBrdfLutSampler;

    bool                 iblLoaded;

    // ── scene lighting UBO ───────────────────────────────────────────
    VkDescriptorSetLayout lightingSetLayout;
    VkDescriptorPool      lightingPool;
    VkDescriptorSet       lightingSets[MAX_FRAMES_IN_FLIGHT];
    VkBuffer              lightingUBO[MAX_FRAMES_IN_FLIGHT];
    VkDeviceMemory        lightingUBOMemory[MAX_FRAMES_IN_FLIGHT];
    void* lightingUBOMapped[MAX_FRAMES_IN_FLIGHT];
    /* LightingData is defined in renderer.h — use a raw byte buffer here
       to break the circular dependency. Cast to LightingData* at use sites. */
    uint8_t               lightingDataRaw[1024];  // expanded for shadow matrices

    // ── SSBO: per-mesh data (model matrix, texture index, flags) ─────
    VkBuffer              meshSSBO[MAX_FRAMES_IN_FLIGHT];
    VkDeviceMemory        meshSSBOMemory[MAX_FRAMES_IN_FLIGHT];
    void*                 meshSSBOMapped[MAX_FRAMES_IN_FLIGHT];
    VkDescriptorSetLayout ssboSetLayout;
    VkDescriptorPool      ssboPool;
    VkDescriptorSet       ssboSets[MAX_FRAMES_IN_FLIGHT];

    // ── indirect draw buffer ────────────────────────────────────────
    VkBuffer         indirectBuffer;       /* GPU-written output, read by draw */
    VkDeviceMemory   indirectBufferMemory;
    VkBuffer         srcIndirectBuffer;    /* CPU-written source, read by compute */
    VkDeviceMemory   srcIndirectBufferMemory;
    void*            srcIndirectBufferMapped;
    uint32_t         indirectDrawCount;
    uint32_t         ssboFramesDirty;    /* counts down from MAX_FRAMES_IN_FLIGHT to 0 */
    uint64_t*        meshDirtyBits;      /* one bit per mesh, per frame */
    uint32_t         meshDirtyCapacity;  /* in bits */

    RgGraph* renderGraph;
} VulkanContext;

extern VulkanContext context;


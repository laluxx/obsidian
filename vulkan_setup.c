#include "vulkan_setup.h"
#include "renderer.h"
#include "context.h"
#include "scene.h"
#include "camera.h"
#include <vulkan/vulkan_core.h>
#include <cglm/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pbr.vert.spv.h"
#include "pbr.frag.spv.h"
#include "2D.vert.spv.h"
#include "2D.frag.spv.h"
#include "cull.comp.spv.h"
#include "stb_image.h"
#include "equirect2cube.comp.spv.h"
#include "irradiance.comp.spv.h"
#include "prefilter.comp.spv.h"
#include "brdf_lut.comp.spv.h"
#include "skybox.vert.spv.h"
#include "skybox.frag.spv.h"

/// Globals
bool skyboxEnabled = true;
bool iblLightingEnabled = true;
VkPipeline skyboxPipeline = VK_NULL_HANDLE;
VkPipelineLayout skyboxPipelineLayout = VK_NULL_HANDLE;
VkImage iblSkyboxImage = VK_NULL_HANDLE;
VkDeviceMemory iblSkyboxMemory = VK_NULL_HANDLE;
VkImageView iblSkyboxView = VK_NULL_HANDLE;

/// Validation

#define ENABLE_VALIDATION_LAYERS 1

#if ENABLE_VALIDATION_LAYERS
#define VALIDATION_LAYERS_COUNT 1
static const char* validationLayers[VALIDATION_LAYERS_COUNT] = {
    "VK_LAYER_KHRONOS_validation"
};
#else
#define VALIDATION_LAYERS_COUNT 0
static const char** validationLayers = NULL;
#endif

/// Globals

bool ambientOcclusionEnabled = true;

static VkPipelineCache pipelineCache = VK_NULL_HANDLE;

VkPipeline pipelineIndirectSolid    = VK_NULL_HANDLE;
VkPipeline pipelineIndirectTextured = VK_NULL_HANDLE;

/// Helpers

static void createBuffer(VulkanContext* ctx, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer* buffer, VkDeviceMemory* bufferMemory);

/* Create a shader module from an embedded SPIR-V byte array. */
static VkShaderModule createShaderModule(VkDevice device,
                                         const uint8_t* code,
                                         size_t codeSize)
{
    VkShaderModuleCreateInfo ci = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = codeSize,
        .pCode    = (const uint32_t*)code
    };
    VkShaderModule mod;
    if (vkCreateShaderModule(device, &ci, NULL, &mod) != VK_SUCCESS) {
        fprintf(stderr, "vkCreateShaderModule failed\n");
        exit(EXIT_FAILURE);
    }
    return mod;
}

/* Shared vertex-input / input-assembly structs for 3D geometry. */
static void fill3DVertexInput(VkPipelineVertexInputStateCreateInfo*    vi,
                               VkPipelineInputAssemblyStateCreateInfo* ia,
                               VkVertexInputBindingDescription*        bind,
                               VkVertexInputAttributeDescription       attrs[5],
                               VkPrimitiveTopology                     topology)
{
    *bind = (VkVertexInputBindingDescription){
        .binding   = 0,
        .stride    = sizeof(Vertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX
    };
    attrs[0] = (VkVertexInputAttributeDescription){.location=0, .binding=0, .format=VK_FORMAT_R32G32B32_SFLOAT,    .offset=offsetof(Vertex, pos)};
    attrs[1] = (VkVertexInputAttributeDescription){.location=1, .binding=0, .format=VK_FORMAT_R32G32B32A32_SFLOAT, .offset=offsetof(Vertex, color)};
    attrs[2] = (VkVertexInputAttributeDescription){.location=2, .binding=0, .format=VK_FORMAT_R32G32B32_SFLOAT,    .offset=offsetof(Vertex, normal)};
    attrs[3] = (VkVertexInputAttributeDescription){.location=3, .binding=0, .format=VK_FORMAT_R32G32_SFLOAT,       .offset=offsetof(Vertex, texCoord)};
    attrs[4] = (VkVertexInputAttributeDescription){.location=4, .binding=0, .format=VK_FORMAT_R32G32B32A32_SFLOAT, .offset=offsetof(Vertex, tangent)};

    *vi = (VkPipelineVertexInputStateCreateInfo){
        .sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount   = 1,
        .pVertexBindingDescriptions      = bind,
        .vertexAttributeDescriptionCount = 5,
        .pVertexAttributeDescriptions    = attrs
    };
    *ia = (VkPipelineInputAssemblyStateCreateInfo){
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology               = topology,
        .primitiveRestartEnable = VK_FALSE
    };
}


/* Shared vertex-input / input-assembly structs for 2D geometry. */
static void fill2DVertexInput(VkPipelineVertexInputStateCreateInfo*    vi,
                               VkPipelineInputAssemblyStateCreateInfo* ia,
                               VkVertexInputBindingDescription*        bind,
                               VkVertexInputAttributeDescription       attrs[4])
{
    *bind = (VkVertexInputBindingDescription){
        .binding   = 0,
        .stride    = sizeof(Vertex2D),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX
    };
    attrs[0] = (VkVertexInputAttributeDescription){.location=0, .binding=0, .format=VK_FORMAT_R32G32_SFLOAT,       .offset=offsetof(Vertex2D, pos)};
    attrs[1] = (VkVertexInputAttributeDescription){.location=1, .binding=0, .format=VK_FORMAT_R32G32B32A32_SFLOAT, .offset=offsetof(Vertex2D, color)};
    attrs[2] = (VkVertexInputAttributeDescription){.location=2, .binding=0, .format=VK_FORMAT_R32G32_SFLOAT,       .offset=offsetof(Vertex2D, texCoord)};
    attrs[3] = (VkVertexInputAttributeDescription){.location=3, .binding=0, .format=VK_FORMAT_R32G32_SINT,         .offset=offsetof(Vertex2D, textureIndex)};

    *vi = (VkPipelineVertexInputStateCreateInfo){
        .sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount   = 1,
        .pVertexBindingDescriptions      = bind,
        .vertexAttributeDescriptionCount = 4,
        .pVertexAttributeDescriptions    = attrs
    };
    *ia = (VkPipelineInputAssemblyStateCreateInfo){
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology               = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE
    };
}

/* Shared rasterizer, multisample, color-blend, dynamic-state blocks. */
static VkPipelineRasterizationStateCreateInfo makeRasterizer(float lineWidth) {
    return (VkPipelineRasterizationStateCreateInfo){
        .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode             = VK_POLYGON_MODE_FILL,
        .lineWidth               = lineWidth,
        .cullMode                = VK_CULL_MODE_NONE,
        .frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE,
    };
}

static const VkPipelineMultisampleStateCreateInfo kMultisampling = {
    .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
};

static const VkPipelineColorBlendAttachmentState kBlendAlpha = {
    .colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    .blendEnable         = VK_TRUE,
    .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
    .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    .colorBlendOp        = VK_BLEND_OP_ADD,
    .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
    .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
    .alphaBlendOp        = VK_BLEND_OP_ADD,
};

static VkPipelineColorBlendStateCreateInfo makeColorBlend(
    const VkPipelineColorBlendAttachmentState* att)
{
    return (VkPipelineColorBlendStateCreateInfo){
        .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments    = att,
    };
}

static const VkDynamicState kDynStates[] = {
    VK_DYNAMIC_STATE_VIEWPORT,
    VK_DYNAMIC_STATE_SCISSOR
};
static const VkPipelineDynamicStateCreateInfo kDynamicState = {
    .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
    .dynamicStateCount = 2,
    .pDynamicStates    = kDynStates
};

static const VkPipelineViewportStateCreateInfo kViewportState = {
    .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
    .viewportCount = 1,
    .scissorCount  = 1,
};

static VkPipelineDepthStencilStateCreateInfo makeDepth(bool test, bool write) {
    return (VkPipelineDepthStencilStateCreateInfo){
        .sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable  = test  ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = write ? VK_TRUE : VK_FALSE,
        .depthCompareOp   = test  ? VK_COMPARE_OP_LESS : VK_COMPARE_OP_ALWAYS,
    };
}

/// Instance / Device / Surface

bool checkExtensionSupport(const char** requiredExtensions, uint32_t requiredCount)
{
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &count, NULL);
    VkExtensionProperties* avail = malloc(count * sizeof(*avail));
    vkEnumerateInstanceExtensionProperties(NULL, &count, avail);

    for (uint32_t i = 0; i < requiredCount; i++) {
        bool found = false;
        for (uint32_t j = 0; j < count; j++) {
            if (strcmp(requiredExtensions[i], avail[j].extensionName) == 0) {
                found = true; break;
            }
        }
        if (!found) { free(avail); return false; }
    }
    free(avail);
    return true;
}

bool checkValidationLayerSupport(void)
{
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, NULL);
    VkLayerProperties* avail = malloc(count * sizeof(*avail));
    vkEnumerateInstanceLayerProperties(&count, avail);

    for (uint32_t i = 0; i < VALIDATION_LAYERS_COUNT; i++) {
        bool found = false;
        for (uint32_t j = 0; j < count; j++) {
            if (strcmp(validationLayers[i], avail[j].layerName) == 0) {
                found = true; break;
            }
        }
        if (!found) { free(avail); return false; }
    }
    free(avail);
    return true;
}

void createInstance(VulkanContext* ctx)
{
#if ENABLE_VALIDATION_LAYERS
    if (!checkValidationLayerSupport()) {
        fprintf(stderr, "Validation layers requested but not available!\n");
        exit(EXIT_FAILURE);
    }
#endif
    VkApplicationInfo appInfo = {
        .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName   = "Revox",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName        = "No Engine",
        .engineVersion      = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion         = VK_API_VERSION_1_1
    };
    uint32_t     glfwExtCount = 0;
    const char** glfwExt      = glfwGetRequiredInstanceExtensions(&glfwExtCount);

    VkInstanceCreateInfo ci = {
        .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo        = &appInfo,
        .enabledExtensionCount   = glfwExtCount,
        .ppEnabledExtensionNames = glfwExt,
        .enabledLayerCount       = VALIDATION_LAYERS_COUNT,
        .ppEnabledLayerNames     = validationLayers
    };
    if (vkCreateInstance(&ci, NULL, &ctx->instance) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create Vulkan instance\n");
        exit(EXIT_FAILURE);
    }
}

static uint32_t findGraphicsQueueFamily(VkPhysicalDevice dev)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, NULL);
    VkQueueFamilyProperties* props = malloc(count * sizeof(*props));
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, props);
    for (uint32_t i = 0; i < count; i++) {
        if (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            free(props);
            return i;
        }
    }
    free(props);
    return UINT32_MAX;
}

static int scoreDevice(VkPhysicalDevice dev)
{
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceProperties(dev, &props);
    vkGetPhysicalDeviceMemoryProperties(dev, &mem);

    if (findGraphicsQueueFamily(dev) == UINT32_MAX) return -1;

    int score = 0;
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)   score += 100000;
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score += 10000;

    for (uint32_t i = 0; i < mem.memoryHeapCount; i++)
        if (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            score += (int)(mem.memoryHeaps[i].size / (1024 * 1024));

    return score;
}

void pickPhysicalDevice(VulkanContext* ctx)
{
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(ctx->instance, &count, NULL);
    if (count == 0) { fprintf(stderr, "No Vulkan-capable GPU found\n"); exit(EXIT_FAILURE); }

    VkPhysicalDevice* devs = malloc(count * sizeof(*devs));
    vkEnumeratePhysicalDevices(ctx->instance, &count, devs);

    VkPhysicalDevice best = VK_NULL_HANDLE;
    int bestScore = -1;
    for (uint32_t i = 0; i < count; i++) {
        int s = scoreDevice(devs[i]);
        if (s > bestScore) { bestScore = s; best = devs[i]; }
    }
    free(devs);

    if (best == VK_NULL_HANDLE) { fprintf(stderr, "No suitable GPU found\n"); exit(EXIT_FAILURE); }
    ctx->physicalDevice = best;
    ctx->graphicsQueueFamily = findGraphicsQueueFamily(best);

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(best, &props);
    fprintf(stdout, "Selected GPU: %s\n", props.deviceName);
}

void createLogicalDevice(VulkanContext* ctx)
{
    float pri = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = ctx->graphicsQueueFamily,
        .queueCount       = 1,
        .pQueuePriorities = &pri
    };

    /* Only request extensions the engine actually uses. Ray-tracing extensions
       are listed here for future use; remove any your driver doesn't expose. */
    static const char* exts[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
    };
    static const uint32_t extCount = sizeof(exts) / sizeof(exts[0]);

    VkPhysicalDeviceDescriptorIndexingFeaturesEXT indexingFeatures = {
        .sType                                        = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT,
        .pNext                                        = NULL,
        .descriptorBindingPartiallyBound              = VK_TRUE,
        .runtimeDescriptorArray                       = VK_TRUE,
        .descriptorBindingSampledImageUpdateAfterBind = VK_TRUE,
        .shaderSampledImageArrayNonUniformIndexing    = VK_TRUE,
    };

    VkPhysicalDeviceFeatures features = { .wideLines = VK_TRUE, .multiDrawIndirect = VK_TRUE };

    VkPhysicalDeviceVulkan11Features features11 = {
        .sType                = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        .pNext                = indexingFeatures.pNext,
        .shaderDrawParameters = VK_TRUE,
    };
    indexingFeatures.pNext = &features11;

    VkDeviceCreateInfo ci = {
        .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext                   = &indexingFeatures,
        .queueCreateInfoCount    = 1,
        .pQueueCreateInfos       = &qci,
        .enabledExtensionCount   = extCount,
        .ppEnabledExtensionNames = exts,
        .pEnabledFeatures        = &features,
        .enabledLayerCount       = VALIDATION_LAYERS_COUNT,
        .ppEnabledLayerNames     = validationLayers
    };
    if (vkCreateDevice(ctx->physicalDevice, &ci, NULL, &ctx->device) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create logical device\n");
        exit(EXIT_FAILURE);
    }
    vkGetDeviceQueue(ctx->device, ctx->graphicsQueueFamily, 0, &ctx->graphicsQueue);
}

/// SWAPCHAIN

void createSwapChain(VulkanContext* ctx)
{
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx->physicalDevice, ctx->surface, &caps);

    VkExtent2D ext;
    if (caps.currentExtent.width != UINT32_MAX) {
        ext = caps.currentExtent;
    } else {
        int w, h;
        glfwGetFramebufferSize(ctx->window, &w, &h);
        ext.width  = (uint32_t)w;
        ext.height = (uint32_t)h;
        ext.width  = ext.width  < caps.minImageExtent.width  ? caps.minImageExtent.width  :
                     ext.width  > caps.maxImageExtent.width  ? caps.maxImageExtent.width  : ext.width;
        ext.height = ext.height < caps.minImageExtent.height ? caps.minImageExtent.height :
                     ext.height > caps.maxImageExtent.height ? caps.maxImageExtent.height : ext.height;
    }

    uint32_t imgCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imgCount > caps.maxImageCount)
        imgCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci = {
        .sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface          = ctx->surface,
        .minImageCount    = imgCount,
        .imageFormat      = VK_FORMAT_B8G8R8A8_SRGB,
        .imageColorSpace  = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent      = ext,
        .imageArrayLayers = 1,
        .imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform     = caps.currentTransform,
        .compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode      = VK_PRESENT_MODE_IMMEDIATE_KHR,
        .clipped          = VK_TRUE,
        .oldSwapchain     = VK_NULL_HANDLE
    };
    if (vkCreateSwapchainKHR(ctx->device, &ci, NULL, &ctx->swapChain) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create swap chain\n");
        exit(EXIT_FAILURE);
    }

    vkGetSwapchainImagesKHR(ctx->device, ctx->swapChain, &ctx->swapChainImageCount, NULL);
    ctx->swapChainImages = malloc(ctx->swapChainImageCount * sizeof(VkImage));
    vkGetSwapchainImagesKHR(ctx->device, ctx->swapChain, &ctx->swapChainImageCount, ctx->swapChainImages);
    ctx->swapChainImageFormat = VK_FORMAT_B8G8R8A8_SRGB;
    ctx->swapChainExtent      = ext;
}

void createImageViews(VulkanContext* ctx)
{
    ctx->swapChainImageViews = malloc(ctx->swapChainImageCount * sizeof(VkImageView));
    for (uint32_t i = 0; i < ctx->swapChainImageCount; i++) {
        VkImageViewCreateInfo ci = {
            .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image    = ctx->swapChainImages[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format   = ctx->swapChainImageFormat,
            .components = {
                VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY
            },
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1
            }
        };
        if (vkCreateImageView(ctx->device, &ci, NULL, &ctx->swapChainImageViews[i]) != VK_SUCCESS) {
            fprintf(stderr, "Failed to create image view %u\n", i);
            exit(EXIT_FAILURE);
        }
    }
}

/// Render pass

void createRenderPass(VulkanContext* ctx)
{
    VkAttachmentDescription attachments[2] = {
        /* color */
        {
            .format         = ctx->swapChainImageFormat,
            .samples        = VK_SAMPLE_COUNT_1_BIT,
            .loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
        },
        /* depth */
        {
            .format         = ctx->depthFormat,
            .samples        = VK_SAMPLE_COUNT_1_BIT,
            .loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
        }
    };
    VkAttachmentReference colorRef = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass = {
        .pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount    = 1,
        .pColorAttachments       = &colorRef,
        .pDepthStencilAttachment = &depthRef
    };

    /* FIX: include depth stage in the subpass dependency */
    VkSubpassDependency dep = {
        .srcSubpass    = VK_SUBPASS_EXTERNAL,
        .dstSubpass    = 0,
        .srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
    };

    VkRenderPassCreateInfo ci = {
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 2,
        .pAttachments    = attachments,
        .subpassCount    = 1,
        .pSubpasses      = &subpass,
        .dependencyCount = 1,
        .pDependencies   = &dep
    };
    if (vkCreateRenderPass(ctx->device, &ci, NULL, &ctx->renderPass) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create render pass\n");
        exit(EXIT_FAILURE);
    }
}

/// Descriptor layouts / Pools

void createDescriptorSetLayout(VulkanContext* ctx)
{
    /* set=0 : UBO (used by all 3D pipelines — vertex AND fragment read cameraPos/time) */
    VkDescriptorSetLayoutBinding b = {
        .binding         = 0,
        .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = 1,
        .stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
    };
    VkDescriptorSetLayoutCreateInfo ci = {
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings    = &b
    };
    vkCreateDescriptorSetLayout(ctx->device, &ci, NULL, &ctx->descriptorSetLayout);
}

void create2DDescriptorSetLayout(VulkanContext* ctx)
{
    /* set=0 (2D) or set=1 (3D textured/SDF) : combined-image-sampler */
    VkDescriptorSetLayoutBinding b = {
        .binding         = 0,
        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT
    };
    VkDescriptorSetLayoutCreateInfo ci = {
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings    = &b
    };
    if (vkCreateDescriptorSetLayout(ctx->device, &ci, NULL, &ctx->descriptorSetLayout2D) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create 2D descriptor set layout\n");
        exit(EXIT_FAILURE);
    }
}

void createDescriptorPool(VulkanContext* ctx)
{
    VkDescriptorPoolSize ps = {
        .type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = MAX_FRAMES_IN_FLIGHT
    };
    VkDescriptorPoolCreateInfo ci = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .poolSizeCount = 1,
        .pPoolSizes    = &ps,
        .maxSets       = MAX_FRAMES_IN_FLIGHT
    };
    if (vkCreateDescriptorPool(ctx->device, &ci, NULL, &ctx->descriptorPool) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create descriptor pool\n");
        exit(EXIT_FAILURE);
    }
}

void create2DDescriptorPool(VulkanContext* ctx)
{
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_TEXTURES };
    VkDescriptorPoolCreateInfo ci = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .poolSizeCount = 1,
        .pPoolSizes    = &ps,
        .maxSets       = MAX_TEXTURES
    };
    if (vkCreateDescriptorPool(ctx->device, &ci, NULL, &ctx->descriptorPool2D) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create 2D descriptor pool\n");
        exit(EXIT_FAILURE);
    }
}

void createDescriptorSet(VulkanContext* ctx)
{
    VkDescriptorSetLayout layouts[MAX_FRAMES_IN_FLIGHT];
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        layouts[i] = ctx->descriptorSetLayout;

    VkDescriptorSetAllocateInfo ai = {
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = ctx->descriptorPool,
        .descriptorSetCount = MAX_FRAMES_IN_FLIGHT,
        .pSetLayouts        = layouts
    };
    if (vkAllocateDescriptorSets(ctx->device, &ai, ctx->descriptorSets) != VK_SUCCESS) {
        fprintf(stderr, "Failed to allocate descriptor sets\n");
        exit(EXIT_FAILURE);
    }

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkDescriptorBufferInfo bi = {
            .buffer = ctx->uniformBuffers[i],
            .offset = 0,
            .range  = sizeof(UniformBufferObject)
        };
        VkWriteDescriptorSet w = {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = ctx->descriptorSets[i],
            .dstBinding      = 0,
            .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .pBufferInfo     = &bi
        };
        vkUpdateDescriptorSets(ctx->device, 1, &w, 0, NULL);
    }
}

/// Pipeline layouts
//
//  (created once, shared across merged pipelines)
//
//   2D family  (color & textured & SDF):
//      layout2D    ->  no descriptor sets, push mat4 in VS
//      layoutTex2D ->  set=0:sampler, push mat4 in VS
//
//   3D family  (solid & textured & SDF & line):
//      layout3D    ->  set=0:UBO, push PushConstants in VS+FS
//      layoutTex3D ->  set=0:UBO  set=1:sampler, push PushConstants in VS+FS
//
void createAllPipelineLayouts(VulkanContext* ctx)
{
    /* 2D push: mat4 projection, vertex stage only */
    VkPushConstantRange mat4Range = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset     = 0,
        .size       = sizeof(mat4)
    };

    /* 3D push: PushConstants (16 bytes), both stages */
    VkPushConstantRange pcRange = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset     = 0,
        .size       = sizeof(PushConstants)
    };

    /* ── 2D unified layout: set=0 bindless array, push mat4 in VS ───── */
    vkCreatePipelineLayout(ctx->device,
        &(VkPipelineLayoutCreateInfo){
            .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount         = 1,
            .pSetLayouts            = &ctx->bindlessSetLayout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges    = &mat4Range
        }, NULL, &ctx->pipelineLayoutTextured2D);
    /* aliases — both 2D paths use the same unified layout */
    ctx->pipelineLayout2D    = ctx->pipelineLayoutTextured2D;

    /* ── 3D unified PBR layout ───────────────────────────────────────────
       set=0  UBO
       set=1  bindless texture array
       set=2  SSBO  (gltf meshes OR immediate-mode slots)
       set=3  lighting UBO
       push   PushConstants { aoEnabled, iblEnabled, meshIndex, _pad }   */
    VkDescriptorSetLayout pbr3DSets[4] = {
        ctx->descriptorSetLayout,   /* set=0 UBO     */
        ctx->bindlessSetLayout,     /* set=1 bindless*/
        ctx->ssboSetLayout,         /* set=2 SSBO    */
        ctx->lightingSetLayout,     /* set=3 lighting*/
    };
    vkCreatePipelineLayout(ctx->device,
        &(VkPipelineLayoutCreateInfo){
            .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount         = 4,
            .pSetLayouts            = pbr3DSets,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges    = &pcRange
        }, NULL, &ctx->pipelineLayout);

    /* All 3D variants share the same layout */
    ctx->pipelineLayoutLine       = ctx->pipelineLayout;
    ctx->pipelineLayoutTextured3D = ctx->pipelineLayout;
    ctx->pipelineLayoutIndirect   = ctx->pipelineLayout;
}

/// GRAPHICS PIPELINES
// all created in ONE batch call
void createGraphicsPipelines(VulkanContext* ctx)
{
    VkDescriptorSetLayout skyboxLayouts[2] = { ctx->descriptorSetLayout, ctx->lightingSetLayout };
    VkPipelineLayoutCreateInfo skyboxPlci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 2, .pSetLayouts = skyboxLayouts };
    vkCreatePipelineLayout(ctx->device, &skyboxPlci, NULL, &skyboxPipelineLayout);

    VkPipelineCacheCreateInfo cacheCI = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    vkCreatePipelineCache(ctx->device, &cacheCI, NULL, &pipelineCache);

    VkPipelineRasterizationStateCreateInfo rast1  = makeRasterizer(1.0f);
    VkPipelineRasterizationStateCreateInfo rastLW = makeRasterizer(2.0f);
    VkPipelineColorBlendStateCreateInfo    blend  = makeColorBlend(&kBlendAlpha);
    VkPipelineDepthStencilStateCreateInfo  depth3D = makeDepth(true,  true);
    VkPipelineDepthStencilStateCreateInfo  depth2D = makeDepth(false, false);

    /* 3D vertex input */
    VkVertexInputBindingDescription        bind3D;
    VkVertexInputAttributeDescription      attr3D[5];
    VkPipelineVertexInputStateCreateInfo   vi3D;
    VkPipelineInputAssemblyStateCreateInfo ia3D, iaLine;
    fill3DVertexInput(&vi3D, &ia3D, &bind3D, attr3D, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    iaLine = (VkPipelineInputAssemblyStateCreateInfo){
        .sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST
    };

    /* 2D vertex input */
    VkVertexInputBindingDescription        bind2D;
    VkVertexInputAttributeDescription      attr2D[4];
    VkPipelineVertexInputStateCreateInfo   vi2D;
    VkPipelineInputAssemblyStateCreateInfo ia2D;
    fill2DVertexInput(&vi2D, &ia2D, &bind2D, attr2D);

    /* ── shader modules ── */
    VkShaderModule vsPBR  = createShaderModule(ctx->device, pbr_vert_spv,  sizeof(pbr_vert_spv));
    VkShaderModule fsPBR  = createShaderModule(ctx->device, pbr_frag_spv,  sizeof(pbr_frag_spv));
    VkShaderModule vs2D   = createShaderModule(ctx->device, __2D_vert_spv, sizeof(__2D_vert_spv));
    VkShaderModule fs2D   = createShaderModule(ctx->device, __2D_frag_spv, sizeof(__2D_frag_spv));

#define STAGE(stg, mod) \
    {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, stg, mod, "main", NULL}

    VkPipelineShaderStageCreateInfo ssPBR[2]     = { STAGE(VK_SHADER_STAGE_VERTEX_BIT,   vsPBR),
                                                      STAGE(VK_SHADER_STAGE_FRAGMENT_BIT, fsPBR) };
    VkPipelineShaderStageCreateInfo ss2DColor[2] = { STAGE(VK_SHADER_STAGE_VERTEX_BIT,   vs2D),
                                                      STAGE(VK_SHADER_STAGE_FRAGMENT_BIT, fs2D)  };

    VkShaderModule vsSkybox = createShaderModule(ctx->device, skybox_vert_spv, sizeof(skybox_vert_spv));
    VkShaderModule fsSkybox = createShaderModule(ctx->device, skybox_frag_spv, sizeof(skybox_frag_spv));
    VkPipelineShaderStageCreateInfo ssSkybox[2]  = { STAGE(VK_SHADER_STAGE_VERTEX_BIT, vsSkybox),
                                                      STAGE(VK_SHADER_STAGE_FRAGMENT_BIT, fsSkybox) };

    VkPipelineDepthStencilStateCreateInfo depthSkybox = makeDepth(true, false);
    depthSkybox.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    /*
       Index  Pipeline
       0      3D PBR triangles  (direct + indirect, same pipeline)
       1      3D PBR lines
       2      2D color
       3      2D textured
    */
    VkGraphicsPipelineCreateInfo pci[4] = {
        /* 0: 3D PBR triangles */
        {
            .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount          = 2, .pStages             = ssPBR,
            .pVertexInputState   = &vi3D, .pInputAssemblyState = &ia3D,
            .pViewportState      = &kViewportState,
            .pRasterizationState = &rast1, .pMultisampleState  = &kMultisampling,
            .pColorBlendState    = &blend, .pDepthStencilState = &depth3D,
            .pDynamicState       = &kDynamicState,
            .layout              = ctx->pipelineLayout,
            .renderPass          = ctx->renderPass
        },
        /* 1: 3D PBR lines */
        {
            .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount          = 2, .pStages             = ssPBR,
            .pVertexInputState   = &vi3D, .pInputAssemblyState = &iaLine,
            .pViewportState      = &kViewportState,
            .pRasterizationState = &rastLW, .pMultisampleState = &kMultisampling,
            .pColorBlendState    = &blend,  .pDepthStencilState= &depth3D,
            .pDynamicState       = &kDynamicState,
            .layout              = ctx->pipelineLayout,
            .renderPass          = ctx->renderPass
        },
        /* 2: 2D unified (color + texture, driven by textureIndex) */
        {
            .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount          = 2, .pStages             = ss2DColor,
            .pVertexInputState   = &vi2D, .pInputAssemblyState = &ia2D,
            .pViewportState      = &kViewportState,
            .pRasterizationState = &rast1, .pMultisampleState  = &kMultisampling,
            .pColorBlendState    = &blend, .pDepthStencilState = &depth2D,
            .pDynamicState       = &kDynamicState,
            .layout              = ctx->pipelineLayoutTextured2D,
            .renderPass          = ctx->renderPass
        },
        /* 3: Skybox */
        {
            .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount          = 2, .pStages             = ssSkybox,
            .pVertexInputState   = &(VkPipelineVertexInputStateCreateInfo){ .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO },
            .pInputAssemblyState = &ia3D,
            .pViewportState      = &kViewportState,
            .pRasterizationState = &rast1, .pMultisampleState  = &kMultisampling,
            .pColorBlendState    = &blend, .pDepthStencilState = &depthSkybox,
            .pDynamicState       = &kDynamicState,
            .layout              = skyboxPipelineLayout,
            .renderPass          = ctx->renderPass
        }
    };

    VkPipeline pipelines[4];
    if (vkCreateGraphicsPipelines(ctx->device, pipelineCache, 4, pci, NULL, pipelines) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create graphics pipelines\n");
        exit(EXIT_FAILURE);
    }

#undef STAGE

    /* One PBR pipeline serves all 3D draw paths */
    ctx->graphicsPipeline           = pipelines[0];
    ctx->graphicsPipelineTextured3D = pipelines[0];
    ctx->graphicsPipelineLine       = pipelines[1];
    pipelineIndirectSolid           = pipelines[0];
    pipelineIndirectTextured        = pipelines[0];
    /* Single unified 2D pipeline — handles colored and textured quads */
    ctx->graphicsPipeline2D         = pipelines[2];
    ctx->graphicsPipelineTextured2D = pipelines[2];
    skyboxPipeline                  = pipelines[3];

    vkDestroyShaderModule(ctx->device, vsPBR, NULL);
    vkDestroyShaderModule(ctx->device, fsPBR, NULL);
    vkDestroyShaderModule(ctx->device, vs2D,  NULL);
    vkDestroyShaderModule(ctx->device, fs2D,  NULL);
    vkDestroyShaderModule(ctx->device, vsSkybox, NULL);
    vkDestroyShaderModule(ctx->device, fsSkybox, NULL);
}

/* Indirect pipeline layout — created AFTER createMeshSSBO so ssboSetLayout is valid */
void createIndirectPipelineLayout(VulkanContext* ctx)
{
    /* Unified layout already created in createAllPipelineLayouts — nothing to do */
    (void)ctx;
}

void createComputeCullPipeline(VulkanContext* ctx)
{
    /* ── descriptor set layout ─────────────────────────────────────── */
    VkDescriptorSetLayoutBinding bindings[4] = {
        /* binding 0: mesh SSBO (readonly) */
        {
            .binding         = 0,
            .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT
        },
        /* binding 1: source draw commands (readonly, CPU-written) */
        {
            .binding         = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT
        },
        /* binding 2: destination draw commands (writeonly, GPU-read) */
        {
            .binding         = 2,
            .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT
        },
        /* binding 3: frustum UBO */
        {
            .binding         = 3,
            .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT
        }
    };

    VkDescriptorSetLayoutCreateInfo lci = {
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 4,
        .pBindings    = bindings
    };
    vkCreateDescriptorSetLayout(ctx->device, &lci, NULL, &ctx->computeCullSetLayout);

    /* ── pipeline layout ────────────────────────────────────────────── */
    VkPipelineLayoutCreateInfo plci = {
        .sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts    = &ctx->computeCullSetLayout
    };
    vkCreatePipelineLayout(ctx->device, &plci, NULL, &ctx->computeCullPipelineLayout);

    /* ── shader module ──────────────────────────────────────────────── */
    VkShaderModule cullShader = createShaderModule(ctx->device,
                                    cull_comp_spv, sizeof(cull_comp_spv));

    VkComputePipelineCreateInfo cpci = {
        .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage  = {
            .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage  = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = cullShader,
            .pName  = "main"
        },
        .layout = ctx->computeCullPipelineLayout
    };
    vkCreateComputePipelines(ctx->device, pipelineCache, 1, &cpci, NULL,
                             &ctx->computeCullPipeline);
    vkDestroyShaderModule(ctx->device, cullShader, NULL);

    /* ── frustum UBOs (one per frame) ───────────────────────────────── */
    typedef struct { vec4 planes[6]; uint32_t meshCount; uint32_t _pad[3]; } FrustumUBO;
    VkDeviceSize uboSize = sizeof(FrustumUBO);

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        createBuffer(ctx, uboSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &ctx->frustumUBOBuffer[i], &ctx->frustumUBOMemory[i]);
        vkMapMemory(ctx->device, ctx->frustumUBOMemory[i], 0, uboSize, 0, &ctx->frustumUBOMapped[i]);
    }

    /* ── descriptor pool ────────────────────────────────────────────── */
    VkDescriptorPoolSize poolSizes[2] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, MAX_FRAMES_IN_FLIGHT * 3 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, MAX_FRAMES_IN_FLIGHT     }
    };
    VkDescriptorPoolCreateInfo pci = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets       = MAX_FRAMES_IN_FLIGHT,
        .poolSizeCount = 2,
        .pPoolSizes    = poolSizes
    };
    vkCreateDescriptorPool(ctx->device, &pci, NULL, &ctx->computeCullPool);

    /* ── descriptor sets ────────────────────────────────────────────── */
    VkDescriptorSetLayout layouts[MAX_FRAMES_IN_FLIGHT];
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        layouts[i] = ctx->computeCullSetLayout;

    VkDescriptorSetAllocateInfo dai = {
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = ctx->computeCullPool,
        .descriptorSetCount = MAX_FRAMES_IN_FLIGHT,
        .pSetLayouts        = layouts
    };
    vkAllocateDescriptorSets(ctx->device, &dai, ctx->computeCullSets);

    /* write descriptors — SSBO sizes known from createMeshSSBO/createIndirectBuffer */
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkDescriptorBufferInfo ssboInfo = {
            .buffer = ctx->meshSSBO[i], .offset = 0, .range = VK_WHOLE_SIZE
        };
        VkDescriptorBufferInfo srcInfo = {
            .buffer = ctx->srcIndirectBuffer, .offset = 0, .range = VK_WHOLE_SIZE
        };
        VkDescriptorBufferInfo dstInfo = {
            .buffer = ctx->indirectBuffer, .offset = 0, .range = VK_WHOLE_SIZE
        };
        VkDescriptorBufferInfo uboInfo = {
            .buffer = ctx->frustumUBOBuffer[i], .offset = 0, .range = VK_WHOLE_SIZE
        };
        VkWriteDescriptorSet writes[4] = {
            {
                .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet          = ctx->computeCullSets[i],
                .dstBinding      = 0,
                .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .pBufferInfo     = &ssboInfo
            },
            {
                .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet          = ctx->computeCullSets[i],
                .dstBinding      = 1,
                .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .pBufferInfo     = &srcInfo
            },
            {
                .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet          = ctx->computeCullSets[i],
                .dstBinding      = 2,
                .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .pBufferInfo     = &dstInfo
            },
            {
                .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet          = ctx->computeCullSets[i],
                .dstBinding      = 3,
                .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .descriptorCount = 1,
                .pBufferInfo     = &uboInfo
            }
        };
        vkUpdateDescriptorSets(ctx->device, 4, writes, 0, NULL);
    }

    fprintf(stdout, "Compute frustum cull pipeline created\n");
}

void dispatchFrustumCull(VulkanContext* ctx, VkCommandBuffer cmd)
{
    if (ctx->indirectDrawCount == 0) return;

    uint32_t f = ctx->currentFrame;

    /* ── upload frustum planes ──────────────────────────────────────── */
    typedef struct { vec4 planes[6]; uint32_t meshCount; uint32_t _pad[3]; } FrustumUBO;
    FrustumUBO ubo;
    ubo.meshCount = ctx->indirectDrawCount;

    mat4 vp;
    glm_mat4_mul(camera.projection_matrix, camera.view_matrix, vp);

    /* Gribb-Hartmann plane extraction */
    for (int j = 0; j < 4; j++) {
        ubo.planes[0][j] = vp[j][3] + vp[j][0]; ubo.planes[1][j] = vp[j][3] - vp[j][0];
        ubo.planes[2][j] = vp[j][3] + vp[j][1]; ubo.planes[3][j] = vp[j][3] - vp[j][1];
        ubo.planes[4][j] = vp[j][3] + vp[j][2]; ubo.planes[5][j] = vp[j][3] - vp[j][2];
    }

    for (int i = 0; i < 6; i++) {
        float len = sqrtf(ubo.planes[i][0]*ubo.planes[i][0] +
                          ubo.planes[i][1]*ubo.planes[i][1] +
                          ubo.planes[i][2]*ubo.planes[i][2]);
        if (len > 0.0f) {
            ubo.planes[i][0] /= len; ubo.planes[i][1] /= len;
            ubo.planes[i][2] /= len; ubo.planes[i][3] /= len;
        }
    }
    memcpy(ctx->frustumUBOMapped[f], &ubo, sizeof(FrustumUBO));

    /* ── barrier: host write (frustum UBO) → compute read ──────────── */
    VkBufferMemoryBarrier hostBarrier = {
        .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask       = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer              = ctx->frustumUBOBuffer[f],
        .offset              = 0,
        .size                = VK_WHOLE_SIZE
    };
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, NULL, 1, &hostBarrier, 0, NULL);

    /* ── dispatch compute — reads srcIndirectBuffer, writes indirectBuffer ── */
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->computeCullPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            ctx->computeCullPipelineLayout, 0, 1,
                            &ctx->computeCullSets[f], 0, NULL);

    uint32_t groupCount = (ctx->indirectDrawCount + 63) / 64;
    vkCmdDispatch(cmd, groupCount, 1, 1);

    /* ── barrier: compute write → indirect draw read ────────────────── */
    VkBufferMemoryBarrier drawBarrier = {
        .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask       = VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer              = ctx->indirectBuffer,
        .offset              = 0,
        .size                = VK_WHOLE_SIZE
    };
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
        0, 0, NULL, 1, &drawBarrier, 0, NULL);
}

void create2DGraphicsPipeline(VulkanContext* ctx)        { /* handled by createGraphicsPipelines */ }
void createTextured2DGraphicsPipeline(VulkanContext* ctx){ /* handled by createGraphicsPipelines */ }
void create3DTexturedGraphicsPipeline(VulkanContext* ctx){ /* handled by createGraphicsPipelines */ }
void createLineGraphicsPipeline(VulkanContext* ctx)      { /* handled by createGraphicsPipelines */ }

/// Framebuffers / Command pool / Command buffers

void createFramebuffers(VulkanContext* ctx)
{
    ctx->swapChainFramebuffers = malloc(ctx->swapChainImageCount * sizeof(VkFramebuffer));
    for (uint32_t i = 0; i < ctx->swapChainImageCount; i++) {
        VkImageView atts[2] = { ctx->swapChainImageViews[i], ctx->depthImageView };
        VkFramebufferCreateInfo ci = {
            .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass      = ctx->renderPass,
            .attachmentCount = 2,
            .pAttachments    = atts,
            .width           = ctx->swapChainExtent.width,
            .height          = ctx->swapChainExtent.height,
            .layers          = 1
        };
        if (vkCreateFramebuffer(ctx->device, &ci, NULL, &ctx->swapChainFramebuffers[i]) != VK_SUCCESS) {
            fprintf(stderr, "Failed to create framebuffer %u\n", i);
            exit(EXIT_FAILURE);
        }
    }
}

void createCommandPool(VulkanContext* ctx)
{
    VkCommandPoolCreateInfo ci = {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
                            VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = ctx->graphicsQueueFamily
    };
    if (vkCreateCommandPool(ctx->device, &ci, NULL, &ctx->commandPool) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create command pool\n");
        exit(EXIT_FAILURE);
    }
}

void createCommandBuffers(VulkanContext* ctx)
{
    if (ctx->commandBuffers) {
        vkFreeCommandBuffers(ctx->device, ctx->commandPool,
                             ctx->swapChainImageCount, ctx->commandBuffers);
        free(ctx->commandBuffers);
        ctx->commandBuffers = NULL;
    }
    ctx->commandBuffers = malloc(ctx->swapChainImageCount * sizeof(VkCommandBuffer));
    VkCommandBufferAllocateInfo ai = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = ctx->commandPool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = ctx->swapChainImageCount
    };
    if (vkAllocateCommandBuffers(ctx->device, &ai, ctx->commandBuffers) != VK_SUCCESS) {
        fprintf(stderr, "Failed to allocate command buffers\n");
        exit(EXIT_FAILURE);
    }
}

/// SYNC OBJECTS

void createSyncObjects(VulkanContext* ctx)
{
    ctx->imageAvailableSemaphores = malloc(MAX_FRAMES_IN_FLIGHT * sizeof(VkSemaphore));
    ctx->inFlightFences           = malloc(MAX_FRAMES_IN_FLIGHT * sizeof(VkFence));
    ctx->renderFinishedSemaphores = malloc(ctx->swapChainImageCount * sizeof(VkSemaphore));
    ctx->imagesInFlight           = malloc(ctx->swapChainImageCount * sizeof(VkFence));

    VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo     fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                 .flags = VK_FENCE_CREATE_SIGNALED_BIT };

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (vkCreateSemaphore(ctx->device, &si, NULL, &ctx->imageAvailableSemaphores[i]) != VK_SUCCESS ||
            vkCreateFence    (ctx->device, &fi, NULL, &ctx->inFlightFences[i])           != VK_SUCCESS) {
            fprintf(stderr, "Failed to create sync objects for frame %u\n", i);
            exit(EXIT_FAILURE);
        }
    }
    for (uint32_t i = 0; i < ctx->swapChainImageCount; i++) {
        if (vkCreateSemaphore(ctx->device, &si, NULL, &ctx->renderFinishedSemaphores[i]) != VK_SUCCESS) {
            fprintf(stderr, "Failed to create renderFinishedSemaphore %u\n", i);
            exit(EXIT_FAILURE);
        }
        ctx->imagesInFlight[i] = VK_NULL_HANDLE;
    }
}

/// Depth resources

void createDepthResources(VulkanContext* ctx)
{
    ctx->depthFormat = VK_FORMAT_D32_SFLOAT;

    VkImageCreateInfo imgCI = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = ctx->depthFormat,
        .extent        = { ctx->swapChainExtent.width, ctx->swapChainExtent.height, 1 },
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };
    if (vkCreateImage(ctx->device, &imgCI, NULL, &ctx->depthImage) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create depth image\n"); exit(EXIT_FAILURE);
    }

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(ctx->device, ctx->depthImage, &req);

    VkMemoryAllocateInfo ai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = req.size,
        .memoryTypeIndex = findMemoryType(ctx->physicalDevice, req.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    };
    if (vkAllocateMemory(ctx->device, &ai, NULL, &ctx->depthImageMemory) != VK_SUCCESS) {
        fprintf(stderr, "Failed to allocate depth image memory\n"); exit(EXIT_FAILURE);
    }
    vkBindImageMemory(ctx->device, ctx->depthImage, ctx->depthImageMemory, 0);

    VkImageViewCreateInfo viewCI = {
        .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image            = ctx->depthImage,
        .viewType         = VK_IMAGE_VIEW_TYPE_2D,
        .format           = ctx->depthFormat,
        .subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 }
    };
    if (vkCreateImageView(ctx->device, &viewCI, NULL, &ctx->depthImageView) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create depth image view\n"); exit(EXIT_FAILURE);
    }
}

/// Uniform buffer

static void createBuffer(VulkanContext* ctx, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer* buffer, VkDeviceMemory* bufferMemory) {
    VkBufferCreateInfo bufferInfo = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    vkCreateBuffer(ctx->device, &bufferInfo, NULL, buffer);
    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(ctx->device, *buffer, &memReq);
    VkMemoryAllocateInfo allocInfo = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = memReq.size, .memoryTypeIndex = findMemoryType(ctx->physicalDevice, memReq.memoryTypeBits, properties) };
    vkAllocateMemory(ctx->device, &allocInfo, NULL, bufferMemory);
    vkBindBufferMemory(ctx->device, *buffer, *bufferMemory, 0);
}

void createUniformBuffer(VulkanContext* ctx)
{
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        createBuffer(ctx, sizeof(UniformBufferObject), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &ctx->uniformBuffers[i], &ctx->uniformBuffersMemory[i]);
        vkMapMemory(ctx->device, ctx->uniformBuffersMemory[i], 0, sizeof(UniformBufferObject), 0, &ctx->uboMapped[i]);
    }
}

void updateUniformBuffer(VulkanContext* ctx)
{
    UniformBufferObject ubo;
    glm_mat4_copy(camera.view_matrix,       ubo.view);
    glm_mat4_copy(camera.projection_matrix, ubo.proj);
    glm_mat4_mul(camera.projection_matrix, camera.view_matrix, ubo.vp);
    ubo.cameraPos[0] = camera.position[0];
    ubo.cameraPos[1] = camera.position[1];
    ubo.cameraPos[2] = camera.position[2];
    ubo.cameraPos[3] = 0.0f;
    ubo.time         = (float)glfwGetTime();
    memcpy(ctx->uboMapped[ctx->currentFrame], &ubo, sizeof(ubo));
}

/// Record command buffer

void recordCommandBuffer(VulkanContext* ctx, uint32_t imageIndex)
{
    VkCommandBuffer cmd = ctx->commandBuffers[imageIndex];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        /* ONE_TIME_SUBMIT is more efficient for buffers re-recorded every frame */
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) {
        fprintf(stderr, "Failed to begin command buffer\n"); exit(EXIT_FAILURE);
    }

    VkClearValue clearValues[2] = {
        { .color        = {{ctx->clearColor.r, ctx->clearColor.g,
                            ctx->clearColor.b, ctx->clearColor.a}} },
        { .depthStencil = {1.0f, 0} }
    };
    VkRenderPassBeginInfo rpi = {
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass      = ctx->renderPass,
        .framebuffer     = ctx->swapChainFramebuffers[imageIndex],
        .renderArea      = { {0,0}, ctx->swapChainExtent },
        .clearValueCount = 2,
        .pClearValues    = clearValues
    };

    /* ── FRUSTUM CULLING: must run BEFORE render pass ───────────────── */
    dispatchFrustumCull(ctx, cmd);

    vkCmdBeginRenderPass(cmd, &rpi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport = {
        .width    = (float)ctx->swapChainExtent.width,
        .height   = (float)ctx->swapChainExtent.height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f
    };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor = { {0,0}, ctx->swapChainExtent };
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    pushConstants.ambientOcclusionEnabled = ambientOcclusionEnabled ? 1 : 0;
    pushConstants.iblEnabled = (ctx->iblLoaded && iblLightingEnabled) ? 1 : 0;

    CTX_LIGHTING(ctx)->iblEnabled = (ctx->iblLoaded && iblLightingEnabled) ? 1 : 0;
    CTX_LIGHTING(ctx)->ambientIntensity = 1.0f; // Ensure IBL isn't multiplied by 0!

    updateLightingUBO(ctx);

    /* ── Bind the 4 descriptor sets once — valid for ALL 3D draws ───────
       set=0  UBO
       set=1  bindless texture array
       set=2  GLTF mesh SSBO  (indirect pass uses gl_BaseInstanceARB)
       set=3  lighting UBO                                                 */
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ctx->graphicsPipeline);

    VkDescriptorSet gltfSets[4] = {
        ctx->descriptorSets[ctx->currentFrame],
        ctx->bindlessSet,
        ctx->ssboSets[ctx->currentFrame],
        ctx->lightingSets[ctx->currentFrame],
    };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            ctx->pipelineLayout, 0, 4, gltfSets, 0, NULL);

    /* indirect = -1 in meshIndex means use gl_BaseInstanceARB */
    pushConstants.meshIndex = -1;
    vkCmdPushConstants(cmd, ctx->pipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(PushConstants), &pushConstants);

    /* ── INDIRECT PASS: GLTF meshes ─────────────────────────────────── */
    if (ctx->indirectDrawCount > 0) {
        VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &ctx->megaVertexBuffer, &zero);
        vkCmdBindIndexBuffer(cmd, ctx->megaIndexBuffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexedIndirect(cmd, ctx->indirectBuffer, 0,
                                 ctx->indirectDrawCount, sizeof(VkDrawIndexedIndirectCommand));
    }

    /* ── DIRECT PASS: immediate-mode (sphere, cube, etc.) ───────────────
       Swap set=2 to the imm SSBO — sets 0,1,3 stay the same             */
    VkDescriptorSet immSet = imm_ssbo_get_set(ctx->currentFrame);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            ctx->pipelineLayout, 2, 1, &immSet, 0, NULL);
    renderer_draw(cmd);

    /* ── 3D lines ────────────────────────────────────────────────────── */
    if (lineVertexCount > 0) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ctx->graphicsPipelineLine);
        line_renderer_draw(cmd);
    }

    /* ── Skybox ─────────────────────────────────────────────────────── */
    if (skyboxEnabled && iblSkyboxView != VK_NULL_HANDLE) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyboxPipeline);
        VkDescriptorSet skyboxSets[2] = { ctx->descriptorSets[ctx->currentFrame], ctx->lightingSets[ctx->currentFrame] };
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyboxPipelineLayout, 0, 2, skyboxSets, 0, NULL);
        vkCmdDraw(cmd, 36, 1, 0, 0);
    }

    /* ── 2D ─────────────────────────────────────────────────────────── */
    renderer2D_draw(cmd);

    vkCmdEndRenderPass(cmd);
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        fprintf(stderr, "Failed to record command buffer\n"); exit(EXIT_FAILURE);
    }
}

/// Draw frame

void drawFrame(VulkanContext* ctx)
{
    uint32_t frameIndex = ctx->currentFrame;
    VkFence  fence      = ctx->inFlightFences[frameIndex];

    vkWaitForFences(ctx->device, 1, &fence, VK_TRUE, UINT64_MAX);

    uint32_t imageIndex;
    VkResult result = vkAcquireNextImageKHR(ctx->device, ctx->swapChain, UINT64_MAX,
                                            ctx->imageAvailableSemaphores[frameIndex],
                                            VK_NULL_HANDLE, &imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) { recreateSwapChain(ctx); return; }
    else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        fprintf(stderr, "Failed to acquire swap chain image\n"); exit(EXIT_FAILURE);
    }

    if (ctx->imagesInFlight[imageIndex] != VK_NULL_HANDLE)
        vkWaitForFences(ctx->device, 1, &ctx->imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX);
    ctx->imagesInFlight[imageIndex] = fence;

    updateUniformBuffer(ctx);
    imm_ssbo_begin_frame(ctx, ctx->currentFrame);
    recordCommandBuffer(ctx, imageIndex);

    VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    VkSubmitInfo si = {
        .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount   = 1,
        .pWaitSemaphores      = &ctx->imageAvailableSemaphores[frameIndex],
        .pWaitDstStageMask    = waitStages,
        .commandBufferCount   = 1,
        .pCommandBuffers      = &ctx->commandBuffers[imageIndex],
        .signalSemaphoreCount = 1,
        .pSignalSemaphores    = &ctx->renderFinishedSemaphores[imageIndex]
    };
    vkResetFences(ctx->device, 1, &fence);
    if (vkQueueSubmit(ctx->graphicsQueue, 1, &si, fence) != VK_SUCCESS) {
        fprintf(stderr, "Failed to submit draw command buffer\n"); exit(EXIT_FAILURE);
    }

    VkPresentInfoKHR pi = {
        .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores    = &ctx->renderFinishedSemaphores[imageIndex],
        .swapchainCount     = 1,
        .pSwapchains        = &ctx->swapChain,
        .pImageIndices      = &imageIndex
    };
    result = vkQueuePresentKHR(ctx->graphicsQueue, &pi);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        recreateSwapChain(ctx);
    else if (result != VK_SUCCESS) {
        fprintf(stderr, "Failed to present swap chain image\n"); exit(EXIT_FAILURE);
    }

    ctx->currentFrame = (ctx->currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
}

/// Swap Chain recreation

void cleanupSwapChainResources(VulkanContext* ctx, VkSwapchainKHR oldSwapchain)
{
    vkDeviceWaitIdle(ctx->device);

    if (ctx->swapChainFramebuffers) {
        for (uint32_t i = 0; i < ctx->swapChainImageCount; i++)
            vkDestroyFramebuffer(ctx->device, ctx->swapChainFramebuffers[i], NULL);
        free(ctx->swapChainFramebuffers);
        ctx->swapChainFramebuffers = NULL;
    }
    if (ctx->commandBuffers && ctx->commandPool) {
        vkFreeCommandBuffers(ctx->device, ctx->commandPool,
                             ctx->swapChainImageCount, ctx->commandBuffers);
        free(ctx->commandBuffers);
        ctx->commandBuffers = NULL;
    }
    if (ctx->swapChainImageViews) {
        for (uint32_t i = 0; i < ctx->swapChainImageCount; i++)
            vkDestroyImageView(ctx->device, ctx->swapChainImageViews[i], NULL);
        free(ctx->swapChainImageViews);
        ctx->swapChainImageViews = NULL;
    }
    if (ctx->depthImageView   != VK_NULL_HANDLE) { vkDestroyImageView(ctx->device, ctx->depthImageView,  NULL); ctx->depthImageView   = VK_NULL_HANDLE; }
    if (ctx->depthImage       != VK_NULL_HANDLE) { vkDestroyImage    (ctx->device, ctx->depthImage,      NULL); ctx->depthImage       = VK_NULL_HANDLE; }
    if (ctx->depthImageMemory != VK_NULL_HANDLE) { vkFreeMemory      (ctx->device, ctx->depthImageMemory,NULL); ctx->depthImageMemory = VK_NULL_HANDLE; }
    if (oldSwapchain          != VK_NULL_HANDLE)   vkDestroySwapchainKHR(ctx->device, oldSwapchain, NULL);
    if (ctx->swapChainImages) { free(ctx->swapChainImages); ctx->swapChainImages = NULL; }
    ctx->swapChainImageCount = 0;
}

void cleanupSwapChain(VulkanContext* ctx)
{
    cleanupSwapChainResources(ctx, ctx->swapChain);
}

void recreateSwapChain(VulkanContext* ctx)
{
    int w = 0, h = 0;
    glfwGetFramebufferSize(ctx->window, &w, &h);
    while (w == 0 || h == 0) {
        glfwGetFramebufferSize(ctx->window, &w, &h);
        glfwWaitEvents();
    }

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        if (ctx->inFlightFences[i] != VK_NULL_HANDLE)
            vkWaitForFences(ctx->device, 1, &ctx->inFlightFences[i], VK_TRUE, UINT64_MAX);

    VkSwapchainKHR oldSwapchain = ctx->swapChain;

    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx->physicalDevice, ctx->surface, &caps);

    uint32_t imgCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imgCount > caps.maxImageCount)
        imgCount = caps.maxImageCount;

    VkExtent2D ext;
    if (caps.currentExtent.width != UINT32_MAX) {
        ext = caps.currentExtent;
    } else {
        ext.width  = (uint32_t)w;
        ext.height = (uint32_t)h;
        ext.width  = ext.width  < caps.minImageExtent.width  ? caps.minImageExtent.width  :
                     ext.width  > caps.maxImageExtent.width  ? caps.maxImageExtent.width  : ext.width;
        ext.height = ext.height < caps.minImageExtent.height ? caps.minImageExtent.height :
                     ext.height > caps.maxImageExtent.height ? caps.maxImageExtent.height : ext.height;
    }

    VkSwapchainCreateInfoKHR ci = {
        .sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface          = ctx->surface,
        .minImageCount    = imgCount,
        .imageFormat      = ctx->swapChainImageFormat,
        .imageColorSpace  = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent      = ext,
        .imageArrayLayers = 1,
        .imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform     = caps.currentTransform,
        .compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode      = VK_PRESENT_MODE_IMMEDIATE_KHR,
        .clipped          = VK_TRUE,
        .oldSwapchain     = oldSwapchain
    };
    VkSwapchainKHR newSwapchain;
    if (vkCreateSwapchainKHR(ctx->device, &ci, NULL, &newSwapchain) != VK_SUCCESS) {
        fprintf(stderr, "Failed to recreate swap chain\n"); exit(EXIT_FAILURE);
    }

    cleanupSwapChainResources(ctx, oldSwapchain);

    ctx->swapChain       = newSwapchain;
    ctx->swapChainExtent = ext;

    vkGetSwapchainImagesKHR(ctx->device, ctx->swapChain, &ctx->swapChainImageCount, NULL);
    ctx->swapChainImages = malloc(ctx->swapChainImageCount * sizeof(VkImage));
    vkGetSwapchainImagesKHR(ctx->device, ctx->swapChain, &ctx->swapChainImageCount, ctx->swapChainImages);

    createImageViews(ctx);
    createDepthResources(ctx);
    createFramebuffers(ctx);
    createCommandBuffers(ctx);

    camera.aspect_ratio = (float)ctx->swapChainExtent.width / (float)ctx->swapChainExtent.height;
    glm_perspective(glm_rad(camera.fov), camera.aspect_ratio, 0.1f, 100.0f, camera.projection_matrix);
}

/// Cleanup

void cleanup(VulkanContext* ctx)
{
    vkDeviceWaitIdle(ctx->device);

    renderer_shutdown();
    imm_ssbo_shutdown(&context);
    line_renderer_shutdown();
    meshes_destroy(ctx->device, &scene.meshes);
    texture_pool_cleanup(ctx);

    /* descriptor resources */
    if (ctx->descriptorSetLayout2D) { vkDestroyDescriptorSetLayout(ctx->device, ctx->descriptorSetLayout2D, NULL);  ctx->descriptorSetLayout2D = VK_NULL_HANDLE; }
    if (ctx->descriptorPool2D)      { vkDestroyDescriptorPool(ctx->device, ctx->descriptorPool2D,           NULL);  ctx->descriptorPool2D      = VK_NULL_HANDLE; }
    if (ctx->bindlessSetLayout)     { vkDestroyDescriptorSetLayout(ctx->device, ctx->bindlessSetLayout,     NULL);  ctx->bindlessSetLayout     = VK_NULL_HANDLE; }
    if (ctx->bindlessPool)          { vkDestroyDescriptorPool(ctx->device, ctx->bindlessPool,               NULL);  ctx->bindlessPool          = VK_NULL_HANDLE; }
    /* lighting UBO */
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (ctx->lightingUBOMapped[i])   { vkUnmapMemory  (ctx->device, ctx->lightingUBOMemory[i]);                    ctx->lightingUBOMapped[i]   = NULL;           }
        if (ctx->lightingUBO[i])         { vkDestroyBuffer(ctx->device, ctx->lightingUBO[i],         NULL);            ctx->lightingUBO[i]         = VK_NULL_HANDLE; }
        if (ctx->lightingUBOMemory[i])   { vkFreeMemory   (ctx->device, ctx->lightingUBOMemory[i],   NULL);            ctx->lightingUBOMemory[i]   = VK_NULL_HANDLE; }
    }
    if (ctx->lightingSetLayout) { vkDestroyDescriptorSetLayout(ctx->device, ctx->lightingSetLayout, NULL); ctx->lightingSetLayout = VK_NULL_HANDLE; }
    if (ctx->lightingPool)      { vkDestroyDescriptorPool     (ctx->device, ctx->lightingPool,      NULL); ctx->lightingPool      = VK_NULL_HANDLE; }
    /* IBL */
    destroyIBL(ctx);

    /* command buffers */
    if (ctx->commandBuffers && ctx->commandPool) {
        vkFreeCommandBuffers(ctx->device, ctx->commandPool, ctx->swapChainImageCount, ctx->commandBuffers);
        free(ctx->commandBuffers); ctx->commandBuffers = NULL;
    }

    /* sync objects */
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (ctx->imageAvailableSemaphores[i]) vkDestroySemaphore(ctx->device, ctx->imageAvailableSemaphores[i], NULL);
        if (ctx->inFlightFences[i])           vkDestroyFence    (ctx->device, ctx->inFlightFences[i],           NULL);
    }
    free(ctx->imageAvailableSemaphores);
    free(ctx->inFlightFences);
    for (uint32_t i = 0; i < ctx->swapChainImageCount; i++)
        if (ctx->renderFinishedSemaphores[i])
            vkDestroySemaphore(ctx->device, ctx->renderFinishedSemaphores[i], NULL);
    free(ctx->renderFinishedSemaphores);
    free(ctx->imagesInFlight);

    if (ctx->commandPool) { vkDestroyCommandPool(ctx->device, ctx->commandPool, NULL); ctx->commandPool = VK_NULL_HANDLE; }

    cleanupSwapChainResources(ctx, ctx->swapChain);

    /* pipelines */
#define DESTROY_PIPELINE(p)       if (ctx->p) { vkDestroyPipeline      (ctx->device, ctx->p, NULL); ctx->p = VK_NULL_HANDLE; }
#define DESTROY_LAYOUT(l)         if (ctx->l) { vkDestroyPipelineLayout(ctx->device, ctx->l, NULL); ctx->l = VK_NULL_HANDLE; }
    /* pipelineIndirectSolid and pipelineIndirectTextured alias graphicsPipeline[0] —
       null them out first so DESTROY_PIPELINE doesn't double-free.                   */
    pipelineIndirectSolid    = VK_NULL_HANDLE;
    pipelineIndirectTextured = VK_NULL_HANDLE;

    /* graphicsPipelineTextured3D aliases graphicsPipeline (pipelines[0]).
       Null the aliases before the macro runs to prevent double-destroy.               */
    ctx->graphicsPipelineTextured3D = VK_NULL_HANDLE;

    DESTROY_PIPELINE(graphicsPipeline)       /* pipelines[0] */
    DESTROY_PIPELINE(graphicsPipelineLine)   /* pipelines[1] */
    DESTROY_PIPELINE(graphicsPipeline2D)     /* pipelines[2] */
    DESTROY_PIPELINE(graphicsPipelineTextured2D) /* pipelines[3] */
    /* aliases already nulled — these are no-ops but kept for safety */
    DESTROY_PIPELINE(graphicsPipelineTextured3D)

    /* These all alias other layouts — null before destroy to prevent double-free */
    ctx->pipelineLayoutLine       = VK_NULL_HANDLE;
    ctx->pipelineLayoutTextured3D = VK_NULL_HANDLE; /* aliases pipelineLayout */
    ctx->pipelineLayoutIndirect   = VK_NULL_HANDLE; /* aliases pipelineLayout */

    DESTROY_LAYOUT(pipelineLayout)           /* the real 3D PBR layout */
    DESTROY_LAYOUT(pipelineLayout2D)
    DESTROY_LAYOUT(pipelineLayoutTextured2D)
    /* aliases already nulled — these are no-ops */
    DESTROY_LAYOUT(pipelineLayoutTextured3D)
    DESTROY_LAYOUT(pipelineLayoutLine)
    DESTROY_LAYOUT(pipelineLayoutIndirect)

    if (skyboxPipeline) { vkDestroyPipeline(ctx->device, skyboxPipeline, NULL); skyboxPipeline = VK_NULL_HANDLE; }
    if (skyboxPipelineLayout) { vkDestroyPipelineLayout(ctx->device, skyboxPipelineLayout, NULL); skyboxPipelineLayout = VK_NULL_HANDLE; }

#undef DESTROY_PIPELINE
#undef DESTROY_LAYOUT

    if (ctx->computeCullPipeline)       { vkDestroyPipeline      (ctx->device, ctx->computeCullPipeline,       NULL); ctx->computeCullPipeline       = VK_NULL_HANDLE; }
    if (ctx->computeCullPipelineLayout) { vkDestroyPipelineLayout(ctx->device, ctx->computeCullPipelineLayout, NULL); ctx->computeCullPipelineLayout = VK_NULL_HANDLE; }
    if (ctx->computeCullSetLayout)      { vkDestroyDescriptorSetLayout(ctx->device, ctx->computeCullSetLayout, NULL); ctx->computeCullSetLayout      = VK_NULL_HANDLE; }
    if (ctx->computeCullPool)           { vkDestroyDescriptorPool(ctx->device, ctx->computeCullPool,           NULL); ctx->computeCullPool           = VK_NULL_HANDLE; }
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (ctx->frustumUBOMapped[i])   { vkUnmapMemory  (ctx->device, ctx->frustumUBOMemory[i]);                    ctx->frustumUBOMapped[i]       = NULL;           }
        if (ctx->frustumUBOBuffer[i])   { vkDestroyBuffer(ctx->device, ctx->frustumUBOBuffer[i],   NULL);            ctx->frustumUBOBuffer[i]       = VK_NULL_HANDLE; }
        if (ctx->frustumUBOMemory[i])   { vkFreeMemory   (ctx->device, ctx->frustumUBOMemory[i],   NULL);            ctx->frustumUBOMemory[i]       = VK_NULL_HANDLE; }
    }
    if (pipelineCache != VK_NULL_HANDLE) { vkDestroyPipelineCache(ctx->device, pipelineCache, NULL); pipelineCache = VK_NULL_HANDLE; }

    /* 2D vertex buffer — one per frame */
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (ctx->vertexBuffer2D[i])       { vkDestroyBuffer(ctx->device, ctx->vertexBuffer2D[i],       NULL); ctx->vertexBuffer2D[i]       = VK_NULL_HANDLE; }
        if (ctx->vertexBufferMemory2D[i]) { vkFreeMemory   (ctx->device, ctx->vertexBufferMemory2D[i], NULL); ctx->vertexBufferMemory2D[i] = VK_NULL_HANDLE; }
    }

    if (ctx->renderPass)           { vkDestroyRenderPass         (ctx->device, ctx->renderPass,           NULL); ctx->renderPass              = VK_NULL_HANDLE; }

    /* per-frame uniform buffers — unmap then destroy */
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (ctx->uboMapped[i])            { vkUnmapMemory  (ctx->device, ctx->uniformBuffersMemory[i]);          ctx->uboMapped[i]            = NULL;           }
        if (ctx->uniformBuffers[i])       { vkDestroyBuffer(ctx->device, ctx->uniformBuffers[i],          NULL); ctx->uniformBuffers[i]       = VK_NULL_HANDLE; }
        if (ctx->uniformBuffersMemory[i]) { vkFreeMemory   (ctx->device, ctx->uniformBuffersMemory[i],    NULL); ctx->uniformBuffersMemory[i] = VK_NULL_HANDLE; }
    }
    if (ctx->descriptorPool)       { vkDestroyDescriptorPool     (ctx->device, ctx->descriptorPool,       NULL); ctx->descriptorPool          = VK_NULL_HANDLE; }
    if (ctx->descriptorSetLayout)  { vkDestroyDescriptorSetLayout(ctx->device, ctx->descriptorSetLayout,  NULL); ctx->descriptorSetLayout     = VK_NULL_HANDLE; }

    /* mega vertex buffer */
    if (ctx->megaVertexBuffer)       { vkDestroyBuffer(ctx->device, ctx->megaVertexBuffer,       NULL); ctx->megaVertexBuffer       = VK_NULL_HANDLE; }
    if (ctx->megaVertexBufferMemory) { vkFreeMemory   (ctx->device, ctx->megaVertexBufferMemory, NULL); ctx->megaVertexBufferMemory = VK_NULL_HANDLE; }
    /* mega index buffer */
    if (ctx->megaIndexBuffer)        { vkDestroyBuffer(ctx->device, ctx->megaIndexBuffer,        NULL); ctx->megaIndexBuffer        = VK_NULL_HANDLE; }
    if (ctx->megaIndexBufferMemory)  { vkFreeMemory   (ctx->device, ctx->megaIndexBufferMemory,  NULL); ctx->megaIndexBufferMemory  = VK_NULL_HANDLE; }

    /* dynamic buffers */
    if (ctx->dynamicStagingMapped)   { vkUnmapMemory  (ctx->device, ctx->dynamicStagingMemory);         ctx->dynamicStagingMapped   = NULL;           }
    if (ctx->dynamicStagingBuffer)   { vkDestroyBuffer(ctx->device, ctx->dynamicStagingBuffer,   NULL); ctx->dynamicStagingBuffer   = VK_NULL_HANDLE; }
    if (ctx->dynamicStagingMemory)   { vkFreeMemory   (ctx->device, ctx->dynamicStagingMemory,   NULL); ctx->dynamicStagingMemory   = VK_NULL_HANDLE; }
    if (ctx->dynamicDeviceBuffer)    { vkDestroyBuffer(ctx->device, ctx->dynamicDeviceBuffer,    NULL); ctx->dynamicDeviceBuffer    = VK_NULL_HANDLE; }
    if (ctx->dynamicDeviceMemory)    { vkFreeMemory   (ctx->device, ctx->dynamicDeviceMemory,    NULL); ctx->dynamicDeviceMemory    = VK_NULL_HANDLE; }

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (ctx->meshSSBOMapped[i])   { vkUnmapMemory(ctx->device, ctx->meshSSBOMemory[i]);                        ctx->meshSSBOMapped[i]   = NULL;           }
        if (ctx->meshSSBO[i])         { vkDestroyBuffer(ctx->device, ctx->meshSSBO[i],         NULL);              ctx->meshSSBO[i]         = VK_NULL_HANDLE; }
        if (ctx->meshSSBOMemory[i])   { vkFreeMemory   (ctx->device, ctx->meshSSBOMemory[i],   NULL);              ctx->meshSSBOMemory[i]   = VK_NULL_HANDLE; }
    }
    if (ctx->meshDirtyBits)        { free(ctx->meshDirtyBits); ctx->meshDirtyBits = NULL; }
    if (ctx->ssboSetLayout)        { vkDestroyDescriptorSetLayout(ctx->device, ctx->ssboSetLayout,        NULL); ctx->ssboSetLayout        = VK_NULL_HANDLE; }
    if (ctx->ssboPool)                { vkDestroyDescriptorPool     (ctx->device, ctx->ssboPool,             NULL); ctx->ssboPool             = VK_NULL_HANDLE; }
    if (ctx->srcIndirectBufferMapped) { vkUnmapMemory  (ctx->device, ctx->srcIndirectBufferMemory);              ctx->srcIndirectBufferMapped = NULL;           }
    if (ctx->srcIndirectBuffer)       { vkDestroyBuffer(ctx->device, ctx->srcIndirectBuffer,        NULL);       ctx->srcIndirectBuffer       = VK_NULL_HANDLE; }
    if (ctx->srcIndirectBufferMemory) { vkFreeMemory   (ctx->device, ctx->srcIndirectBufferMemory,  NULL);       ctx->srcIndirectBufferMemory = VK_NULL_HANDLE; }
    if (ctx->indirectBuffer)          { vkDestroyBuffer(ctx->device, ctx->indirectBuffer,           NULL);       ctx->indirectBuffer          = VK_NULL_HANDLE; }
    if (ctx->indirectBufferMemory)    { vkFreeMemory   (ctx->device, ctx->indirectBufferMemory,     NULL);       ctx->indirectBufferMemory    = VK_NULL_HANDLE; }

    if (ctx->device)               { vkDestroyDevice             (ctx->device,                            NULL); ctx->device                  = VK_NULL_HANDLE; }
    if (ctx->surface)              { vkDestroySurfaceKHR         (ctx->instance, ctx->surface,            NULL); ctx->surface                 = VK_NULL_HANDLE; }
    if (ctx->instance)             { vkDestroyInstance           (ctx->instance,                          NULL); ctx->instance                = VK_NULL_HANDLE; }
    if (ctx->window)               { glfwDestroyWindow           (ctx->window                                 ); ctx->window                  = NULL; }
    glfwTerminate();
}


/// Misc

/// Mega vertex buffer + dynamic buffer helpers

void copyBuffer(VkDevice device, VkCommandPool pool, VkQueue queue,
                VkBuffer src, VkBuffer dst, VkDeviceSize size,
                VkDeviceSize srcOffset, VkDeviceSize dstOffset)
{
    VkCommandBuffer cmd = beginSingleTimeCommands(device, pool);
    VkBufferCopy region = { .srcOffset = srcOffset, .dstOffset = dstOffset, .size = size };
    vkCmdCopyBuffer(cmd, src, dst, 1, &region);
    endSingleTimeCommands(device, pool, queue, cmd);
}

void createMegaVertexBuffer(VulkanContext* ctx, VkDeviceSize size)
{
    ctx->megaVertexBufferSize   = size;
    ctx->megaVertexBufferOffset = 0;
    createBuffer(ctx, size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &ctx->megaVertexBuffer, &ctx->megaVertexBufferMemory);
    fprintf(stdout, "Mega vertex buffer: %.1f MB (DEVICE_LOCAL)\n", (double)size / (1024.0 * 1024.0));
}

/* Upload vertices into the mega buffer, return the BASE VERTEX INDEX for DrawIndexed/Draw offset.
   Uses a temporary staging buffer so the final data lives in DEVICE_LOCAL memory. */
void createUploadStagingBuffer(VulkanContext* ctx, VkDeviceSize size)
{
    ctx->uploadStagingSize   = size;
    ctx->uploadStagingOffset = 0;
    createBuffer(ctx, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &ctx->uploadStagingBuffer, &ctx->uploadStagingMemory);
    vkMapMemory(ctx->device, ctx->uploadStagingMemory, 0, size, 0, &ctx->uploadStagingMapped);

    /* pre-allocate region arrays */
    ctx->pendingVertexCopyCapacity = 256;
    ctx->pendingVertexCopyCount    = 0;
    ctx->pendingVertexCopies       = malloc(256 * sizeof(VkBufferCopy));
    ctx->pendingIndexCopyCapacity  = 256;
    ctx->pendingIndexCopyCount     = 0;
    ctx->pendingIndexCopies        = malloc(256 * sizeof(VkBufferCopy));

    fprintf(stdout, "Upload staging buffer: %.1f MB\n", (double)size / (1024.0 * 1024.0));
}

void flushUploadStagingBuffer(VulkanContext* ctx)
{
    if (ctx->pendingVertexCopyCount == 0 && ctx->pendingIndexCopyCount == 0) return;

    VkCommandBuffer cmd = beginSingleTimeCommands(ctx->device, ctx->commandPool);

    if (ctx->pendingVertexCopyCount > 0) {
        vkCmdCopyBuffer(cmd, ctx->uploadStagingBuffer, ctx->megaVertexBuffer,
                        ctx->pendingVertexCopyCount, ctx->pendingVertexCopies);
    }
    if (ctx->pendingIndexCopyCount > 0) {
        vkCmdCopyBuffer(cmd, ctx->uploadStagingBuffer, ctx->megaIndexBuffer,
                        ctx->pendingIndexCopyCount, ctx->pendingIndexCopies);
    }

    endSingleTimeCommands(ctx->device, ctx->commandPool, ctx->graphicsQueue, cmd);

    /* reset for next batch */
    ctx->pendingVertexCopyCount = 0;
    ctx->pendingIndexCopyCount  = 0;
    ctx->uploadStagingOffset    = 0;

    fprintf(stdout, "Flushed upload staging buffer: %u vertex + %u index regions\n",
            ctx->pendingVertexCopyCount, ctx->pendingIndexCopyCount);
}

void destroyUploadStagingBuffer(VulkanContext* ctx)
{
    if (ctx->uploadStagingMapped)  { vkUnmapMemory(ctx->device, ctx->uploadStagingMemory); ctx->uploadStagingMapped = NULL; }
    if (ctx->uploadStagingBuffer)  { vkDestroyBuffer(ctx->device, ctx->uploadStagingBuffer, NULL); ctx->uploadStagingBuffer = VK_NULL_HANDLE; }
    if (ctx->uploadStagingMemory)  { vkFreeMemory(ctx->device, ctx->uploadStagingMemory, NULL);    ctx->uploadStagingMemory = VK_NULL_HANDLE; }
    if (ctx->pendingVertexCopies)  { free(ctx->pendingVertexCopies); ctx->pendingVertexCopies = NULL; }
    if (ctx->pendingIndexCopies)   { free(ctx->pendingIndexCopies);  ctx->pendingIndexCopies  = NULL; }
    ctx->uploadStagingSize            = 0;
    ctx->uploadStagingOffset          = 0;
    ctx->pendingVertexCopyCount       = 0;
    ctx->pendingVertexCopyCapacity    = 0;
    ctx->pendingIndexCopyCount        = 0;
    ctx->pendingIndexCopyCapacity     = 0;
}

uint32_t megaBufferAllocate(VulkanContext* ctx, Vertex* vertices, uint32_t vertexCount)
{
    VkDeviceSize uploadSize = vertexCount * sizeof(Vertex);
    VkDeviceSize dstOffset  = (VkDeviceSize)ctx->megaVertexBufferOffset * sizeof(Vertex);

    if (dstOffset + uploadSize > ctx->megaVertexBufferSize) {
        fprintf(stderr, "Mega vertex buffer overflow!\n");
        return UINT32_MAX;
    }

    /* if this upload won't fit in the staging buffer, flush first */
    if (ctx->uploadStagingBuffer &&
        ctx->uploadStagingOffset + uploadSize > ctx->uploadStagingSize) {
        flushUploadStagingBuffer(ctx);
    }

    if (ctx->uploadStagingBuffer) {
        /* fast path: memcpy into persistent staging, record region */
        memcpy((uint8_t*)ctx->uploadStagingMapped + ctx->uploadStagingOffset,
               vertices, uploadSize);

        if (ctx->pendingVertexCopyCount == ctx->pendingVertexCopyCapacity) {
            ctx->pendingVertexCopyCapacity *= 2;
            ctx->pendingVertexCopies = realloc(ctx->pendingVertexCopies,
                ctx->pendingVertexCopyCapacity * sizeof(VkBufferCopy));
        }
        ctx->pendingVertexCopies[ctx->pendingVertexCopyCount++] = (VkBufferCopy){
            .srcOffset = ctx->uploadStagingOffset,
            .dstOffset = dstOffset,
            .size      = uploadSize
        };
        ctx->uploadStagingOffset += uploadSize;
    } else {
        /* fallback: old per-mesh staging path */
        VkBuffer stagingBuf; VkDeviceMemory stagingMem;
        createBuffer(ctx, uploadSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &stagingBuf, &stagingMem);
        void* mapped;
        vkMapMemory(ctx->device, stagingMem, 0, uploadSize, 0, &mapped);
        memcpy(mapped, vertices, uploadSize);
        vkUnmapMemory(ctx->device, stagingMem);
        copyBuffer(ctx->device, ctx->commandPool, ctx->graphicsQueue,
                   stagingBuf, ctx->megaVertexBuffer, uploadSize, 0, dstOffset);
        vkDestroyBuffer(ctx->device, stagingBuf, NULL);
        vkFreeMemory(ctx->device, stagingMem, NULL);
    }

    uint32_t baseVertex = ctx->megaVertexBufferOffset;
    ctx->megaVertexBufferOffset += vertexCount;
    return baseVertex;
}

void createMegaIndexBuffer(VulkanContext* ctx, VkDeviceSize size)
{
    ctx->megaIndexBufferSize   = size;
    ctx->megaIndexBufferOffset = 0;
    createBuffer(ctx, size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &ctx->megaIndexBuffer, &ctx->megaIndexBufferMemory);
    fprintf(stdout, "Mega index buffer: %.1f MB (DEVICE_LOCAL)\n", (double)size / (1024.0 * 1024.0));
}

/* Upload indices into the mega index buffer. Returns the BASE INDEX (firstIndex for DrawIndexed). */
uint32_t megaIndexBufferAllocate(VulkanContext* ctx, uint32_t* indices, uint32_t indexCount)
{
    VkDeviceSize uploadSize = indexCount * sizeof(uint32_t);
    VkDeviceSize dstOffset  = (VkDeviceSize)ctx->megaIndexBufferOffset * sizeof(uint32_t);

    if (dstOffset + uploadSize > ctx->megaIndexBufferSize) {
        fprintf(stderr, "Mega index buffer overflow!\n");
        return UINT32_MAX;
    }

    if (ctx->uploadStagingBuffer &&
        ctx->uploadStagingOffset + uploadSize > ctx->uploadStagingSize) {
        flushUploadStagingBuffer(ctx);
    }

    if (ctx->uploadStagingBuffer) {
        memcpy((uint8_t*)ctx->uploadStagingMapped + ctx->uploadStagingOffset,
               indices, uploadSize);

        if (ctx->pendingIndexCopyCount == ctx->pendingIndexCopyCapacity) {
            ctx->pendingIndexCopyCapacity *= 2;
            ctx->pendingIndexCopies = realloc(ctx->pendingIndexCopies,
                ctx->pendingIndexCopyCapacity * sizeof(VkBufferCopy));
        }
        ctx->pendingIndexCopies[ctx->pendingIndexCopyCount++] = (VkBufferCopy){
            .srcOffset = ctx->uploadStagingOffset,
            .dstOffset = dstOffset,
            .size      = uploadSize
        };
        ctx->uploadStagingOffset += uploadSize;
    } else {
        /* fallback */
        VkBuffer stagingBuf; VkDeviceMemory stagingMem;
        createBuffer(ctx, uploadSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &stagingBuf, &stagingMem);
        void* mapped;
        vkMapMemory(ctx->device, stagingMem, 0, uploadSize, 0, &mapped);
        memcpy(mapped, indices, uploadSize);
        vkUnmapMemory(ctx->device, stagingMem);
        copyBuffer(ctx->device, ctx->commandPool, ctx->graphicsQueue,
                   stagingBuf, ctx->megaIndexBuffer, uploadSize, 0, dstOffset);
        vkDestroyBuffer(ctx->device, stagingBuf, NULL);
        vkFreeMemory(ctx->device, stagingMem, NULL);
    }

    uint32_t baseIndex = ctx->megaIndexBufferOffset;
    ctx->megaIndexBufferOffset += indexCount;
    return baseIndex;
}

void createDynamicBuffers(VulkanContext* ctx, VkDeviceSize size)
{
    ctx->dynamicBufferSize = size;
    createBuffer(ctx, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &ctx->dynamicStagingBuffer, &ctx->dynamicStagingMemory);
    vkMapMemory(ctx->device, ctx->dynamicStagingMemory, 0, size, 0, &ctx->dynamicStagingMapped);

    createBuffer(ctx, size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &ctx->dynamicDeviceBuffer, &ctx->dynamicDeviceMemory);

    fprintf(stdout, "Dynamic buffers: %.1f MB staging + %.1f MB device-local\n", (double)size/(1024*1024), (double)size/(1024*1024));
}

/// Bindless texture array

void createBindlessDescriptorLayout(VulkanContext* ctx)
{
    /* One binding: array of MAX_TEXTURES combined-image-samplers,
       all accessible from the fragment stage.
       VARIABLE_DESCRIPTOR_COUNT lets us allocate fewer than MAX_TEXTURES
       at pool/set creation time without validation errors.               */
    VkDescriptorSetLayoutBinding binding = {
        .binding            = 0,
        .descriptorType     = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount    = MAX_TEXTURES,
        .stageFlags         = VK_SHADER_STAGE_FRAGMENT_BIT,
        .pImmutableSamplers = NULL
    };

    VkDescriptorBindingFlagsEXT bindingFlags =
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT_EXT |
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT_EXT;

    VkDescriptorSetLayoutBindingFlagsCreateInfoEXT bindingFlagsCI = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO_EXT,
        .bindingCount  = 1,
        .pBindingFlags = &bindingFlags,
    };

    VkDescriptorSetLayoutCreateInfo ci = {
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext        = &bindingFlagsCI,
        .flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT_EXT,
        .bindingCount = 1,
        .pBindings    = &binding
    };
    if (vkCreateDescriptorSetLayout(ctx->device, &ci, NULL, &ctx->bindlessSetLayout) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create bindless descriptor set layout\n");
        exit(EXIT_FAILURE);
    }
}

void createBindlessDescriptorPool(VulkanContext* ctx)
{
    VkDescriptorPoolSize ps = {
        .type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = MAX_TEXTURES
    };
    VkDescriptorPoolCreateInfo ci = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        /* UPDATE_AFTER_BIND lets us write slots incrementally as textures load */
        .flags         = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
        .maxSets       = 1,
        .poolSizeCount = 1,
        .pPoolSizes    = &ps
    };
    if (vkCreateDescriptorPool(ctx->device, &ci, NULL, &ctx->bindlessPool) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create bindless descriptor pool\n");
        exit(EXIT_FAILURE);
    }
}

void createBindlessDescriptorSet(VulkanContext* ctx)
{
    VkDescriptorSetAllocateInfo ai = {
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = ctx->bindlessPool,
        .descriptorSetCount = 1,
        .pSetLayouts        = &ctx->bindlessSetLayout
    };
    if (vkAllocateDescriptorSets(ctx->device, &ai, &ctx->bindlessSet) != VK_SUCCESS) {
        fprintf(stderr, "Failed to allocate bindless descriptor set\n");
        exit(EXIT_FAILURE);
    }
    ctx->bindlessTextureCount = 0;
}

/* Call once per texture after it's loaded to register it into the bindless array.
   Returns the slot index the shader will use.                                     */
void bindlessRegisterTexture(VulkanContext* ctx, uint32_t slot,
                             VkImageView view, VkSampler sampler)
{
    VkDescriptorImageInfo img = {
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .imageView   = view,
        .sampler     = sampler
    };
    VkWriteDescriptorSet w = {
        .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet          = ctx->bindlessSet,
        .dstBinding      = 0,
        .dstArrayElement = slot,          /* write into slot N of the array */
        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .pImageInfo      = &img
    };
    vkUpdateDescriptorSets(ctx->device, 1, &w, 0, NULL);
    ctx->bindlessTextureCount = slot + 1;
}

/// Lighting descriptors

void createLightingDescriptors(VulkanContext* ctx)
{
    VkDescriptorSetLayoutBinding b[5] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
        { .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
        { .binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
        { .binding = 4, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT }
    };
    VkDescriptorBindingFlagsEXT flags[5] = { 0, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT_EXT, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT_EXT, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT_EXT, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT_EXT };
    VkDescriptorSetLayoutBindingFlagsCreateInfoEXT flagsInfo = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO_EXT, .bindingCount = 5, .pBindingFlags = flags };
    VkDescriptorSetLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .pNext = &flagsInfo, .bindingCount = 5, .pBindings = b };
    vkCreateDescriptorSetLayout(ctx->device, &lci, NULL, &ctx->lightingSetLayout);

    VkDescriptorPoolSize ps[2] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, MAX_FRAMES_IN_FLIGHT },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_FRAMES_IN_FLIGHT * 4 }
    };
    VkDescriptorPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = MAX_FRAMES_IN_FLIGHT, .poolSizeCount = 2, .pPoolSizes = ps
    };
    vkCreateDescriptorPool(ctx->device, &pci, NULL, &ctx->lightingPool);

    VkDeviceSize size = sizeof(LightingData);
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkBufferCreateInfo bci = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size, .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE
        };
        vkCreateBuffer(ctx->device, &bci, NULL, &ctx->lightingUBO[i]);
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(ctx->device, ctx->lightingUBO[i], &mr);
        VkMemoryAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
            .memoryTypeIndex = findMemoryType(ctx->physicalDevice, mr.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
        };
        vkAllocateMemory(ctx->device, &ai, NULL, &ctx->lightingUBOMemory[i]);
        vkBindBufferMemory(ctx->device, ctx->lightingUBO[i], ctx->lightingUBOMemory[i], 0);
        vkMapMemory(ctx->device, ctx->lightingUBOMemory[i], 0, size, 0, &ctx->lightingUBOMapped[i]);

        VkDescriptorSetAllocateInfo dsai = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = ctx->lightingPool, .descriptorSetCount = 1,
            .pSetLayouts = &ctx->lightingSetLayout
        };
        vkAllocateDescriptorSets(ctx->device, &dsai, &ctx->lightingSets[i]);

        VkDescriptorBufferInfo dbi = { .buffer = ctx->lightingUBO[i], .offset = 0, .range = size };
        VkWriteDescriptorSet w = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = ctx->lightingSets[i], .dstBinding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1, .pBufferInfo = &dbi
        };
        vkUpdateDescriptorSets(ctx->device, 1, &w, 0, NULL);
    }
    fprintf(stdout, "Lighting UBO descriptors created\n");
}

void updateLightingUBO(VulkanContext* ctx)
{
    memcpy(ctx->lightingUBOMapped[ctx->currentFrame], ctx->lightingDataRaw, sizeof(LightingData));
}

/// SSBO + Indirect draw

void createMeshSSBO(VulkanContext* ctx, uint32_t maxMeshes)
{
    VkDeviceSize size = maxMeshes * sizeof(MeshGPUData);

    VkDescriptorSetLayoutBinding b = {
        .binding         = 0,
        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
    };
    VkDescriptorSetLayoutCreateInfo lci = {
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &b
    };
    vkCreateDescriptorSetLayout(ctx->device, &lci, NULL, &ctx->ssboSetLayout);

    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, MAX_FRAMES_IN_FLIGHT };
    VkDescriptorPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = MAX_FRAMES_IN_FLIGHT, .poolSizeCount = 1, .pPoolSizes = &ps
    };
    vkCreateDescriptorPool(ctx->device, &pci, NULL, &ctx->ssboPool);

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        createBuffer(ctx, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &ctx->meshSSBO[i], &ctx->meshSSBOMemory[i]);
        vkMapMemory(ctx->device, ctx->meshSSBOMemory[i], 0, size, 0, &ctx->meshSSBOMapped[i]);

        /* Allocate + write descriptor set */
        VkDescriptorSetAllocateInfo dsai = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = ctx->ssboPool,
            .descriptorSetCount = 1,
            .pSetLayouts = &ctx->ssboSetLayout
        };
        vkAllocateDescriptorSets(ctx->device, &dsai, &ctx->ssboSets[i]);

        VkDescriptorBufferInfo dbi = {
            .buffer = ctx->meshSSBO[i], .offset = 0, .range = size
        };
        VkWriteDescriptorSet w = {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = ctx->ssboSets[i],
            .dstBinding      = 0,
            .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .pBufferInfo     = &dbi
        };
        vkUpdateDescriptorSets(ctx->device, 1, &w, 0, NULL);
    }
    fprintf(stdout, "Mesh SSBO: %.1f MB x%d frames\n",
            (double)size/(1024*1024), MAX_FRAMES_IN_FLIGHT);

    /* allocate per-mesh dirty bitfield — one uint64 per 64 meshes per frame */
    ctx->meshDirtyCapacity = ((maxMeshes + 63) / 64) * 64;
    uint32_t words = ctx->meshDirtyCapacity / 64;
    ctx->meshDirtyBits = malloc(words * MAX_FRAMES_IN_FLIGHT * sizeof(uint64_t));
    /* start fully dirty so first frames upload everything */
    memset(ctx->meshDirtyBits, 0xFF,
           words * MAX_FRAMES_IN_FLIGHT * sizeof(uint64_t));
}

void createIndirectBuffer(VulkanContext* ctx, uint32_t maxMeshes)
{
    VkDeviceSize size = maxMeshes * sizeof(VkDrawIndexedIndirectCommand);

    /* Source buffer: CPU writes original draw commands, compute reads */
    createBuffer(ctx, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &ctx->srcIndirectBuffer, &ctx->srcIndirectBufferMemory);
    vkMapMemory(ctx->device, ctx->srcIndirectBufferMemory, 0, size, 0, &ctx->srcIndirectBufferMapped);

    /* Destination buffer: compute writes, GPU draw reads — DEVICE_LOCAL */
    createBuffer(ctx, size, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &ctx->indirectBuffer, &ctx->indirectBufferMemory);

    fprintf(stdout, "Indirect buffers: %u draw slots (src HOST_VISIBLE, dst DEVICE_LOCAL)\n", maxMeshes);
}

void markMeshesSSBODirty(VulkanContext* ctx)
{
    ctx->ssboFramesDirty = MAX_FRAMES_IN_FLIGHT;
    /* mark all meshes dirty across all frames */
    if (ctx->meshDirtyBits) {
        uint32_t words = ctx->meshDirtyCapacity / 64;
        memset(ctx->meshDirtyBits, 0xFF,
               words * MAX_FRAMES_IN_FLIGHT * sizeof(uint64_t));
    }
}

void markMeshDirty(VulkanContext* ctx, uint32_t meshIndex)
{
    uint32_t word = meshIndex / 64;
    uint64_t bit  = 1ULL << (meshIndex % 64);
    if (ctx->meshDirtyBits && word < ctx->meshDirtyCapacity / 64) {
        for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; f++)
            ctx->meshDirtyBits[word * MAX_FRAMES_IN_FLIGHT + f] |= bit;
    }
    ctx->ssboFramesDirty = MAX_FRAMES_IN_FLIGHT;
}

void updateMeshSSBOAndIndirect(VulkanContext* ctx, Meshes* meshes)
{
    uint32_t count = (uint32_t)meshes->count;
    ctx->indirectDrawCount = count;

    /* Rebuild indirect buffer every time (it's HOST_VISIBLE, cheap to write) */
    VkDrawIndexedIndirectCommand* cmds = (VkDrawIndexedIndirectCommand*)ctx->srcIndirectBufferMapped;

    for (uint32_t i = 0; i < count; i++) {
        Mesh* m = &meshes->items[i];

        if (m->megaBaseVertex == UINT32_MAX) {
            cmds[i].indexCount    = 0;
            cmds[i].instanceCount = 0;
            cmds[i].firstIndex    = 0;
            cmds[i].vertexOffset  = 0;
            cmds[i].firstInstance = i;
            continue;
        }

        cmds[i].indexCount    = m->indexCount;
        cmds[i].instanceCount = 1;
        cmds[i].firstIndex    = (m->megaBaseIndex == UINT32_MAX) ? 0 : m->megaBaseIndex;
        cmds[i].vertexOffset  = (int32_t)m->megaBaseVertex;
        cmds[i].firstInstance = i;
    }

    /* Mark all in-flight frames as needing SSBO upload */
    markMeshesSSBODirty(ctx);
}

/* Call once per frame from beginFrame — uploads SSBO only to currentFrame if dirty */
void flushMeshSSBO(VulkanContext* ctx, Meshes* meshes)
{
    if (ctx->ssboFramesDirty == 0) return;

    uint32_t count = (uint32_t)meshes->count;
    uint32_t f     = ctx->currentFrame;

    MeshGPUData* dst = (MeshGPUData*)ctx->meshSSBOMapped[f];

    for (uint32_t i = 0; i < count; i++) {
        /* skip meshes that are clean for this frame */
        uint32_t word = i / 64;
        uint64_t bit  = 1ULL << (i % 64);
        bool dirty = (ctx->meshDirtyBits == NULL) ||
                     (word < ctx->meshDirtyCapacity / 64 &&
                      (ctx->meshDirtyBits[word * MAX_FRAMES_IN_FLIGHT + f] & bit));
        if (!dirty) continue;

        Mesh* m = &meshes->items[i];
        if (m->megaBaseVertex == UINT32_MAX) {
            memset(&dst[i], 0, sizeof(MeshGPUData));
            continue;
        }
        glm_mat4_copy(m->model, dst[i].model);
        mat4 inv;
        glm_mat4_inv(m->model, inv);
        glm_mat4_transpose(inv);
        glm_mat4_copy(inv, dst[i].normalMatrix);

        /* PBR texture slots */
        dst[i].albedoIndex        = (m->texture && m->texture->loaded)
                                    ? (int)m->texture->bindlessSlot : -1;
        dst[i].normalMapIndex     = m->normalMapIndex;
        dst[i].metallicRoughIndex = m->metallicRoughIndex;
        dst[i].aoIndex            = m->aoIndex;
        dst[i].emissiveIndex      = m->emissiveIndex;

        /* Material constant factors */
        glm_vec4_copy(m->baseColorFactor,  dst[i].baseColorFactor);
        dst[i].metallicFactor    = m->metallicFactor;
        dst[i].roughnessFactor   = m->roughnessFactor;
        dst[i].emissiveStrength  = m->emissiveStrength;
        glm_vec3_copy(m->emissiveFactor,   dst[i].emissiveFactor);

        dst[i].isUnlit      = m->is_unlit ? 1 : 0;
        dst[i].alphaMode    = m->alpha_mode;
        dst[i].alphaCutoff  = m->alpha_cutoff;
        glm_vec3_copy(m->aabbMin, dst[i].aabbMin);
        glm_vec3_copy(m->aabbMax, dst[i].aabbMax);
        dst[i].aabbMin[3]   = 0.0f;
        dst[i].aabbMax[3]   = 0.0f;

        /* clear dirty bit for this frame */
        if (ctx->meshDirtyBits && word < ctx->meshDirtyCapacity / 64)
            ctx->meshDirtyBits[word * MAX_FRAMES_IN_FLIGHT + f] &= ~bit;
    }

    ctx->ssboFramesDirty--;
}

static void create_ibl_image(VulkanContext* ctx, uint32_t w, uint32_t h, uint32_t mips, uint32_t layers, VkFormat format, VkImageUsageFlags usage, VkImage* img, VkDeviceMemory* mem) {
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = format,
        .extent = {w, h, 1}, .mipLevels = mips, .arrayLayers = layers, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    if (layers == 6) ci.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    vkCreateImage(ctx->device, &ci, NULL, img);
    VkMemoryRequirements req; vkGetImageMemoryRequirements(ctx->device, *img, &req);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
        .memoryTypeIndex = findMemoryType(ctx->physicalDevice, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    vkAllocateMemory(ctx->device, &ai, NULL, mem);
    vkBindImageMemory(ctx->device, *img, *mem, 0);
}

static VkImageView create_ibl_view(VulkanContext* ctx, VkImage img, VkFormat format, VkImageViewType type, uint32_t baseMip, uint32_t mipCount, uint32_t layerCount) {
    VkImageViewCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = type, .format = format,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mipCount, 0, layerCount } };
    VkImageView view; vkCreateImageView(ctx->device, &ci, NULL, &view);
    return view;
}

static void transition_image_compute(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, uint32_t baseMip, uint32_t mipCount, uint32_t layerCount) {
    VkImageMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = oldLayout, .newLayout = newLayout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mipCount, 0, layerCount} };

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED) barrier.srcAccessMask = 0;
    else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    else if (oldLayout == VK_IMAGE_LAYOUT_GENERAL) barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;

    if (newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    else if (newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    else if (newLayout == VK_IMAGE_LAYOUT_GENERAL) barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
}

static void write_ibl_descriptor_set(VkDevice device, VkSampler tempSampler, VkDescriptorSet set, VkImageView sampView, VkImageView storView) {
    VkDescriptorImageInfo dSamp = { .sampler = tempSampler, .imageView = sampView, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo dStor = { .imageView = storView, .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w[2] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .pImageInfo = &dSamp },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .pImageInfo = &dStor }
    };
    vkUpdateDescriptorSets(device, sampView ? 2 : 1, sampView ? w : &w[1], 0, NULL);
}

bool loadIBL(VulkanContext* ctx, const char* hdr_path) {
    int w, h, channels;
    float* pixels = stbi_loadf(hdr_path, &w, &h, &channels, 4);
    if (!pixels) { fprintf(stderr, "Failed to load HDR: %s\n", hdr_path); return false; }

    VkFormat fmt32 = VK_FORMAT_R32G32B32A32_SFLOAT;

    // 1. Source HDR Image
    VkImage hdrImg; VkDeviceMemory hdrMem;
    create_ibl_image(ctx, w, h, 1, 1, fmt32, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &hdrImg, &hdrMem);
    VkImageView hdrView = create_ibl_view(ctx, hdrImg, fmt32, VK_IMAGE_VIEW_TYPE_2D, 0, 1, 1);

    VkDeviceSize size = w * h * 4 * sizeof(float);
    VkBuffer stgBuf; VkDeviceMemory stgMem;
    createBuffer(ctx, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &stgBuf, &stgMem);
    void* mapped; vkMapMemory(ctx->device, stgMem, 0, size, 0, &mapped);
    memcpy(mapped, pixels, size); vkUnmapMemory(ctx->device, stgMem);
    stbi_image_free(pixels);

    // 2. Temp Environment Cubemap
    VkImage envImg; VkDeviceMemory envMem;
    create_ibl_image(ctx, 1024, 1024, 1, 6, fmt32, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &envImg, &envMem);
    VkImageView envCubeView = create_ibl_view(ctx, envImg, fmt32, VK_IMAGE_VIEW_TYPE_CUBE, 0, 1, 6);
    VkImageView envArrayView = create_ibl_view(ctx, envImg, fmt32, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 0, 1, 6);

    // 3. Final IBL Images
    destroyIBL(ctx);
    create_ibl_image(ctx, 32, 32, 1, 6, fmt32, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &ctx->iblIrradianceImage, &ctx->iblIrradianceMemory);
    ctx->iblIrradianceView = create_ibl_view(ctx, ctx->iblIrradianceImage, fmt32, VK_IMAGE_VIEW_TYPE_CUBE, 0, 1, 6);
    VkImageView irradArrayView = create_ibl_view(ctx, ctx->iblIrradianceImage, fmt32, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 0, 1, 6);

    create_ibl_image(ctx, 128, 128, 5, 6, fmt32, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &ctx->iblPrefilterImage, &ctx->iblPrefilterMemory);
    ctx->iblPrefilterView = create_ibl_view(ctx, ctx->iblPrefilterImage, fmt32, VK_IMAGE_VIEW_TYPE_CUBE, 0, 5, 6);
    VkImageView prefArrayViews[5];
    for(uint32_t i=0; i<5; i++) prefArrayViews[i] = create_ibl_view(ctx, ctx->iblPrefilterImage, fmt32, VK_IMAGE_VIEW_TYPE_2D_ARRAY, i, 1, 6);

    create_ibl_image(ctx, 512, 512, 1, 1, VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &ctx->iblBrdfLutImage, &ctx->iblBrdfLutMemory);
    ctx->iblBrdfLutView = create_ibl_view(ctx, ctx->iblBrdfLutImage, VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_VIEW_TYPE_2D, 0, 1, 1);

    // Temp Sampler
    VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxAnisotropy = 1.0f };
    VkSampler tempSampler; vkCreateSampler(ctx->device, &sci, NULL, &tempSampler);

    // Descriptor Layouts & Pipelines
    VkDescriptorSetLayoutBinding bGen[] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}
    };
    VkDescriptorSetLayoutBinding bLut[] = { {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL} };

    VkDescriptorSetLayoutCreateInfo lciGen = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bGen };
    VkDescriptorSetLayoutCreateInfo lciLut = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = bLut };
    VkDescriptorSetLayout layoutGen, layoutLut;
    vkCreateDescriptorSetLayout(ctx->device, &lciGen, NULL, &layoutGen);
    vkCreateDescriptorSetLayout(ctx->device, &lciLut, NULL, &layoutLut);

    VkPushConstantRange pcRange = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = sizeof(float) };
    VkPipelineLayoutCreateInfo plciGen = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &layoutGen, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcRange };
    VkPipelineLayoutCreateInfo plciLut = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &layoutLut };
    VkPipelineLayout pipeLayoutGen, pipeLayoutLut;
    vkCreatePipelineLayout(ctx->device, &plciGen, NULL, &pipeLayoutGen);
    vkCreatePipelineLayout(ctx->device, &plciLut, NULL, &pipeLayoutLut);

    VkShaderModule modEq   = createShaderModule(ctx->device, equirect2cube_comp_spv, sizeof(equirect2cube_comp_spv));
    VkShaderModule modIrr  = createShaderModule(ctx->device, irradiance_comp_spv, sizeof(irradiance_comp_spv));
    VkShaderModule modPref = createShaderModule(ctx->device, prefilter_comp_spv, sizeof(prefilter_comp_spv));
    VkShaderModule modBrdf = createShaderModule(ctx->device, brdf_lut_comp_spv, sizeof(brdf_lut_comp_spv));

    VkComputePipelineCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .pName = "main" } };
    VkPipeline pipeEq, pipeIrr, pipePref, pipeBrdf;
    cpci.layout = pipeLayoutGen; cpci.stage.module = modEq;   vkCreateComputePipelines(ctx->device, VK_NULL_HANDLE, 1, &cpci, NULL, &pipeEq);
    cpci.layout = pipeLayoutGen; cpci.stage.module = modIrr;  vkCreateComputePipelines(ctx->device, VK_NULL_HANDLE, 1, &cpci, NULL, &pipeIrr);
    cpci.layout = pipeLayoutGen; cpci.stage.module = modPref; vkCreateComputePipelines(ctx->device, VK_NULL_HANDLE, 1, &cpci, NULL, &pipePref);
    cpci.layout = pipeLayoutLut; cpci.stage.module = modBrdf; vkCreateComputePipelines(ctx->device, VK_NULL_HANDLE, 1, &cpci, NULL, &pipeBrdf);

    // Descriptor Sets
    VkDescriptorPoolSize pSizes[] = { {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 10}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 10} };
    VkDescriptorPoolCreateInfo poolCI = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 10, .poolSizeCount = 2, .pPoolSizes = pSizes };
    VkDescriptorPool tempPool; vkCreateDescriptorPool(ctx->device, &poolCI, NULL, &tempPool);

    VkDescriptorSetLayout layouts[] = {layoutGen, layoutGen, layoutGen, layoutGen, layoutGen, layoutGen, layoutGen, layoutLut};
    VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = tempPool, .descriptorSetCount = 8, .pSetLayouts = layouts };
    VkDescriptorSet sets[8]; vkAllocateDescriptorSets(ctx->device, &dsai, sets);

    write_ibl_descriptor_set(ctx->device, tempSampler, sets[0], hdrView, envArrayView);
    write_ibl_descriptor_set(ctx->device, tempSampler, sets[1], envCubeView, irradArrayView);
    for(uint32_t i=0; i<5; i++) write_ibl_descriptor_set(ctx->device, tempSampler, sets[2+i], envCubeView, prefArrayViews[i]);

    VkDescriptorImageInfo dLut = { .imageView = ctx->iblBrdfLutView, .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet wLut = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[7], .dstBinding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .pImageInfo = &dLut };
    vkUpdateDescriptorSets(ctx->device, 1, &wLut, 0, NULL);

    // Execute Compute
    VkCommandBuffer cmd = beginSingleTimeCommands(ctx->device, ctx->commandPool);

    transition_image_compute(cmd, hdrImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1, 1);
    VkBufferImageCopy region = { .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {(uint32_t)w, (uint32_t)h, 1} };
    vkCmdCopyBufferToImage(cmd, stgBuf, hdrImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    transition_image_compute(cmd, hdrImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1, 1);

    transition_image_compute(cmd, envImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, 1, 6);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeEq);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayoutGen, 0, 1, &sets[0], 0, NULL);
    vkCmdDispatch(cmd, 1024/8, 1024/8, 6);

    transition_image_compute(cmd, envImg, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1, 6);
    transition_image_compute(cmd, ctx->iblIrradianceImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, 1, 6);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeIrr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayoutGen, 0, 1, &sets[1], 0, NULL);
    vkCmdDispatch(cmd, 32/8, 32/8, 6);
    transition_image_compute(cmd, ctx->iblIrradianceImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1, 6);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipePref);
    for(uint32_t i=0; i<5; i++) {
        uint32_t mipSize = 128 >> i;
        transition_image_compute(cmd, ctx->iblPrefilterImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, i, 1, 6);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayoutGen, 0, 1, &sets[2+i], 0, NULL);
        float roughness = (float)i / 4.0f;
        vkCmdPushConstants(cmd, pipeLayoutGen, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(float), &roughness);
        vkCmdDispatch(cmd, mipSize/8, mipSize/8, 6);
        transition_image_compute(cmd, ctx->iblPrefilterImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, i, 1, 6);
    }

    transition_image_compute(cmd, ctx->iblBrdfLutImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, 1, 1);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeBrdf);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayoutLut, 0, 1, &sets[7], 0, NULL);
    vkCmdDispatch(cmd, 512/8, 512/8, 1);
    transition_image_compute(cmd, ctx->iblBrdfLutImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1, 1);

    endSingleTimeCommands(ctx->device, ctx->commandPool, ctx->graphicsQueue, cmd);

    // Persist the Skybox
    iblSkyboxImage = envImg;
    iblSkyboxMemory = envMem;
    iblSkyboxView = envCubeView;

    // Cleanup Temp Resources
    vkDestroyBuffer(ctx->device, stgBuf, NULL); vkFreeMemory(ctx->device, stgMem, NULL);
    vkDestroyImageView(ctx->device, hdrView, NULL); vkDestroyImage(ctx->device, hdrImg, NULL); vkFreeMemory(ctx->device, hdrMem, NULL);
    vkDestroyImageView(ctx->device, envArrayView, NULL);
    for(uint32_t i=0; i<5; i++) vkDestroyImageView(ctx->device, prefArrayViews[i], NULL);
    vkDestroyImageView(ctx->device, irradArrayView, NULL);
    vkDestroySampler(ctx->device, tempSampler, NULL);
    vkDestroyPipeline(ctx->device, pipeEq, NULL); vkDestroyPipeline(ctx->device, pipeIrr, NULL);
    vkDestroyPipeline(ctx->device, pipePref, NULL); vkDestroyPipeline(ctx->device, pipeBrdf, NULL);
    vkDestroyPipelineLayout(ctx->device, pipeLayoutGen, NULL); vkDestroyPipelineLayout(ctx->device, pipeLayoutLut, NULL);
    vkDestroyDescriptorSetLayout(ctx->device, layoutGen, NULL); vkDestroyDescriptorSetLayout(ctx->device, layoutLut, NULL);
    vkDestroyDescriptorPool(ctx->device, tempPool, NULL);
    vkDestroyShaderModule(ctx->device, modEq, NULL); vkDestroyShaderModule(ctx->device, modIrr, NULL);
    vkDestroyShaderModule(ctx->device, modPref, NULL); vkDestroyShaderModule(ctx->device, modBrdf, NULL);

    // Create Permanent Samplers
    sci.maxLod = 1.0f; vkCreateSampler(ctx->device, &sci, NULL, &ctx->iblIrradianceSampler);
    sci.maxLod = 5.0f; vkCreateSampler(ctx->device, &sci, NULL, &ctx->iblPrefilterSampler);
    sci.maxLod = 1.0f; vkCreateSampler(ctx->device, &sci, NULL, &ctx->iblBrdfLutSampler);

    // Inject directly into lighting sets (set = 3) for ALL frames!
    VkDescriptorImageInfo iInfos[4] = {
        { .sampler = ctx->iblIrradianceSampler, .imageView = ctx->iblIrradianceView, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { .sampler = ctx->iblPrefilterSampler,  .imageView = ctx->iblPrefilterView,  .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { .sampler = ctx->iblBrdfLutSampler,    .imageView = ctx->iblBrdfLutView,    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { .sampler = ctx->iblPrefilterSampler,  .imageView = iblSkyboxView,          .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }
    };
    for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; f++) {
        VkWriteDescriptorSet writes[4];
        for(int i=0; i<4; i++) {
            writes[i] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ctx->lightingSets[f],
                .dstBinding = i + 1, .dstArrayElement = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1, .pImageInfo = &iInfos[i] };
        }
        vkUpdateDescriptorSets(ctx->device, 4, writes, 0, NULL);
    }

    ctx->iblLoaded = true;
    printf("IBL dynamically baked and loaded successfully from: %s\n", hdr_path);
    return true;
}

void destroyIBL(VulkanContext* ctx)
{
    if (iblSkyboxView)   { vkDestroyImageView(ctx->device, iblSkyboxView,   NULL); iblSkyboxView   = VK_NULL_HANDLE; }
    if (iblSkyboxImage)  { vkDestroyImage    (ctx->device, iblSkyboxImage,  NULL); iblSkyboxImage  = VK_NULL_HANDLE; }
    if (iblSkyboxMemory) { vkFreeMemory      (ctx->device, iblSkyboxMemory, NULL); iblSkyboxMemory = VK_NULL_HANDLE; }
    if (ctx->iblIrradianceView)   { vkDestroyImageView(ctx->device, ctx->iblIrradianceView,   NULL); ctx->iblIrradianceView   = VK_NULL_HANDLE; }
    if (ctx->iblIrradianceImage)  { vkDestroyImage    (ctx->device, ctx->iblIrradianceImage,  NULL); ctx->iblIrradianceImage  = VK_NULL_HANDLE; }
    if (ctx->iblIrradianceMemory) { vkFreeMemory      (ctx->device, ctx->iblIrradianceMemory, NULL); ctx->iblIrradianceMemory = VK_NULL_HANDLE; }
    if (ctx->iblIrradianceSampler){ vkDestroySampler  (ctx->device, ctx->iblIrradianceSampler,NULL); ctx->iblIrradianceSampler= VK_NULL_HANDLE; }

    if (ctx->iblPrefilterView)    { vkDestroyImageView(ctx->device, ctx->iblPrefilterView,    NULL); ctx->iblPrefilterView    = VK_NULL_HANDLE; }
    if (ctx->iblPrefilterImage)   { vkDestroyImage    (ctx->device, ctx->iblPrefilterImage,   NULL); ctx->iblPrefilterImage   = VK_NULL_HANDLE; }
    if (ctx->iblPrefilterMemory)  { vkFreeMemory      (ctx->device, ctx->iblPrefilterMemory,  NULL); ctx->iblPrefilterMemory  = VK_NULL_HANDLE; }
    if (ctx->iblPrefilterSampler) { vkDestroySampler  (ctx->device, ctx->iblPrefilterSampler, NULL); ctx->iblPrefilterSampler = VK_NULL_HANDLE; }

    if (ctx->iblBrdfLutView)      { vkDestroyImageView(ctx->device, ctx->iblBrdfLutView,      NULL); ctx->iblBrdfLutView      = VK_NULL_HANDLE; }
    if (ctx->iblBrdfLutImage)     { vkDestroyImage    (ctx->device, ctx->iblBrdfLutImage,     NULL); ctx->iblBrdfLutImage     = VK_NULL_HANDLE; }
    if (ctx->iblBrdfLutMemory)    { vkFreeMemory      (ctx->device, ctx->iblBrdfLutMemory,    NULL); ctx->iblBrdfLutMemory    = VK_NULL_HANDLE; }
    if (ctx->iblBrdfLutSampler)   { vkDestroySampler  (ctx->device, ctx->iblBrdfLutSampler,   NULL); ctx->iblBrdfLutSampler   = VK_NULL_HANDLE; }

    ctx->iblLoaded = false;
}

void clear_background(Color color)  { context.clearColor = color; }
void toggle_ambient_occlusion(void) { ambientOcclusionEnabled = !ambientOcclusionEnabled; }
void toggle_skybox(void) { skyboxEnabled = !skyboxEnabled; }
void toggle_ibl_lighting(void) { iblLightingEnabled = !iblLightingEnabled; }

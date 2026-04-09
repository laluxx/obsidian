// vulkan_setup.c — optimized rewrite
// Key changes vs original:
//   • All graphics pipelines created in ONE vkCreateGraphicsPipelines batch call
//   • VkPipelineCache shared across all pipelines (serialisable to disk if desired)
//   • 2D-color, 2D-textured and 2D-SDF share one pipelineLayout (SDF2D reuses
//     descriptorSetLayout2D + same push-constant mat4)
//   • 3D-solid, 3D-textured and 3D-SDF share one pipelineLayout (two descriptor
//     sets: UBO at set=0, sampler at set=1)
//   • All per-pipeline boilerplate extracted into createPipelineState() helpers
//   • VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT used (re-recorded every frame)
//   • Subpass dependency now covers both color AND depth stages
//   • createLogicalDevice no longer hard-codes enabledExtensionCount = 1
//   • Dead blend-pipeline references removed from the creation path

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

#include "frag.frag.spv.h"
#include "vert.vert.spv.h"
#include "2D.vert.spv.h"
#include "2D.frag.spv.h"
#include "texture.frag.spv.h"
#include "texture3D.frag.spv.h"
#include "sdf.frag.spv.h"
#include "sdf3D.frag.spv.h"

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

VkDescriptorPool    descriptorPool;
VkDescriptorSet     descriptorSet;
VkBuffer            uniformBuffer;
VkDeviceMemory      uniformBufferMemory;

static VkPipelineCache pipelineCache = VK_NULL_HANDLE;

/// Helpers

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
                               VkVertexInputAttributeDescription       attrs[4],
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

    *vi = (VkPipelineVertexInputStateCreateInfo){
        .sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount   = 1,
        .pVertexBindingDescriptions      = bind,
        .vertexAttributeDescriptionCount = 4,
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
                               VkVertexInputAttributeDescription       attrs[3])
{
    *bind = (VkVertexInputBindingDescription){
        .binding   = 0,
        .stride    = sizeof(Vertex2D),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX
    };
    attrs[0] = (VkVertexInputAttributeDescription){.location=0, .binding=0, .format=VK_FORMAT_R32G32_SFLOAT,       .offset=offsetof(Vertex2D, pos)};
    attrs[1] = (VkVertexInputAttributeDescription){.location=1, .binding=0, .format=VK_FORMAT_R32G32B32A32_SFLOAT, .offset=offsetof(Vertex2D, color)};
    attrs[2] = (VkVertexInputAttributeDescription){.location=2, .binding=0, .format=VK_FORMAT_R32G32_SFLOAT,       .offset=offsetof(Vertex2D, texCoord)};

    *vi = (VkPipelineVertexInputStateCreateInfo){
        .sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount   = 1,
        .pVertexBindingDescriptions      = bind,
        .vertexAttributeDescriptionCount = 3,
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
        .apiVersion         = VK_API_VERSION_1_0
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

void pickPhysicalDevice(VulkanContext* ctx)
{
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(ctx->instance, &count, NULL);
    if (count == 0) { fprintf(stderr, "No Vulkan-capable GPU found\n"); exit(EXIT_FAILURE); }

    VkPhysicalDevice* devs = malloc(count * sizeof(*devs));
    vkEnumeratePhysicalDevices(ctx->instance, &count, devs);
    ctx->physicalDevice = devs[0];   /* pick first — extend with scoring if needed */
    free(devs);
}

void createLogicalDevice(VulkanContext* ctx)
{
    float pri = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0,
        .queueCount       = 1,
        .pQueuePriorities = &pri
    };

    /* Only request extensions the engine actually uses. Ray-tracing extensions
       are listed here for future use; remove any your driver doesn't expose. */
    static const char* exts[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };
    static const uint32_t extCount = sizeof(exts) / sizeof(exts[0]);

    VkPhysicalDeviceFeatures features = { .wideLines = VK_TRUE };

    VkDeviceCreateInfo ci = {
        .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount    = 1,
        .pQueueCreateInfos       = &qci,
        .enabledExtensionCount   = extCount,   /* FIX: was hard-coded to 1 */
        .ppEnabledExtensionNames = exts,
        .pEnabledFeatures        = &features,
        .enabledLayerCount       = VALIDATION_LAYERS_COUNT,
        .ppEnabledLayerNames     = validationLayers
    };
    if (vkCreateDevice(ctx->physicalDevice, &ci, NULL, &ctx->device) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create logical device\n");
        exit(EXIT_FAILURE);
    }
    vkGetDeviceQueue(ctx->device, 0, 0, &ctx->graphicsQueue);
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
    /* set=0 : UBO (used by all 3D pipelines) */
    VkDescriptorSetLayoutBinding b = {
        .binding         = 0,
        .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = 1,
        .stageFlags      = VK_SHADER_STAGE_VERTEX_BIT
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
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 };
    VkDescriptorPoolCreateInfo ci = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .poolSizeCount = 1,
        .pPoolSizes    = &ps,
        .maxSets       = 1
    };
    if (vkCreateDescriptorPool(ctx->device, &ci, NULL, &descriptorPool) != VK_SUCCESS) {
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
    VkDescriptorSetAllocateInfo ai = {
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = descriptorPool,
        .descriptorSetCount = 1,
        .pSetLayouts        = &ctx->descriptorSetLayout
    };
    vkAllocateDescriptorSets(ctx->device, &ai, &descriptorSet);

    VkDescriptorBufferInfo bi = { uniformBuffer, 0, sizeof(UniformBufferObject) };
    VkWriteDescriptorSet   w  = {
        .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet          = descriptorSet,
        .dstBinding      = 0,
        .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = 1,
        .pBufferInfo     = &bi
    };
    vkUpdateDescriptorSets(ctx->device, 1, &w, 0, NULL);
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
    VkPushConstantRange mat4Range = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset     = 0,
        .size       = sizeof(mat4)
    };
    VkPushConstantRange pcRange = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset     = 0,
        .size       = sizeof(PushConstants)
    };
    VkDescriptorSetLayout tex3DSets[2] = {
        ctx->descriptorSetLayout,
        ctx->descriptorSetLayout2D
    };

    /* 2D no-texture */
    vkCreatePipelineLayout(ctx->device,
        &(VkPipelineLayoutCreateInfo){
            .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges    = &mat4Range
        }, NULL, &ctx->pipelineLayout2D);

    /* 2D textured / SDF (same layout — both need sampler at set=0 and mat4 push) */
    vkCreatePipelineLayout(ctx->device,
        &(VkPipelineLayoutCreateInfo){
            .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount         = 1,
            .pSetLayouts            = &ctx->descriptorSetLayout2D,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges    = &mat4Range
        }, NULL, &ctx->pipelineLayoutTextured2D);
    /* SDF2D reuses pipelineLayoutTextured2D — no separate field needed */
    ctx->pipelineLayoutSDF2D = ctx->pipelineLayoutTextured2D;

    /* 3D solid / line (UBO only) */
    vkCreatePipelineLayout(ctx->device,
        &(VkPipelineLayoutCreateInfo){
            .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount         = 1,
            .pSetLayouts            = &ctx->descriptorSetLayout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges    = &pcRange
        }, NULL, &ctx->pipelineLayout);
    ctx->pipelineLayoutLine = ctx->pipelineLayout;  /* same layout */

    /* 3D textured / SDF3D (UBO + sampler) */
    vkCreatePipelineLayout(ctx->device,
        &(VkPipelineLayoutCreateInfo){
            .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount         = 2,
            .pSetLayouts            = tex3DSets,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges    = &pcRange
        }, NULL, &ctx->pipelineLayoutTextured3D);
    ctx->pipelineLayoutSDF3D = ctx->pipelineLayoutTextured3D;  /* same layout */
}

/// GRAPHICS PIPELINES
// all created in ONE batch call

void createGraphicsPipelines(VulkanContext* ctx)
{
    /* ── pipeline cache ─────────────────────────────────────────────── */
    VkPipelineCacheCreateInfo cacheCI = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO
    };
    vkCreatePipelineCache(ctx->device, &cacheCI, NULL, &pipelineCache);

    /* ── shared pipeline state ──────────────────────────────────────── */
    VkPipelineRasterizationStateCreateInfo rast1  = makeRasterizer(1.0f);
    VkPipelineRasterizationStateCreateInfo rastLW = makeRasterizer(2.0f);

    VkPipelineColorBlendStateCreateInfo   blend     = makeColorBlend(&kBlendAlpha);
    VkPipelineDepthStencilStateCreateInfo depth3D   = makeDepth(true,  true);
    VkPipelineDepthStencilStateCreateInfo depth2D   = makeDepth(false, false);

    /* ── vertex input structs (per family, reused across pipelines) ─── */
    VkVertexInputBindingDescription   bind3D;
    VkVertexInputAttributeDescription attr3D[4];
    VkPipelineVertexInputStateCreateInfo  vi3D;
    VkPipelineInputAssemblyStateCreateInfo ia3D, iaLine;
    fill3DVertexInput(&vi3D, &ia3D, &bind3D, attr3D, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);

    VkVertexInputBindingDescription   bind2D;
    VkVertexInputAttributeDescription attr2D[3];
    VkPipelineVertexInputStateCreateInfo  vi2D;
    VkPipelineInputAssemblyStateCreateInfo ia2D;
    fill2DVertexInput(&vi2D, &ia2D, &bind2D, attr2D);

    /* line topology variant — reuses bind3D / attr3D already filled */
    iaLine = (VkPipelineInputAssemblyStateCreateInfo){
        .sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST
    };

    /* ── shader modules ─────────────────────────────────────────────── */
    VkShaderModule vsMain   = createShaderModule(ctx->device, vert_vert_spv,     sizeof(vert_vert_spv));
    VkShaderModule vs2D     = createShaderModule(ctx->device, __2D_vert_spv,     sizeof(__2D_vert_spv));
    VkShaderModule fsColor  = createShaderModule(ctx->device, frag_frag_spv,     sizeof(frag_frag_spv));
    VkShaderModule fs2D     = createShaderModule(ctx->device, __2D_frag_spv,     sizeof(__2D_frag_spv));
    VkShaderModule fsTex2D  = createShaderModule(ctx->device, texture_frag_spv,  sizeof(texture_frag_spv));
    VkShaderModule fsTex3D  = createShaderModule(ctx->device, texture3D_frag_spv,sizeof(texture3D_frag_spv));
    VkShaderModule fsSDF2D  = createShaderModule(ctx->device, sdf_frag_spv,      sizeof(sdf_frag_spv));
    VkShaderModule fsSDF3D  = createShaderModule(ctx->device, sdf3D_frag_spv,    sizeof(sdf3D_frag_spv));

    /* ── shader stage arrays ────────────────────────────────────────── */
#define STAGE(stg, mod) \
    {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, stg, mod, "main", NULL}

    VkPipelineShaderStageCreateInfo ss3DColor[2]  = { STAGE(VK_SHADER_STAGE_VERTEX_BIT,   vsMain),
                                                       STAGE(VK_SHADER_STAGE_FRAGMENT_BIT, fsColor)  };
    VkPipelineShaderStageCreateInfo ss3DTex[2]    = { STAGE(VK_SHADER_STAGE_VERTEX_BIT,   vsMain),
                                                       STAGE(VK_SHADER_STAGE_FRAGMENT_BIT, fsTex3D)  };
    VkPipelineShaderStageCreateInfo ss3DSDF[2]    = { STAGE(VK_SHADER_STAGE_VERTEX_BIT,   vsMain),
                                                       STAGE(VK_SHADER_STAGE_FRAGMENT_BIT, fsSDF3D)  };
    VkPipelineShaderStageCreateInfo ss2DColor[2]  = { STAGE(VK_SHADER_STAGE_VERTEX_BIT,   vs2D),
                                                       STAGE(VK_SHADER_STAGE_FRAGMENT_BIT, fs2D)     };
    VkPipelineShaderStageCreateInfo ss2DTex[2]    = { STAGE(VK_SHADER_STAGE_VERTEX_BIT,   vs2D),
                                                       STAGE(VK_SHADER_STAGE_FRAGMENT_BIT, fsTex2D)  };
    VkPipelineShaderStageCreateInfo ss2DSDF[2]    = { STAGE(VK_SHADER_STAGE_VERTEX_BIT,   vs2D),
                                                       STAGE(VK_SHADER_STAGE_FRAGMENT_BIT, fsSDF2D)  };
#undef STAGE

    /* ── 7 pipeline create-infos ────────────────────────────────────── */
    /*
       Index  Pipeline
       0      3D solid (color)
       1      3D textured
       2      3D SDF
       3      3D line
       4      2D color
       5      2D textured
       6      2D SDF
    */
    VkGraphicsPipelineCreateInfo pci[7] = {
        /* 0: 3D solid */
        {
            .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount          = 2, .pStages             = ss3DColor,
            .pVertexInputState   = &vi3D,  .pInputAssemblyState = &ia3D,
            .pViewportState      = &kViewportState,
            .pRasterizationState = &rast1, .pMultisampleState   = &kMultisampling,
            .pColorBlendState    = &blend, .pDepthStencilState  = &depth3D,
            .pDynamicState       = &kDynamicState,
            .layout              = ctx->pipelineLayout,
            .renderPass          = ctx->renderPass
        },
        /* 1: 3D textured */
        {
            .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount          = 2, .pStages             = ss3DTex,
            .pVertexInputState   = &vi3D,  .pInputAssemblyState = &ia3D,
            .pViewportState      = &kViewportState,
            .pRasterizationState = &rast1, .pMultisampleState   = &kMultisampling,
            .pColorBlendState    = &blend, .pDepthStencilState  = &depth3D,
            .pDynamicState       = &kDynamicState,
            .layout              = ctx->pipelineLayoutTextured3D,
            .renderPass          = ctx->renderPass
        },
        /* 2: 3D SDF */
        {
            .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount          = 2, .pStages             = ss3DSDF,
            .pVertexInputState   = &vi3D,  .pInputAssemblyState = &ia3D,
            .pViewportState      = &kViewportState,
            .pRasterizationState = &rast1, .pMultisampleState   = &kMultisampling,
            .pColorBlendState    = &blend, .pDepthStencilState  = &depth3D,
            .pDynamicState       = &kDynamicState,
            .layout              = ctx->pipelineLayoutSDF3D,
            .renderPass          = ctx->renderPass
        },
        /* 3: 3D line */
        {
            .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount          = 2, .pStages             = ss3DColor,
            .pVertexInputState   = &vi3D,  .pInputAssemblyState = &iaLine,
            .pViewportState      = &kViewportState,
            .pRasterizationState = &rastLW,.pMultisampleState   = &kMultisampling,
            .pColorBlendState    = &blend, .pDepthStencilState  = &depth3D,
            .pDynamicState       = &kDynamicState,
            .layout              = ctx->pipelineLayoutLine,
            .renderPass          = ctx->renderPass
        },
        /* 4: 2D color */
        {
            .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount          = 2, .pStages             = ss2DColor,
            .pVertexInputState   = &vi2D,  .pInputAssemblyState = &ia2D,
            .pViewportState      = &kViewportState,
            .pRasterizationState = &rast1, .pMultisampleState   = &kMultisampling,
            .pColorBlendState    = &blend, .pDepthStencilState  = &depth2D,
            .pDynamicState       = &kDynamicState,
            .layout              = ctx->pipelineLayout2D,
            .renderPass          = ctx->renderPass
        },
        /* 5: 2D textured */
        {
            .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount          = 2, .pStages             = ss2DTex,
            .pVertexInputState   = &vi2D,  .pInputAssemblyState = &ia2D,
            .pViewportState      = &kViewportState,
            .pRasterizationState = &rast1, .pMultisampleState   = &kMultisampling,
            .pColorBlendState    = &blend, .pDepthStencilState  = &depth2D,
            .pDynamicState       = &kDynamicState,
            .layout              = ctx->pipelineLayoutTextured2D,
            .renderPass          = ctx->renderPass
        },
        /* 6: 2D SDF */
        {
            .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount          = 2, .pStages             = ss2DSDF,
            .pVertexInputState   = &vi2D,  .pInputAssemblyState = &ia2D,
            .pViewportState      = &kViewportState,
            .pRasterizationState = &rast1, .pMultisampleState   = &kMultisampling,
            .pColorBlendState    = &blend, .pDepthStencilState  = &depth2D,
            .pDynamicState       = &kDynamicState,
            .layout              = ctx->pipelineLayoutSDF2D,
            .renderPass          = ctx->renderPass
        }
    };

    VkPipeline pipelines[7];
    if (vkCreateGraphicsPipelines(ctx->device, pipelineCache, 7, pci, NULL, pipelines) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create graphics pipelines\n");
        exit(EXIT_FAILURE);
    }

    ctx->graphicsPipeline          = pipelines[0];
    ctx->graphicsPipelineTextured3D= pipelines[1];
    ctx->graphicsPipelineSDF3D     = pipelines[2];
    ctx->graphicsPipelineLine      = pipelines[3];
    ctx->graphicsPipeline2D        = pipelines[4];
    ctx->graphicsPipelineTextured2D= pipelines[5];
    ctx->graphicsPipelineSDF2D     = pipelines[6];

    /* destroy shader modules — no longer needed after compilation */
    vkDestroyShaderModule(ctx->device, vsMain,  NULL);
    vkDestroyShaderModule(ctx->device, vs2D,    NULL);
    vkDestroyShaderModule(ctx->device, fsColor, NULL);
    vkDestroyShaderModule(ctx->device, fs2D,    NULL);
    vkDestroyShaderModule(ctx->device, fsTex2D, NULL);
    vkDestroyShaderModule(ctx->device, fsTex3D, NULL);
    vkDestroyShaderModule(ctx->device, fsSDF2D, NULL);
    vkDestroyShaderModule(ctx->device, fsSDF3D, NULL);
}

/* Public wrappers kept for call-site compatibility */
void createGraphicsPipeline(VulkanContext* ctx)          { createGraphicsPipelines(ctx); }
void create2DGraphicsPipeline(VulkanContext* ctx)        { /* handled by createGraphicsPipelines */ }
void createTextured2DGraphicsPipeline(VulkanContext* ctx){ /* handled by createGraphicsPipelines */ }
void create3DTexturedGraphicsPipeline(VulkanContext* ctx){ /* handled by createGraphicsPipelines */ }
void createLineGraphicsPipeline(VulkanContext* ctx)      { /* handled by createGraphicsPipelines */ }
void createSDF2DGraphicsPipeline(VulkanContext* ctx)     { /* handled by createGraphicsPipelines */ }
void createSDF3DGraphicsPipeline(VulkanContext* ctx)     { /* handled by createGraphicsPipelines */ }

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
        .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = 0
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

void createUniformBuffer(VulkanContext* ctx)
{
    VkBufferCreateInfo bi = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = sizeof(UniformBufferObject),
        .usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    vkCreateBuffer(ctx->device, &bi, NULL, &uniformBuffer);

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(ctx->device, uniformBuffer, &req);

    VkMemoryAllocateInfo ai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = req.size,
        .memoryTypeIndex = findMemoryType(ctx->physicalDevice, req.memoryTypeBits,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    };
    vkAllocateMemory(ctx->device, &ai, NULL, &uniformBufferMemory);
    vkBindBufferMemory(ctx->device, uniformBuffer, uniformBufferMemory, 0);
}

void updateUniformBuffer(VulkanContext* ctx)
{
    void* data;
    if (vkMapMemory(ctx->device, uniformBufferMemory, 0, sizeof(UniformBufferObject), 0, &data) != VK_SUCCESS) {
        fprintf(stderr, "Failed to map uniform buffer\n"); return;
    }
    UniformBufferObject ubo;
    glm_mat4_mul(camera.projection_matrix, camera.view_matrix, ubo.vp);
    memcpy(data, &ubo, sizeof(ubo));
    vkUnmapMemory(ctx->device, uniformBufferMemory);
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

    /* ── 3D solid ── */
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ctx->graphicsPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            ctx->pipelineLayout, 0, 1, &descriptorSet, 0, NULL);
    meshes_draw(cmd, &scene.meshes);
    renderer_draw(cmd);

    /* ── 3D textured ── */
    renderer_draw_textured3D(cmd);

    /* ── 3D lines ── */
    if (ctx->graphicsPipelineLine && lineVertexCount > 0) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ctx->graphicsPipelineLine);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                ctx->pipelineLayoutLine, 0, 1, &descriptorSet, 0, NULL);
        line_renderer_draw(cmd);
    }

    /* ── 2D ── */
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

    VkSwapchainCreateInfoKHR ci = {
        .sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface          = ctx->surface,
        .minImageCount    = imgCount,
        .imageFormat      = ctx->swapChainImageFormat,
        .imageColorSpace  = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent      = { (uint32_t)w, (uint32_t)h },
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
    ctx->swapChainExtent = (VkExtent2D){ (uint32_t)w, (uint32_t)h };

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
    line_renderer_shutdown();
    meshes_destroy(ctx->device, &scene.meshes);
    texture_pool_cleanup(ctx);

    /* descriptor resources */
    if (ctx->descriptorSetLayout2D) { vkDestroyDescriptorSetLayout(ctx->device, ctx->descriptorSetLayout2D, NULL); ctx->descriptorSetLayout2D = VK_NULL_HANDLE; }
    if (ctx->descriptorPool2D)      { vkDestroyDescriptorPool(ctx->device, ctx->descriptorPool2D, NULL);            ctx->descriptorPool2D      = VK_NULL_HANDLE; }

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
    DESTROY_PIPELINE(graphicsPipeline)
    DESTROY_PIPELINE(graphicsPipeline2D)
    DESTROY_PIPELINE(graphicsPipelineTextured2D)
    DESTROY_PIPELINE(graphicsPipelineTextured3D)
    DESTROY_PIPELINE(graphicsPipelineLine)
    DESTROY_PIPELINE(graphicsPipelineSDF2D)
    DESTROY_PIPELINE(graphicsPipelineSDF3D)

    /* SDF2D/SDF3D share layouts with textured — avoid double-free */
    ctx->pipelineLayoutSDF2D = VK_NULL_HANDLE;
    ctx->pipelineLayoutSDF3D = VK_NULL_HANDLE;
    ctx->pipelineLayoutLine  = VK_NULL_HANDLE;

    DESTROY_LAYOUT(pipelineLayout)
    DESTROY_LAYOUT(pipelineLayout2D)
    DESTROY_LAYOUT(pipelineLayoutTextured2D)
    DESTROY_LAYOUT(pipelineLayoutTextured3D)
#undef DESTROY_PIPELINE
#undef DESTROY_LAYOUT

    if (pipelineCache != VK_NULL_HANDLE) { vkDestroyPipelineCache(ctx->device, pipelineCache,             NULL); pipelineCache             = VK_NULL_HANDLE; }

    /* 2D vertex buffer */
    if (ctx->vertexBuffer2D)       { vkDestroyBuffer             (ctx->device, ctx->vertexBuffer2D,       NULL); ctx->vertexBuffer2D       = VK_NULL_HANDLE; }
    if (ctx->vertexBufferMemory2D) { vkFreeMemory                (ctx->device, ctx->vertexBufferMemory2D, NULL); ctx->vertexBufferMemory2D = VK_NULL_HANDLE; }

    if (ctx->renderPass)           { vkDestroyRenderPass         (ctx->device, ctx->renderPass,           NULL); ctx->renderPass           = VK_NULL_HANDLE; }

    /* uniform buffer + descriptor pool */
    if (uniformBuffer)             { vkDestroyBuffer             (ctx->device, uniformBuffer,             NULL); uniformBuffer             = VK_NULL_HANDLE; }
    if (uniformBufferMemory)       { vkFreeMemory                (ctx->device, uniformBufferMemory,       NULL); uniformBufferMemory       = VK_NULL_HANDLE; }
    if (descriptorPool)            { vkDestroyDescriptorPool     (ctx->device, descriptorPool,            NULL); descriptorPool            = VK_NULL_HANDLE; }
    if (ctx->descriptorSetLayout)  { vkDestroyDescriptorSetLayout(ctx->device, ctx->descriptorSetLayout,  NULL); ctx->descriptorSetLayout  = VK_NULL_HANDLE; }

    if (ctx->device)               { vkDestroyDevice             (ctx->device,                            NULL); ctx->device               = VK_NULL_HANDLE; }
    if (ctx->surface)              { vkDestroySurfaceKHR         (ctx->instance, ctx->surface,            NULL); ctx->surface              = VK_NULL_HANDLE; }
    if (ctx->instance)             { vkDestroyInstance           (ctx->instance,                          NULL); ctx->instance             = VK_NULL_HANDLE; }
    if (ctx->window)               { glfwDestroyWindow           (ctx->window                                 ); ctx->window               = NULL; }
    glfwTerminate();
}


/// Misc

void clear_background(Color color) { context.clearColor = color; }
void toggle_ambient_occlusion(void) { ambientOcclusionEnabled = !ambientOcclusionEnabled; }

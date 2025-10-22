#include "obj.h"
#include <vulkan/vulkan_core.h>
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "frag.frag.spv.h"
#include "vert.vert.spv.h"
#include "2D.vert.spv.h"
#include "2D.frag.spv.h"
#include "texture.frag.spv.h"


#include <cglm/cglm.h>
#include "camera.h"
#include "renderer.h"
#include "scene.h"

#include "context.h"
#include "common.h"

#include <time.h>  


#define WIDTH 800
#define HEIGHT 600

#define MAX_FRAMES_IN_FLIGHT 2
#define ENABLE_VALIDATION_LAYERS 1

#if ENABLE_VALIDATION_LAYERS
#define VALIDATION_LAYERS_COUNT 1
const char* validationLayers[VALIDATION_LAYERS_COUNT] = {
    "VK_LAYER_KHRONOS_validation"
};
#else
#define VALIDATION_LAYERS_COUNT 0
const char** validationLayers = NULL;
#endif


VkDescriptorPool descriptorPool;
VkDescriptorSet descriptorSet;

VkBuffer uniformBuffer;
VkDeviceMemory uniformBufferMemory;

typedef struct {
    vec3 pos;
    vec4 color;
    float scale;
} Block;


#define WORLD_WIDTH  10
#define WORLD_HEIGHT 10
#define WORLD_DEPTH  10

Block blocks[WORLD_WIDTH][WORLD_HEIGHT][WORLD_DEPTH];

/* Texture2D texture; */
/* Texture2D texture2; */


typedef struct {
    mat4 vp;
} UniformBufferObject;

void create2DDescriptorSetLayout(VulkanContext* context) {
    VkDescriptorSetLayoutBinding samplerLayoutBinding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .pImmutableSamplers = NULL
    };
    
    VkDescriptorSetLayoutCreateInfo layoutInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &samplerLayoutBinding
    };
    
    if (vkCreateDescriptorSetLayout(context->device, &layoutInfo, NULL, &context->descriptorSetLayout2D) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create 2D descriptor set layout\n");
        exit(EXIT_FAILURE);
    }
}


void create2DDescriptorPool(VulkanContext* context) {
    VkDescriptorPoolSize poolSize = {
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = MAX_TEXTURES  // Allow for maximum textures
    };

    VkDescriptorPoolCreateInfo poolInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .poolSizeCount = 1,
        .pPoolSizes = &poolSize,
        .maxSets = MAX_TEXTURES  // Allow for maximum sets
    };

    if (vkCreateDescriptorPool(context->device, &poolInfo, NULL, &context->descriptorPool2D) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create 2D descriptor pool\n");
        exit(EXIT_FAILURE);
    }
}

void createDescriptorSet(VulkanContext* context) {
    VkDescriptorSetAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptorPool,
        .descriptorSetCount = 1,
        .pSetLayouts = &context->descriptorSetLayout
    };

    vkAllocateDescriptorSets(context->device, &allocInfo, &descriptorSet);

    VkDescriptorBufferInfo bufferInfo = {
        .buffer = uniformBuffer,
        .offset = 0,
        .range = sizeof(UniformBufferObject)
    };

    VkWriteDescriptorSet descriptorWrite = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = descriptorSet,
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = 1,
        .pBufferInfo = &bufferInfo
    };

    vkUpdateDescriptorSets(context->device, 1, &descriptorWrite, 0, NULL);
}

void createDescriptorPool(VulkanContext* context) {
    VkDescriptorPoolSize poolSize = {
        .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = 1
    };

    VkDescriptorPoolCreateInfo poolInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .poolSizeCount = 1,
        .pPoolSizes = &poolSize,
        .maxSets = 1
    };

    if (vkCreateDescriptorPool(context->device, &poolInfo, NULL, &descriptorPool) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create descriptor pool\n");
        exit(EXIT_FAILURE);
    }
}

bool checkExtensionSupport(const char** requiredExtensions, uint32_t requiredCount) {
    uint32_t extensionCount = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &extensionCount, NULL);
    VkExtensionProperties* availableExtensions = malloc(extensionCount * sizeof(VkExtensionProperties));
    vkEnumerateInstanceExtensionProperties(NULL, &extensionCount, availableExtensions);

    for (uint32_t i = 0; i < requiredCount; i++) {
        bool found = false;
        for (uint32_t j = 0; j < extensionCount; j++) {
            if (strcmp(requiredExtensions[i], availableExtensions[j].extensionName) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            free(availableExtensions);
            return false;
        }
    }

    free(availableExtensions);
    return true;
}

bool checkValidationLayerSupport() {
    uint32_t layerCount;
    vkEnumerateInstanceLayerProperties(&layerCount, NULL);
    VkLayerProperties* availableLayers = malloc(layerCount * sizeof(VkLayerProperties));
    vkEnumerateInstanceLayerProperties(&layerCount, availableLayers);

    for (uint32_t i = 0; i < VALIDATION_LAYERS_COUNT; i++) {
        bool found = false;
        for (uint32_t j = 0; j < layerCount; j++) {
            if (strcmp(validationLayers[i], availableLayers[j].layerName) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            free(availableLayers);
            return false;
        }
    }

    free(availableLayers);
    return true;
}

void initWindow(VulkanContext* context) {
    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    context->window = glfwCreateWindow(WIDTH, HEIGHT, "Revox", NULL, NULL);
}


void createInstance(VulkanContext* context) {
#if ENABLE_VALIDATION_LAYERS
    if (!checkValidationLayerSupport()) {
        fprintf(stderr, "Validation layers requested but not available!\n");
        exit(EXIT_FAILURE);
    }
#endif

    VkApplicationInfo appInfo = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Revox",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "No Engine",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_0
    };

    uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

    VkInstanceCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &appInfo,
        .enabledExtensionCount = glfwExtensionCount,
        .ppEnabledExtensionNames = glfwExtensions,
        .enabledLayerCount = VALIDATION_LAYERS_COUNT,
        .ppEnabledLayerNames = validationLayers
    };

    if (vkCreateInstance(&createInfo, NULL, &context->instance) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create Vulkan instance\n");
        exit(EXIT_FAILURE);
    }
}

void pickPhysicalDevice(VulkanContext* context) {
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(context->instance, &deviceCount, NULL);

    if (deviceCount == 0) {
        fprintf(stderr, "Failed to find GPUs with Vulkan support\n");
        exit(EXIT_FAILURE);
    }

    VkPhysicalDevice* devices = malloc(deviceCount * sizeof(VkPhysicalDevice));
    vkEnumeratePhysicalDevices(context->instance, &deviceCount, devices);

    // Just pick the first GPU for simplicity
    context->physicalDevice = devices[0];
    free(devices);
}

void createLogicalDevice(VulkanContext* context) {
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0,
        .queueCount = 1,
        .pQueuePriorities = &queuePriority
    };

    /* const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME}; */

    const char* deviceExtensions[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
        VK_KHR_SPIRV_1_4_EXTENSION_NAME,
        VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME
    };


    VkPhysicalDeviceFeatures deviceFeatures = {0};

    VkDeviceCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        /* .queueCreateInfoCount = 8, */
        .pQueueCreateInfos = &queueCreateInfo,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = deviceExtensions,
        .pEnabledFeatures = &deviceFeatures,
        .enabledLayerCount = VALIDATION_LAYERS_COUNT,
        .ppEnabledLayerNames = validationLayers
    };

    if (vkCreateDevice(context->physicalDevice, &createInfo, NULL, &context->device) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create logical device\n");
        exit(EXIT_FAILURE);
    }

    vkGetDeviceQueue(context->device, 0, 0, &context->graphicsQueue);
}

void createSwapChain(VulkanContext* context) {
    VkSurfaceCapabilitiesKHR capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(context->physicalDevice, context->surface, &capabilities);

    VkSurfaceFormatKHR surfaceFormat = {
        .format = VK_FORMAT_B8G8R8A8_SRGB,
        .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR
    };

    /* VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR; // Supported by all implementations, it enforces VSync */
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR; // No VSync, low latency, but possible tearing
    /* VkPresentModeKHR presentMode = VK_PRESENT_MODE_MAILBOX_KHR; // Triple buffering, lower latency than FIFO, no tearing, but higher GPU load. */
    /* VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_RELAXED_KHR; */

    VkExtent2D extent = capabilities.currentExtent.width != UINT32_MAX ? 
        capabilities.currentExtent : 
        (VkExtent2D){WIDTH, HEIGHT};

    // Ensure we stay within bounds
    uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount) {
        imageCount = capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR createInfo = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = context->surface,
        .minImageCount = imageCount,
        .imageFormat = surfaceFormat.format,
        .imageColorSpace = surfaceFormat.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = capabilities.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = presentMode,
        .clipped = VK_TRUE,
        .oldSwapchain = VK_NULL_HANDLE
    };

    if (vkCreateSwapchainKHR(context->device, &createInfo, NULL, &context->swapChain) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create swap chain\n");
        exit(EXIT_FAILURE);
    }

    vkGetSwapchainImagesKHR(context->device, context->swapChain, &context->swapChainImageCount, NULL);
    context->swapChainImages = malloc(context->swapChainImageCount * sizeof(VkImage));
    vkGetSwapchainImagesKHR(context->device, context->swapChain, &context->swapChainImageCount, context->swapChainImages);

    context->swapChainImageFormat = surfaceFormat.format;
    context->swapChainExtent = extent;
}

void createImageViews(VulkanContext* context) {
    context->swapChainImageViews = malloc(context->swapChainImageCount * sizeof(VkImageView));

    for (uint32_t i = 0; i < context->swapChainImageCount; i++) {
        VkImageViewCreateInfo createInfo = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = context->swapChainImages[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = context->swapChainImageFormat,
            .components = {
                .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                .a = VK_COMPONENT_SWIZZLE_IDENTITY
            },
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1
            }
        };

        if (vkCreateImageView(context->device, &createInfo, NULL, &context->swapChainImageViews[i]) != VK_SUCCESS) {
            fprintf(stderr, "Failed to create image views\n");
            exit(EXIT_FAILURE);
        }
    }
}

void createRenderPass(VulkanContext* context) {
    VkAttachmentDescription colorAttachment = {
        .format = context->swapChainImageFormat,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
    };

    VkAttachmentDescription depthAttachment = {
        .format = context->depthFormat,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    };


    VkAttachmentReference colorAttachmentRef = {
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
    };

    VkAttachmentReference depthAttachmentRef = {
        .attachment = 1,
        .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    };

    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &colorAttachmentRef,
        .pDepthStencilAttachment = &depthAttachmentRef,
    };


    // Add subpass dependency for layout transitions
    VkSubpassDependency dependency = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
    };

    VkRenderPassCreateInfo renderPassInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 2,
        /* .pAttachments = &colorAttachment, */
        .pAttachments = (VkAttachmentDescription[]){ colorAttachment, depthAttachment },
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency
    };

    if (vkCreateRenderPass(context->device, &renderPassInfo, NULL, &context->renderPass) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create render pass\n");
        exit(EXIT_FAILURE);
    }
}


void createTextured2DGraphicsPipeline(VulkanContext* context) {
    // Load shaders from header files
    VkShaderModule vertShaderModule2D;
    VkShaderModule fragShaderModuleTextured;
    
    // Create vertex shader module (same as regular 2D)
    {
        VkShaderModuleCreateInfo createInfo = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(__2D_vert_spv),
            .pCode = (const uint32_t*)__2D_vert_spv
        };

        if (vkCreateShaderModule(context->device, &createInfo, NULL, &vertShaderModule2D) != VK_SUCCESS) {
            fprintf(stderr, "Failed to create 2D vertex shader module\n");
            exit(EXIT_FAILURE);
        }
    }

    // Create fragment shader module for textured rendering
    {
        VkShaderModuleCreateInfo createInfo = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(texture_frag_spv),
            .pCode = (const uint32_t*)texture_frag_spv
        };

        if (vkCreateShaderModule(context->device, &createInfo, NULL, &fragShaderModuleTextured) != VK_SUCCESS) {
            fprintf(stderr, "Failed to create textured fragment shader module\n");
            exit(EXIT_FAILURE);
        }
    }

    VkPipelineShaderStageCreateInfo vertShaderStageInfo2D = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_VERTEX_BIT,
        .module = vertShaderModule2D,
        .pName = "main"
    };

    VkPipelineShaderStageCreateInfo fragShaderStageInfoTextured = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
        .module = fragShaderModuleTextured,
        .pName = "main"
    };

    VkPipelineShaderStageCreateInfo shaderStagesTextured[] = {vertShaderStageInfo2D, fragShaderStageInfoTextured};

    // 2D vertex input (same as regular 2D)
    VkVertexInputBindingDescription bindingDescription2D = {
        .binding = 0,
        .stride = sizeof(Vertex2D),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX
    };

    VkVertexInputAttributeDescription attributeDescriptions2D[3] = {
        {.binding = 0, .location = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(Vertex2D, pos)},
        {.binding = 0, .location = 1, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(Vertex2D, color)},
        {.binding = 0, .location = 2, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(Vertex2D, texCoord)}
    };
    
    VkPipelineVertexInputStateCreateInfo vertexInputInfo2D = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &bindingDescription2D,
        .vertexAttributeDescriptionCount = 3,
        .pVertexAttributeDescriptions = attributeDescriptions2D
    };
    
    VkPipelineInputAssemblyStateCreateInfo inputAssembly2D = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE
    };
    
    VkViewport viewport = {
        .x = 0.0f,
        .y = 0.0f,
        .width = (float)context->swapChainExtent.width,
        .height = (float)context->swapChainExtent.height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f
    };
    
    VkRect2D scissor = {
        .offset = {0, 0},
        .extent = context->swapChainExtent
    };
    
    VkPipelineViewportStateCreateInfo viewportState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .pViewports = &viewport,
        .scissorCount = 1,
        .pScissors = &scissor
    };
    
    VkPipelineRasterizationStateCreateInfo rasterizer = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .lineWidth = 1.0f,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable = VK_FALSE
    };
    
    VkPipelineMultisampleStateCreateInfo multisampling = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .sampleShadingEnable = VK_FALSE,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT
    };
    
    VkPipelineColorBlendAttachmentState colorBlendAttachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp = VK_BLEND_OP_ADD,
    };
    
    VkPipelineColorBlendStateCreateInfo colorBlending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .attachmentCount = 1,
        .pAttachments = &colorBlendAttachment
    };
    
    VkPipelineDepthStencilStateCreateInfo depthStencil2D = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_FALSE,
        .depthWriteEnable = VK_FALSE,
        .depthCompareOp = VK_COMPARE_OP_ALWAYS,
        .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable = VK_FALSE,
    };
    
    VkPushConstantRange pushConstantRange2D = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(mat4)
    };

    // Pipeline layout for textured 2D - uses descriptor sets for textures
    VkPipelineLayoutCreateInfo pipelineLayoutInfoTextured2D = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &context->descriptorSetLayout2D,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pushConstantRange2D,
    };
    
    if (vkCreatePipelineLayout(context->device, &pipelineLayoutInfoTextured2D, NULL, &context->pipelineLayoutTextured2D) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create textured 2D pipeline layout\n");
        exit(EXIT_FAILURE);
    }
    
    VkGraphicsPipelineCreateInfo pipelineInfoTextured2D = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = shaderStagesTextured,
        .pVertexInputState = &vertexInputInfo2D,
        .pInputAssemblyState = &inputAssembly2D,
        .pViewportState = &viewportState,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pColorBlendState = &colorBlending,
        .layout = context->pipelineLayoutTextured2D,
        .renderPass = context->renderPass,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .pDepthStencilState = &depthStencil2D,
    };
    
    if (vkCreateGraphicsPipelines(context->device, VK_NULL_HANDLE, 1, &pipelineInfoTextured2D, NULL, &context->graphicsPipelineTextured2D) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create textured 2D graphics pipeline\n");
        exit(EXIT_FAILURE);
    }
    
    vkDestroyShaderModule(context->device, fragShaderModuleTextured, NULL);
    vkDestroyShaderModule(context->device, vertShaderModule2D, NULL);
}

void create2DGraphicsPipeline(VulkanContext* context) {
    // Load 2D shaders from header files
    VkShaderModule vertShaderModule2D;
    VkShaderModule fragShaderModule2D;
    
    // Create vertex shader module from header
    {
        VkShaderModuleCreateInfo createInfo = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(__2D_vert_spv),
            .pCode = (const uint32_t*)__2D_vert_spv
        };

        if (vkCreateShaderModule(context->device, &createInfo, NULL, &vertShaderModule2D) != VK_SUCCESS) {
            fprintf(stderr, "Failed to create 2D vertex shader module\n");
            exit(EXIT_FAILURE);
        }
    }

    // Create fragment shader module from header  
    {
        VkShaderModuleCreateInfo createInfo = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(__2D_frag_spv),
            .pCode = (const uint32_t*)__2D_frag_spv
        };

        if (vkCreateShaderModule(context->device, &createInfo, NULL, &fragShaderModule2D) != VK_SUCCESS) {
            fprintf(stderr, "Failed to create 2D fragment shader module\n");
            exit(EXIT_FAILURE);
        }
    }

    VkPipelineShaderStageCreateInfo vertShaderStageInfo2D = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_VERTEX_BIT,
        .module = vertShaderModule2D,
        .pName = "main"
    };

    VkPipelineShaderStageCreateInfo fragShaderStageInfo2D = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
        .module = fragShaderModule2D,
        .pName = "main"
    };

    VkPipelineShaderStageCreateInfo shaderStages2D[] = {vertShaderStageInfo2D, fragShaderStageInfo2D};

    // 2D vertex input
    VkVertexInputBindingDescription bindingDescription2D = {
        .binding = 0,
        .stride = sizeof(Vertex2D),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX
    };

    VkVertexInputAttributeDescription attributeDescriptions2D[3] = {
        {.binding = 0, .location = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(Vertex2D, pos)},
        {.binding = 0, .location = 1, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(Vertex2D, color)},
        {.binding = 0, .location = 2, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(Vertex2D, texCoord)}
    };
    
    VkPipelineVertexInputStateCreateInfo vertexInputInfo2D = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &bindingDescription2D,
        .vertexAttributeDescriptionCount = 3,
        .pVertexAttributeDescriptions = attributeDescriptions2D
    };
    
    VkPipelineInputAssemblyStateCreateInfo inputAssembly2D = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE
    };
    
    // Viewport and scissor
    VkViewport viewport = {
        .x = 0.0f,
        .y = 0.0f,
        .width = (float)context->swapChainExtent.width,
        .height = (float)context->swapChainExtent.height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f
    };
    
    VkRect2D scissor = {
        .offset = {0, 0},
        .extent = context->swapChainExtent
    };
    
    VkPipelineViewportStateCreateInfo viewportState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .pViewports = &viewport,
        .scissorCount = 1,
        .pScissors = &scissor
    };
    
    // Rasterizer
    VkPipelineRasterizationStateCreateInfo rasterizer = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .lineWidth = 1.0f,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable = VK_FALSE
    };
    
    // Multisampling
    VkPipelineMultisampleStateCreateInfo multisampling = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .sampleShadingEnable = VK_FALSE,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT
    };
    
    // Color blending
    VkPipelineColorBlendAttachmentState colorBlendAttachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp = VK_BLEND_OP_ADD,
    };
    
    VkPipelineColorBlendStateCreateInfo colorBlending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .attachmentCount = 1,
        .pAttachments = &colorBlendAttachment
    };
    
    // Disable depth testing for 2D
    VkPipelineDepthStencilStateCreateInfo depthStencil2D = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_FALSE,
        .depthWriteEnable = VK_FALSE,
        .depthCompareOp = VK_COMPARE_OP_ALWAYS,
        .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable = VK_FALSE,
    };
    
    // 2D push constants - ONLY projection matrix (64 bytes)
    VkPushConstantRange pushConstantRange2D = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(mat4) // 64 bytes - just the projection matrix
    };

    VkPipelineLayoutCreateInfo pipelineLayoutInfo2D = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 0,  // NO descriptor sets for colored pipeline!
        .pSetLayouts = NULL,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pushConstantRange2D,
    };
    
    if (vkCreatePipelineLayout(context->device, &pipelineLayoutInfo2D, NULL, &context->pipelineLayout2D) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create 2D pipeline layout\n");
        exit(EXIT_FAILURE);
    }
    
    VkGraphicsPipelineCreateInfo pipelineInfo2D = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = shaderStages2D,
        .pVertexInputState = &vertexInputInfo2D,
        .pInputAssemblyState = &inputAssembly2D,
        .pViewportState = &viewportState,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pColorBlendState = &colorBlending,
        .layout = context->pipelineLayout2D,
        .renderPass = context->renderPass,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .pDepthStencilState = &depthStencil2D,
    };
    
    if (vkCreateGraphicsPipelines(context->device, VK_NULL_HANDLE, 1, &pipelineInfo2D, NULL, &context->graphicsPipeline2D) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create 2D graphics pipeline\n");
        exit(EXIT_FAILURE);
    }
    
    vkDestroyShaderModule(context->device, fragShaderModule2D, NULL);
    vkDestroyShaderModule(context->device, vertShaderModule2D, NULL);
}



void createGraphicsPipeline(VulkanContext* context) {
    // Vertex shader
    VkShaderModule vertShaderModule;
    {
        VkShaderModuleCreateInfo createInfo = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(vert_vert_spv),
            .pCode = (const uint32_t*)vert_vert_spv
        };
        
        if (vkCreateShaderModule(context->device, &createInfo, NULL, &vertShaderModule) != VK_SUCCESS) {
            fprintf(stderr, "Failed to create vertex shader module\n");
            exit(EXIT_FAILURE);
        }
    }
    
    // Fragment shader
    VkShaderModule fragShaderModule;
    {
        VkShaderModuleCreateInfo createInfo = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(frag_frag_spv),
            .pCode = (const uint32_t*)frag_frag_spv
        };
        
        if (vkCreateShaderModule(context->device, &createInfo, NULL, &fragShaderModule) != VK_SUCCESS) {
            fprintf(stderr, "Failed to create fragment shader module\n");
            exit(EXIT_FAILURE);
        }
    }
    
    // Pipeline stages
    VkPipelineShaderStageCreateInfo vertShaderStageInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_VERTEX_BIT,
        .module = vertShaderModule,
        .pName = "main"
    };
    
    VkPipelineShaderStageCreateInfo fragShaderStageInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
        .module = fragShaderModule,
        .pName = "main"
    };
    
    VkPipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo, fragShaderStageInfo};
    
    VkVertexInputBindingDescription bindingDescription = {
        .binding = 0,
        .stride = sizeof(Vertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX
    };
    
    // ATTRIBUTE DESCRIPTOR
    VkVertexInputAttributeDescription attributeDescriptions[3] = {
        {
            .binding = 0,
            .location = 0,
            .format = VK_FORMAT_R32G32B32_SFLOAT,
            .offset = offsetof(Vertex, pos)
        },
        {
            .binding = 0,
            .location = 1,
            .format = VK_FORMAT_R32G32B32A32_SFLOAT,
            .offset = offsetof(Vertex, color)
        },
        {
            .binding = 0,
            .location = 2,  // NEW: Normal attribute
            .format = VK_FORMAT_R32G32B32_SFLOAT,
            .offset = offsetof(Vertex, normal)
        }
    };
    
    VkPipelineVertexInputStateCreateInfo vertexInputInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &bindingDescription,
        .vertexAttributeDescriptionCount = 3,  // Changed from 2 to 3
        .pVertexAttributeDescriptions = attributeDescriptions
    };
    
    // Input assembly
    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE
    };
    
    // Viewport and scissor
    VkViewport viewport = {
        .x = 0.0f,
        .y = 0.0f,
        .width = (float)context->swapChainExtent.width,
        .height = (float)context->swapChainExtent.height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f
    };
    
    VkRect2D scissor = {
        .offset = {0, 0},
        .extent = context->swapChainExtent
    };
    
    VkPipelineViewportStateCreateInfo viewportState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .pViewports = &viewport,
        .scissorCount = 1,
        .pScissors = &scissor
    };
    
    // Rasterizer
    VkPipelineRasterizationStateCreateInfo rasterizer = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        /* .polygonMode = VK_POLYGON_MODE_LINE, */
        /* .polygonMode = VK_POLYGON_MODE_POINT, */
        .lineWidth = 1.0f,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable = VK_FALSE
    };
    
    // Multisampling
    VkPipelineMultisampleStateCreateInfo multisampling = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .sampleShadingEnable = VK_FALSE,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT
    };
    
    // Color blending
    VkPipelineColorBlendAttachmentState colorBlendAttachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp = VK_BLEND_OP_ADD,
    };
    
    
    VkPipelineColorBlendStateCreateInfo colorBlending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .attachmentCount = 1,
        .pAttachments = &colorBlendAttachment
    };
    
    
    // Pipeline layout
    VkPushConstantRange pushConstantRange = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(PushConstants), // Now includes both model matrix and AO state
    };
    
    VkPipelineLayoutCreateInfo pipelineLayoutInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &context->descriptorSetLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pushConstantRange,
    };
    
    
    pipelineLayoutInfo.pSetLayouts = &context->descriptorSetLayout;
    pipelineLayoutInfo.setLayoutCount = 1;
    
    
    if (vkCreatePipelineLayout(context->device, &pipelineLayoutInfo, NULL, &context->pipelineLayout) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create pipeline layout\n");
        exit(EXIT_FAILURE);
    }
    
    
    VkPipelineDepthStencilStateCreateInfo depthStencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_LESS,
        .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable = VK_FALSE,
    };
    
    // Create graphics pipeline
    VkGraphicsPipelineCreateInfo pipelineInfo = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = shaderStages,
        .pVertexInputState = &vertexInputInfo,
        .pInputAssemblyState = &inputAssembly,
        .pViewportState = &viewportState,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pColorBlendState = &colorBlending,
        .layout = context->pipelineLayout,
        .renderPass = context->renderPass,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .pDepthStencilState = &depthStencil,
    };
    
    if (vkCreateGraphicsPipelines(context->device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &context->graphicsPipeline) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create graphics pipeline\n");
        exit(EXIT_FAILURE);
    }
    
    // Clean up shader modules
    vkDestroyShaderModule(context->device, fragShaderModule, NULL);
    vkDestroyShaderModule(context->device, vertShaderModule, NULL);
}

void createFramebuffers(VulkanContext* context) {
    context->swapChainFramebuffers = malloc(context->swapChainImageCount * sizeof(VkFramebuffer));
    
    for (uint32_t i = 0; i < context->swapChainImageCount; i++) {
        VkImageView attachments[] = {
            context->swapChainImageViews[i],
            context->depthImageView
        };
        
        
        VkFramebufferCreateInfo framebufferInfo = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = context->renderPass,
            .attachmentCount = 2,
            .pAttachments = attachments,
            .width = context->swapChainExtent.width,
            .height = context->swapChainExtent.height,
            .layers = 1
        };
        
        if (vkCreateFramebuffer(context->device, &framebufferInfo, NULL, &context->swapChainFramebuffers[i]) != VK_SUCCESS) {
            fprintf(stderr, "Failed to create framebuffer\n");
            exit(EXIT_FAILURE);
        }
    }
}

void createCommandPool(VulkanContext* context) {
    VkCommandPoolCreateInfo poolInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = 0 // Assuming graphics queue family is 0
    };
    
    if (vkCreateCommandPool(context->device, &poolInfo, NULL, &context->commandPool) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create command pool\n");
        exit(EXIT_FAILURE);
    }
}

void createCommandBuffers(VulkanContext* context) {
    context->commandBuffers = malloc(context->swapChainImageCount * sizeof(VkCommandBuffer));
    
    VkCommandBufferAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = context->commandPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = context->swapChainImageCount
    };
    
    if (vkAllocateCommandBuffers(context->device, &allocInfo, context->commandBuffers) != VK_SUCCESS) {
        fprintf(stderr, "Failed to allocate command buffers\n");
        exit(EXIT_FAILURE);
    }
    
    for (uint32_t i = 0; i < context->swapChainImageCount; i++) {
        VkCommandBufferBeginInfo beginInfo = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
        };
        
        if (vkBeginCommandBuffer(context->commandBuffers[i], &beginInfo) != VK_SUCCESS) {
            fprintf(stderr, "Failed to begin recording command buffer\n");
            exit(EXIT_FAILURE);
        }
        
        
        VkClearValue clearValues[2];
        clearValues[0].color = (VkClearColorValue){{0.0f, 0.0f, 0.0f, 1.0f}};
        clearValues[1].depthStencil = (VkClearDepthStencilValue){1.0f, 0};
        
        VkRenderPassBeginInfo renderPassInfo = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = context->renderPass,
            .framebuffer = context->swapChainFramebuffers[i],
            
            
            .renderArea = {
                .offset = {0, 0},
                .extent = context->swapChainExtent
            },
            .clearValueCount = 2,
            .pClearValues = clearValues,
        };
        
        
        vkCmdBeginRenderPass(context->commandBuffers[i], &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        
        
        vkCmdBindPipeline(context->commandBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, context->graphicsPipeline);
        vkCmdBindDescriptorSets(context->commandBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, context->pipelineLayout, 0, 1, &descriptorSet, 0, NULL);
        
        renderer_draw(context->commandBuffers[i]);
        
        
        vkCmdEndRenderPass(context->commandBuffers[i]);
        
        if (vkEndCommandBuffer(context->commandBuffers[i]) != VK_SUCCESS) {
            fprintf(stderr, "Failed to record command buffer\n");
            exit(EXIT_FAILURE);
        }
    }
}


void createSyncObjects(VulkanContext* context) {
    context->imageAvailableSemaphores = malloc(MAX_FRAMES_IN_FLIGHT * sizeof(VkSemaphore));
    context->inFlightFences = malloc(MAX_FRAMES_IN_FLIGHT * sizeof(VkFence));
    
    context->renderFinishedSemaphores = malloc(context->swapChainImageCount * sizeof(VkSemaphore));
    context->imagesInFlight = malloc(context->swapChainImageCount * sizeof(VkFence));
    
    VkSemaphoreCreateInfo semaphoreInfo = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
    };
    
    VkFenceCreateInfo fenceInfo = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT
    };
    
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (vkCreateSemaphore(context->device, &semaphoreInfo, NULL, &context->imageAvailableSemaphores[i]) != VK_SUCCESS ||
            vkCreateFence(context->device, &fenceInfo, NULL, &context->inFlightFences[i]) != VK_SUCCESS) {
            fprintf(stderr, "Failed to create sync objects for frame %u\n", i);
            exit(EXIT_FAILURE);
        }
    }
    
    for (uint32_t i = 0; i < context->swapChainImageCount; i++) {
        if (vkCreateSemaphore(context->device, &semaphoreInfo, NULL, &context->renderFinishedSemaphores[i]) != VK_SUCCESS) {
            fprintf(stderr, "Failed to create renderFinishedSemaphore for image %u\n", i);
            exit(EXIT_FAILURE);
        }
        context->imagesInFlight[i] = VK_NULL_HANDLE;
    }
}

void createDepthResources(VulkanContext* context) {
    context->depthFormat = VK_FORMAT_D32_SFLOAT;
    
    // Create depth image
    VkImageCreateInfo depthImageInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .extent.width = context->swapChainExtent.width,
        .extent.height = context->swapChainExtent.height,
        .extent.depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .format = context->depthFormat,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateImage(context->device, &depthImageInfo, NULL, &context->depthImage) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create depth image\n");
        exit(EXIT_FAILURE);
    }
    
    VkMemoryRequirements depthMemReq;
    vkGetImageMemoryRequirements(context->device, context->depthImage, &depthMemReq);
    
    VkMemoryAllocateInfo depthAllocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = depthMemReq.size,
        .memoryTypeIndex = findMemoryType(context->physicalDevice, depthMemReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    };
    if (vkAllocateMemory(context->device, &depthAllocInfo, NULL, &context->depthImageMemory) != VK_SUCCESS) {
        fprintf(stderr, "Failed to allocate depth image memory\n");
        exit(EXIT_FAILURE);
    }
    vkBindImageMemory(context->device, context->depthImage, context->depthImageMemory, 0);
    
    VkImageViewCreateInfo depthImageViewInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = context->depthImage,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = context->depthFormat,
        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
        .subresourceRange.baseMipLevel = 0,
        .subresourceRange.levelCount = 1,
        .subresourceRange.baseArrayLayer = 0,
        .subresourceRange.layerCount = 1,
    };
    if (vkCreateImageView(context->device, &depthImageViewInfo, NULL, &context->depthImageView) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create depth image view\n");
        exit(EXIT_FAILURE);
    }
}

// Main render loop
void drawFrame(VulkanContext* context) {
    uint32_t frameIndex = context->currentFrame;
    VkFence inFlightFence = context->inFlightFences[frameIndex];
    
    vkWaitForFences(context->device, 1, &inFlightFence, VK_TRUE, UINT64_MAX);
    
    uint32_t imageIndex;
    VkResult result = vkAcquireNextImageKHR(
                                            context->device,
                                            context->swapChain,
                                            UINT64_MAX,
                                            context->imageAvailableSemaphores[frameIndex],
                                            VK_NULL_HANDLE,
                                            &imageIndex
                                            );
    
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        // TODO: handle swapchain recreation
        return;
    } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        fprintf(stderr, "Failed to acquire swap chain image\n");
        exit(EXIT_FAILURE);
    }
    
    if (context->imagesInFlight[imageIndex] != VK_NULL_HANDLE) {
        vkWaitForFences(context->device, 1, &context->imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX);
    }
    
    context->imagesInFlight[imageIndex] = inFlightFence;
    
    vkResetFences(context->device, 1, &inFlightFence);
    
    VkSemaphore waitSemaphores[] = { context->imageAvailableSemaphores[frameIndex] };
    VkSemaphore signalSemaphores[] = { context->renderFinishedSemaphores[imageIndex] };
    VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    
    VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = waitSemaphores,
        .pWaitDstStageMask = waitStages,
        .commandBufferCount = 1,
        .pCommandBuffers = &context->commandBuffers[imageIndex],
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = signalSemaphores
    };
    
    if (vkQueueSubmit(context->graphicsQueue, 1, &submitInfo, inFlightFence) != VK_SUCCESS) {
        fprintf(stderr, "Failed to submit draw command buffer\n");
        exit(EXIT_FAILURE);
    }
    
    VkPresentInfoKHR presentInfo = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = signalSemaphores,
        .swapchainCount = 1,
        .pSwapchains = &context->swapChain,
        .pImageIndices = &imageIndex,
        .pResults = NULL
    };
    
    result = vkQueuePresentKHR(context->graphicsQueue, &presentInfo);
    
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        // TODO: handle swapchain recreation
    } else if (result != VK_SUCCESS) {
        fprintf(stderr, "Failed to present swap chain image\n");
        exit(EXIT_FAILURE);
    }
    
    context->currentFrame = (context->currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
}

void cleanup(VulkanContext* context) {
    vkDeviceWaitIdle(context->device);
    
    renderer_shutdown();
    meshes_destroy(context->device, &scene.meshes);
    
    // Texture pool is now cleaned up before this function is called
    // (in main before cleanup() is called)
    
    // Clean up 2D descriptor resources
    if (context->descriptorSetLayout2D) 
        vkDestroyDescriptorSetLayout(context->device, context->descriptorSetLayout2D, NULL);
    if (context->descriptorPool2D) 
        vkDestroyDescriptorPool(context->device, context->descriptorPool2D, NULL);
    
    // SYNC OBJECTS
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (context->imageAvailableSemaphores[i])
            vkDestroySemaphore(context->device, context->imageAvailableSemaphores[i], NULL);
        if (context->inFlightFences[i])
            vkDestroyFence(context->device, context->inFlightFences[i], NULL);
    }
    free(context->imageAvailableSemaphores);
    free(context->inFlightFences);
    
    for (uint32_t i = 0; i < context->swapChainImageCount; i++) {
        if (context->renderFinishedSemaphores[i])
            vkDestroySemaphore(context->device, context->renderFinishedSemaphores[i], NULL);
    }
    free(context->renderFinishedSemaphores);
    free(context->imagesInFlight);
    
    // COMMANDS
    if (context->commandPool) vkDestroyCommandPool(context->device, context->commandPool, NULL);
    free(context->commandBuffers);
    
    // FRAMEBUFFERS & IMAGE VIEWS
    for (uint32_t i = 0; i < context->swapChainImageCount; i++) {
        if (context->swapChainFramebuffers[i])
            vkDestroyFramebuffer(context->device, context->swapChainFramebuffers[i], NULL);
        if (context->swapChainImageViews[i])
            vkDestroyImageView(context->device, context->swapChainImageViews[i], NULL);
    }
    free(context->swapChainFramebuffers);
    free(context->swapChainImageViews);
    free(context->swapChainImages);
    
    // GRAPHICS PIPELINE
    if (context->graphicsPipeline) vkDestroyPipeline(context->device, context->graphicsPipeline, NULL);
    if (context->pipelineLayout) vkDestroyPipelineLayout(context->device, context->pipelineLayout, NULL);
    
    // 2D GRAPHICS PIPELINE
    if (context->graphicsPipeline2D) vkDestroyPipeline(context->device, context->graphicsPipeline2D, NULL);
    if (context->pipelineLayout2D) vkDestroyPipelineLayout(context->device, context->pipelineLayout2D, NULL);
    
    // 2D VERTEX BUFFER
    if (context->vertexBuffer2D) vkDestroyBuffer(context->device, context->vertexBuffer2D, NULL);
    if (context->vertexBufferMemory2D) vkFreeMemory(context->device, context->vertexBufferMemory2D, NULL);
    
    // TEXTURE GRAPHICS PIPELINE
    if (context->graphicsPipelineTextured2D) 
        vkDestroyPipeline(context->device, context->graphicsPipelineTextured2D, NULL);
    if (context->pipelineLayoutTextured2D) 
        vkDestroyPipelineLayout(context->device, context->pipelineLayoutTextured2D, NULL);
    
    if (context->renderPass) vkDestroyRenderPass(context->device, context->renderPass, NULL);
    
    // DEPTH
    if (context->depthImageView) vkDestroyImageView(context->device, context->depthImageView, NULL);
    if (context->depthImage) vkDestroyImage(context->device, context->depthImage, NULL);
    if (context->depthImageMemory) vkFreeMemory(context->device, context->depthImageMemory, NULL);
    
    // UNIFORM BUFFER & DESCRIPTORS
    if (uniformBuffer) vkDestroyBuffer(context->device, uniformBuffer, NULL);
    if (uniformBufferMemory) vkFreeMemory(context->device, uniformBufferMemory, NULL);
    if (descriptorPool) vkDestroyDescriptorPool(context->device, descriptorPool, NULL);
    if (context->descriptorSetLayout) vkDestroyDescriptorSetLayout(context->device, context->descriptorSetLayout, NULL);
    
    // SWAPCHAIN & DEVICE
    if (context->swapChain) vkDestroySwapchainKHR(context->device, context->swapChain, NULL);
    if (context->device) vkDestroyDevice(context->device, NULL);
    
    // SURFACE & INSTANCE
    if (context->surface) vkDestroySurfaceKHR(context->instance, context->surface, NULL);
    if (context->instance) vkDestroyInstance(context->instance, NULL);
    
    // WINDOW
    if (context->window) glfwDestroyWindow(context->window);
    glfwTerminate();
}

double lastX = WIDTH / 2.0f, lastY = HEIGHT / 2.0f;
bool firstMouse = true;
float delta_time;
float last_frame = 0.0f;


bool shiftPressed;
bool ctrlPressed;
bool altPressed;
bool ambientOcclusionEnabled = true;


void screenshot( VkDevice device, VkPhysicalDevice physicalDevice, VkCommandPool commandPool, VkQueue queue, VkImage srcImage, VkFormat imageFormat, uint32_t width, uint32_t height, const char* filename);

void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    Camera* cam = glfwGetWindowUserPointer(window);
    shiftPressed = mods & GLFW_MOD_SHIFT;
    ctrlPressed  = mods & GLFW_MOD_CONTROL;
    altPressed   = mods & GLFW_MOD_ALT;
    
    if (key == GLFW_KEY_TAB && action == GLFW_PRESS) {
        screenshot(
                   context.device,
                   context.physicalDevice,
                   context.commandPool,
                   context.graphicsQueue,
                   context.swapChainImages[0], // or whichever image
                   VK_FORMAT_B8G8R8A8_SRGB,    // or another swapchain format
                   context.swapChainExtent.width,
                   context.swapChainExtent.height,
                   "screenshot.png"
                   );
    }
    
    if (key == GLFW_KEY_T && action == GLFW_PRESS) {
        ambientOcclusionEnabled = !ambientOcclusionEnabled;
        printf("Ambient Occlusion: %s\n", ambientOcclusionEnabled ? "ENABLED" : "DISABLED");
    }
    
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
        cam->active = !cam->active;
        
        if (cam->active) {
            // Enable camera control, disable cursor
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            printf("Camera control ENABLED\n");
        } else {
            // Disable camera control, enable cursor
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            printf("Camera control DISABLED - Editor mode\n");
        }
    }
}


void mouse_callback(GLFWwindow* window, double xpos, double ypos) {
    if (firstMouse) {
        lastX = xpos;
        lastY = ypos;
        firstMouse = false;
    }
    double xoffset = xpos - lastX;
    double yoffset = lastY - ypos; // reversed
    lastX = xpos;
    lastY = ypos;
    
    Camera* cam = glfwGetWindowUserPointer(window);
    camera_process_mouse(cam, xoffset, yoffset);
}



void createUniformBuffer(VulkanContext* context) {
    VkDeviceSize bufferSize = sizeof(UniformBufferObject);
    
    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bufferSize,
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    
    vkCreateBuffer(context->device, &bufferInfo, NULL, &uniformBuffer);
    
    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(context->device, uniformBuffer, &memRequirements);
    
    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memRequirements.size,
        .memoryTypeIndex = 0
    };
    
    allocInfo.memoryTypeIndex = findMemoryType(context->physicalDevice, memRequirements.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    
    vkAllocateMemory(context->device, &allocInfo, NULL, &uniformBufferMemory);
    vkBindBufferMemory(context->device, uniformBuffer, uniformBufferMemory, 0);
}


void createDescriptorSetLayout(VulkanContext* context) {
    VkDescriptorSetLayoutBinding uboLayoutBinding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .pImmutableSamplers = NULL
    };
    
    VkDescriptorSetLayoutCreateInfo layoutInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &uboLayoutBinding
    };
    
    vkCreateDescriptorSetLayout(context->device, &layoutInfo, NULL, &context->descriptorSetLayout);
}


void recordCommandBuffer(VulkanContext* context, uint32_t imageIndex) {
    VkCommandBuffer cmd = context->commandBuffers[imageIndex];
    
    VkCommandBufferBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
    };
    
    vkBeginCommandBuffer(cmd, &beginInfo);
    
    VkClearValue clearValues[2];
    clearValues[0].color = (VkClearColorValue){{0.0f, 0.0f, 0.0f, 1.0f}};
    clearValues[1].depthStencil = (VkClearDepthStencilValue){1.0f, 0};
    
    VkRenderPassBeginInfo renderPassInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = context->renderPass,
        .framebuffer = context->swapChainFramebuffers[imageIndex],
        .renderArea = {
            .offset = {0, 0},
            .extent = context->swapChainExtent
        },
        .clearValueCount = 2,
        .pClearValues = clearValues,
    };
    
    vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    
    // RENDER 3D Content First
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, context->graphicsPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, context->pipelineLayout, 0, 1, &descriptorSet, 0, NULL);
    
    // Set AO state once globally
    pushConstants.ambientOcclusionEnabled = ambientOcclusionEnabled ? 1 : 0;
    
    // Draw scene meshes
    for (size_t i = 0; i < scene.meshes.count; i++) {
        Mesh* mesh_ptr = &scene.meshes.items[i];
        mesh(cmd, mesh_ptr);
    }
    
    // Draw renderer content
    renderer_draw(cmd);
    
    // RENDER 2D Content On Top
    // NO descriptor sets bound for 2D pipeline!
    renderer2D_draw(cmd);
    
    vkCmdEndRenderPass(cmd);
    
    vkEndCommandBuffer(cmd);
}

vec4 red    = {1.0f, 0.0f, 0.0f, 1.0f};
vec4 green  = {0.0f, 1.0f, 0.0f, 1.0f};
vec4 blue   = {0.0f, 0.0f, 1.0f, 1.0f};
vec4 yellow = {1.0f, 1.0f, 0.0f, 1.0f};

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"


void screenshot(
                VkDevice device,
                VkPhysicalDevice physicalDevice,
                VkCommandPool commandPool,
                VkQueue queue,
                VkImage srcImage,
                VkFormat imageFormat,
                uint32_t width,
                uint32_t height,
                const char* filename)
{
    VkResult err;
    
    // 1. Create CPU-accessible buffer
    VkDeviceSize imageSize = width * height * 4;
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingBufferMemory;
    
    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = imageSize,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    
    err = vkCreateBuffer(device, &bufferInfo, NULL, &stagingBuffer);
    assert(err == VK_SUCCESS);
    
    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(device, stagingBuffer, &memRequirements);
    
    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memRequirements.size,
        .memoryTypeIndex = findMemoryType(physicalDevice, memRequirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    
    err = vkAllocateMemory(device, &allocInfo, NULL, &stagingBufferMemory);
    assert(err == VK_SUCCESS);
    vkBindBufferMemory(device, stagingBuffer, stagingBufferMemory, 0);
    
    // 2. Create command buffer for copy
    VkCommandBufferAllocateInfo cmdBufAllocInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandPool = commandPool,
        .commandBufferCount = 1,
    };
    
    VkCommandBuffer cmdBuf;
    vkAllocateCommandBuffers(device, &cmdBufAllocInfo, &cmdBuf);
    
    VkCommandBufferBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cmdBuf, &beginInfo);
    
    // 3. Transition image layout if necessary (skip if already TRANSFER_SRC_OPTIMAL)
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,  // adjust if different!
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = srcImage,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    vkCmdPipelineBarrier(
                         cmdBuf,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0,
                         0, NULL,
                         0, NULL,
                         1, &barrier);
    
    // 4. Copy image to buffer
    VkBufferImageCopy region = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .imageOffset = {0, 0, 0},
        .imageExtent = {width, height, 1},
    };
    
    vkCmdCopyImageToBuffer(cmdBuf, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stagingBuffer, 1, &region);
    
    // 5. Barrier to host read
    VkBufferMemoryBarrier bufBarrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = stagingBuffer,
        .offset = 0,
        .size = imageSize,
    };
    vkCmdPipelineBarrier(
                         cmdBuf,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT,
                         0,
                         0, NULL,
                         1, &bufBarrier,
                         0, NULL);
    
    vkEndCommandBuffer(cmdBuf);
    
    // 6. Submit and wait
    VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmdBuf,
    };
    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    
    // 7. Map buffer and save with stb_image_write
    void* data;
    vkMapMemory(device, stagingBufferMemory, 0, imageSize, 0, &data);
    
    // NOTE: Vulkan default is BGRA, convert to RGBA for PNG
    uint8_t* rgba_pixels = malloc(imageSize);
    for (uint32_t i = 0; i < width * height; ++i) {
        uint8_t* src = (uint8_t*)data + i * 4;
        uint8_t* dst = rgba_pixels + i * 4;
        dst[0] = src[2]; // R
        dst[1] = src[1]; // G
        dst[2] = src[0]; // B
        dst[3] = src[3]; // A
    }
    
    stbi_write_png(filename, width, height, 4, rgba_pixels, width * 4);
    
    free(rgba_pixels);
    
    vkUnmapMemory(device, stagingBufferMemory);
    
    // 8. Cleanup
    vkFreeCommandBuffers(device, commandPool, 1, &cmdBuf);
    vkDestroyBuffer(device, stagingBuffer, NULL);
    vkFreeMemory(device, stagingBufferMemory, NULL);
    printf("took screenshot");
}




int main() {
    context.currentFrame = 0;
    
    
    initWindow(&context);
    glfwSetInputMode(context.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    
    createInstance(&context);
    
    Camera camera;
    vec3 camera_pos = {0.0f, 2.0f, 0.0f}; // 2 blocks up
    vec3 camera_target = {0.0f, 2.0f, 0.0f}; // looking at world center, player-eye level
    camera_init(&camera, camera_pos, 90.0f, 0.0f, (float)WIDTH / (float)HEIGHT);
    
    camera.active = true;
    glfwSetInputMode(context.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);    
    
    
    glfwSetWindowUserPointer(context.window, &camera);
    glfwSetCursorPosCallback(context.window, mouse_callback);
    glfwSetKeyCallback(context.window, key_callback);
    
    
    
    if (glfwCreateWindowSurface(context.instance, context.window, NULL, &context.surface) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create window surface\n");
        exit(EXIT_FAILURE);
    }
    
    pickPhysicalDevice(&context);
    createLogicalDevice(&context);
    createSwapChain(&context);
    createImageViews(&context);
    createDepthResources(&context);
    
    
    createRenderPass(&context);
    
    createUniformBuffer(&context);
    createDescriptorSetLayout(&context);
    
    createGraphicsPipeline(&context);
    
    
    create2DDescriptorSetLayout(&context);
    create2DDescriptorPool(&context);
    create2DGraphicsPipeline(&context);
    createTextured2DGraphicsPipeline(&context);
    renderer2D_init();
    
    createDescriptorPool(&context);
    createDescriptorSet(&context);
    
    
    createFramebuffers(&context);
    createCommandPool(&context);
    
    renderer_init(
                  context.device,
                  context.physicalDevice,
                  context.commandPool,
                  context.graphicsQueue
                  );
    
    createCommandBuffers(&context);
    createSyncObjects(&context);
    
    
    srand((unsigned int)time(0)); // call this once at startup
    
    for (int x = 0; x < WORLD_WIDTH;  ++x)
        for (int y = 0; y < WORLD_HEIGHT; ++y)
            for (int z = 0; z < WORLD_DEPTH; ++z) {
                blocks[x][y][z].pos[0] = (x - WORLD_WIDTH  / 2) /* + 0.5f */;
                blocks[x][y][z].pos[1] = (y - WORLD_HEIGHT / 2) /* + 0.5f */;
                blocks[x][y][z].pos[2] = (z - WORLD_DEPTH  / 2) /* + 0.5f */;
                blocks[x][y][z].color[0] = (float)rand() / (float)RAND_MAX;
                blocks[x][y][z].color[1] = (float)rand() / (float)RAND_MAX;
                blocks[x][y][z].color[2] = (float)rand() / (float)RAND_MAX;
                blocks[x][y][z].color[3] = 1.0f;
            }
    
    for (int x = 0; x < WORLD_WIDTH;  ++x)
        for (int z = 0; z < WORLD_DEPTH; ++z) {
            int y = 0; // ground level
            blocks[x][y][z].pos[0] = (x - WORLD_WIDTH  / 2);
            blocks[x][y][z].pos[1] = (y - WORLD_HEIGHT / 2);
            blocks[x][y][z].pos[2] = (z - WORLD_DEPTH  / 2);
            blocks[x][y][z].color[0] = (float)rand() / (float)RAND_MAX;
            blocks[x][y][z].color[1] = (float)rand() / (float)RAND_MAX;
            blocks[x][y][z].color[2] = (float)rand() / (float)RAND_MAX;
            blocks[x][y][z].color[3] = 1.0f;
        }
    
    
    
    scene_init(&scene); // Initialize the scene before loading meshes
    
    /* Mesh teapot = */ load_obj("./assets/teapot.obj", red);
    /* Mesh cow = */ load_obj("./assets/cow.obj", blue);
    
    Mesh first = scene.meshes.items[0];
  

    texture_pool_init(&context);

    // Load textures and get their indices
    int32_t tex1 = texture_pool_add(&context, "./assets/textures/puta.jpg");
    int32_t tex2 = texture_pool_add(&context, "./assets/textures/prototype/Green/texture_01.png");
    
    // Get texture pointers
    Texture2D* texture1 = texture_pool_get(tex1);
    Texture2D* texture2 = texture_pool_get(tex2);
    
    while (!glfwWindowShouldClose(context.window)) {
        float current_frame = glfwGetTime();
        delta_time = current_frame - last_frame;
        last_frame = current_frame;
        
        glfwPollEvents();
        camera_process_keyboard(&camera, context.window, delta_time);
        camera_update(&camera);
        
        UniformBufferObject ubo;
        glm_mat4_mul(camera.projection_matrix, camera.view_matrix, ubo.vp);
        void* data;
        vkMapMemory(context.device, uniformBufferMemory, 0, sizeof(ubo), 0, &data);
        memcpy(data, &ubo, sizeof(ubo));
        vkUnmapMemory(context.device, uniformBufferMemory);
        
        renderer_clear();
        renderer2D_clear();        
        
        vec3 v0 = { -0.5f, -0.5f, 0.0f }; // Bottom Left
        vec3 v1 = {  0.5f, -0.5f, 0.0f }; // Bottom Right
        vec3 v2 = {  0.5f,  0.5f, 0.0f }; // Top Right
        vec3 v3 = { -0.5f,  0.5f, 0.0f }; // Top Left
        vec3 center = { 0.0f, 0.0f, 0.0f }; // Center
        triangle(v0, center, v1, red);    // Bottom
        triangle(v1, center, v2, green);  // Right
        triangle(v2, center, v3, blue);   // Top
        triangle(v3, center, v0, yellow); // Left
        
        
        /* for (int x = 0; x < WORLD_WIDTH;  ++x) */
        /* for (int y = 0; y < WORLD_HEIGHT; ++y) */
        /* for (int z = 0; z < WORLD_DEPTH; ++z) */
        /*     cube(blocks[x][y][z].pos, 1.0f, blocks[x][y][z].color); */
        
        
        /* cube((vec3){0, 0, 0}, 1.0f, red); */
        
        sphere((vec3){0,0,30}, 5, 80, 80, yellow);
        
        static float cow_rotation = 0.0f;
        
        cow_rotation += delta_time; // radians per frame
        
        glm_mat4_identity(scene.meshes.items[1].model);
        glm_rotate(scene.meshes.items[1].model, cow_rotation, (vec3){2.0f, 1.0f, 0.2f});
        
        quad2D((vec2){10, 10}, (vec2){50, 50}, BLUE);  // bottom left
        quad2D((vec2){70, 10}, (vec2){50, 50}, WHITE); // bottom right
        quad2D((vec2){10, 70}, (vec2){50, 50}, RED);   // top left
        quad2D((vec2){70, 70}, (vec2){50, 50}, GREEN); // top right
        
        /* texture2D((vec2){100, 100}, (vec2){200, 200}, &texture, WHITE); */
        /* texture2D((vec2){300, 300}, (vec2){600, 600}, &texture2, WHITE); */

        if (texture1) {
            texture2D((vec2){100, 100}, (vec2){200, 200}, texture1, WHITE);
            texture2D((vec2){500, 100}, (vec2){150, 150}, texture1, WHITE);
        }
        
        if (texture2) {
            texture2D((vec2){300, 300}, (vec2){600, 600}, texture2, WHITE);
        }
        
        
        
        
        
        
        
        renderer_upload();
        renderer2D_upload();
        
        // --- Draw frame (split into acquire, record, present) ---
        uint32_t frameIndex = context.currentFrame;
        VkFence inFlightFence = context.inFlightFences[frameIndex];
        vkWaitForFences(context.device, 1, &inFlightFence, VK_TRUE, UINT64_MAX);
        
        uint32_t imageIndex;
        VkResult result = vkAcquireNextImageKHR(
                                                context.device, context.swapChain, UINT64_MAX,
                                                context.imageAvailableSemaphores[frameIndex], VK_NULL_HANDLE, &imageIndex
                                                );
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            // Handle swapchain recreation
            continue;
        } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            fprintf(stderr, "Failed to acquire swap chain image\n");
            exit(EXIT_FAILURE);
        }
        
        if (context.imagesInFlight[imageIndex] != VK_NULL_HANDLE) {
            vkWaitForFences(context.device, 1, &context.imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX);
        }
        context.imagesInFlight[imageIndex] = inFlightFence;
        vkResetFences(context.device, 1, &inFlightFence);
        
        // --- Here! Re-record the command buffer for this swapchain image ---
        recordCommandBuffer(&context, imageIndex);
        
        VkSemaphore waitSemaphores[] = { context.imageAvailableSemaphores[frameIndex] };
        VkSemaphore signalSemaphores[] = { context.renderFinishedSemaphores[imageIndex] };
        VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
        VkSubmitInfo submitInfo = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = waitSemaphores,
            .pWaitDstStageMask = waitStages,
            .commandBufferCount = 1,
            .pCommandBuffers = &context.commandBuffers[imageIndex],
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = signalSemaphores
        };
        if (vkQueueSubmit(context.graphicsQueue, 1, &submitInfo, inFlightFence) != VK_SUCCESS) {
            fprintf(stderr, "Failed to submit draw command buffer\n");
            exit(EXIT_FAILURE);
        }
        
        VkPresentInfoKHR presentInfo = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = signalSemaphores,
            .swapchainCount = 1,
            .pSwapchains = &context.swapChain,
            .pImageIndices = &imageIndex,
            .pResults = NULL
        };
        result = vkQueuePresentKHR(context.graphicsQueue, &presentInfo);
        
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            // Handle swapchain recreation
        } else if (result != VK_SUCCESS) {
            fprintf(stderr, "Failed to present swap chain image\n");
            exit(EXIT_FAILURE);
        }
        
        
        context.currentFrame = (context.currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
    }
    
    vkDeviceWaitIdle(context.device);

    texture_pool_cleanup(&context);
    cleanup(&context);
    
    return EXIT_SUCCESS;
}

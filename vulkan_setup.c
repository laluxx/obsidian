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


bool ambientOcclusionEnabled = true;

VkDescriptorPool descriptorPool;
VkDescriptorSet descriptorSet;

VkBuffer uniformBuffer;
VkDeviceMemory uniformBufferMemory;


int lineWidth = 2.0f;


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


    /* VkPhysicalDeviceFeatures deviceFeatures = {0}; */
    VkPhysicalDeviceFeatures deviceFeatures = {
        .wideLines = VK_TRUE
    };

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

    VkPresentModeKHR presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;

    // FIX: Get actual current framebuffer size instead of using hardcoded WIDTH/HEIGHT
    VkExtent2D extent;
    if (capabilities.currentExtent.width != UINT32_MAX) {
        extent = capabilities.currentExtent;
    } else {
        // Fallback: query actual window size
        int width, height;
        glfwGetFramebufferSize(context->window, &width, &height);

        extent.width = (uint32_t)width;
        extent.height = (uint32_t)height;

        // Clamp to allowed values
        extent.width = extent.width < capabilities.minImageExtent.width ?
                       capabilities.minImageExtent.width : extent.width;
        extent.width = extent.width > capabilities.maxImageExtent.width ?
                       capabilities.maxImageExtent.width : extent.width;
        extent.height = extent.height < capabilities.minImageExtent.height ?
                        capabilities.minImageExtent.height : extent.height;
        extent.height = extent.height > capabilities.maxImageExtent.height ?
                        capabilities.maxImageExtent.height : extent.height;
    }

    // Rest of the function stays the same...
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

    /* printf("Swapchain created with extent: %dx%d\n", extent.width, extent.height); */
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
    VkShaderModule vertShaderModule2D;
    VkShaderModule fragShaderModuleTextured;

    {
        VkShaderModuleCreateInfo createInfo = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(__2D_vert_spv),
            .pCode = (const uint32_t*)__2D_vert_spv
        };
        vkCreateShaderModule(context->device, &createInfo, NULL, &vertShaderModule2D);
    }

    {
        VkShaderModuleCreateInfo createInfo = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(texture_frag_spv),
            .pCode = (const uint32_t*)texture_frag_spv
        };
        vkCreateShaderModule(context->device, &createInfo, NULL, &fragShaderModuleTextured);
    }

    VkPipelineShaderStageCreateInfo shaderStagesTextured[] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vertShaderModule2D,
            .pName = "main"
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fragShaderModuleTextured,
            .pName = "main"
        }
    };

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

    // DYNAMIC viewport/scissor
    VkPipelineViewportStateCreateInfo viewportState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .pViewports = NULL,
        .scissorCount = 1,
        .pScissors = NULL
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

    VkPipelineLayoutCreateInfo pipelineLayoutInfoTextured2D = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &context->descriptorSetLayout2D,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pushConstantRange2D,
    };

    vkCreatePipelineLayout(context->device, &pipelineLayoutInfoTextured2D, NULL, &context->pipelineLayoutTextured2D);

    // DYNAMIC STATE
    VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dynamicStates
    };

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
        .pDepthStencilState = &depthStencil2D,
        .pDynamicState = &dynamicState,  // ADD THIS
        .layout = context->pipelineLayoutTextured2D,
        .renderPass = context->renderPass,
        .subpass = 0
    };

    vkCreateGraphicsPipelines(context->device, VK_NULL_HANDLE, 1, &pipelineInfoTextured2D, NULL, &context->graphicsPipelineTextured2D);

    vkDestroyShaderModule(context->device, fragShaderModuleTextured, NULL);
    vkDestroyShaderModule(context->device, vertShaderModule2D, NULL);
}

void create2DGraphicsPipeline(VulkanContext* context) {
    VkShaderModule vertShaderModule2D;
    VkShaderModule fragShaderModule2D;

    {
        VkShaderModuleCreateInfo createInfo = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(__2D_vert_spv),
            .pCode = (const uint32_t*)__2D_vert_spv
        };
        vkCreateShaderModule(context->device, &createInfo, NULL, &vertShaderModule2D);
    }

    {
        VkShaderModuleCreateInfo createInfo = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(__2D_frag_spv),
            .pCode = (const uint32_t*)__2D_frag_spv
        };
        vkCreateShaderModule(context->device, &createInfo, NULL, &fragShaderModule2D);
    }

    VkPipelineShaderStageCreateInfo shaderStages2D[] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vertShaderModule2D,
            .pName = "main"
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fragShaderModule2D,
            .pName = "main"
        }
    };

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

    // DYNAMIC viewport/scissor
    VkPipelineViewportStateCreateInfo viewportState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .pViewports = NULL,
        .scissorCount = 1,
        .pScissors = NULL
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

    VkPipelineLayoutCreateInfo pipelineLayoutInfo2D = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 0,
        .pSetLayouts = NULL,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pushConstantRange2D,
    };

    vkCreatePipelineLayout(context->device, &pipelineLayoutInfo2D, NULL, &context->pipelineLayout2D);

    // DYNAMIC STATE
    VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dynamicStates
    };

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
        .pDepthStencilState = &depthStencil2D,
        .pDynamicState = &dynamicState,  // ADD THIS
        .layout = context->pipelineLayout2D,
        .renderPass = context->renderPass,
        .subpass = 0
    };

    vkCreateGraphicsPipelines(context->device, VK_NULL_HANDLE, 1, &pipelineInfo2D, NULL, &context->graphicsPipeline2D);

    vkDestroyShaderModule(context->device, fragShaderModule2D, NULL);
    vkDestroyShaderModule(context->device, vertShaderModule2D, NULL);
}

void create3DTexturedGraphicsPipeline(VulkanContext* context) {
    VkShaderModule vertShaderModule;
    VkShaderModule fragShaderModuleTextured;

    {
        VkShaderModuleCreateInfo createInfo = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(vert_vert_spv),
            .pCode = (const uint32_t*)vert_vert_spv
        };
        vkCreateShaderModule(context->device, &createInfo, NULL, &vertShaderModule);
    }

    {
        VkShaderModuleCreateInfo createInfo = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(texture3D_frag_spv),
            .pCode = (const uint32_t*)texture3D_frag_spv
        };
        vkCreateShaderModule(context->device, &createInfo, NULL, &fragShaderModuleTextured);
    }

    VkPipelineShaderStageCreateInfo shaderStagesTextured[] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vertShaderModule,
            .pName = "main"
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fragShaderModuleTextured,
            .pName = "main"
        }
    };

    VkVertexInputBindingDescription bindingDescription = {
        .binding = 0,
        .stride = sizeof(Vertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX
    };

    VkVertexInputAttributeDescription attributeDescriptions[4] = {
        {.binding = 0, .location = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex, pos)},
        {.binding = 0, .location = 1, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(Vertex, color)},
        {.binding = 0, .location = 2, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex, normal)},
        {.binding = 0, .location = 3, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(Vertex, texCoord)}
    };

    VkPipelineVertexInputStateCreateInfo vertexInputInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &bindingDescription,
        .vertexAttributeDescriptionCount = 4,
        .pVertexAttributeDescriptions = attributeDescriptions
    };

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE
    };

    // DYNAMIC viewport/scissor
    VkPipelineViewportStateCreateInfo viewportState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .pViewports = NULL,
        .scissorCount = 1,
        .pScissors = NULL
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

    VkPipelineDepthStencilStateCreateInfo depthStencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_LESS,
        .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable = VK_FALSE,
    };

    VkPushConstantRange pushConstantRange = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(PushConstants)
    };

    VkDescriptorSetLayout layouts[2] = {
        context->descriptorSetLayout,
        context->descriptorSetLayout2D
    };

    VkPipelineLayoutCreateInfo pipelineLayoutInfoTextured3D = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 2,
        .pSetLayouts = layouts,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pushConstantRange,
    };

    vkCreatePipelineLayout(context->device, &pipelineLayoutInfoTextured3D, NULL, &context->pipelineLayoutTextured3D);

    // DYNAMIC STATE
    VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dynamicStates
    };

    VkGraphicsPipelineCreateInfo pipelineInfoTextured3D = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = shaderStagesTextured,
        .pVertexInputState = &vertexInputInfo,
        .pInputAssemblyState = &inputAssembly,
        .pViewportState = &viewportState,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pColorBlendState = &colorBlending,
        .pDepthStencilState = &depthStencil,
        .pDynamicState = &dynamicState,  // ADD THIS
        .layout = context->pipelineLayoutTextured3D,
        .renderPass = context->renderPass,
        .subpass = 0
    };

    vkCreateGraphicsPipelines(context->device, VK_NULL_HANDLE, 1, &pipelineInfoTextured3D, NULL, &context->graphicsPipelineTextured3D);

    vkDestroyShaderModule(context->device, fragShaderModuleTextured, NULL);
    vkDestroyShaderModule(context->device, vertShaderModule, NULL);
}


void createLineGraphicsPipeline(VulkanContext* context) {
    VkShaderModule vertShaderModule;
    VkShaderModule fragShaderModule;

    {
        VkShaderModuleCreateInfo createInfo = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(vert_vert_spv),
            .pCode = (const uint32_t*)vert_vert_spv
        };
        vkCreateShaderModule(context->device, &createInfo, NULL, &vertShaderModule);
    }

    {
        VkShaderModuleCreateInfo createInfo = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(frag_frag_spv),
            .pCode = (const uint32_t*)frag_frag_spv
        };
        vkCreateShaderModule(context->device, &createInfo, NULL, &fragShaderModule);
    }

    VkPipelineShaderStageCreateInfo shaderStages[] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vertShaderModule,
            .pName = "main"
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fragShaderModule,
            .pName = "main"
        }
    };

    VkVertexInputBindingDescription bindingDescription = {
        .binding = 0,
        .stride = sizeof(Vertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX
    };

    VkVertexInputAttributeDescription attributeDescriptions[4] = {
        {.binding = 0, .location = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex, pos)},
        {.binding = 0, .location = 1, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(Vertex, color)},
        {.binding = 0, .location = 2, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex, normal)},
        {.binding = 0, .location = 3, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(Vertex, texCoord)}
    };

    VkPipelineVertexInputStateCreateInfo vertexInputInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &bindingDescription,
        .vertexAttributeDescriptionCount = 4,
        .pVertexAttributeDescriptions = attributeDescriptions
    };

    VkPipelineInputAssemblyStateCreateInfo inputAssemblyLine = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
        .primitiveRestartEnable = VK_FALSE
    };

    // DYNAMIC viewport/scissor
    VkPipelineViewportStateCreateInfo viewportState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .pViewports = NULL,
        .scissorCount = 1,
        .pScissors = NULL
    };

    VkPipelineRasterizationStateCreateInfo rasterizer = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .lineWidth = lineWidth,
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

    VkPipelineDepthStencilStateCreateInfo depthStencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_LESS,
        .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable = VK_FALSE,
    };

    VkPushConstantRange pushConstantRange = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(PushConstants)
    };

    VkPipelineLayoutCreateInfo pipelineLayoutInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &context->descriptorSetLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pushConstantRange,
    };

    vkCreatePipelineLayout(context->device, &pipelineLayoutInfo, NULL, &context->pipelineLayoutLine);

    // DYNAMIC STATE
    VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dynamicStates
    };

    VkGraphicsPipelineCreateInfo pipelineInfoLine = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = shaderStages,
        .pVertexInputState = &vertexInputInfo,
        .pInputAssemblyState = &inputAssemblyLine,
        .pViewportState = &viewportState,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pColorBlendState = &colorBlending,
        .pDepthStencilState = &depthStencil,
        .pDynamicState = &dynamicState,  // ADD THIS
        .layout = context->pipelineLayoutLine,
        .renderPass = context->renderPass,
        .subpass = 0
    };

    vkCreateGraphicsPipelines(context->device, VK_NULL_HANDLE, 1, &pipelineInfoLine, NULL, &context->graphicsPipelineLine);

    vkDestroyShaderModule(context->device, fragShaderModule, NULL);
    vkDestroyShaderModule(context->device, vertShaderModule, NULL);
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
        vkCreateShaderModule(context->device, &createInfo, NULL, &vertShaderModule);
    }

    // Fragment shader
    VkShaderModule fragShaderModule;
    {
        VkShaderModuleCreateInfo createInfo = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(frag_frag_spv),
            .pCode = (const uint32_t*)frag_frag_spv
        };
        vkCreateShaderModule(context->device, &createInfo, NULL, &fragShaderModule);
    }

    VkPipelineShaderStageCreateInfo shaderStages[] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vertShaderModule,
            .pName = "main"
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fragShaderModule,
            .pName = "main"
        }
    };

    VkVertexInputBindingDescription bindingDescription = {
        .binding = 0,
        .stride = sizeof(Vertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX
    };

    VkVertexInputAttributeDescription attributeDescriptions[4] = {
        {.binding = 0, .location = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex, pos)},
        {.binding = 0, .location = 1, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(Vertex, color)},
        {.binding = 0, .location = 2, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex, normal)},
        {.binding = 0, .location = 3, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(Vertex, texCoord)}
    };

    VkPipelineVertexInputStateCreateInfo vertexInputInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &bindingDescription,
        .vertexAttributeDescriptionCount = 4,
        .pVertexAttributeDescriptions = attributeDescriptions
    };

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE
    };

    // DYNAMIC: Don't set static viewport/scissor
    VkPipelineViewportStateCreateInfo viewportState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .pViewports = NULL,
        .scissorCount = 1,
        .pScissors = NULL
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

    VkPipelineDepthStencilStateCreateInfo depthStencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_LESS,
        .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable = VK_FALSE,
    };

    VkPushConstantRange pushConstantRange = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(PushConstants)
    };

    VkPipelineLayoutCreateInfo pipelineLayoutInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &context->descriptorSetLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pushConstantRange,
    };

    vkCreatePipelineLayout(context->device, &pipelineLayoutInfo, NULL, &context->pipelineLayout);

    // DYNAMIC STATE: Viewport and Scissor
    VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dynamicStates
    };

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
        .pDepthStencilState = &depthStencil,
        .pDynamicState = &dynamicState,  // ADD THIS
        .layout = context->pipelineLayout,
        .renderPass = context->renderPass,
        .subpass = 0
    };

    vkCreateGraphicsPipelines(context->device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &context->graphicsPipeline);

    vkDestroyShaderModule(context->device, fragShaderModule, NULL);
    vkDestroyShaderModule(context->device, vertShaderModule, NULL);
}

/// SDF

void createSDF2DGraphicsPipeline(VulkanContext* context) {
    VkShaderModule vertShaderModule2D;
    VkShaderModule fragShaderModuleSDF;

    {
        VkShaderModuleCreateInfo createInfo = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(__2D_vert_spv),
            .pCode = (const uint32_t*)__2D_vert_spv
        };
        vkCreateShaderModule(context->device, &createInfo, NULL, &vertShaderModule2D);
    }

    {
        VkShaderModuleCreateInfo createInfo = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(sdf_frag_spv),
            .pCode = (const uint32_t*)sdf_frag_spv
        };
        vkCreateShaderModule(context->device, &createInfo, NULL, &fragShaderModuleSDF);
    }

    VkPipelineShaderStageCreateInfo shaderStagesSDF[] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vertShaderModule2D,
            .pName = "main"
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fragShaderModuleSDF,
            .pName = "main"
        }
    };

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

    VkPipelineViewportStateCreateInfo viewportState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .pViewports = NULL,
        .scissorCount = 1,
        .pScissors = NULL
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

    VkPipelineLayoutCreateInfo pipelineLayoutInfoSDF2D = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &context->descriptorSetLayout2D,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pushConstantRange2D,
    };

    vkCreatePipelineLayout(context->device, &pipelineLayoutInfoSDF2D, NULL, &context->pipelineLayoutSDF2D);

    VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dynamicStates
    };

    VkGraphicsPipelineCreateInfo pipelineInfoSDF2D = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = shaderStagesSDF,
        .pVertexInputState = &vertexInputInfo2D,
        .pInputAssemblyState = &inputAssembly2D,
        .pViewportState = &viewportState,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pColorBlendState = &colorBlending,
        .pDepthStencilState = &depthStencil2D,
        .pDynamicState = &dynamicState,
        .layout = context->pipelineLayoutSDF2D,
        .renderPass = context->renderPass,
        .subpass = 0
    };

    vkCreateGraphicsPipelines(context->device, VK_NULL_HANDLE, 1, &pipelineInfoSDF2D, NULL, &context->graphicsPipelineSDF2D);

    vkDestroyShaderModule(context->device, fragShaderModuleSDF, NULL);
    vkDestroyShaderModule(context->device, vertShaderModule2D, NULL);
}

void createSDF3DGraphicsPipeline(VulkanContext* context) {
    VkShaderModule vertShaderModule;
    VkShaderModule fragShaderModuleSDF;

    {
        VkShaderModuleCreateInfo createInfo = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(vert_vert_spv),
            .pCode = (const uint32_t*)vert_vert_spv
        };
        vkCreateShaderModule(context->device, &createInfo, NULL, &vertShaderModule);
    }

    {
        VkShaderModuleCreateInfo createInfo = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(sdf3D_frag_spv),
            .pCode = (const uint32_t*)sdf3D_frag_spv
        };
        vkCreateShaderModule(context->device, &createInfo, NULL, &fragShaderModuleSDF);
    }

    VkPipelineShaderStageCreateInfo shaderStagesSDF[] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vertShaderModule,
            .pName = "main"
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fragShaderModuleSDF,
            .pName = "main"
        }
    };

    VkVertexInputBindingDescription bindingDescription = {
        .binding = 0,
        .stride = sizeof(Vertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX
    };

    VkVertexInputAttributeDescription attributeDescriptions[4] = {
        {.binding = 0, .location = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex, pos)},
        {.binding = 0, .location = 1, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(Vertex, color)},
        {.binding = 0, .location = 2, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex, normal)},
        {.binding = 0, .location = 3, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(Vertex, texCoord)}
    };

    VkPipelineVertexInputStateCreateInfo vertexInputInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &bindingDescription,
        .vertexAttributeDescriptionCount = 4,
        .pVertexAttributeDescriptions = attributeDescriptions
    };

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE
    };

    VkPipelineViewportStateCreateInfo viewportState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .pViewports = NULL,
        .scissorCount = 1,
        .pScissors = NULL
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

    VkPipelineDepthStencilStateCreateInfo depthStencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_LESS,
        .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable = VK_FALSE,
    };

    VkPushConstantRange pushConstantRange = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(PushConstants)
    };

    VkDescriptorSetLayout layouts[2] = {
        context->descriptorSetLayout,
        context->descriptorSetLayout2D
    };

    VkPipelineLayoutCreateInfo pipelineLayoutInfoSDF3D = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 2,
        .pSetLayouts = layouts,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pushConstantRange,
    };

    vkCreatePipelineLayout(context->device, &pipelineLayoutInfoSDF3D, NULL, &context->pipelineLayoutSDF3D);

    VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dynamicStates
    };

    VkGraphicsPipelineCreateInfo pipelineInfoSDF3D = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = shaderStagesSDF,
        .pVertexInputState = &vertexInputInfo,
        .pInputAssemblyState = &inputAssembly,
        .pViewportState = &viewportState,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pColorBlendState = &colorBlending,
        .pDepthStencilState = &depthStencil,
        .pDynamicState = &dynamicState,
        .layout = context->pipelineLayoutSDF3D,
        .renderPass = context->renderPass,
        .subpass = 0
    };

    vkCreateGraphicsPipelines(context->device, VK_NULL_HANDLE, 1, &pipelineInfoSDF3D, NULL, &context->graphicsPipelineSDF3D);

    vkDestroyShaderModule(context->device, fragShaderModuleSDF, NULL);
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
    // Free existing command buffers if any (safety check)
    if (context->commandBuffers) {
        vkFreeCommandBuffers(context->device, context->commandPool,
                            context->swapChainImageCount, context->commandBuffers);
        free(context->commandBuffers);
        context->commandBuffers = NULL;
    }

    // Allocate new command buffers
    context->commandBuffers = malloc(context->swapChainImageCount * sizeof(VkCommandBuffer));
    if (!context->commandBuffers) {
        fprintf(stderr, "Failed to allocate memory for command buffers\n");
        exit(EXIT_FAILURE);
    }

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

    // IMPORTANT: Do NOT record commands here!
    // Command buffers will be recorded dynamically each frame in recordCommandBuffer()
    // This allows us to set the correct viewport/scissor for the current framebuffer size

    // Optional: Set debug names for command buffers (if you have debugging enabled)
    for (uint32_t i = 0; i < context->swapChainImageCount; i++) {
        // You can set object names here if using validation layers
        // For example:
        // VkDebugUtilsObjectNameInfoEXT nameInfo = {
        //     .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
        //     .objectType = VK_OBJECT_TYPE_COMMAND_BUFFER,
        //     .objectHandle = (uint64_t)context->commandBuffers[i],
        //     .pObjectName = "Primary Command Buffer"
        // };
        // vkSetDebugUtilsObjectNameEXT(context->device, &nameInfo);
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

void updateUniformBuffer(VulkanContext* context) {
    void* data;
    VkResult result = vkMapMemory(context->device, uniformBufferMemory, 0, sizeof(UniformBufferObject), 0, &data);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "Failed to map uniform buffer memory\n");
        return;
    }

    UniformBufferObject ubo;

    // Calculate VP matrix (projection * view)
    // Note: Depending on your matrix order (row-major vs column-major), you might need to adjust this
    glm_mat4_mul(camera.projection_matrix, camera.view_matrix, ubo.vp);

    memcpy(data, &ubo, sizeof(ubo));
    vkUnmapMemory(context->device, uniformBufferMemory);
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
        recreateSwapChain(context);
        return;
    } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        fprintf(stderr, "Failed to acquire swap chain image\n");
        exit(EXIT_FAILURE);
    }

    // Check if a previous frame is using this image
    if (context->imagesInFlight[imageIndex] != VK_NULL_HANDLE) {
        vkWaitForFences(context->device, 1, &context->imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX);
    }

    // Mark the image as now being in use by this frame
    context->imagesInFlight[imageIndex] = inFlightFence;

    // Update uniform buffer
    updateUniformBuffer(context);

    // Record command buffer for this specific framebuffer
    recordCommandBuffer(context, imageIndex);

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

    vkResetFences(context->device, 1, &inFlightFence);

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
        recreateSwapChain(context);
    } else if (result != VK_SUCCESS) {
        fprintf(stderr, "Failed to present swap chain image\n");
        exit(EXIT_FAILURE);
    }

    context->currentFrame = (context->currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
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


void clear_background(Color color) {
    context.clearColor = color;
}

void recordCommandBuffer(VulkanContext* context, uint32_t imageIndex) {
    VkCommandBuffer cmd = context->commandBuffers[imageIndex];

    // Reset command buffer (important for re-recording)
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT
    };

    if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
        fprintf(stderr, "Failed to begin recording command buffer\n");
        exit(EXIT_FAILURE);
    }

    VkClearValue clearValues[2];
    clearValues[0].color = (VkClearColorValue){
        {context->clearColor.r, context->clearColor.g,
         context->clearColor.b, context->clearColor.a}
    };
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

    // Set dynamic viewport and scissor - MUST be done BEFORE any drawing commands!
    VkViewport viewport = {
        .x = 0.0f,
        .y = 0.0f,
        .width = (float)context->swapChainExtent.width,
        .height = (float)context->swapChainExtent.height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f
    };
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor = {
        .offset = {0, 0},
        .extent = context->swapChainExtent
    };
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Set AO state
    pushConstants.ambientOcclusionEnabled = ambientOcclusionEnabled ? 1 : 0;

    // --- RENDER 3D SOLID GEOMETRY ---
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, context->graphicsPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, context->pipelineLayout,
                            0, 1, &descriptorSet, 0, NULL);

    meshes_draw(cmd, &scene.meshes);
    renderer_draw(cmd);

    // --- RENDER 3D TEXTURED GEOMETRY ---
    renderer_draw_textured3D(cmd);

    // --- RENDER LINES ---
    if (context->graphicsPipelineLine && lineVertexCount > 0) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, context->graphicsPipelineLine);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, context->pipelineLayout,
                                0, 1, &descriptorSet, 0, NULL);
        line_renderer_draw(cmd);
    }

    // --- RENDER 2D CONTENT ---
    renderer2D_draw(cmd);

    vkCmdEndRenderPass(cmd);

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        fprintf(stderr, "Failed to record command buffer\n");
        exit(EXIT_FAILURE);
    }
}

// Update the original cleanupSwapChain to use the new function
void cleanupSwapChain(VulkanContext* context) {
    cleanupSwapChainResources(context, context->swapChain);
}

// New function to clean up swapchain resources without touching pipelines
void cleanupSwapChainResources(VulkanContext* context, VkSwapchainKHR oldSwapchain) {
    // CRITICAL: Wait for all GPU operations to complete before destroying resources
    vkDeviceWaitIdle(context->device);

    // Destroy framebuffers
    if (context->swapChainFramebuffers) {
        for (uint32_t i = 0; i < context->swapChainImageCount; i++) {
            if (context->swapChainFramebuffers[i] != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(context->device, context->swapChainFramebuffers[i], NULL);
                context->swapChainFramebuffers[i] = VK_NULL_HANDLE;
            }
        }
        free(context->swapChainFramebuffers);
        context->swapChainFramebuffers = NULL;
    }

    // Free command buffers first (they reference the framebuffers)
    if (context->commandBuffers && context->commandPool) {
        vkFreeCommandBuffers(context->device, context->commandPool,
                            context->swapChainImageCount, context->commandBuffers);
        free(context->commandBuffers);
        context->commandBuffers = NULL;
    }

    // Destroy image views
    if (context->swapChainImageViews) {
        for (uint32_t i = 0; i < context->swapChainImageCount; i++) {
            if (context->swapChainImageViews[i] != VK_NULL_HANDLE) {
                vkDestroyImageView(context->device, context->swapChainImageViews[i], NULL);
                context->swapChainImageViews[i] = VK_NULL_HANDLE;
            }
        }
        free(context->swapChainImageViews);
        context->swapChainImageViews = NULL;
    }

    // Destroy depth resources
    if (context->depthImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(context->device, context->depthImageView, NULL);
        context->depthImageView = VK_NULL_HANDLE;
    }
    if (context->depthImage != VK_NULL_HANDLE) {
        vkDestroyImage(context->device, context->depthImage, NULL);
        context->depthImage = VK_NULL_HANDLE;
    }
    if (context->depthImageMemory != VK_NULL_HANDLE) {
        vkFreeMemory(context->device, context->depthImageMemory, NULL);
        context->depthImageMemory = VK_NULL_HANDLE;
    }

    // Destroy the OLD swapchain
    if (oldSwapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(context->device, oldSwapchain, NULL);
    }

    if (context->swapChainImages) {
        free(context->swapChainImages);
        context->swapChainImages = NULL;
    }

    context->swapChainImageCount = 0;
}

void recreateSwapChain(VulkanContext* context) {
    // Handle minimization
    int width = 0, height = 0;
    glfwGetFramebufferSize(context->window, &width, &height);
    while (width == 0 || height == 0) {
        glfwGetFramebufferSize(context->window, &width, &height);
        glfwWaitEvents();
    }

    // Wait for ALL frames in flight to complete
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (context->inFlightFences[i] != VK_NULL_HANDLE) {
            vkWaitForFences(context->device, 1, &context->inFlightFences[i], VK_TRUE, UINT64_MAX);
        }
    }

    // Store old swapchain
    VkSwapchainKHR oldSwapchain = context->swapChain;

    // Get new surface capabilities
    VkSurfaceCapabilitiesKHR capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(context->physicalDevice, context->surface, &capabilities);

    // Create new swapchain
    VkSwapchainCreateInfoKHR createInfo = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = context->surface,
        .minImageCount = capabilities.minImageCount + 1,
        .imageFormat = context->swapChainImageFormat,
        .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent = { (uint32_t)width, (uint32_t)height },
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = capabilities.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR,
        .clipped = VK_TRUE,
        .oldSwapchain = oldSwapchain
    };

    if (capabilities.maxImageCount > 0 && createInfo.minImageCount > capabilities.maxImageCount) {
        createInfo.minImageCount = capabilities.maxImageCount;
    }

    VkSwapchainKHR newSwapchain;
    if (vkCreateSwapchainKHR(context->device, &createInfo, NULL, &newSwapchain) != VK_SUCCESS) {
        fprintf(stderr, "Failed to recreate swap chain\n");
        exit(EXIT_FAILURE);
    }

    // Clean up old resources
    cleanupSwapChainResources(context, oldSwapchain);

    // Update context with new swapchain
    context->swapChain = newSwapchain;
    context->swapChainExtent = (VkExtent2D){ (uint32_t)width, (uint32_t)height };

    // Get new swapchain images
    vkGetSwapchainImagesKHR(context->device, context->swapChain, &context->swapChainImageCount, NULL);
    context->swapChainImages = malloc(context->swapChainImageCount * sizeof(VkImage));
    vkGetSwapchainImagesKHR(context->device, context->swapChain, &context->swapChainImageCount, context->swapChainImages);

    // Recreate dependent resources
    createImageViews(context);
    createDepthResources(context);
    createFramebuffers(context);

    // Recreate command buffers (allocate only, not record)
    createCommandBuffers(context);

    // Update camera aspect ratio
    camera.aspect_ratio = (float)context->swapChainExtent.width /
                          (float)context->swapChainExtent.height;
    glm_perspective(glm_rad(camera.fov), camera.aspect_ratio, 0.1f, 100.0f,
                    camera.projection_matrix);
}

void cleanup(VulkanContext* context) {
    // Wait for device to finish ALL operations
    vkDeviceWaitIdle(context->device);

    renderer_shutdown();
    line_renderer_shutdown();
    meshes_destroy(context->device, &scene.meshes);
    texture_pool_cleanup(context);

    // Clean up 2D descriptor resources
    if (context->descriptorSetLayout2D) {
        vkDestroyDescriptorSetLayout(context->device, context->descriptorSetLayout2D, NULL);
        context->descriptorSetLayout2D = VK_NULL_HANDLE;
    }
    if (context->descriptorPool2D) {
        vkDestroyDescriptorPool(context->device, context->descriptorPool2D, NULL);
        context->descriptorPool2D = VK_NULL_HANDLE;
    }

    // Free command buffers first
    if (context->commandBuffers && context->commandPool) {
        vkFreeCommandBuffers(context->device, context->commandPool,
                            context->swapChainImageCount, context->commandBuffers);
        free(context->commandBuffers);
        context->commandBuffers = NULL;
    }

    // SYNC OBJECTS
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (context->imageAvailableSemaphores[i]) {
            vkDestroySemaphore(context->device, context->imageAvailableSemaphores[i], NULL);
            context->imageAvailableSemaphores[i] = VK_NULL_HANDLE;
        }
        if (context->inFlightFences[i]) {
            vkDestroyFence(context->device, context->inFlightFences[i], NULL);
            context->inFlightFences[i] = VK_NULL_HANDLE;
        }
    }
    free(context->imageAvailableSemaphores);
    free(context->inFlightFences);

    for (uint32_t i = 0; i < context->swapChainImageCount; i++) {
        if (context->renderFinishedSemaphores[i]) {
            vkDestroySemaphore(context->device, context->renderFinishedSemaphores[i], NULL);
            context->renderFinishedSemaphores[i] = VK_NULL_HANDLE;
        }
    }
    free(context->renderFinishedSemaphores);
    free(context->imagesInFlight);

    // COMMAND POOL
    if (context->commandPool) {
        vkDestroyCommandPool(context->device, context->commandPool, NULL);
        context->commandPool = VK_NULL_HANDLE;
    }

    // Clean up swapchain resources (this will destroy framebuffers, image views, depth, etc.)
    cleanupSwapChainResources(context, context->swapChain);

    // DESTROY ALL PIPELINES
    if (context->graphicsPipeline) {
        vkDestroyPipeline(context->device, context->graphicsPipeline, NULL);
        context->graphicsPipeline = VK_NULL_HANDLE;
    }
    if (context->graphicsPipeline2D) {
        vkDestroyPipeline(context->device, context->graphicsPipeline2D, NULL);
        context->graphicsPipeline2D = VK_NULL_HANDLE;
    }
    if (context->graphicsPipelineTextured2D) {
        vkDestroyPipeline(context->device, context->graphicsPipelineTextured2D, NULL);
        context->graphicsPipelineTextured2D = VK_NULL_HANDLE;
    }
    if (context->graphicsPipelineTextured3D) {
        vkDestroyPipeline(context->device, context->graphicsPipelineTextured3D, NULL);
        context->graphicsPipelineTextured3D = VK_NULL_HANDLE;
    }
    if (context->graphicsPipelineLine) {
        vkDestroyPipeline(context->device, context->graphicsPipelineLine, NULL);
        context->graphicsPipelineLine = VK_NULL_HANDLE;
    }
    if (context->graphicsPipelineSDF2D) {
        vkDestroyPipeline(context->device, context->graphicsPipelineSDF2D, NULL);
        context->graphicsPipelineSDF2D = VK_NULL_HANDLE;
    }
    if (context->graphicsPipelineSDF3D) {
        vkDestroyPipeline(context->device, context->graphicsPipelineSDF3D, NULL);
        context->graphicsPipelineSDF3D = VK_NULL_HANDLE;
    }

    // DESTROY PIPELINE LAYOUTS
    if (context->pipelineLayout) {
        vkDestroyPipelineLayout(context->device, context->pipelineLayout, NULL);
        context->pipelineLayout = VK_NULL_HANDLE;
    }
    if (context->pipelineLayout2D) {
        vkDestroyPipelineLayout(context->device, context->pipelineLayout2D, NULL);
        context->pipelineLayout2D = VK_NULL_HANDLE;
    }
    if (context->pipelineLayoutTextured2D) {
        vkDestroyPipelineLayout(context->device, context->pipelineLayoutTextured2D, NULL);
        context->pipelineLayoutTextured2D = VK_NULL_HANDLE;
    }
    if (context->pipelineLayoutTextured3D) {
        vkDestroyPipelineLayout(context->device, context->pipelineLayoutTextured3D, NULL);
        context->pipelineLayoutTextured3D = VK_NULL_HANDLE;
    }
    if (context->pipelineLayoutLine) {
        vkDestroyPipelineLayout(context->device, context->pipelineLayoutLine, NULL);
        context->pipelineLayoutLine = VK_NULL_HANDLE;
    }
    if (context->pipelineLayoutSDF2D) {
        vkDestroyPipelineLayout(context->device, context->pipelineLayoutSDF2D, NULL);
        context->pipelineLayoutSDF2D = VK_NULL_HANDLE;
    }
    if (context->pipelineLayoutSDF3D) {
        vkDestroyPipelineLayout(context->device, context->pipelineLayoutSDF3D, NULL);
        context->pipelineLayoutSDF3D = VK_NULL_HANDLE;
    }

    // 2D VERTEX BUFFER
    if (context->vertexBuffer2D) {
        vkDestroyBuffer(context->device, context->vertexBuffer2D, NULL);
        context->vertexBuffer2D = VK_NULL_HANDLE;
    }
    if (context->vertexBufferMemory2D) {
        vkFreeMemory(context->device, context->vertexBufferMemory2D, NULL);
        context->vertexBufferMemory2D = VK_NULL_HANDLE;
    }

    if (context->renderPass) {
        vkDestroyRenderPass(context->device, context->renderPass, NULL);
        context->renderPass = VK_NULL_HANDLE;
    }

    // UNIFORM BUFFER & DESCRIPTORS
    if (uniformBuffer) {
        vkDestroyBuffer(context->device, uniformBuffer, NULL);
        uniformBuffer = VK_NULL_HANDLE;
    }
    if (uniformBufferMemory) {
        vkFreeMemory(context->device, uniformBufferMemory, NULL);
        uniformBufferMemory = VK_NULL_HANDLE;
    }
    if (descriptorPool) {
        vkDestroyDescriptorPool(context->device, descriptorPool, NULL);
        descriptorPool = VK_NULL_HANDLE;
    }
    if (context->descriptorSetLayout) {
        vkDestroyDescriptorSetLayout(context->device, context->descriptorSetLayout, NULL);
        context->descriptorSetLayout = VK_NULL_HANDLE;
    }

    // DEVICE
    if (context->device) {
        vkDestroyDevice(context->device, NULL);
        context->device = VK_NULL_HANDLE;
    }

    // SURFACE & INSTANCE
    if (context->surface) {
        vkDestroySurfaceKHR(context->instance, context->surface, NULL);
        context->surface = VK_NULL_HANDLE;
    }
    if (context->instance) {
        vkDestroyInstance(context->instance, NULL);
        context->instance = VK_NULL_HANDLE;
    }

    // WINDOW
    if (context->window) {
        glfwDestroyWindow(context->window);
        context->window = NULL;
    }
    glfwTerminate();
}

void toggle_ambient_occlusion() {
    ambientOcclusionEnabled = !ambientOcclusionEnabled;
}

#include <vulkan/vulkan_core.h>
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "frag.spv.h"
#include "vert.spv.h"

#include <cglm/cglm.h>
#include "camera.h"
#include "renderer.h"

#include <time.h>  


// Constants
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
} Block;


#define WORLD_WIDTH  10
#define WORLD_HEIGHT 10
#define WORLD_DEPTH  10

Block blocks[WORLD_WIDTH][WORLD_HEIGHT][WORLD_DEPTH];


typedef struct {
    mat4 vp;
} UniformBufferObject;

typedef struct {
    GLFWwindow *window;
    VkInstance instance;
    VkPhysicalDevice physicalDevice;
    VkDevice device;
    VkQueue graphicsQueue;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapChain;
    VkImage *swapChainImages;
    uint32_t swapChainImageCount;
    VkFormat swapChainImageFormat;
    VkExtent2D swapChainExtent;
    VkImageView *swapChainImageViews;
    VkRenderPass renderPass;
    VkPipelineLayout pipelineLayout;
    VkPipeline graphicsPipeline;
    VkFramebuffer *swapChainFramebuffers;
    VkCommandPool commandPool;
    VkCommandBuffer *commandBuffers;
    VkSemaphore *imageAvailableSemaphores;
    VkFence *inFlightFences;
    uint32_t currentFrame;

    VkFence* imagesInFlight; // size: swapChainImageCount
    VkSemaphore* renderFinishedSemaphores; // size = swapChainImageCount
    VkDescriptorSetLayout descriptorSetLayout;

    VkImage depthImage;
    VkDeviceMemory depthImageMemory;
    VkImageView depthImageView;
    VkFormat depthFormat;


} VulkanContext;

uint32_t findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties);

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



// Helper function to check extension support
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

// Helper function to check validation layer support
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

// Initialize GLFW window
void initWindow(VulkanContext* context) {
    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    context->window = glfwCreateWindow(WIDTH, HEIGHT, "Vulkan Triangle", NULL, NULL);
}

// Create Vulkan instance
void createInstance(VulkanContext* context) {
#if ENABLE_VALIDATION_LAYERS
    if (!checkValidationLayerSupport()) {
        fprintf(stderr, "Validation layers requested but not available!\n");
        exit(EXIT_FAILURE);
    }
#endif

    VkApplicationInfo appInfo = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Vulkan Triangle",
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

    const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    VkPhysicalDeviceFeatures deviceFeatures = {0};

    VkDeviceCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
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

void createGraphicsPipeline(VulkanContext* context) {
    // Vertex shader
    VkShaderModule vertShaderModule;
    {
        VkShaderModuleCreateInfo createInfo = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(vert_spv),
            .pCode = (const uint32_t*)vert_spv
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
            .codeSize = sizeof(frag_spv),
            .pCode = (const uint32_t*)frag_spv
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

    VkVertexInputAttributeDescription attributeDescriptions[2] = {
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
        }
    };

    VkPipelineVertexInputStateCreateInfo vertexInputInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &bindingDescription,
        .vertexAttributeDescriptionCount = 2,
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
        .blendEnable = VK_FALSE
    };

    VkPipelineColorBlendStateCreateInfo colorBlending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .attachmentCount = 1,
        .pAttachments = &colorBlendAttachment
    };

    // Pipeline layout
    VkPipelineLayoutCreateInfo pipelineLayoutInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &context->descriptorSetLayout
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
    vkDeviceWaitIdle(context->device); // Wait for all operations to complete

    renderer_shutdown();
    
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




void mouse_callback(GLFWwindow* window, double xpos, double ypos) {
    if (firstMouse) {
        lastX = xpos;
        lastY = ypos;
        firstMouse = false;
    }
    double xoffset = xpos - lastX;
    double yoffset = lastY - ypos; // reversed for natural feel
    lastX = xpos;
    lastY = ypos;

    Camera* cam = glfwGetWindowUserPointer(window);
    camera_process_mouse(cam, xoffset, yoffset);
}




uint32_t findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    fprintf(stderr, "Failed to find suitable memory type!\n");
    exit(EXIT_FAILURE);
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
        .memoryTypeIndex = 0 // you'll need a function to find this
    };

    // Find suitable memory type (implement your own findMemoryType)
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

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, context->graphicsPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, context->pipelineLayout, 0, 1, &descriptorSet, 0, NULL);

    renderer_draw(cmd);

    vkCmdEndRenderPass(cmd);

    vkEndCommandBuffer(cmd);
}





vec4 red    = {1.0f, 0.0f, 0.0f, 0.5f};
vec4 green  = {0.0f, 1.0f, 0.0f, 1.0f};
vec4 blue   = {0.0f, 0.0f, 1.0f, 1.0f};
vec4 yellow = {1.0f, 1.0f, 0.0f, 1.0f};



int main() {
    VulkanContext context = {0};
    context.currentFrame = 0;

    initWindow(&context);
    glfwSetInputMode(context.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    createInstance(&context);

    Camera camera;
    vec3 camera_pos = {0.0f, 2.0f, 0.0f}; // 2 blocks up
    vec3 camera_target = {0.0f, 2.0f, 0.0f}; // looking at world center, player-eye level
    camera_init(&camera, camera_pos, 90.0f, 0.0f, (float)WIDTH / (float)HEIGHT);


    glfwSetWindowUserPointer(context.window, &camera);
    glfwSetCursorPosCallback(context.window, mouse_callback);



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
             blocks[x][y][z].pos[0] = (x - WORLD_WIDTH  / 2) + 0.5f;
             blocks[x][y][z].pos[1] = (y - WORLD_HEIGHT / 2) + 0.5f;
             blocks[x][y][z].pos[2] = (z - WORLD_DEPTH  / 2) + 0.5f;
             blocks[x][y][z].color[0] = (float)rand() / (float)RAND_MAX;
             blocks[x][y][z].color[1] = (float)rand() / (float)RAND_MAX;
             blocks[x][y][z].color[2] = (float)rand() / (float)RAND_MAX;
             blocks[x][y][z].color[3] = 1.0f;
         }

 for (int x = 0; x < WORLD_WIDTH;  ++x)
     for (int z = 0; z < WORLD_DEPTH; ++z) {
         int y = 0; // ground level
         blocks[x][y][z].pos[0] = (x - WORLD_WIDTH  / 2) + 0.5f;
         blocks[x][y][z].pos[1] = (y - WORLD_HEIGHT / 2) + 0.5f;
         blocks[x][y][z].pos[2] = (z - WORLD_DEPTH  / 2) + 0.5f;
         blocks[x][y][z].color[0] = (float)rand() / (float)RAND_MAX;
         blocks[x][y][z].color[1] = (float)rand() / (float)RAND_MAX;
         blocks[x][y][z].color[2] = (float)rand() / (float)RAND_MAX;
         blocks[x][y][z].color[3] = 1.0f;
     }




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

        /* vec3 v0 = { -0.5f, -0.5f, 0.0f }; // Bottom Left */
        /* vec3 v1 = {  0.5f, -0.5f, 0.0f }; // Bottom Right */
        /* vec3 v2 = {  0.5f,  0.5f, 0.0f }; // Top Right */
        /* vec3 v3 = { -0.5f,  0.5f, 0.0f }; // Top Left */
        /* vec3 center = { 0.0f, 0.0f, 0.0f }; // Center */
        /* triangle(v0, center, v1, red);    // Bottom */
        /* triangle(v1, center, v2, green);  // Right */
        /* triangle(v2, center, v3, blue);   // Top */
        /* triangle(v3, center, v0, yellow); // Left */


        for (int x = 0; x < WORLD_WIDTH;  ++x)
        for (int y = 0; y < WORLD_HEIGHT; ++y)
        for (int z = 0; z < WORLD_DEPTH; ++z)
            cube(blocks[x][y][z].pos, 1.0f, blocks[x][y][z].color);

        renderer_upload();

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
    cleanup(&context);
    
    return EXIT_SUCCESS;
}

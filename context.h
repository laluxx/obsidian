#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

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

    VkFence* imagesInFlight;
    VkSemaphore* renderFinishedSemaphores;
    VkDescriptorSetLayout descriptorSetLayout;

    VkImage depthImage;
    VkDeviceMemory depthImageMemory;
    VkImageView depthImageView;
    VkFormat depthFormat;

    // 2D rendering pipeline
    VkPipeline graphicsPipeline2D;
    VkPipelineLayout pipelineLayout2D;
    

    // Texture rendering pipeline
    VkPipeline graphicsPipelineTextured2D;   // For textured shapes

    // 2D vertex buffer
    VkBuffer vertexBuffer2D;
    VkDeviceMemory vertexBufferMemory2D;
    VkPipelineLayout pipelineLayoutTextured2D;
    
    // 2D descriptor set layout for textures - ADD THESE
    VkDescriptorSetLayout descriptorSetLayout2D;
    VkDescriptorPool descriptorPool2D;
    VkDescriptorSet descriptorSet2D;


    VkPipeline graphicsPipelineTextured3D;
    VkPipelineLayout pipelineLayoutTextured3D;

} VulkanContext;

extern VulkanContext context;

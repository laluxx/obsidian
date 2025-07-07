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

    VkFence* imagesInFlight; // size: swapChainImageCount
    VkSemaphore* renderFinishedSemaphores; // size = swapChainImageCount
    VkDescriptorSetLayout descriptorSetLayout;

    VkImage depthImage;
    VkDeviceMemory depthImageMemory;
    VkImageView depthImageView;
    VkFormat depthFormat;
} VulkanContext;


extern VulkanContext context;

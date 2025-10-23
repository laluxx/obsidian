#pragma once

#include <vulkan/vulkan_core.h>

typedef struct {
    float r;
    float g;
    float b;
    float a;
} Color;

#define BLACK   ((Color){0.0f, 0.0f, 0.0f, 1.0f})
#define WHITE   ((Color){1.0f, 1.0f, 1.0f, 1.0f})
#define RED     ((Color){1.0f, 0.0f, 0.0f, 1.0f})
#define GREEN   ((Color){0.0f, 1.0f, 0.0f, 1.0f})
#define BLUE    ((Color){0.0f, 0.0f, 1.0f, 1.0f})
#define YELLOW  ((Color){1.0f, 1.0f, 0.0f, 1.0f})
#define CYAN    ((Color){0.0f, 1.0f, 1.0f, 1.0f})
#define MAGENTA ((Color){1.0f, 0.0f, 1.0f, 1.0f})
#define GRAY    ((Color){0.5f, 0.5f, 0.5f, 1.0f})


Color hexToColor(const char *hex);



uint32_t findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties);

void copyBufferToImage(VkCommandBuffer commandBuffer, VkBuffer buffer, VkImage image, uint32_t width, uint32_t height);
void transitionImageLayout(VkCommandBuffer commandBuffer, VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout);
void endSingleTimeCommands(VkDevice device, VkCommandPool commandPool, VkQueue queue, VkCommandBuffer commandBuffer);
VkCommandBuffer beginSingleTimeCommands(VkDevice device, VkCommandPool commandPool);

#include "common.h"
#include <stdio.h>
#include <stdlib.h>

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


///

VkCommandBuffer beginSingleTimeCommands(VkDevice device, VkCommandPool commandPool) {
    VkCommandBufferAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandPool = commandPool,
        .commandBufferCount = 1
    };

    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer);

    VkCommandBufferBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };

    vkBeginCommandBuffer(commandBuffer, &beginInfo);
    return commandBuffer;
}

void endSingleTimeCommands(VkDevice device, VkCommandPool commandPool, VkQueue queue, VkCommandBuffer commandBuffer) {
    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &commandBuffer
    };

    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
}

void transitionImageLayout(VkCommandBuffer commandBuffer, VkImage image, VkFormat format, 
                          VkImageLayout oldLayout, VkImageLayout newLayout) {
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = oldLayout,
        .newLayout = newLayout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1
        }
    };

    VkPipelineStageFlags sourceStage;
    VkPipelineStageFlags destinationStage;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else {
        fprintf(stderr, "Unsupported layout transition!\n");
        return;
    }

    vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0, 0, NULL, 0, NULL, 1, &barrier);
}

void copyBufferToImage(VkCommandBuffer commandBuffer, VkBuffer buffer, VkImage image, uint32_t width, uint32_t height) {
    VkBufferImageCopy region = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1
        },
        .imageOffset = {0, 0, 0},
        .imageExtent = {width, height, 1}
    };

    vkCmdCopyBufferToImage(commandBuffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}


#include <string.h>
#include <math.h>


Color hexToColor(const char *hex) {
    int r, g, b, a = 255;
    size_t len = strlen(hex);
    
    if (len == 9) { // #RRGGBBAA
        sscanf(hex, "#%02x%02x%02x%02x", &r, &g, &b, &a);
    } else if (len == 7) { // #RRGGBB
        sscanf(hex, "#%02x%02x%02x", &r, &g, &b);
    } else { // Default to red if invalid ERROR
        return (Color){1.0f, 0.0f, 0.0f, 1.0f};
    }
    
    // Convert from 0-255 to 0.0-1.0
    float rf = r / 255.0f;
    float gf = g / 255.0f;
    float bf = b / 255.0f;
    float af = a / 255.0f;
    
    // Convert sRGB to linear
    rf = (rf <= 0.04045f) ? rf / 12.92f : powf((rf + 0.055f) / 1.055f, 2.4f);
    gf = (gf <= 0.04045f) ? gf / 12.92f : powf((gf + 0.055f) / 1.055f, 2.4f);
    bf = (bf <= 0.04045f) ? bf / 12.92f : powf((bf + 0.055f) / 1.055f, 2.4f);
    
    return (Color){rf, gf, bf, af};
}

/* Color hexToColor(const char *hex) { */
/*     int r, g, b, a = 255; */
/*     size_t len = strlen(hex); */
    
/*     if (len == 9) { // #RRGGBBAA */
/*         sscanf(hex, "#%02x%02x%02x%02x", &r, &g, &b, &a); */
/*     } else if (len == 7) { // #RRGGBB */
/*         sscanf(hex, "#%02x%02x%02x", &r, &g, &b); */
/*     } else { // Default to red if invalid ERROR */
/*         return (Color){1.0f, 0.0f, 0.0f, 1.0f}; */
/*     } */
    
/*     // Convert from 0-255 to 0.0-1.0 */
/*     return (Color){ */
/*         r / 255.0f, */
/*         g / 255.0f, */
/*         b / 255.0f, */
/*         a / 255.0f */
/*     }; */
/* } */


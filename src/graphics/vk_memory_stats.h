#pragma once

#include "context.h"
#include "allocation_owner.h"

#include <stdbool.h>
#include <stdint.h>

#define OBS_VK_MEMORY_MAX_HEAPS VK_MAX_MEMORY_HEAPS

typedef struct {
    uint32_t heap_count;
    uint64_t live_bytes[OBS_VK_MEMORY_MAX_HEAPS];
    uint64_t peak_bytes[OBS_VK_MEMORY_MAX_HEAPS];
    uint64_t live_allocations[OBS_VK_MEMORY_MAX_HEAPS];
    uint64_t peak_allocations[OBS_VK_MEMORY_MAX_HEAPS];
    uint32_t owner_count;
    ObsAllocationOwner owners[OBS_ALLOCATION_OWNER_LIMIT];
} ObsVkMemoryStats;

/* Consumed by the next tracked allocation on this thread. Names are copied. */
void obs_vk_memory_next_owner(const char *name);
bool obs_vk_memory_tracker_init(VkDevice device, VkPhysicalDevice physical_device);
VkResult obs_vk_allocate_memory_tracked(
    VkDevice device, const VkMemoryAllocateInfo *allocate_info,
    const VkAllocationCallbacks *allocator, VkDeviceMemory *memory);
void obs_vk_free_memory_tracked(VkDevice device, VkDeviceMemory memory,
                                const VkAllocationCallbacks *allocator);
bool obs_vk_memory_tracker_snapshot(VkDevice device, ObsVkMemoryStats *stats);
void obs_vk_memory_tracker_destroy(VkDevice device);

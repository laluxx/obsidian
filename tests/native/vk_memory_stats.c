#include "vk_memory_stats.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint64_t next_memory = 1;
static uint32_t allocate_calls;
static uint32_t free_calls;
static bool fail_allocate;

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice physical_device,
    VkPhysicalDeviceMemoryProperties *properties) {
    (void)physical_device;
    memset(properties, 0, sizeof(*properties));
    properties->memoryHeapCount = 2;
    properties->memoryTypeCount = 3;
    properties->memoryTypes[0].heapIndex = 0;
    properties->memoryTypes[1].heapIndex = 1;
    properties->memoryTypes[2].heapIndex = 0;
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(
    VkDevice device, const VkMemoryAllocateInfo *allocate_info,
    const VkAllocationCallbacks *allocator, VkDeviceMemory *memory) {
    (void)device;
    (void)allocate_info;
    (void)allocator;
    if (fail_allocate) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *memory = (VkDeviceMemory)(uintptr_t)next_memory++;
    allocate_calls++;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkFreeMemory(
    VkDevice device, VkDeviceMemory memory,
    const VkAllocationCallbacks *allocator) {
    (void)device;
    (void)memory;
    (void)allocator;
    free_calls++;
}

static VkDeviceMemory allocate(VkDevice device, uint32_t type, uint64_t bytes) {
    VkMemoryAllocateInfo info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = bytes,
        .memoryTypeIndex = type,
    };
    VkDeviceMemory memory = VK_NULL_HANDLE;
    assert(obs_vk_allocate_memory_tracked(device, &info, NULL, &memory) ==
           VK_SUCCESS);
    return memory;
}

int main(void) {
    VkDevice device = (VkDevice)(uintptr_t)1;
    VkPhysicalDevice physical_device = (VkPhysicalDevice)(uintptr_t)2;
    assert(obs_vk_memory_tracker_init(device, physical_device));

    obs_vk_memory_next_owner("geometry");
    VkDeviceMemory heap0 = allocate(device, 0, 1024);
    obs_vk_memory_next_owner("texture");
    VkDeviceMemory heap1 = allocate(device, 1, 2048);
    ObsVkMemoryStats stats = {0};
    assert(obs_vk_memory_tracker_snapshot(device, &stats));
    assert(stats.heap_count == 2);
    assert(stats.live_bytes[0] == 1024 && stats.live_allocations[0] == 1);
    assert(stats.live_bytes[1] == 2048 && stats.live_allocations[1] == 1);
    assert(stats.peak_bytes[0] == 1024 && stats.peak_bytes[1] == 2048);
    assert(stats.owner_count == 2);
    assert(strcmp(stats.owners[0].name, "geometry") == 0);
    assert(stats.owners[0].live_bytes == 1024);
    assert(stats.owners[1].live_bytes == 2048);
    fail_allocate = true;
    obs_vk_memory_next_owner("failed");
    VkMemoryAllocateInfo rejected = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = 4096, .memoryTypeIndex = 0};
    VkDeviceMemory rejected_memory = VK_NULL_HANDLE;
    assert(obs_vk_allocate_memory_tracked(device, &rejected, NULL,
                                         &rejected_memory) ==
           VK_ERROR_OUT_OF_DEVICE_MEMORY);
    assert(obs_vk_memory_tracker_snapshot(device, &stats));
    assert(stats.owner_count == 2 && stats.live_bytes[0] == 1024);
    fail_allocate = false;

    obs_vk_free_memory_tracked(device, heap0, NULL);
    VkDeviceMemory replacement = allocate(device, 2, 512);
    assert(obs_vk_memory_tracker_snapshot(device, &stats));
    assert(stats.live_bytes[0] == 512 && stats.live_allocations[0] == 1);
    assert(stats.peak_bytes[0] == 1024 && stats.peak_allocations[0] == 1);
    assert(stats.owner_count == 3);
    assert(stats.owners[0].live_bytes == 0);
    assert(stats.owners[0].peak_bytes == 1024);
    assert(strcmp(stats.owners[2].name, "unknown") == 0);
    assert(stats.owners[2].live_bytes == 512);

    obs_vk_free_memory_tracked(device, heap1, NULL);
    obs_vk_free_memory_tracked(device, replacement, NULL);
    assert(obs_vk_memory_tracker_snapshot(device, &stats));
    assert(stats.live_bytes[0] == 0 && stats.live_bytes[1] == 0);
    assert(stats.live_allocations[0] == 0 && stats.live_allocations[1] == 0);
    for (uint32_t i = 0; i < stats.owner_count; ++i)
        assert(stats.owners[i].live_bytes == 0 &&
               stats.owners[i].live_allocations == 0);
    assert(stats.peak_bytes[0] == 1024 && stats.peak_bytes[1] == 2048);
    assert(allocate_calls == 3 && free_calls == 3);

    obs_vk_memory_tracker_destroy(device);
    assert(!obs_vk_memory_tracker_snapshot(device, &stats));
    puts("Vulkan memory tracker: heap attribution, reuse, live/peak accounting PASS");
    return 0;
}

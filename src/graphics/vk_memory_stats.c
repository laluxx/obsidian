#include "vk_memory_stats.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    VkDevice device;
    VkDeviceMemory memory;
    uint64_t bytes;
    uint32_t heap_index;
    uint32_t owner_index;
    bool active;
    size_t next_free;
} AllocationRecord;

typedef struct {
    VkDevice device;
    bool enabled;
    uint32_t heap_count;
    uint32_t memory_type_count;
    uint32_t memory_type_heap[VK_MAX_MEMORY_TYPES];
    ObsVkMemoryStats stats;
} DeviceTracker;

#define OBS_VK_MEMORY_TRACKER_DEVICE_LIMIT 4

static pthread_mutex_t tracker_mutex = PTHREAD_MUTEX_INITIALIZER;
static DeviceTracker trackers[OBS_VK_MEMORY_TRACKER_DEVICE_LIMIT];
static AllocationRecord *records;
static size_t record_count;
static size_t record_capacity;
static size_t free_record_head = SIZE_MAX;
static _Thread_local const char *next_owner;

void obs_vk_memory_next_owner(const char *name) {
    next_owner = name;
}

static DeviceTracker *find_tracker(VkDevice device) {
    for (size_t i = 0; i < OBS_VK_MEMORY_TRACKER_DEVICE_LIMIT; ++i)
        if (trackers[i].device == device)
            return &trackers[i];
    return NULL;
}

bool obs_vk_memory_tracker_init(VkDevice device, VkPhysicalDevice physical_device) {
    if (device == VK_NULL_HANDLE || physical_device == VK_NULL_HANDLE)
        return false;

    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);

    pthread_mutex_lock(&tracker_mutex);
    DeviceTracker *tracker = find_tracker(device);
    if (!tracker) {
        for (size_t i = 0; i < OBS_VK_MEMORY_TRACKER_DEVICE_LIMIT; ++i) {
            if (trackers[i].device == VK_NULL_HANDLE) {
                tracker = &trackers[i];
                break;
            }
        }
    }
    if (tracker) {
        memset(tracker, 0, sizeof(*tracker));
        tracker->device = device;
        tracker->enabled = true;
        tracker->heap_count = properties.memoryHeapCount;
        tracker->memory_type_count = properties.memoryTypeCount;
        tracker->stats.heap_count = properties.memoryHeapCount;
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
            tracker->memory_type_heap[i] = properties.memoryTypes[i].heapIndex;
    }
    pthread_mutex_unlock(&tracker_mutex);
    return tracker != NULL;
}

static bool reserve_record(void) {
    if (record_count < record_capacity)
        return true;
    size_t next = record_capacity ? record_capacity * 2 : 256;
    if (next < record_capacity || next > SIZE_MAX / sizeof(*records))
        return false;
    AllocationRecord *grown = realloc(records, next * sizeof(*records));
    if (!grown)
        return false;
    records = grown;
    record_capacity = next;
    return true;
}

static void disable_tracker(DeviceTracker *tracker) {
    tracker->enabled = false;
    memset(&tracker->stats, 0, sizeof(tracker->stats));
}

VkResult obs_vk_allocate_memory_tracked(
    VkDevice device, const VkMemoryAllocateInfo *allocate_info,
    const VkAllocationCallbacks *allocator, VkDeviceMemory *memory) {
    const char *owner_name = next_owner ? next_owner : "unknown";
    next_owner = NULL;
    VkResult result = vkAllocateMemory(
        device, allocate_info, allocator, memory);
    if (result != VK_SUCCESS || !allocate_info || !memory ||
        *memory == VK_NULL_HANDLE)
        return result;

    pthread_mutex_lock(&tracker_mutex);
    DeviceTracker *tracker = find_tracker(device);
    if (tracker && tracker->enabled) {
        uint32_t type = allocate_info->memoryTypeIndex;
        if (type >= tracker->memory_type_count) {
            disable_tracker(tracker);
        } else {
            uint32_t heap = tracker->memory_type_heap[type];
            if (heap >= tracker->heap_count || heap >= OBS_VK_MEMORY_MAX_HEAPS ||
                UINT64_MAX - tracker->stats.live_bytes[heap] <
                    allocate_info->allocationSize ||
                tracker->stats.live_allocations[heap] == UINT64_MAX) {
                disable_tracker(tracker);
            } else {
                uint32_t owner = 0;
                while (owner < tracker->stats.owner_count &&
                       strcmp(tracker->stats.owners[owner].name, owner_name))
                    ++owner;
                if (owner == tracker->stats.owner_count) {
                    if (owner == OBS_ALLOCATION_OWNER_LIMIT ||
                        strlen(owner_name) >= OBS_ALLOCATION_OWNER_NAME_LIMIT) {
                        disable_tracker(tracker);
                        pthread_mutex_unlock(&tracker_mutex);
                        return result;
                    }
                    strcpy(tracker->stats.owners[owner].name, owner_name);
                    ++tracker->stats.owner_count;
                }
                ObsAllocationOwner *owner_stats = &tracker->stats.owners[owner];
                if (UINT64_MAX - owner_stats->live_bytes <
                    allocate_info->allocationSize ||
                    owner_stats->live_allocations == UINT64_MAX) {
                    disable_tracker(tracker);
                    pthread_mutex_unlock(&tracker_mutex);
                    return result;
                }
                size_t record_index;
                if (free_record_head != SIZE_MAX) {
                    record_index = free_record_head;
                    free_record_head = records[record_index].next_free;
                } else if (reserve_record()) {
                    record_index = record_count++;
                } else {
                    disable_tracker(tracker);
                    pthread_mutex_unlock(&tracker_mutex);
                    return result;
                }
                records[record_index] = (AllocationRecord){
                    .device = device,
                    .memory = *memory,
                    .bytes = allocate_info->allocationSize,
                    .heap_index = heap,
                    .owner_index = owner,
                    .active = true,
                    .next_free = SIZE_MAX
                };
                tracker->stats.live_bytes[heap] +=
                    allocate_info->allocationSize;
                tracker->stats.live_allocations[heap]++;
                owner_stats->live_bytes += allocate_info->allocationSize;
                ++owner_stats->live_allocations;
                if (owner_stats->live_bytes > owner_stats->peak_bytes)
                    owner_stats->peak_bytes = owner_stats->live_bytes;
                if (owner_stats->live_allocations > owner_stats->peak_allocations)
                    owner_stats->peak_allocations = owner_stats->live_allocations;
                if (tracker->stats.live_bytes[heap] >
                    tracker->stats.peak_bytes[heap])
                    tracker->stats.peak_bytes[heap] =
                        tracker->stats.live_bytes[heap];
                if (tracker->stats.live_allocations[heap] >
                    tracker->stats.peak_allocations[heap])
                    tracker->stats.peak_allocations[heap] =
                        tracker->stats.live_allocations[heap];
            }
        }
    }
    pthread_mutex_unlock(&tracker_mutex);
    return result;
}

void obs_vk_free_memory_tracked(VkDevice device, VkDeviceMemory memory,
                                const VkAllocationCallbacks *allocator) {
    pthread_mutex_lock(&tracker_mutex);
    DeviceTracker *tracker = find_tracker(device);
    if (tracker && tracker->enabled && memory != VK_NULL_HANDLE) {
        for (size_t i = record_count; i > 0; --i) {
            AllocationRecord *record = &records[i - 1];
            if (!record->active || record->device != device ||
                record->memory != memory)
                continue;
            uint32_t heap = record->heap_index;
            if (heap < tracker->heap_count &&
                tracker->stats.live_bytes[heap] >= record->bytes &&
                tracker->stats.live_allocations[heap] > 0) {
                tracker->stats.live_bytes[heap] -= record->bytes;
                tracker->stats.live_allocations[heap]--;
                ObsAllocationOwner *owner = &tracker->stats.owners[record->owner_index];
                owner->live_bytes -= record->bytes;
                --owner->live_allocations;
            } else {
                disable_tracker(tracker);
            }
            record->active = false;
            record->next_free = free_record_head;
            free_record_head = i - 1;
            break;
        }
    }
    pthread_mutex_unlock(&tracker_mutex);
    vkFreeMemory(device, memory, allocator);
}

bool obs_vk_memory_tracker_snapshot(VkDevice device, ObsVkMemoryStats *stats) {
    if (!stats)
        return false;
    pthread_mutex_lock(&tracker_mutex);
    DeviceTracker *tracker = find_tracker(device);
    bool enabled = tracker && tracker->enabled;
    if (enabled)
        *stats = tracker->stats;
    else
        memset(stats, 0, sizeof(*stats));
    pthread_mutex_unlock(&tracker_mutex);
    return enabled;
}

void obs_vk_memory_tracker_destroy(VkDevice device) {
    pthread_mutex_lock(&tracker_mutex);
    DeviceTracker *tracker = find_tracker(device);
    if (tracker) {
        memset(tracker, 0, sizeof(*tracker));
        for (size_t i = 0; i < record_count; ++i) {
            AllocationRecord *record = &records[i];
            if (record->device == device && record->active) {
                record->active = false;
                record->next_free = free_record_head;
                free_record_head = i;
            }
        }
    }
    bool any_enabled = false;
    for (size_t i = 0; i < OBS_VK_MEMORY_TRACKER_DEVICE_LIMIT; ++i)
        any_enabled |= trackers[i].enabled;
    if (!any_enabled) {
        free(records);
        records = NULL;
        record_count = 0;
        record_capacity = 0;
        free_record_head = SIZE_MAX;
    }
    pthread_mutex_unlock(&tracker_mutex);
}

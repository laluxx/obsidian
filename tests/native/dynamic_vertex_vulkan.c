#include "window.h"
#include "renderer.h"
#include "vulkan_setup.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool fail_next_map;
static bool fail_next_allocate;
VKAPI_ATTR VkResult VKAPI_CALL __real_vkAllocateMemory(
    VkDevice, const VkMemoryAllocateInfo *, const VkAllocationCallbacks *, VkDeviceMemory *);
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkAllocateMemory(VkDevice device,
    const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *allocator,
    VkDeviceMemory *memory) {
    if (fail_next_allocate) {
        fail_next_allocate = false;
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    return __real_vkAllocateMemory(device, info, allocator, memory);
}
VKAPI_ATTR VkResult VKAPI_CALL __real_vkMapMemory(VkDevice, VkDeviceMemory,
    VkDeviceSize, VkDeviceSize, VkMemoryMapFlags, void **);
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkMapMemory(VkDevice device,
    VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size,
    VkMemoryMapFlags flags, void **mapped) {
    if (fail_next_map) {
        fail_next_map = false;
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    return __real_vkMapMemory(device, memory, offset, size, flags, mapped);
}

static void draw(void *arg) {
    uint32_t count = *(uint32_t *)arg;
    Vertex vertex = {0};
    vertex.pos[0] = 1.25f;
    vertex.color[0] = 0.75f;
    assert(append_vertices(NULL, 0) == 0);
    assert(append_vertices(NULL, 1) == UINT32_MAX);
    for (uint32_t i = 0; i < count; ++i) {
        assert(append_vertices(&vertex, 1) == i);
        Vertex *vertices = get_dynamic_vertices();
        assert(vertices && vertices[0].pos[0] == 1.25f);
        assert(vertices[i].color[0] == 0.75f);
    }
    uint32_t capacity = OBS_DYNAMIC_VERTEX_STRIDE(&context);
    if (capacity < MAX_DYNAMIC_VERTICES) {
        /* Force replacement map failure, then prove the old CPU prefix and
         * write cursor remain usable and a retry can publish the growth. */
        uint32_t extra = capacity - count + 1;
        Vertex *tail = calloc(extra, sizeof(*tail));
        assert(tail);
        void *old_mapping = context.dynamicStagingMapped;
        fail_next_map = true;
        assert(append_vertices(tail, extra) == UINT32_MAX);
        assert(!fail_next_map);
        assert(context.dynamicStagingMapped == old_mapping);
        assert(get_dynamic_vertex_count() == count);
        assert(get_dynamic_vertices()[0].pos[0] == 1.25f);
        fail_next_allocate = true;
        assert(append_vertices(tail, extra) == UINT32_MAX);
        assert(!fail_next_allocate);
        assert(context.dynamicStagingMapped == old_mapping);
        assert(get_dynamic_vertex_count() == count);
        assert(append_vertices(tail, extra) == count);
        assert(get_dynamic_vertices()[0].pos[0] == 1.25f);
        free(tail);
    } else {
        assert(append_vertices(&vertex, 1) == UINT32_MAX);
    }
    cube((vec3){0, 0, 0}, 3, WHITE);
}

int main(int argc, char **argv) {
    bool windowed = argc > 1 && strcmp(argv[1], "--window") == 0;
    obs_set_editor_enabled(false);
    if (windowed) assert(initWindow(320, 240, "Dynamic arena lifetime contract"));
    else assert(obs_headless_init(320, 240, true));
    ObsWindow *extra = windowed ? obs_window_create(320, 240, "Second frame owner") : NULL;
    if (windowed) assert(extra);
    uint32_t counts[] = {65, 4097, 17, 65537, 513, MAX_DYNAMIC_VERTICES};
    for (unsigned i = 0; i < sizeof(counts)/sizeof(counts[0]); ++i) {
        if (windowed) {
            ObsWindow *owner = i % 2 ? extra : context.primary;
            obs_begin_frame(owner);
            draw(&counts[i]);
            obs_end_frame(owner);
        } else assert(obs_headless_render_frame(draw, &counts[i]));
    }
    if (extra) obs_window_destroy(extra);
    cleanup(&context);
    puts("dynamic vertices: growth, retained prefix, map-failure recovery, capacity and frame ownership PASS");
    return 0;
}

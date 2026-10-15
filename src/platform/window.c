/// window — platform subsystem
#define _POSIX_C_SOURCE 200809L
#include "window.h"
#include "audio.h"
#include "input.h"
#include "keychords.h"
#include "gltf_loader.h"
#include "camera.h"
#include "theme.h"
#include "vulkan_setup.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "font.h"
#include "audio.h"
#include "easing.h"
#include "editor.h"
#include "editor_frames.h"
#include "gizmo.h"
#include "keychords.h"
#include "vertico.h"
#include "add_menu.h"
#include "physics.h"
#include "../monad/runtime.h"
#include "live_editor.h"
#include "frame_state.h"
#include "frame_api.h"
#include "resize_policy.h"
#include "atomic_file.h"
#include "sdf.h"

/// Implementation

float delta_time;
float last_frame = 0.0f;

static int current_cursor_mode = GLFW_CURSOR_NORMAL;
static bool s_editor_enabled = true;
static bool s_frame_profile_enabled;
static uint32_t s_last_headless_frame_slot;
static ObsFrameProfile s_last_frame_profile;
static ObsFrameProfile s_completed_frame_profile;
static uint64_t s_frame_profile_sequence;
static ObsGpuFrameProfile s_gpu_ready[8];
static size_t s_gpu_ready_head;
static size_t s_gpu_ready_count;

static uint64_t obs_profile_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

void obs_frame_profile_enable(bool enabled) {
    s_frame_profile_enabled = enabled;
    s_frame_profile_sequence = 0;
    memset(&s_last_frame_profile, 0, sizeof(s_last_frame_profile));
    memset(&s_completed_frame_profile, 0, sizeof(s_completed_frame_profile));
}

uint64_t obs_frame_profile_command_scope_begin(void) {
    return s_frame_profile_enabled ? obs_profile_now_ns() : 0;
}

void obs_frame_profile_command_scope_end(ObsFrameCommandScope scope,
                                         uint64_t start_ns) {
    if (!s_frame_profile_enabled || start_ns == 0) return;
    uint64_t elapsed = obs_profile_now_ns() - start_ns;
    switch (scope) {
    case OBS_FRAME_COMMAND_RENDER_GRAPH:
        s_last_frame_profile.command_render_graph_ns += elapsed;
        break;
    case OBS_FRAME_COMMAND_TRANSPARENCY:
        s_last_frame_profile.command_transparency_ns += elapsed;
        break;
    case OBS_FRAME_COMMAND_UI:
        s_last_frame_profile.command_ui_ns += elapsed;
        break;
    }
}

void obs_frame_profile_get(ObsFrameProfile *out) {
    if (out) *out = s_completed_frame_profile;
}

static void obs_gpu_profile_collect_slot(uint32_t frame_index) {
    if (!context.profileTimestampPool ||
        !context.profileQueryPending[frame_index])
        return;

    context.profileQueryPending[frame_index] = false;

    if (s_gpu_ready_count == sizeof(s_gpu_ready) / sizeof(s_gpu_ready[0]))
        return;
    size_t tail = (s_gpu_ready_head + s_gpu_ready_count) %
                  (sizeof(s_gpu_ready) / sizeof(s_gpu_ready[0]));
    ObsGpuFrameProfile profile = {
        .frame_sequence = context.profileFrameSequence[frame_index]
    };
    for (uint32_t stage = 0; stage < OBS_GPU_PROFILE_STAGE_COUNT; ++stage) {
        uint64_t timestamps[2] = {0};
        uint32_t query_base = obs_gpu_profile_query_index(
            frame_index, (ObsGpuProfileStage)stage, 0);
        if (vkGetQueryPoolResults(
                context.device, context.profileTimestampPool, query_base, 2,
                sizeof(timestamps), timestamps, sizeof(uint64_t),
                VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
            continue;
        if (!obs_gpu_profile_timestamp_delta_ns(
                timestamps[0], timestamps[1],
                context.profileTimestampValidBits,
                context.profileTimestampPeriod, &profile.stage_ns[stage]))
            continue;
        profile.stage_mask |= UINT32_C(1) << stage;
    }
    profile.gpu_ns = profile.stage_ns[OBS_GPU_PROFILE_FRAME];
    if (!(profile.stage_mask & (UINT32_C(1) << OBS_GPU_PROFILE_FRAME)))
        return;
    s_gpu_ready[tail] = profile;
    s_gpu_ready_count++;
}

bool obs_gpu_frame_profile_supported(uint32_t *valid_bits,
                                     double *timestamp_period_ns) {
    if (valid_bits) *valid_bits = 0;
    if (timestamp_period_ns) *timestamp_period_ns = 0.0;
    if (!context.device || !context.physicalDevice) return false;

    VkPhysicalDeviceProperties device_properties;
    vkGetPhysicalDeviceProperties(context.physicalDevice, &device_properties);
    if (!device_properties.limits.timestampComputeAndGraphics ||
        !(device_properties.limits.timestampPeriod > 0.0f))
        return false;

    uint32_t queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(context.physicalDevice,
                                             &queue_count, NULL);
    VkQueueFamilyProperties *queue_properties =
        calloc(queue_count, sizeof(*queue_properties));
    if (!queue_properties) return false;
    vkGetPhysicalDeviceQueueFamilyProperties(context.physicalDevice,
                                             &queue_count, queue_properties);
    if (context.graphicsQueueFamily >= queue_count ||
        !(queue_properties[context.graphicsQueueFamily].queueFlags &
          VK_QUEUE_GRAPHICS_BIT)) {
        free(queue_properties);
        return false;
    }
    uint32_t bits =
        queue_properties[context.graphicsQueueFamily].timestampValidBits;
    free(queue_properties);
    if (!bits || bits > 64) return false;
    if (valid_bits) *valid_bits = bits;
    if (timestamp_period_ns)
        *timestamp_period_ns = device_properties.limits.timestampPeriod;
    return true;
}

bool obs_gpu_frame_profile_enable(void) {
    if (context.profileTimestampPool) return true;
    uint32_t valid_bits = 0;
    double timestamp_period = 0.0;
    if (!obs_gpu_frame_profile_supported(&valid_bits, &timestamp_period))
        return false;

    VkQueryPoolCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = OBS_GPU_PROFILE_QUERY_COUNT(MAX_FRAMES_IN_FLIGHT)
    };
    if (vkCreateQueryPool(context.device, &create_info, NULL,
                          &context.profileTimestampPool) != VK_SUCCESS)
        return false;
    obs_vk_name_object(&context, VK_OBJECT_TYPE_QUERY_POOL,
        (uint64_t)(uintptr_t)context.profileTimestampPool,
        "Obsidian GPU frame timestamp queries");

    context.profileTimestampValidBits = valid_bits;
    context.profileTimestampPeriod = timestamp_period;
    context.profileGeometryUploadGpuNs = 0;
    context.profileGeometryUploadGpuSampled = false;
    memset(context.profileQueryPending, 0, sizeof(context.profileQueryPending));
    memset(context.profileFrameSequence, 0, sizeof(context.profileFrameSequence));
    memset(context.profileStageNs, 0, sizeof(context.profileStageNs));
    memset(context.profileStageMask, 0, sizeof(context.profileStageMask));
    s_gpu_ready_head = 0;
    s_gpu_ready_count = 0;
    return true;
}

bool obs_gpu_frame_profile_take(ObsGpuFrameProfile *out) {
    if (!out || s_gpu_ready_count == 0) return false;
    *out = s_gpu_ready[s_gpu_ready_head];
    s_gpu_ready_head = (s_gpu_ready_head + 1) %
                       (sizeof(s_gpu_ready) / sizeof(s_gpu_ready[0]));
    s_gpu_ready_count--;
    return true;
}

void obs_gpu_frame_profile_flush(void) {
    if (!context.device || !context.profileTimestampPool) return;
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        obs_gpu_profile_collect_slot(i);
}

void obs_gpu_frame_profile_destroy(void) {
    if (context.device && context.profileTimestampPool)
        vkDestroyQueryPool(context.device, context.profileTimestampPool, NULL);
    context.profileTimestampPool = VK_NULL_HANDLE;
    context.profileTimestampValidBits = 0;
    context.profileTimestampPeriod = 0.0;
    context.profileGeometryUploadGpuNs = 0;
    context.profileGeometryUploadGpuSampled = false;
    memset(context.profileQueryPending, 0, sizeof(context.profileQueryPending));
    s_gpu_ready_head = 0;
    s_gpu_ready_count = 0;
}

void obs_set_editor_enabled(bool enabled) {
    if (context.window) {
        fprintf(stderr, "obs_set_editor_enabled must be called before initWindow\n");
        return;
    }
    s_editor_enabled = enabled;
}

bool obs_editor_enabled(void) {
    return s_editor_enabled;
}

extern vec3 orbitPivot;
extern float orbitDistance;

static void open_add_menu_at_cursor(void) {
    double mx, my;
    getCursorPos(context.window, &mx, &my);
    add_menu_open(mx, my);
}


// ── ObsWindow: multi-window API ──────────────────────────────────────────────
// The engine supports any number of OS windows.  Device-global state lives in
// VulkanContext and is created once (by initWindow on the primary window);
// each ObsWindow owns only its surface, swapchain, depth buffer, command
// buffers and image sync.  Frames are drawn sequentially across windows and
// the global frame slot advances per window frame (see context.h).

ObsWindow *s_event_window = NULL;          // window currently processing input

ObsWindow *obs_current_event_window(void) { return s_event_window; }

static void obs_window_register(ObsWindow *w) {
    w->next      = context.windows;
    context.windows = w;
}

static void obs_window_unregister(ObsWindow *w) {
    ObsWindow **pp = &context.windows;
    while (*pp && *pp != w) pp = &(*pp)->next;
    if (*pp) *pp = w->next;
}

static void obs_window_install_callbacks(GLFWwindow *win) {
    glfwSetCharCallback(win, internal_char_callback);
    glfwSetKeyCallback(win, internal_key_callback);
    glfwSetMouseButtonCallback(win, internal_mouse_button_callback);
    glfwSetCursorPosCallback(win, internal_cursor_position_callback);
    glfwSetScrollCallback(win, internal_scroll_callback);
    glfwSetFramebufferSizeCallback(win, internal_window_resize_callback);
    glfwSetWindowFocusCallback(win, internal_window_focus_callback);
    glfwSetWindowCloseCallback(win, internal_window_close_callback);
    glfwSetWindowPosCallback(win, internal_window_pos_callback);
    glfwSetDropCallback(win, internal_drop_callback);
}

// GLFW half: usable before the Vulkan instance exists.
ObsWindow *obs_window_create_glfw(int width, int height, const char *title) {
    ObsWindow *w = calloc(1, sizeof(ObsWindow));
    if (!w) return NULL;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    w->window = glfwCreateWindow(width, height, title, NULL, NULL);
    if (!w->window) { free(w); return NULL; }

    glfwMakeContextCurrent(w->window);
    obs_window_install_callbacks(w->window);
    obs_window_register(w);
    return w;
}

// Vulkan half, stage 1: the surface (needs the instance).
bool obs_window_create_surface(ObsWindow *w, VulkanContext *ctx) {
    if (glfwCreateWindowSurface(ctx->instance, w->window, NULL, &w->surface) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create window surface\n");
        return false;
    }
    return true;
}

// Vulkan half, stage 2: everything that needs the device.
bool obs_window_init_swapchain(ObsWindow *w, VulkanContext *ctx) {
    createSwapChain(ctx, w);
    createImageViews(ctx, w);
    createDepthResources(ctx, w);
    createWindowSyncObjects(ctx, w);

    context.drawExtent = w->swapChainExtent;
    return true;
}

// Command buffers come from the shared command pool, which is created after
// the device — call this once the pool exists.
bool obs_window_init_command_buffers(ObsWindow *w, VulkanContext *ctx) {
    createCommandBuffers(ctx, w);
    return true;
}

// Convenience for windows created after the device exists (e.g. a new editor
// frame): GLFW + surface + swapchain in one call.
static ObsWindow *obs_window_create_with_visibility(int width, int height,
                                                   const char *title,
                                                   bool visible) {
    if (!context.device) {
        fprintf(stderr, "obs_window_create: device not initialized; use "
                        "obs_window_create_glfw for the primary window\n");
        return NULL;
    }
    glfwWindowHint(GLFW_VISIBLE, visible ? GLFW_TRUE : GLFW_FALSE);
    ObsWindow *w = obs_window_create_glfw(width, height, title);
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    if (!w) return NULL;
    if (!obs_window_create_surface(w, &context) ||
        !obs_window_init_swapchain(w, &context) ||
        !obs_window_init_command_buffers(w, &context)) {
        obs_window_destroy(w);
        return NULL;
    }
    return w;
}

ObsWindow *obs_window_create(int width, int height, const char *title) {
    return obs_window_create_with_visibility(width, height, title, true);
}

ObsWindow *obs_window_create_hidden(int width, int height, const char *title) {
    return obs_window_create_with_visibility(width, height, title, false);
}

void obs_window_destroy(ObsWindow *w) {
    if (!w) return;
    if (s_event_window == w) s_event_window = NULL;
    if (context.device) vkDeviceWaitIdle(context.device);
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (context.frameSlotWindow[i] == w) {
            context.frameSlotWindow[i] = NULL;
            context.frameSlotGeneration[i] = 0;
        }
    }
    obs_window_unregister(w);
    obs_window_wait_swapchain_presentations(w);
    obs_window_drain_retired_swapchains(w);
    cleanupSwapChainResources(&context, w, w->swapChain);
    if (w->surface)  vkDestroySurfaceKHR(context.instance, w->surface, NULL);
    GLFWwindow *destroyed_window = w->window;
    if (w->window) {
        glfwDestroyWindow(w->window);
        w->window = NULL;
    }

    // Keep the legacy aliases valid when an application destroys its current
    // or primary window.  The generic API never relies on these aliases, but
    // older engine modules still do.
    if (context.primary == w) context.primary = context.windows;
    if (context.window == destroyed_window)
        context.window = context.primary ? context.primary->window : NULL;
    free(w);
}

void obs_window_wait_swapchain_presentations(ObsWindow *w) {
    if (!w || !context.device || !w->presentFences) return;
    for (uint32_t i = 0; i < w->swapChainImageCount; i++) {
        if (w->presentFencePending && w->presentFencePending[i] &&
            w->presentFences[i] != VK_NULL_HANDLE) {
            vkWaitForFences(context.device, 1, &w->presentFences[i],
                            VK_TRUE, UINT64_MAX);
            w->presentFencePending[i] = false;
        }
    }
}

static bool retired_generation_in_flight(const ObsRetiredSwapchain *retired) {
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (context.frameSlotWindow[i] == retired->owner &&
            context.frameSlotGeneration[i] == retired->generation)
            return true;
    }
    return false;
}

static bool retired_presentations_complete(ObsWindow *resources) {
    if (!resources->presentFences || !resources->presentFencePending)
        return true;
    for (uint32_t i = 0; i < resources->swapChainImageCount; i++) {
        if (!resources->presentFencePending[i]) continue;
        VkResult status = vkGetFenceStatus(context.device,
                                           resources->presentFences[i]);
        if (status == VK_SUCCESS)
            resources->presentFencePending[i] = false;
        else
            return false;
    }
    return true;
}

void obs_window_collect_retired_swapchains(ObsWindow *w) {
    if (!w || !context.device) return;
    ObsRetiredSwapchain **link = &w->retiredSwapchains;
    while (*link) {
        ObsRetiredSwapchain *retired = *link;
        if (retired_generation_in_flight(retired) ||
            !retired_presentations_complete(&retired->resources)) {
            link = &retired->next;
            continue;
        }
        *link = retired->next;
        cleanupSwapChainResources(&context, &retired->resources,
                                  retired->resources.swapChain);
        free(retired);
    }
}

void obs_window_drain_retired_swapchains(ObsWindow *w) {
    if (!w || !context.device) return;
    ObsRetiredSwapchain *retired = w->retiredSwapchains;
    w->retiredSwapchains = NULL;
    while (retired) {
        ObsRetiredSwapchain *next = retired->next;
        obs_window_wait_swapchain_presentations(&retired->resources);
        cleanupSwapChainResources(&context, &retired->resources,
                                  retired->resources.swapChain);
        free(retired);
        retired = next;
    }
}

GLFWwindow *obs_window_glfw(ObsWindow *w) { return w ? w->window : NULL; }

ObsWindow *obs_window_from_glfw(GLFWwindow *win) {
    for (ObsWindow *w = context.windows; w; w = w->next)
        if (w->window == win) return w;
    return NULL;
}

uint32_t obs_window_extent_width(ObsWindow *w)  { return w ? w->swapChainExtent.width  : 0; }
uint32_t obs_window_extent_height(ObsWindow *w) { return w ? w->swapChainExtent.height : 0; }

bool obs_window_should_close(ObsWindow *w) {
    return w ? glfwWindowShouldClose(w->window) : true;
}

void obs_window_focus(ObsWindow *w) {
    if (w) glfwFocusWindow(w->window);
}

void obs_window_set_current(ObsWindow *w) {
    context.window = w ? w->window : NULL;
}

extern void setWindowResizeIncrements_x11(GLFWwindow *window, int char_width, int char_height,
                                          int min_width, int min_height);

void obs_window_set_resize_increments(ObsWindow *w, int cw, int ch, int minw, int minh) {
#if defined(__linux__) || defined(__unix__)
    if (w) setWindowResizeIncrements_x11(w->window, cw, ch, minw, minh);
#else
    (void)w; (void)cw; (void)ch; (void)minw; (void)minh;
#endif
}

void obs_begin_frame(ObsWindow *w) {
    if (!w) return;
    if (s_frame_profile_enabled)
        memset(&s_last_frame_profile, 0, sizeof(s_last_frame_profile));
    static uint64_t s_last_timing_tick;
    const uint64_t editor_tick = obs_editor_frames_tick();
    if (!obs_editor_frames_iteration_active() ||
        editor_tick != s_last_timing_tick) {
        float current_frame = getTime();
        delta_time = current_frame - last_frame;
        last_frame = current_frame;
        s_last_timing_tick = editor_tick;
    }

    // Wait for the GPU to finish with this frame SLOT before any CPU-mapped
    // buffer is touched.  The slot is global and advances once per window
    // frame, so this also serializes consecutive windows correctly.
    uint32_t frameIndex = context.currentFrame;
    VkFence inFlightFence = context.inFlightFences[frameIndex];
    uint64_t profile_start = s_frame_profile_enabled ? obs_profile_now_ns() : 0;
    VkResult fence_result = vkWaitForFences(context.device, 1, &inFlightFence,
                                             VK_TRUE, UINT64_MAX);
    if (fence_result != VK_SUCCESS) {
        fprintf(stderr,
                "Obsidian: frame-slot fence failed (VkResult=%d, frame=%u), "
                "refusing mapped writes\n",
                (int)fence_result, frameIndex);
        abort();
    }
    context.frameSlotWindow[frameIndex] = NULL;
    context.frameSlotGeneration[frameIndex] = 0;
    if (context.profileTimestampPool)
        obs_gpu_profile_collect_slot(frameIndex);
    if (s_frame_profile_enabled)
        s_last_frame_profile.begin_fence_wait_ns = obs_profile_now_ns() - profile_start;
    if (s_frame_profile_enabled)
        s_last_frame_profile.frame_sequence = s_frame_profile_sequence++;

    profile_start = s_frame_profile_enabled ? obs_profile_now_ns() : 0;
    obs_window_collect_retired_swapchains(w);
    if (w->suspended) {
        int framebuffer_width = 0, framebuffer_height = 0;
        glfwGetFramebufferSize(w->window, &framebuffer_width, &framebuffer_height);
        if (framebuffer_width > 0 && framebuffer_height > 0) {
            w->suspended = false;
            w->swapchainOutOfDate = true;
        }
    }
    const double seconds_since_resize = getTime() - w->resizeLastEventTime;
    if (obs_resize_rebuild_due(w->swapchainOutOfDate, w->framebufferResized,
                               seconds_since_resize))
        recreateSwapChain(&context, w);
    if (s_frame_profile_enabled)
        s_last_frame_profile.swapchain_maintenance_ns =
            obs_profile_now_ns() - profile_start;

    profile_start = s_frame_profile_enabled ? obs_profile_now_ns() : 0;
    if (w->render2DOnly) begin_frame_2d();
    else begin_frame();
    renderer2D_clear();
    if (s_frame_profile_enabled)
        s_last_frame_profile.renderer_setup_ns =
            obs_profile_now_ns() - profile_start;
    if (w->suspended) {
        context.drawExtent = (VkExtent2D){0, 0};
        return;
    }
    context.drawExtent = w->swapChainExtent;
    profile_start = s_frame_profile_enabled ? obs_profile_now_ns() : 0;
    if (!w->render2DOnly)
        updateUniformBuffer(&context);
    if (s_frame_profile_enabled)
        s_last_frame_profile.uniform_update_ns =
            obs_profile_now_ns() - profile_start;
    /* Frame capabilities are published only after CPU renderer setup. */
    profile_start = s_frame_profile_enabled ? obs_profile_now_ns() : 0;
    obs_frame_state_begin(w->swapChainExtent.width,
                          w->swapChainExtent.height,
                          context.currentFrame, (double)delta_time);
    if (s_frame_profile_enabled)
        s_last_frame_profile.frame_capability_ns =
            obs_profile_now_ns() - profile_start;
    /* One compiled Monad callback per CPU frame, with a scoped Frame value. */
    profile_start = s_frame_profile_enabled ? obs_profile_now_ns() : 0;
    obs_monad_frame_tick();
    if (s_frame_profile_enabled)
        s_last_frame_profile.monad_callback_ns =
            obs_profile_now_ns() - profile_start;
}

void obs_end_frame(ObsWindow *w) {
    if (!w) return;
    /* End the borrow even if swapchain recreation causes an early return. */
    obs_frame_state_end();
    if (w->suspended) {
        context.drawExtent = (VkExtent2D){0, 0};
        if (s_frame_profile_enabled)
            s_completed_frame_profile = s_last_frame_profile;
        return;
    }

    /* Slot fence was waited in obs_begin_frame. Publish live SDF writes
     * only into this frame-owned mapped SSBO before recording commands. */
    if (!w->render2DOnly && !obs_sdf_sync_frame(context.currentFrame)) {
        fprintf(stderr, "Obsidian: SDF slot upload rejected\n");
        abort();
    }
    // Upload all geometry accumulated since obs_begin_frame()
    uint64_t profile_start = s_frame_profile_enabled ? obs_profile_now_ns() : 0;
    if (s_frame_profile_enabled) {
        s_last_frame_profile.vertex2d_count = vertexCount2D;
        s_last_frame_profile.vertex2d_data_bytes =
            (uint64_t)vertexCount2D * sizeof(Vertex2D);
    }
    if (!w->render2DOnly)
        line_renderer_upload();
    renderer2D_upload();
    if (s_frame_profile_enabled)
        s_last_frame_profile.geometry_upload_ns = obs_profile_now_ns() - profile_start;

    uint32_t frameIndex = context.currentFrame;
    VkFence  inFlightFence = context.inFlightFences[frameIndex];

    uint32_t imageIndex;
    profile_start = s_frame_profile_enabled ? obs_profile_now_ns() : 0;
    VkResult result = vkAcquireNextImageKHR(
        context.device, w->swapChain, UINT64_MAX,
        context.imageAvailableSemaphores[frameIndex], VK_NULL_HANDLE, &imageIndex
    );
    if (s_frame_profile_enabled)
        s_last_frame_profile.acquire_image_ns = obs_profile_now_ns() - profile_start;
    if (result == VK_SUBOPTIMAL_KHR && !w->framebufferResized) {
        w->framebufferResized = true;
        w->resizeLastEventTime = getTime();
    }
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        w->swapchainOutOfDate = true;
        if (s_frame_profile_enabled)
            s_completed_frame_profile = s_last_frame_profile;
        return;
    } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        fprintf(stderr, "Failed to acquire swap chain image\n");
        exit(EXIT_FAILURE);
    }

    if (w->imagesInFlight[imageIndex] != VK_NULL_HANDLE) {
        profile_start = s_frame_profile_enabled ? obs_profile_now_ns() : 0;
        vkWaitForFences(context.device, 1, &w->imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX);
        if (s_frame_profile_enabled)
            s_last_frame_profile.image_fence_wait_ns = obs_profile_now_ns() - profile_start;
    }
    if (w->presentFences && w->presentFencePending[imageIndex]) {
        vkWaitForFences(context.device, 1, &w->presentFences[imageIndex],
                        VK_TRUE, UINT64_MAX);
        w->presentFencePending[imageIndex] = false;
    }
    w->imagesInFlight[imageIndex] = inFlightFence;

    vkResetFences(context.device, 1, &inFlightFence);

    profile_start = s_frame_profile_enabled ? obs_profile_now_ns() : 0;
    recordCommandBuffer(&context, w, imageIndex);
    if (s_frame_profile_enabled)
        s_last_frame_profile.command_record_ns = obs_profile_now_ns() - profile_start;
    VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    VkSubmitInfo submitInfo = {
        .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount   = 1,
        .pWaitSemaphores      = &context.imageAvailableSemaphores[frameIndex],
        .pWaitDstStageMask    = waitStages,
        .commandBufferCount   = 1,
        .pCommandBuffers      = &w->commandBuffers[imageIndex],
        .signalSemaphoreCount = 1,
        .pSignalSemaphores    = &w->renderFinishedSemaphores[imageIndex]
    };
    profile_start = s_frame_profile_enabled ? obs_profile_now_ns() : 0;
    if (vkQueueSubmit(context.graphicsQueue, 1, &submitInfo, inFlightFence) != VK_SUCCESS) {
        fprintf(stderr, "Failed to submit draw command buffer\n");
        exit(EXIT_FAILURE);
    }
    shadowCacheFrameSubmitted(&context);
    context.frameSlotWindow[frameIndex] = w;
    context.frameSlotGeneration[frameIndex] = w->swapchainGeneration;
    if (context.profileTimestampPool) {
        context.profileQueryPending[frameIndex] = true;
        context.profileFrameSequence[frameIndex] =
            s_frame_profile_enabled ? s_last_frame_profile.frame_sequence : 0;
    }
    if (s_frame_profile_enabled)
        s_last_frame_profile.queue_submit_ns = obs_profile_now_ns() - profile_start;

    VkPresentInfoKHR presentInfo = {
        .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores    = &w->renderFinishedSemaphores[imageIndex],
        .swapchainCount     = 1,
        .pSwapchains        = &w->swapChain,
        .pImageIndices      = &imageIndex
    };
    VkSwapchainPresentFenceInfoKHR presentFenceInfo = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR,
        .swapchainCount = 1,
        .pFences = w->presentFences ? &w->presentFences[imageIndex] : NULL
    };
    if (w->presentFences) {
        vkResetFences(context.device, 1, &w->presentFences[imageIndex]);
        presentInfo.pNext = &presentFenceInfo;
    }
    profile_start = s_frame_profile_enabled ? obs_profile_now_ns() : 0;
    result = vkQueuePresentKHR(context.graphicsQueue, &presentInfo);
    if (w->presentFences &&
        (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR ||
         result == VK_ERROR_OUT_OF_DATE_KHR))
        w->presentFencePending[imageIndex] = true;
    if (s_frame_profile_enabled)
        s_last_frame_profile.queue_present_ns = obs_profile_now_ns() - profile_start;
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        if (result == VK_ERROR_OUT_OF_DATE_KHR)
            w->swapchainOutOfDate = true;
        else if (!w->framebufferResized) {
            w->framebufferResized = true;
            w->resizeLastEventTime = getTime();
        }
    } else if (result != VK_SUCCESS) {
        fprintf(stderr, "Failed to present swap chain image\n");
        exit(EXIT_FAILURE);
    }

    // One global slot step per window frame: the next window (or the next
    // tick of this one) continues on the other slot.
    if (s_frame_profile_enabled)
        s_completed_frame_profile = s_last_frame_profile;
    context.currentFrame = (context.currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
}

bool obs_register_editor_commands(void) {
    bool ok = true;
#define REGISTER_WINDOW_ACTION(name, action) \
    ok = obs_monad_live_editor_register_action((name), (action)) && ok
    REGISTER_WINDOW_ACTION("editor-open-add-menu", open_add_menu_at_cursor);
    REGISTER_WINDOW_ACTION("camera-snap-left", camera_snap_left);
    REGISTER_WINDOW_ACTION("camera-snap-right", camera_snap_right);
    REGISTER_WINDOW_ACTION("camera-snap-up", camera_snap_up);
    REGISTER_WINDOW_ACTION("camera-snap-down", camera_snap_down);
    REGISTER_WINDOW_ACTION("editor-show-keybindings",
                           vertico_show_keybindings);
    REGISTER_WINDOW_ACTION("editor-show-commands", vertico_show_commands);
#undef REGISTER_WINDOW_ACTION
    return ok;
}

GLFWwindow* initWindow(int width, int height, const char* title) {
    if (!glfwInit()) {
        fprintf(stderr, "Failed to initialize GLFW\n");
        return NULL;
    }

    context.currentFrame = 0;

    // The legacy single-window path: the primary window goes through the
    // same ObsWindow machinery everything else uses.
    context.primary = obs_window_create_glfw(width, height, title);
    context.window  = context.primary->window;

    init_input();
    keymap_init(&keymap);
    init_free_type();
    audio_init();

    // Default cursor mode is normal (visible)
    current_cursor_mode = GLFW_CURSOR_NORMAL;
    glfwSetInputMode(context.window, GLFW_CURSOR, current_cursor_mode);

    createInstance(&context);

    vec3 camera_pos = {7.348469f, 6.0f, 7.348469f};
    camera_init(&camera, camera_pos, -135.0f, -30.0f,
                (float)WIDTH / (float)HEIGHT);
    camera.active = false;

    obs_window_create_surface(context.primary, &context);

    pickPhysicalDevice(&context);
    createLogicalDevice(&context);
    obs_window_init_swapchain(context.primary, &context);
    createFrameSyncObjects(&context);
    createShadowResources(&context);

    createUniformBuffer(&context);
    createDescriptorSetLayout(&context);
    create2DDescriptorSetLayout(&context);
    createBindlessDescriptorLayout(&context);
    createBindlessDescriptorPool(&context);
    createBindlessDescriptorSet(&context);
    create2DDescriptorPool(&context);

    createDescriptorPool(&context);
    createDescriptorSet(&context);

    createCommandPool(&context);
    createCommandBuffers(&context, context.primary);

    renderer_init(
        context.device,
        context.physicalDevice,
        context.commandPool,
        context.graphicsQueue
    );

    createMegaVertexBuffer(&context, 4096);
    // The prebuilt sequential-index range for dynamic meshes is 4 MiB.
    createMegaIndexBuffer(&context, 4ULL * 1024 * 1024);
    createMeshSSBO(&context, 64);
    createIndirectBuffer(&context, 4096);

    createLightingDescriptors(&context);
    createAllPipelineLayouts(&context);
    createGraphicsPipelines(&context);
    createComputeCullPipeline(&context);
    createComputeCompactPipeline(&context);

    createUploadStagingBuffer(&context, 64ULL * 1024);

    renderer2D_init();

    line_renderer_init(
        context.device,
        context.physicalDevice,
        context.commandPool,
        context.graphicsQueue
    );

    clear_background((Color){0.0f, 0.0f, 0.0f, 1.0f});

    scene_init(scene);

    /* The editor keymap is a live Monad image; do not silently fall back to
     * stale native editor bindings if that image cannot be created. */
    if (s_editor_enabled && !obs_monad_engine_start()) {
        fprintf(stderr, "Obsidian: required live Monad editor image failed\n");
        return NULL;
    }

    // Default scene lighting — sun from upper-right, moderate ambient
    LightingData* ld = CTX_LIGHTING(&context);
    ld->sun.direction[0] = -0.3f; ld->sun.direction[1] = -0.8f;
    ld->sun.direction[2] = -0.5f; ld->sun.direction[3] =  0.0f;
    ld->sun.color[0]     =  1.0f; ld->sun.color[1]     =  0.95f;
    ld->sun.color[2]     =  0.8f; ld->sun.color[3]     =  3.0f;  // w=intensity
    ld->pointLightCount  = 0;
    ld->ambientIntensity = 0.4f;
    ld->iblEnabled       = 0;
    ld->cameraPos[0]     = 0.0f; ld->cameraPos[1] = 0.0f;
    ld->cameraPos[2]     = 0.0f; ld->cameraPos[3] = 0.0f;

    texture_pool_init();

    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(context.physicalDevice, &properties);
    float maxLineWidth = properties.limits.lineWidthRange[1];
    printf("MAX supported line width: %f\n", maxLineWidth);

    if (s_editor_enabled) {
        initThemes();
        ease_init();
        editor_init();
        gizmo_init();
        vertico_init();
        add_menu_init();
        physics_init();
    }


    if (s_editor_enabled && !obs_register_editor_commands()) {
        fprintf(stderr, "Obsidian: could not register live editor commands\n");
        return NULL;
    }

    if (getenv("OBSIDIAN_MONAD_BOOT")) {
        keychord_bind(&keymap, "C-M-<right>", obs_monad_camera_step,
                      "Compiled Monad camera policy", GLFW_PRESS);
    }
    keychord_bind(&keymap, "<f5>", obs_monad_engine_request_reload,
                  "Reload Monad editor module", GLFW_PRESS);
    obs_monad_engine_boot_from_environment();
    if (s_editor_enabled) keymap_print_bindings(&keymap);
    return context.window;
}

static void headless_target_abort(void) {
    ObsWindow *w = context.primary;
    if (context.device) (void)vkDeviceWaitIdle(context.device);
    if (w && context.device) {
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            if (w->swapChainImageViews && w->swapChainImageViews[i])
                vkDestroyImageView(context.device, w->swapChainImageViews[i],
                                   NULL);
            if (w->rawColorImage[i])
                vkDestroyImage(context.device, w->rawColorImage[i], NULL);
            if (w->rawColorMemory[i])
                vkFreeMemory(context.device, w->rawColorMemory[i], NULL);
            if (w->rawReadbackMapped[i])
                vkUnmapMemory(context.device, w->rawReadbackMemory[i]);
            if (w->rawReadbackBuffer[i])
                vkDestroyBuffer(context.device, w->rawReadbackBuffer[i], NULL);
            if (w->rawReadbackMemory[i])
                vkFreeMemory(context.device, w->rawReadbackMemory[i], NULL);
        }
    }
    free(w ? w->swapChainImages : NULL);
    free(w ? w->swapChainImageViews : NULL);
    if (context.device) {
        obs_vk_memory_tracker_destroy(context.device);
        vkDestroyDevice(context.device, NULL);
    }
    if (context.instance) vkDestroyInstance(context.instance, NULL);
    free(w);
    memset(&context, 0, sizeof(context));
}

bool obs_headless_init(uint32_t width, uint32_t height,
                       bool readback_enabled) {
    if (!width || !height || context.device) return false;
    memset(&context, 0, sizeof(context));
    context.headless = true;
    context.headlessReadbackEnabled = readback_enabled;
    context.primary = calloc(1, sizeof(*context.primary));
    if (!context.primary) return false;
    ObsWindow *w = context.primary;
    w->headless = true;
    w->swapChainExtent = (VkExtent2D){width, height};
    w->swapChainImageFormat = VK_FORMAT_R8G8B8A8_UNORM;
    w->swapChainImageCount = MAX_FRAMES_IN_FLIGHT;
    w->swapChainImages = calloc(w->swapChainImageCount,
                                sizeof(*w->swapChainImages));
    w->swapChainImageViews = calloc(w->swapChainImageCount,
                                    sizeof(*w->swapChainImageViews));
    if (!w->swapChainImages || !w->swapChainImageViews) {
        headless_target_abort();
        return false;
    }
    context.windows = w;

    VkFormatProperties format_properties;
    createInstance(&context);
    pickPhysicalDevice(&context);
    vkGetPhysicalDeviceFormatProperties(context.physicalDevice,
                                        w->swapChainImageFormat,
                                        &format_properties);
    const VkFormatFeatureFlags required_format_features =
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    if ((format_properties.optimalTilingFeatures &
         required_format_features) != required_format_features) {
        headless_target_abort();
        return false;
    }
    createLogicalDevice(&context);
    context.drawExtent = w->swapChainExtent;

    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = w->swapChainImageFormat,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (vkCreateImage(context.device, &image_info, NULL,
                          &w->swapChainImages[i]) != VK_SUCCESS) {
            headless_target_abort();
            return false;
        }
        w->rawColorImage[i] = w->swapChainImages[i];
        char object_name[64];
        snprintf(object_name, sizeof(object_name),
                 "Obsidian headless color target %u", i);
        obs_vk_name_object(&context, VK_OBJECT_TYPE_IMAGE,
            (uint64_t)(uintptr_t)w->rawColorImage[i], object_name);
        VkMemoryRequirements image_requirements;
        vkGetImageMemoryRequirements(context.device, w->swapChainImages[i],
                                     &image_requirements);
        VkMemoryAllocateInfo image_allocation = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = image_requirements.size,
            .memoryTypeIndex = findMemoryType(
                context.physicalDevice, image_requirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
        };
        if (vkAllocateMemory(context.device, &image_allocation, NULL,
                             &w->rawColorMemory[i]) != VK_SUCCESS) {
            headless_target_abort();
            return false;
        }
        if (vkBindImageMemory(context.device, w->swapChainImages[i],
                              w->rawColorMemory[i], 0) != VK_SUCCESS) {
            headless_target_abort();
            return false;
        }
        VkImageViewCreateInfo view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = w->swapChainImages[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = w->swapChainImageFormat,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}
        };
        if (vkCreateImageView(context.device, &view_info, NULL,
                              &w->swapChainImageViews[i]) != VK_SUCCESS) {
            headless_target_abort();
            return false;
        }

        if (readback_enabled) {
            VkDeviceSize output_size = (VkDeviceSize)width * height * 4;
            VkBufferCreateInfo buffer_info = {
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .size = output_size,
                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE
            };
            if (vkCreateBuffer(context.device, &buffer_info, NULL,
                               &w->rawReadbackBuffer[i]) != VK_SUCCESS) {
                headless_target_abort();
                return false;
            }
            VkMemoryRequirements buffer_requirements;
            vkGetBufferMemoryRequirements(context.device,
                                         w->rawReadbackBuffer[i],
                                         &buffer_requirements);
            VkMemoryAllocateInfo buffer_allocation = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .allocationSize = buffer_requirements.size,
                .memoryTypeIndex = findMemoryType(
                    context.physicalDevice, buffer_requirements.memoryTypeBits,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
            };
            if (vkAllocateMemory(context.device, &buffer_allocation, NULL,
                                 &w->rawReadbackMemory[i]) != VK_SUCCESS) {
                headless_target_abort();
                return false;
            }
            if (vkBindBufferMemory(context.device, w->rawReadbackBuffer[i],
                                   w->rawReadbackMemory[i], 0) != VK_SUCCESS) {
                headless_target_abort();
                return false;
            }
            if (vkMapMemory(context.device, w->rawReadbackMemory[i], 0,
                            output_size, 0,
                            &w->rawReadbackMapped[i]) != VK_SUCCESS) {
                headless_target_abort();
                return false;
            }
        }
    }

    camera_init(&camera, (vec3){7.348469f, 6.0f, 7.348469f},
                -135.0f, -30.0f,
                (float)width / (float)height);
    camera.active = false;
    createFrameSyncObjects(&context);
    createShadowResources(&context);
    createUniformBuffer(&context);
    createDescriptorSetLayout(&context);
    create2DDescriptorSetLayout(&context);
    createBindlessDescriptorLayout(&context);
    createBindlessDescriptorPool(&context);
    createBindlessDescriptorSet(&context);
    create2DDescriptorPool(&context);
    createDescriptorPool(&context);
    createDescriptorSet(&context);
    createCommandPool(&context);
    createDepthResources(&context, w);
    createCommandBuffers(&context, w);
    renderer_init(context.device, context.physicalDevice, context.commandPool,
                  context.graphicsQueue);
    createMegaVertexBuffer(&context, 4096);
    createMegaIndexBuffer(&context, 4ULL * 1024 * 1024);
    createMeshSSBO(&context, 64);
    createIndirectBuffer(&context, 4096);
    createLightingDescriptors(&context);
    createAllPipelineLayouts(&context);
    createGraphicsPipelines(&context);
    createComputeCullPipeline(&context);
    createComputeCompactPipeline(&context);
    createUploadStagingBuffer(&context, 64ULL * 1024);
    renderer2D_init();
    line_renderer_init(context.device, context.physicalDevice,
                       context.commandPool, context.graphicsQueue);
    clear_background((Color){0, 0, 0, 1});
    scene_init(scene);
    texture_pool_init();
    LightingData *lighting = CTX_LIGHTING(&context);
    lighting->sun.direction[0] = -0.3f;
    lighting->sun.direction[1] = -0.8f;
    lighting->sun.direction[2] = -0.5f;
    lighting->sun.color[0] = 1.0f;
    lighting->sun.color[1] = 0.95f;
    lighting->sun.color[2] = 0.8f;
    lighting->sun.color[3] = 3.0f;
    lighting->ambientIntensity = 0.4f;
    return true;
}

bool obs_headless_render_frame(ObsHeadlessDrawCallback draw, void *userdata) {
    ObsWindow *w = context.primary;
    if (!context.headless || !w ||
        (context.headlessReadbackEnabled &&
         !w->rawReadbackMapped[context.currentFrame]))
        return false;
    const bool profiling = s_frame_profile_enabled;
    if (profiling)
        memset(&s_last_frame_profile, 0, sizeof(s_last_frame_profile));
    ObsFrameProfile profile = {0};
    uint32_t frame = context.currentFrame;
    VkFence fence = context.inFlightFences[frame];
    uint64_t phase_start = profiling ? obs_profile_now_ns() : 0;
    if (vkWaitForFences(context.device, 1, &fence, VK_TRUE, UINT64_MAX) !=
        VK_SUCCESS)
        return false;
    if (profiling)
        profile.begin_fence_wait_ns = obs_profile_now_ns() - phase_start;
    obs_gpu_profile_collect_slot(frame);
    if (w->render2DOnly) begin_frame_2d();
    else begin_frame();
    renderer2D_clear();
    context.drawExtent = w->swapChainExtent;
    phase_start = profiling ? obs_profile_now_ns() : 0;
    updateUniformBuffer(&context);
    if (profiling)
        profile.uniform_update_ns = obs_profile_now_ns() - phase_start;
    obs_frame_state_begin(w->swapChainExtent.width, w->swapChainExtent.height,
                          frame, 0.0);
    phase_start = profiling ? obs_profile_now_ns() : 0;
    obs_monad_frame_tick();
    if (profiling)
        profile.monad_callback_ns = obs_profile_now_ns() - phase_start;
    if (draw) draw(userdata);
    obs_frame_state_end();
    if (!obs_sdf_sync_frame(frame)) return false;
    phase_start = profiling ? obs_profile_now_ns() : 0;
    sort_meshes_by_alpha(&scene->meshes, camera.position);
    updateMeshSSBOAndIndirect(&context, &scene->meshes);
    flushMeshSSBO(&context, &scene->meshes);
    if (profiling) profile.mesh_sync_ns = obs_profile_now_ns() - phase_start;
    phase_start = profiling ? obs_profile_now_ns() : 0;
    line_renderer_upload();
    renderer2D_upload();
    if (profiling)
        profile.renderer_setup_ns = obs_profile_now_ns() - phase_start;
    phase_start = profiling ? obs_profile_now_ns() : 0;
    recordCommandBuffer(&context, w, frame);
    if (profiling) {
        profile.command_record_ns = obs_profile_now_ns() - phase_start;
        profile.command_render_graph_ns =
            s_last_frame_profile.command_render_graph_ns;
        profile.command_transparency_ns =
            s_last_frame_profile.command_transparency_ns;
        profile.command_ui_ns = s_last_frame_profile.command_ui_ns;
    }
    if (vkResetFences(context.device, 1, &fence) != VK_SUCCESS) return false;
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &w->commandBuffers[frame]
    };
    if (context.profileTimestampPool) {
        context.profileQueryPending[frame] = true;
        context.profileFrameSequence[frame] = s_frame_profile_sequence++;
    }
    phase_start = profiling ? obs_profile_now_ns() : 0;
    if (vkQueueSubmit(context.graphicsQueue, 1, &submit, fence) != VK_SUCCESS)
        return false;
    shadowCacheFrameSubmitted(&context);
    if (profiling)
        profile.queue_submit_ns = obs_profile_now_ns() - phase_start;
    s_last_headless_frame_slot = frame;
    profile.frame_sequence = context.profileTimestampPool
        ? context.profileFrameSequence[frame] : s_frame_profile_sequence++;
    if (context.headlessReadbackEnabled) {
        phase_start = profiling ? obs_profile_now_ns() : 0;
        if (vkWaitForFences(context.device, 1, &fence, VK_TRUE, UINT64_MAX) !=
            VK_SUCCESS)
            return false;
        if (profiling)
            profile.image_fence_wait_ns = obs_profile_now_ns() - phase_start;
        obs_gpu_profile_collect_slot(frame);
    }
    if (profiling) {
        s_last_frame_profile = profile;
        s_completed_frame_profile = profile;
    }
    context.currentFrame = (frame + 1) % MAX_FRAMES_IN_FLIGHT;
    return true;
}

bool obs_headless_render_raw(const char *path, ObsHeadlessDrawCallback draw,
                             void *userdata) {
    ObsWindow *w = context.primary;
    if (!context.headless || !context.headlessReadbackEnabled ||
        !w || !path || !*path ||
        !obs_headless_render_frame(draw, userdata))
        return false;
    void *readback = w->rawReadbackMapped[s_last_headless_frame_slot];
    if (!readback) return false;
    const VkDeviceSize byte_count =
        (VkDeviceSize)w->swapChainExtent.width * w->swapChainExtent.height * 4;
    if (byte_count > SIZE_MAX) return false;
    return obs_atomic_write_bytes(path, readback, (size_t)byte_count);
}

int windowShouldClose() {
    if (!context.primary || !context.primary->window) {
        fprintf(stderr, "Error: Window not initialized. Call initWindow() first.\n");
        return 1;
    }
    return glfwWindowShouldClose(context.primary->window);
}

void beginFrame() {
    /* Demo/application loop: event delivery and pending Monad reload happen
     * between frames, before GPU recording. Embedders using obs_begin_frame
     * directly retain explicit control of their own event loop. */
    uint64_t profile_start = s_frame_profile_enabled ? obs_profile_now_ns() : 0;
    const bool events_here = !obs_editor_frames_iteration_active();
    if (events_here) obs_poll_events();
    uint64_t event_poll_ns = s_frame_profile_enabled && events_here
        ? obs_profile_now_ns() - profile_start : 0;
    ObsEditorFrame *frame = obs_editor_frame_current();
    ObsWindow *active_window = frame
        ? obs_editor_frame_window(frame) : context.primary;
    obs_begin_frame(active_window ? active_window : context.primary);
    if (s_frame_profile_enabled)
        s_last_frame_profile.event_poll_ns = event_poll_ns;

    if (frame && obs_editor_frame_is_2d_only(frame)) {
        obs_editor_frame_render_surface(frame);
        return;
    }

    /* Everything below is engine-demo state (scene, physics, camera,
     * editor): applications with their own loop — like glemax — call
     * obs_begin_frame/obs_end_frame directly and skip it. */
    profile_start = s_frame_profile_enabled ? obs_profile_now_ns() : 0;
    const uint64_t tick = obs_editor_frames_tick();
    ObsSceneBuffer *buffer = frame ? obs_editor_frame_buffer(frame) : NULL;
    const bool animate = !buffer || obs_scene_buffer_should_animate(buffer, tick);
    if (animate) animate_scene(scene, getTime());
    if (s_frame_profile_enabled && animate)
        s_last_frame_profile.scene_animation_ns =
            obs_profile_now_ns() - profile_start;

    profile_start = s_frame_profile_enabled ? obs_profile_now_ns() : 0;
    const bool simulate = !frame || obs_editor_frame_is_primary(frame);
    if (simulate) physics_step(delta_time);
    if (s_frame_profile_enabled && simulate)
        s_last_frame_profile.physics_ns = obs_profile_now_ns() - profile_start;

    profile_start = s_frame_profile_enabled ? obs_profile_now_ns() : 0;
    camera_process_keyboard(&camera, active_window ? active_window->window
                                                    : context.window,
                            delta_time);
    camera_update(&camera);

    if (s_editor_enabled && !camera.active) {
        process_editor_movement(&camera, delta_time);
    }
    if (s_frame_profile_enabled)
        s_last_frame_profile.camera_ns = obs_profile_now_ns() - profile_start;

    profile_start = s_frame_profile_enabled ? obs_profile_now_ns() : 0;
    static vec3 last_sort_pos = {0};
    static Scene *last_sort_scene;
    const bool needs_sort = buffer
        ? obs_scene_buffer_needs_alpha_sort(buffer, camera.position)
        : scene != last_sort_scene ||
            glm_vec3_distance2(camera.position, last_sort_pos) > 0.01f;
    if (needs_sort) {
        sort_meshes_by_alpha(&scene->meshes, camera.position);
        if (!buffer) {
            glm_vec3_copy(camera.position, last_sort_pos);
            last_sort_scene = scene;
        }
        updateMeshSSBOAndIndirect(&context, &scene->meshes);
    }

    flushMeshSSBO(&context, &scene->meshes);
    if (s_frame_profile_enabled)
        s_last_frame_profile.mesh_sync_ns = obs_profile_now_ns() - profile_start;

    profile_start = s_frame_profile_enabled ? obs_profile_now_ns() : 0;
    camera_update_animations(&camera, delta_time);
    if (s_frame_profile_enabled)
        s_last_frame_profile.camera_animation_ns =
            obs_profile_now_ns() - profile_start;

    if (s_editor_enabled) {
        profile_start = s_frame_profile_enabled ? obs_profile_now_ns() : 0;
        editor_update();
        editor_render();
        add_menu_render();
        gizmo_render(editor.inspector.selected_mesh_index);
        if (s_frame_profile_enabled)
            s_last_frame_profile.editor_ns = obs_profile_now_ns() - profile_start;
    }
}

void endFrame() {
    ObsEditorFrame *frame = obs_editor_frame_current();
    ObsWindow *active_window = frame
        ? obs_editor_frame_window(frame) : context.primary;
    obs_end_frame(active_window ? active_window : context.primary);
}


double getTime() {
    return glfwGetTime();
}

void setWindowPos(GLFWwindow* window, int x, int y) {
    glfwSetWindowPos(window, x, y);
}

void getWindowPos(GLFWwindow* window, int* x, int* y) {
    glfwGetWindowPos(window, x, y);
}

void setWindowSize(GLFWwindow* window, int width, int height) {
    glfwSetWindowSize(window, width, height);
}

void getWindowSize(GLFWwindow* window, int* width, int* height) {
    glfwGetWindowSize(window, width, height);
}

void pollEvents(void) {
    obs_poll_events();
}

void obs_poll_events(void) {
    glfwPollEvents();
    /* No script compilation inside a GLFW key callback. One coalesced
     * request is executed at this explicit engine event-loop safe point. */
    obs_monad_engine_process_pending();
}

void obs_wait_events_timeout(double timeout_seconds) {
    if (timeout_seconds < 0.0) timeout_seconds = 0.0;
    glfwWaitEventsTimeout(timeout_seconds);
    obs_monad_engine_process_pending();
}



/// getClipboardString — implementation group
const char* getClipboardString() {
    return glfwGetClipboardString(NULL);
}

void setClipboardString(const char *text) {
    glfwSetClipboardString(NULL, text);
}

void setInputMode(GLFWwindow* window, int mode, int value) {
    glfwSetInputMode(window, mode, value);
}

void getCursorPos(GLFWwindow* window, double* xpos, double* ypos) {
    glfwGetCursorPos(window, xpos, ypos);
}

void setCursorMode(int mode) {
    if (!context.window) {
        fprintf(stderr, "Error: Window not initialized\n");
        return;
    }
    current_cursor_mode = mode;
    glfwSetInputMode(context.window, GLFW_CURSOR, mode);
}

int getCursorMode() {
    return current_cursor_mode;
}

void showCursor() {
    setCursorMode(GLFW_CURSOR_NORMAL);
}

void hideCursor() {
    setCursorMode(GLFW_CURSOR_HIDDEN);
}

void disableCursor() {
    setCursorMode(GLFW_CURSOR_DISABLED);
}

GLFWcursor* createStandardCursor(int shape) {
    return glfwCreateStandardCursor(shape);
}

void setCursor(GLFWcursor* cursor) {
    glfwSetCursor(context.window, cursor);
}


// window.c - no X11 headers here!

#if defined(__linux__) || defined(__unix__)
extern void setWindowResizeIncrements_x11(GLFWwindow *window, int char_width, int char_height,
                                           int min_width, int min_height);
#endif

void setWindowResizeIncrements(int char_width, int char_height, int min_width, int min_height) {
#if defined(__linux__) || defined(__unix__)
    setWindowResizeIncrements_x11(context.window, char_width, char_height, min_width, min_height);
#endif
}

void toggle_editor_mode() {
    camera.active = !camera.active;

    if (camera.active) {
        disableCursor();
        if (camera.use_look_at) {
            camera_disable_orbit_mode(&camera);
            printf("Entered FPS mode - orbit disabled, angles synced\n");
        } else {
            printf("Camera control ENABLED\n");
        }
    } else {
        showCursor();
        printf("Camera control DISABLED - Editor mode\n");
    }
}

extern bool is_framing;
extern CameraFrameTransition frame_transition;
extern float frame_anim_time;
extern const float FRAME_ANIM_DURATION;

void editor_frame_selected(void) {
    if (camera.active) return; // Only frame in editor mode
    extern int g_selected_sdf_index;

    vec3 target_center = {0.0f, 0.0f, 0.0f};
    float target_dist = 10.0f;

    if (editor.inspector.selected_mesh_index >= 0 && editor.inspector.selected_mesh_index < (int)scene->meshes.count) {
        Mesh* m = &scene->meshes.items[editor.inspector.selected_mesh_index];

        vec3 local_center;
        glm_vec3_add(m->aabbMin, m->aabbMax, local_center);
        glm_vec3_scale(local_center, 0.5f, local_center);

        vec4 lc4 = {local_center[0], local_center[1], local_center[2], 1.0f};
        vec4 wc4;
        glm_mat4_mulv(m->model, lc4, wc4);
        glm_vec3_copy(wc4, target_center);

        vec3 extents;
        glm_vec3_sub(m->aabbMax, m->aabbMin, extents);
        float scale = glm_vec3_norm((vec3){m->model[0][0], m->model[0][1], m->model[0][2]});
        target_dist = glm_vec3_norm(extents) * scale * 1.2f;
        if (target_dist < 2.0f) target_dist = 2.0f;
    } else if (g_selected_sdf_index >= 0 &&
               g_selected_sdf_index < (int)scene->sdf_count) {
        const SdfPrimitive *primitive = &scene->sdfs[g_selected_sdf_index];
        mat4 model;
        glm_mat4_inv(primitive->inverseTransform, model);
        vec3 local_extent;
        if (primitive->type == SDF_TYPE_SPHERE) {
            glm_vec3_copy((vec3){primitive->size[0], primitive->size[0],
                                 primitive->size[0]},
                          local_extent);
        } else {
            glm_vec3_copy((vec3){primitive->size[0], primitive->size[1],
                                 primitive->size[2]},
                          local_extent);
        }
        vec3 world_min = {1e30f, 1e30f, 1e30f};
        vec3 world_max = {-1e30f, -1e30f, -1e30f};
        for (int corner = 0; corner < 8; ++corner) {
            vec4 local = {
                (corner & 1) ? local_extent[0] : -local_extent[0],
                (corner & 2) ? local_extent[1] : -local_extent[1],
                (corner & 4) ? local_extent[2] : -local_extent[2], 1.0f};
            vec4 world;
            glm_mat4_mulv(model, local, world);
            for (int axis = 0; axis < 3; ++axis) {
                world_min[axis] = fminf(world_min[axis], world[axis]);
                world_max[axis] = fmaxf(world_max[axis], world[axis]);
            }
        }
        glm_vec3_add(world_min, world_max, target_center);
        glm_vec3_scale(target_center, 0.5f, target_center);
        vec3 extent;
        glm_vec3_sub(world_max, world_min, extent);
        target_dist = glm_vec3_norm(extent) * 1.2f;
        if (target_dist < 2.0f) target_dist = 2.0f;
    }

    CameraFrameTransition transition;
    camera_frame_transition_begin(&camera, target_center, target_dist,
                                  &transition);
    if (obs_editor_frame_current()) {
        obs_editor_frame_start_camera_transition(&transition,
                                                 FRAME_ANIM_DURATION);
    } else {
        frame_transition = transition;
        is_framing = true;
        frame_anim_time = 0.0f;
    }

    glm_vec3_copy(target_center, orbitPivot);
    orbitDistance = target_dist;

}

extern CameraFrameTransition frame_transition;
extern float frame_anim_time;
extern const float FRAME_ANIM_DURATION;

extern bool is_zooming;
extern vec3 zoom_start_pos;
extern vec3 zoom_target_pos;
extern float zoom_anim_time;
extern const float ZOOM_ANIM_DURATION;

void process_editor_movement(Camera* cam, float deltaTime) {
    if (obs_editor_frame_current()) {
        if (obs_editor_frame_update_camera_transition(cam, deltaTime)) {
            is_zooming = false;
        } else {
            (void)obs_editor_frame_update_zoom(cam, deltaTime);
        }
    } else if (is_framing) {
        is_zooming = false;
        frame_anim_time += deltaTime;
        float t = frame_anim_time / FRAME_ANIM_DURATION;
        if (t >= 1.0f) {
            t = 1.0f;
            is_framing = false;
        }
        float ease_t = ease_expo_out(t);
        camera_frame_transition_apply(cam, &frame_transition, ease_t);
    } else if (is_zooming) {
        zoom_anim_time += deltaTime;
        float t = zoom_anim_time / ZOOM_ANIM_DURATION;
        if (t >= 1.0f) {
            t = 1.0f;
            is_zooming = false;
        }
        float ease_t = ease_quart_out(t);
        glm_vec3_lerp(zoom_start_pos, zoom_target_pos, ease_t, cam->position);
    }

    if (!isMouseButtonDown(MOUSE_BUTTON_RIGHT)) return;

    float speed = 5.0f * deltaTime;

    vec3 forward, right;

    glm_vec3_copy(cam->front, forward);
    glm_vec3_normalize(forward);

    glm_vec3_cross(forward, cam->up, right);
    glm_vec3_normalize(right);

    vec3 movement = {0.0f, 0.0f, 0.0f};

    if (isKeyDown(KEY_W)) {
        vec3 temp;
        glm_vec3_scale(forward, speed, temp);
        glm_vec3_add(movement, temp, movement);
    }
    if (isKeyDown(KEY_S)) {
        vec3 temp;
        glm_vec3_scale(forward, -speed, temp);
        glm_vec3_add(movement, temp, movement);
    }

    if (isKeyDown(KEY_A)) {
        vec3 temp;
        glm_vec3_scale(right, -speed, temp);
        glm_vec3_add(movement, temp, movement);
    }
    if (isKeyDown(KEY_D)) {
        vec3 temp;
        glm_vec3_scale(right, speed, temp);
        glm_vec3_add(movement, temp, movement);
    }

    if (isKeyDown(KEY_SPACE)) {
        vec3 worldUp = {0.0f, 1.0f, 0.0f};
        vec3 temp;
        glm_vec3_scale(worldUp, speed, temp);
        glm_vec3_add(movement, temp, movement);
    }
    if (isKeyDown(KEY_LEFT_SHIFT)) {
        vec3 worldUp = {0.0f, 1.0f, 0.0f};
        vec3 temp;
        glm_vec3_scale(worldUp, speed, temp);
        glm_vec3_sub(movement, temp, movement);
    }

    glm_vec3_add(cam->position, movement, cam->position);
}

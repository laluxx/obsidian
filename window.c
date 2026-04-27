#include "window.h"
#include "audio.h"
#include "input.h"
#include "keychords.h"
#include "gltf_loader.h"
#include "camera.h"
#include "theme.h"
#include "vulkan_setup.h"
#include <stdio.h>
#include "font.h"
#include "audio.h"
#include "easing.h"
#include "editor.h"
#include "gizmo.h"
#include "keychords.h"
#include "vertico.h"
#include "add_menu.h"
#include "physics.h"

float delta_time;
float last_frame = 0.0f;

static int current_cursor_mode = GLFW_CURSOR_NORMAL;

extern vec3 orbitPivot;
extern float orbitDistance;

static void open_add_menu_at_cursor(void) {
    double mx, my;
    getCursorPos(context.window, &mx, &my);
    add_menu_open(mx, my);
}

GLFWwindow* initWindow(int width, int height, const char* title) {
    if (!glfwInit()) {
        fprintf(stderr, "Failed to initialize GLFW\n");
        return NULL;
    }

    context.currentFrame = 0;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    context.window = glfwCreateWindow(width, height, title, NULL, NULL);


    glfwMakeContextCurrent(context.window);
    glfwSetCharCallback(context.window, internal_char_callback);
    glfwSetKeyCallback(context.window, internal_key_callback);
    glfwSetMouseButtonCallback(context.window, internal_mouse_button_callback);
    glfwSetCursorPosCallback(context.window, internal_cursor_position_callback);
    glfwSetScrollCallback(context.window, internal_scroll_callback);
    glfwSetFramebufferSizeCallback(context.window, internal_window_resize_callback);
    glfwSetWindowFocusCallback(context.window, internal_window_focus_callback);
    glfwSetWindowPosCallback(context.window, internal_window_pos_callback);

    init_input();
    keymap_init(&keymap);
    init_free_type();
    audio_init();

    // Default cursor mode is normal (visible)
    current_cursor_mode = GLFW_CURSOR_NORMAL;
    glfwSetInputMode(context.window, GLFW_CURSOR, current_cursor_mode);

    createInstance(&context);

    vec3 camera_pos = {0.0f, 3.0f, 0.0f};
    camera_init(&camera, camera_pos, 90.0f, 0.0f, (float)WIDTH / (float)HEIGHT);
    camera.active = false;

    if (glfwCreateWindowSurface(context.instance, context.window, NULL, &context.surface) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create window surface\n");
        exit(EXIT_FAILURE);
    }

    pickPhysicalDevice(&context);
    createLogicalDevice(&context);
    createSwapChain(&context);
    createImageViews(&context);
    createDepthResources(&context);
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

    renderer_init(
        context.device,
        context.physicalDevice,
        context.commandPool,
        context.graphicsQueue
    );

    // 256 MB to comfortably fit static meshes PLUS 2 frames of 1M dynamic vertices
    createMegaVertexBuffer(&context, 256ULL * 1024 * 1024);
    // 128 MB for indices
    createMegaIndexBuffer(&context, 128ULL * 1024 * 1024);
    // 256 MB dynamic staging budget:
    // 2 frames * 1M vertices * 80 bytes = ~160 MB.
    // The remaining ~96 MB headroom safely absorbs massive UI spikes,
    // dense morph-target animations, and prevents OOM fragmentation
    // before we implement f16/u8 vertex compression.
    createDynamicBuffers(&context, 256ULL * 1024 * 1024);

    createMeshSSBO(&context, 4096);
    createIndirectBuffer(&context, 4096);

    createLightingDescriptors(&context);
    createAllPipelineLayouts(&context);
    createGraphicsPipelines(&context);
    createComputeCullPipeline(&context);
    createComputeCompactPipeline(&context);

    // 64 MB persistent upload staging — eliminates per-mesh staging allocations
    createUploadStagingBuffer(&context, 64ULL * 1024 * 1024);

    renderer2D_init();

    line_renderer_init(
        context.device,
        context.physicalDevice,
        context.commandPool,
        context.graphicsQueue
    );

    clear_background((Color){0.0f, 0.0f, 0.0f, 1.0f});

    createCommandBuffers(&context);
    createSyncObjects(&context);

    scene_init(&scene);

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

    initThemes();
    ease_init();
    editor_init();
    gizmo_init();
    vertico_init();
    add_menu_init();
    physics_init();


    keychord_bind(&keymap, "A",         open_add_menu_at_cursor,   "Open Add Menu",         PRESS);
    keychord_bind(&keymap, "<left>",    camera_snap_left,          "Camera snap left",      PRESS);
    keychord_bind(&keymap, "<right>",   camera_snap_right,         "Camera snap right",     PRESS);
    keychord_bind(&keymap, "<up>",      camera_snap_up,            "Camera snap up",        PRESS);
    keychord_bind(&keymap, "<down>",    camera_snap_down,          "Camera snap down",      PRESS);
    keychord_bind(&keymap, "TAB",       toggle_skybox,             "Toggle the skybox",     PRESS);
    keychord_bind(&keymap, "t",         toggle_ibl_lighting,       "Toggle IBL lighting",   PRESS);
    keychord_bind(&keymap, "l",         toggle_shadows,            "Toggle shadows",        PRESS);

    keychord_bind(&keymap, "C-h c",     vertico_show_keybindings,  "Help keybindings",      PRESS);

    keychord_bind(&keymap, "C-g",       keymap_reset_state,        "Reset Keymap",          PRESS);
    keychord_bind(&keymap, "C-t",       toggle_culling_freeze,     "Toggle culling freeze", PRESS);
    keymap_print_bindings(&keymap);

    return context.window;
}

int windowShouldClose() {
    if (!context.window) {
        fprintf(stderr, "Error: Window not initialized. Call initWindow() first.\n");
        return 1;
    }
    return glfwWindowShouldClose(context.window);
}

void beginFrame() {
    float current_frame = getTime();
    delta_time = current_frame - last_frame;
    last_frame = current_frame;

    // FIX: We must wait for the GPU to finish with this frame index BEFORE modifying CPU-mapped buffers!
    // This prevents massive data races where the CPU overwrites SSBOs while the GPU is still rendering.
    uint32_t frameIndex = context.currentFrame;
    VkFence inFlightFence = context.inFlightFences[frameIndex];
    vkWaitForFences(context.device, 1, &inFlightFence, VK_TRUE, UINT64_MAX);

    /* Reset ALL per-frame CPU render state first — before any user draw calls. */
    begin_frame();
    renderer2D_clear();

    animate_scene(&scene, current_frame);

    physics_step(delta_time);

    camera_process_keyboard(&camera, context.window, delta_time);
    camera_update(&camera);

    if (!camera.active) {
        process_editor_movement(&camera, delta_time);
    }

    static vec3 last_sort_pos = {0};
    float moved = glm_vec3_distance2(camera.position, last_sort_pos);
    if (moved > 0.01f) {
        sort_meshes_by_alpha(&scene.meshes, camera.position);
        glm_vec3_copy(camera.position, last_sort_pos);

        // CRITICAL: We shuffled the CPU array. We must rebuild the GPU indirect buffer
        // so that the Geometry pointers (cmds[i]) match the Material/Transform slots (SSBO[i]).
        // updateMeshSSBOAndIndirect calls markMeshesSSBODirty internally!
        updateMeshSSBOAndIndirect(&context, &scene.meshes);
    }

    flushMeshSSBO(&context, &scene.meshes);
    updateUniformBuffer(&context);

    camera_update_animations(&camera, delta_time);
    editor_update();
    editor_render();
    add_menu_render();
    gizmo_render(editor.inspector.selected_mesh_index);
}

void endFrame() {
    // Check if resize happened and handle it BEFORE rendering
    if (context.framebufferResized) {
        context.framebufferResized = false;
        recreateSwapChain(&context);
        return;
    }

    // Upload all geometry to GPU (slots/counts were set in beginFrame)
    line_renderer_upload();
    renderer2D_upload();

    uint32_t frameIndex   = context.currentFrame;
    VkFence  inFlightFence = context.inFlightFences[frameIndex];

    // Wait was moved to beginFrame() to protect mapped GPU buffers.

    uint32_t imageIndex;
    VkResult result = vkAcquireNextImageKHR(
        context.device, context.swapChain, UINT64_MAX,
        context.imageAvailableSemaphores[frameIndex], VK_NULL_HANDLE, &imageIndex
    );

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapChain(&context);
        return;
    } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        fprintf(stderr, "Failed to acquire swap chain image\n");
        exit(EXIT_FAILURE);
    }

    vkResetFences(context.device, 1, &inFlightFence);

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

    // Flag resize on suboptimal, actual recreation happens at frame start
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        context.framebufferResized = true;
    } else if (result != VK_SUCCESS) {
        fprintf(stderr, "Failed to present swap chain image\n");
        exit(EXIT_FAILURE);
    }


    update_input();
    glfwPollEvents();
    context.currentFrame = (context.currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
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
    glfwPollEvents();
}



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

extern vec3 frame_start_pos;
extern vec3 frame_target_pos;
extern float frame_anim_time;
extern const float FRAME_ANIM_DURATION;

void editor_frame_selected(void) {
    if (camera.active) return; // Only frame in editor mode

    vec3 target_center = {0.0f, 0.0f, 0.0f};
    float target_dist = 10.0f;

    if (editor.inspector.selected_mesh_index >= 0 && editor.inspector.selected_mesh_index < (int)scene.meshes.count) {
        Mesh* m = &scene.meshes.items[editor.inspector.selected_mesh_index];

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
    }

    glm_vec3_copy(camera.position, frame_start_pos);
    glm_vec3_copy(camera.front, frame_target_pos);
    glm_vec3_scale(frame_target_pos, -target_dist, frame_target_pos);
    glm_vec3_add(target_center, frame_target_pos, frame_target_pos);

    glm_vec3_copy(target_center, orbitPivot);
    orbitDistance = target_dist;

    is_framing = true;
    frame_anim_time = 0.0f;
}

extern vec3 frame_start_pos;
extern vec3 frame_target_pos;
extern float frame_anim_time;
extern const float FRAME_ANIM_DURATION;

extern bool is_zooming;
extern vec3 zoom_start_pos;
extern vec3 zoom_target_pos;
extern float zoom_anim_time;
extern const float ZOOM_ANIM_DURATION;

void process_editor_movement(Camera* cam, float deltaTime) {
    if (is_framing) {
        is_zooming = false;
        frame_anim_time += deltaTime;
        float t = frame_anim_time / FRAME_ANIM_DURATION;
        if (t >= 1.0f) {
            t = 1.0f;
            is_framing = false;
        }
        float ease_t = ease_expo_out(t);
        glm_vec3_lerp(frame_start_pos, frame_target_pos, ease_t, cam->position);
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

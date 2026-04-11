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


float delta_time;
float last_frame = 0.0f;

static int current_cursor_mode = GLFW_CURSOR_NORMAL;

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
    camera.active = true;

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

    imm_ssbo_init(&context);

    // 128 MB for static mesh geometry (Sponza + typical scenes fit in ~30–60 MB)
    createMegaVertexBuffer(&context, 128ULL * 1024 * 1024);
    // 64 MB for indices — much smaller than vertices (4 bytes vs 32 bytes per entry)
    createMegaIndexBuffer(&context, 64ULL * 1024 * 1024);
    // 32 MB dynamic budget: 2D UI + lines + morph-target meshes
    createDynamicBuffers(&context, 32ULL * 1024 * 1024);

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

    /* Reset ALL per-frame CPU render state first — before any user draw calls.
       imm_ssbo_begin_frame resets immSlotCount, immDrawCount, vertex_count,
       lineVertexCount, and sets the correct frame index.                      */
    imm_ssbo_begin_frame(&context, context.currentFrame);
    renderer2D_clear();

    animate_scene(&scene, current_frame);

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
        markMeshesSSBODirty(&context);
    }

    flushMeshSSBO(&context, &scene.meshes);
    updateUniformBuffer(&context);
}

void endFrame() {
    // Check if resize happened and handle it BEFORE rendering
    if (context.framebufferResized) {
        context.framebufferResized = false;
        recreateSwapChain(&context);
        return;
    }

    // Upload all geometry to GPU (slots/counts were set in beginFrame)
    renderer_upload();
    line_renderer_upload();
    renderer2D_upload();

    uint32_t frameIndex   = context.currentFrame;
    VkFence  inFlightFence = context.inFlightFences[frameIndex];

    vkWaitForFences(context.device, 1, &inFlightFence, VK_TRUE, UINT64_MAX);

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

void process_editor_movement(Camera* cam, float deltaTime) {
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

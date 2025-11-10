#include "window.h"
#include "input.h"
#include "keychords.h"
#include "font.h"
#include "gltf_loader.h"
#include "camera.h"
#include "theme.h"
#include "vulkan_setup.h"

#include <stdio.h>

float delta_time;
float last_frame = 0.0f;



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

    init_input();
    keymap_init(&keymap);
    init_free_type();

    glfwSetInputMode(context.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    
    createInstance(&context);
    
    vec3 camera_pos = {0.0f, 3.0f, 0.0f}; // 2.0f
    vec3 camera_target = {0.0f, 2.0f, 0.0f};
    camera_init(&camera, camera_pos, 90.0f, 0.0f, (float)WIDTH / (float)HEIGHT);
    
    camera.active = true;
    glfwSetInputMode(context.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    
    if (glfwCreateWindowSurface(context.instance, context.window, NULL, &context.surface) != VK_SUCCESS) {
        fprintf(stderr, "Failed to create window surface\n");
        exit(EXIT_FAILURE);
    }
    
    pickPhysicalDevice(&context);
    createLogicalDevice(&context);
    createSwapChain(&context);
    createImageViews(&context);
    createDepthResources(&context);
    
    createRenderPass(&context);
    
    createUniformBuffer(&context);
    createDescriptorSetLayout(&context);
    
    createGraphicsPipeline(&context);
    
    // 2D descriptor stuff FIRST (before 3D textured pipeline)
    create2DDescriptorSetLayout(&context);
    create2DDescriptorPool(&context);
    
    // THEN 3D textured pipeline (needs 2D descriptor layout)
    create3DTexturedGraphicsPipeline(&context);
    
    // THEN rest of 2D
    create2DGraphicsPipeline(&context);
    createTextured2DGraphicsPipeline(&context);
    createLineGraphicsPipeline(&context);
    renderer2D_init();
    
    createDescriptorPool(&context);
    createDescriptorSet(&context);
    
    createFramebuffers(&context);
    createCommandPool(&context);
    
    // Initialize both 3D renderers
    renderer_init(
                  context.device,
                  context.physicalDevice,
                  context.commandPool,
                  context.graphicsQueue
                  );
    
    renderer_init_textured3D();
    
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
    
    animate_scene(&scene, current_frame);
    
    camera_process_keyboard(&camera, context.window, delta_time);
    camera_update(&camera);

    if (!camera.active) {
        process_editor_movement(&camera, delta_time);
    }

    sort_meshes_by_alpha(&scene.meshes, camera.position); // HERE

    // Update camera uniform buffer
    UniformBufferObject ubo;
    glm_mat4_mul(camera.projection_matrix, camera.view_matrix, ubo.vp);
    void *data;
    vkMapMemory(context.device, uniformBufferMemory, 0, sizeof(ubo), 0, &data);
    memcpy(data, &ubo, sizeof(ubo));
    vkUnmapMemory(context.device, uniformBufferMemory);

    // Clear all render buffers
    renderer_clear();
    renderer_clear_textured3D();
    line_renderer_clear();
    renderer2D_clear();



}

void endFrame() {

    // Upload all geometry to GPU
    renderer_upload();
    renderer_upload_textured3D();  // ADD THIS
    line_renderer_upload();
    renderer2D_upload();
        
     
        
    // RENDER FRAME
    uint32_t frameIndex = context.currentFrame;
    VkFence inFlightFence = context.inFlightFences[frameIndex];
    vkWaitForFences(context.device, 1, &inFlightFence, VK_TRUE, UINT64_MAX);
        
    uint32_t imageIndex;
    VkResult result = vkAcquireNextImageKHR(
                                            context.device, context.swapChain, UINT64_MAX,
                                            context.imageAvailableSemaphores[frameIndex], VK_NULL_HANDLE, &imageIndex
                                            );
        
    /* if (result == VK_ERROR_OUT_OF_DATE_KHR) { */
    /*     continue; */
    /* } else */
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        fprintf(stderr, "Failed to acquire swap chain image\n");
        exit(EXIT_FAILURE);
    }
        
    if (context.imagesInFlight[imageIndex] != VK_NULL_HANDLE) {
        vkWaitForFences(context.device, 1, &context.imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX);
    }
    context.imagesInFlight[imageIndex] = inFlightFence;
    vkResetFences(context.device, 1, &inFlightFence);
        
    // Re-record command buffer with new geometry
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
        
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        // Handle swapchain recreation
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

void toggle_editor_mode() {
    camera.active = !camera.active;

    if (camera.active) {
        setInputMode(context.window, CURSOR, CURSOR_DISABLED);

        // When entering FPS mode, disable orbit mode and sync angles
        if (camera.use_look_at) {
            camera_disable_orbit_mode(&camera);
            printf("Entered FPS mode - orbit disabled, angles synced\n");
        } else {
            printf("Camera control ENABLED\n");
        }
    } else {
        setInputMode(context.window, CURSOR, CURSOR_NORMAL);
        printf("Camera control DISABLED - Editor mode\n");
    }
}


void process_editor_movement(Camera* cam, float deltaTime) {
    /* if (!rightMousePressed) return; */

    if (!isMouseButtonDown(MOUSE_BUTTON_RIGHT))  return;
    
    float speed = 5.0f * deltaTime;  // Adjust speed as needed
    
    vec3 forward, right;
    
    // Use the actual camera forward direction (don't zero out Y)
    glm_vec3_copy(cam->front, forward);
    glm_vec3_normalize(forward);
    
    glm_vec3_cross(forward, cam->up, right);
    glm_vec3_normalize(right);
    
    vec3 movement = {0.0f, 0.0f, 0.0f};
    
    // Forward/backward - now uses actual facing direction
    /* if (keyW) { */
    if (isKeyDown(KEY_W)) {
        vec3 temp;
        glm_vec3_scale(forward, speed, temp);
        glm_vec3_add(movement, temp, movement);
    }
    /* if (keyS) { */
    if (isKeyDown(KEY_S)) {
        vec3 temp;
        glm_vec3_scale(forward, -speed, temp);
        glm_vec3_add(movement, temp, movement);
    }
    
    // Left/right
    /* if (keyA) { */
    if (isKeyDown(KEY_A)) {
        vec3 temp;
        glm_vec3_scale(right, -speed, temp);
        glm_vec3_add(movement, temp, movement);
    }
    /* if (keyD) { */
    if (isKeyDown(KEY_D)) {
        vec3 temp;
        glm_vec3_scale(right, speed, temp);
        glm_vec3_add(movement, temp, movement);
    }
    
    // Up/down - use world up direction
    /* if (keyE || keySpace) { */
    if (isKeyDown(KEY_SPACE)) {
        vec3 worldUp = {0.0f, 1.0f, 0.0f};
        vec3 temp;
        glm_vec3_scale(worldUp, speed, temp);
        glm_vec3_add(movement, temp, movement);
    }
    /* if (keyQ || keyShift) { */
    if (isKeyDown(KEY_LEFT_SHIFT)) {
        vec3 worldUp = {0.0f, 1.0f, 0.0f};
        vec3 temp;
        glm_vec3_scale(worldUp, speed, temp);
        glm_vec3_sub(movement, temp, movement);
    }
    
    glm_vec3_add(cam->position, movement, cam->position);
}

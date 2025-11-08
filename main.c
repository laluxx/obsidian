#include <vulkan/vulkan_core.h>
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "gltf_loader.h"

#include "font.h"
#include "obj.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* #include "frag.frag.spv.h" */
/* #include "vert.vert.spv.h" */
/* #include "2D.vert.spv.h" */
/* #include "2D.frag.spv.h" */
/* #include "texture.frag.spv.h" */
/* #include "texture3D.frag.spv.h" */
#include "vulkan_setup.h"


#include <cglm/cglm.h>
#include "camera.h"
#include "renderer.h"
#include "scene.h"
#include "context.h"
#include "common.h"
#include "input.h"

#include <time.h>  
#include "window.h"



/* #define MAX_FRAMES_IN_FLIGHT 2 */
/* #define ENABLE_VALIDATION_LAYERS 1 */

/* #if ENABLE_VALIDATION_LAYERS */
/* #define VALIDATION_LAYERS_COUNT 1 */
/* const char* validationLayers[VALIDATION_LAYERS_COUNT] = { */
/*     "VK_LAYER_KHRONOS_validation" */
/* }; */
/* #else */
/* #define VALIDATION_LAYERS_COUNT 0 */
/* const char** validationLayers = NULL; */
/* #endif */


// TODO FIX panning up and wown when loooking from upside down 
// FIXME ALL Textures are inversed


double lastX = WIDTH / 2.0f, lastY = HEIGHT / 2.0f;
bool firstMouse = true;
/* float delta_time; */
/* float last_frame = 0.0f; */


bool shiftPressed;
bool ctrlPressed;
bool altPressed;
/* bool ambientOcclusionEnabled = true; */


void screenshot( VkDevice device, VkPhysicalDevice physicalDevice, VkCommandPool commandPool, VkQueue queue, VkImage srcImage, VkFormat imageFormat, uint32_t width, uint32_t height, const char* filename);



bool middleMousePressed = false;
bool rightMousePressed = false;
bool shiftMiddleMousePressed = false;
double lastPanX = 0.0, lastPanY = 0.0;
double lastOrbitX = 0.0, lastOrbitY = 0.0;
vec3 orbitPivot = {0.0f, 0.0f, 0.0f};  // The point we're orbiting around
float orbitDistance = 10.0f;           // Distance from pivot


// Add these global variables at the top with your other globals
bool keyW = false, keyA = false, keyS = false, keyD = false;
bool keyQ = false, keyE = false;
bool keySpace = false, keyShift = false;


#include <stdio.h>
#include <inttypes.h>

/* Camera camera; */

void key_callback(int key, int action, int mods) {
    shiftPressed = mods & GLFW_MOD_SHIFT;
    ctrlPressed  = mods & GLFW_MOD_CONTROL;
    altPressed   = mods & GLFW_MOD_ALT;
    
    if (key == GLFW_KEY_TAB && action == GLFW_PRESS) {
        screenshot(
                   context.device,
                   context.physicalDevice,
                   context.commandPool,
                   context.graphicsQueue,
                   context.swapChainImages[0],
                   VK_FORMAT_B8G8R8A8_SRGB,
                   context.swapChainExtent.width,
                   context.swapChainExtent.height,
                   "screenshot.png"
                   );
    }

    if (key == GLFW_KEY_Z && action == GLFW_PRESS) {
        print_scene_meshes();
    }
    
    // Arrow keys for camera snapping
    if (key == GLFW_KEY_LEFT && action == GLFW_PRESS) {
        camera_snap_to_next_angle(&camera, true, false);  // Counter-clockwise
    }
    if (key == GLFW_KEY_RIGHT && action == GLFW_PRESS) {
        camera_snap_to_next_angle(&camera, false, false);   // Clockwise
    }
    if (key == GLFW_KEY_UP && action == GLFW_PRESS) {
        camera_snap_to_next_angle(&camera, true, true);   // Pitch up (more negative)
    }
    if (key == GLFW_KEY_DOWN && action == GLFW_PRESS) {
        camera_snap_to_next_angle(&camera, false, true);    // Pitch down (more positive)
    }
    
    if (key == GLFW_KEY_T && action == GLFW_PRESS) {
        ambientOcclusionEnabled = !ambientOcclusionEnabled;
        printf("Ambient Occlusion: %s\n", ambientOcclusionEnabled ? "ENABLED" : "DISABLED");
    }
    
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
        camera.active = !camera.active;
        
        if (camera.active) {
            glfwSetInputMode(context.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            
            // When entering FPS mode, disable orbit mode and sync angles
            if (camera.use_look_at) {
                camera_disable_orbit_mode(&camera);
                printf("Entered FPS mode - orbit disabled, angles synced\n");
            } else {
                printf("Camera control ENABLED\n");
            }
        } else {
            glfwSetInputMode(context.window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            printf("Camera control DISABLED - Editor mode\n");
        }
    }
    
    if (key == GLFW_KEY_F && action == GLFW_PRESS) {
        vec3 world_origin = {0.0f, 0.0f, 0.0f};
        camera_set_look_at(&camera, world_origin);
        printf("Looking at world origin (0, 0, 0)\n");
    }
    
    // Track WASD key states
    if (key == GLFW_KEY_W) keyW = (action != GLFW_RELEASE);
    if (key == GLFW_KEY_A) keyA = (action != GLFW_RELEASE);
    if (key == GLFW_KEY_S) keyS = (action != GLFW_RELEASE);
    if (key == GLFW_KEY_D) keyD = (action != GLFW_RELEASE);
    if (key == GLFW_KEY_Q) keyQ = (action != GLFW_RELEASE);
    if (key == GLFW_KEY_E) keyE = (action != GLFW_RELEASE);
    if (key == GLFW_KEY_SPACE) keySpace = (action != GLFW_RELEASE);
    if (key == GLFW_KEY_LEFT_SHIFT) keyShift = (action != GLFW_RELEASE);
}

/* void process_editor_movement(Camera* cam, float deltaTime) { */
/*     if (!rightMousePressed) return; */
    
/*     float speed = 5.0f * deltaTime;  // Adjust speed as needed */
    
/*     vec3 forward, right; */
    
/*     // Use the actual camera forward direction (don't zero out Y) */
/*     glm_vec3_copy(cam->front, forward); */
/*     glm_vec3_normalize(forward); */
    
/*     glm_vec3_cross(forward, cam->up, right); */
/*     glm_vec3_normalize(right); */
    
/*     vec3 movement = {0.0f, 0.0f, 0.0f}; */
    
/*     // Forward/backward - now uses actual facing direction */
/*     if (keyW) { */
/*         vec3 temp; */
/*         glm_vec3_scale(forward, speed, temp); */
/*         glm_vec3_add(movement, temp, movement); */
/*     } */
/*     if (keyS) { */
/*         vec3 temp; */
/*         glm_vec3_scale(forward, -speed, temp); */
/*         glm_vec3_add(movement, temp, movement); */
/*     } */
    
/*     // Left/right */
/*     if (keyA) { */
/*         vec3 temp; */
/*         glm_vec3_scale(right, -speed, temp); */
/*         glm_vec3_add(movement, temp, movement); */
/*     } */
/*     if (keyD) { */
/*         vec3 temp; */
/*         glm_vec3_scale(right, speed, temp); */
/*         glm_vec3_add(movement, temp, movement); */
/*     } */
    
/*     // Up/down - use world up direction */
/*     if (keyE || keySpace) { */
/*         vec3 worldUp = {0.0f, 1.0f, 0.0f}; */
/*         vec3 temp; */
/*         glm_vec3_scale(worldUp, speed, temp); */
/*         glm_vec3_add(movement, temp, movement); */
/*     } */
/*     if (keyQ || keyShift) { */
/*         vec3 worldUp = {0.0f, 1.0f, 0.0f}; */
/*         vec3 temp; */
/*         glm_vec3_scale(worldUp, speed, temp); */
/*         glm_vec3_sub(movement, temp, movement); */
/*     } */
    
/*     glm_vec3_add(cam->position, movement, cam->position); */
/* } */

/* bool raycast_to_ground(Camera* cam, vec3 hitPoint) { */
/*     vec3 rayOrigin, rayDir; */
/*     glm_vec3_copy(cam->position, rayOrigin); */
/*     glm_vec3_copy(cam->front, rayDir); */
/*     glm_vec3_normalize(rayDir); */
    
/*     printf("=== RAYCAST DEBUG ===\n"); */
/*     printf("Camera pos: (%.2f, %.2f, %.2f)\n", rayOrigin[0], rayOrigin[1], rayOrigin[2]); */
/*     printf("Camera dir: (%.2f, %.2f, %.2f)\n", rayDir[0], rayDir[1], rayDir[2]); */
    
/*     // Check if ray is parallel to ground (no Y component) */
/*     if (fabs(rayDir[1]) < 0.0001f) { */
/*         printf("Ray parallel to ground - using camera XZ position\n"); */
/*         hitPoint[0] = rayOrigin[0]; */
/*         hitPoint[1] = 0.0f; */
/*         hitPoint[2] = rayOrigin[2]; */
/*         return false; */
/*     } */
    
/*     // Calculate intersection with plane y = 0 */
/*     // t = -rayOrigin.y / rayDir.y */
/*     float t = -rayOrigin[1] / rayDir[1]; */
    
/*     printf("t parameter: %.2f\n", t); */
    
/*     if (t < 0.0f) { */
/*         printf("Intersection behind camera\n"); */
/*         // Project camera position onto ground plane */
/*         hitPoint[0] = rayOrigin[0]; */
/*         hitPoint[1] = 0.0f; */
/*         hitPoint[2] = rayOrigin[2]; */
/*         return false; */
/*     } */
    
/*     // Calculate hit point */
/*     hitPoint[0] = rayOrigin[0] + rayDir[0] * t; */
/*     hitPoint[1] = 0.0f; */
/*     hitPoint[2] = rayOrigin[2] + rayDir[2] * t; */
    
/*     printf("Ground intersection: (%.2f, %.2f, %.2f), Distance: %.2f\n",  */
/*            hitPoint[0], hitPoint[1], hitPoint[2], t); */
    
/*     return true; */
/* } */

void mouse_button_callback(int button, int action, int mods) {
    // Only handle mouse buttons in editor mode (camera inactive)
    if (!camera.active) {
        // Middle mouse button - orbit/pan
        if (button == GLFW_MOUSE_BUTTON_MIDDLE) {
            if (action == GLFW_PRESS) {
                middleMousePressed = true;
                glfwGetCursorPos(context.window, &lastPanX, &lastPanY);
                glfwGetCursorPos(context.window, &lastOrbitX, &lastOrbitY);
                
                shiftMiddleMousePressed = (mods & GLFW_MOD_SHIFT);
                
                // 1If not panning, calculate the pivot point for orbiting
                if (!shiftMiddleMousePressed) {
                    vec3 hitPoint;
                    bool hitGround = raycast_to_ground(&camera, hitPoint);
                    glm_vec3_copy(hitPoint, orbitPivot);
                    
                    // Calculate distance from camera to pivot
                    vec3 toPivot;
                    glm_vec3_sub(orbitPivot, camera.position, toPivot);
                    orbitDistance = glm_vec3_norm(toPivot);
                    
                    printf("Orbit pivot set to: (%.2f, %.2f, %.2f)\n", orbitPivot[0], orbitPivot[1], orbitPivot[2]);
                    printf("Orbit distance: %.2f\n", orbitDistance);
                }
                
            } else if (action == GLFW_RELEASE) {
                middleMousePressed = false;
                shiftMiddleMousePressed = false;
                
                camera.use_look_at = false;
                printf("Orbit mode disabled - returning to normal camera control\n");
            }
        }
        
        // Right mouse button - freelook with WASD
        if (button == GLFW_MOUSE_BUTTON_RIGHT) {
            if (action == GLFW_PRESS) {
                rightMousePressed = true;
                // Get initial position BEFORE disabling cursor
                glfwGetCursorPos(context.window, &lastX, &lastY);
                glfwSetInputMode(context.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            } else if (action == GLFW_RELEASE) {
                rightMousePressed = false;
                glfwSetInputMode(context.window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            }
        }
    }
}

void scroll_callback(double xoffset, double yoffset) {
    if (!camera.active) {
        float zoomSpeed = 0.5f;
        
        vec3 forward;
        glm_vec3_copy(camera.front, forward);
        glm_vec3_scale(forward, yoffset * zoomSpeed, forward);
        glm_vec3_add(camera.position, forward, camera.position);
    }
}

void cursor_pos_callback(double xpos, double ypos) {
    if (firstMouse) {
        lastX = xpos;
        lastY = ypos;
        firstMouse = false;
        return;  // Skip first frame to avoid jump
    }

    double xoffset = xpos - lastX;
    double yoffset = lastY - ypos;

    if (camera.active) {
        // Normal FPS camera control - always update lastX/lastY
        lastX = xpos;
        lastY = ypos;
        camera_process_mouse(&camera, xoffset, yoffset);
    }
    else if (rightMousePressed) {
        // Right mouse: freelook in editor mode - always update lastX/lastY
        lastX = xpos;
        lastY = ypos;
        camera_process_mouse(&camera, xoffset, yoffset);
    }
    else if (middleMousePressed) {
        if (shiftMiddleMousePressed) {
            if (camera.use_look_at) {
                camera_disable_orbit_mode(&camera);
                printf("Panning - orbit mode disabled\n");
            }
            // SHIFT + MIDDLE MOUSE: PAN
  
            float panSpeed = 0.005f;

            vec3 right, up;
            glm_vec3_cross(camera.front, camera.up, right);
            glm_vec3_normalize(right);
            glm_vec3_copy(camera.up, up);
            glm_vec3_normalize(up);

            vec3 panMovement = {0.0f, 0.0f, 0.0f};

            vec3 rightMove;
            glm_vec3_scale(right, (float)-xoffset * panSpeed, rightMove);
            glm_vec3_add(panMovement, rightMove, panMovement);

            vec3 upMove;
            glm_vec3_scale(up, (float)-yoffset * panSpeed, upMove);
            glm_vec3_add(panMovement, upMove, panMovement);

            glm_vec3_add(camera.position, panMovement, camera.position);

        } else {
            // MIDDLE MOUSE ONLY: ORBIT
            float orbitSpeed = 0.01f;
            camera_orbit_around_point(&camera, orbitPivot,
                                      (float)(xoffset * orbitSpeed),
                                      (float)(yoffset * orbitSpeed));
        }

        lastX = xpos;
        lastY = ypos;
    }
    else {
        // Not doing anything - just update last position to avoid jumps
        lastX = xpos;
        lastY = ypos;
    }
}




/* void recordCommandBuffer(VulkanContext* context, uint32_t imageIndex) { */
/*     VkCommandBuffer cmd = context->commandBuffers[imageIndex]; */

/*     VkCommandBufferBeginInfo beginInfo = { */
/*         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO */
/*     }; */

/*     vkBeginCommandBuffer(cmd, &beginInfo); */

/*     VkClearValue clearValues[2]; */
/*     clearValues[0].color = (VkClearColorValue){{0.0f, 0.0f, 0.0f, 1.0f}}; */
/*     clearValues[1].depthStencil = (VkClearDepthStencilValue){1.0f, 0}; */

/*     VkRenderPassBeginInfo renderPassInfo = { */
/*         .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, */
/*         .renderPass = context->renderPass, */
/*         .framebuffer = context->swapChainFramebuffers[imageIndex], */
/*         .renderArea = { */
/*             .offset = {0, 0}, */
/*             .extent = context->swapChainExtent */
/*         }, */
/*         .clearValueCount = 2, */
/*         .pClearValues = clearValues, */
/*     }; */

/*     vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE); */

/*     // Set AO state once globally for all 3D rendering */
/*     pushConstants.ambientOcclusionEnabled = ambientOcclusionEnabled ? 1 : 0; */

/*     // --- RENDER 3D SOLID GEOMETRY (TRIANGLES) --- */
/*     vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, context->graphicsPipeline); */
/*     vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, context->pipelineLayout,  */
/*                             0, 1, &descriptorSet, 0, NULL); */

/*     // Draw all meshes */
/*     meshes_draw(cmd, &scene.meshes); */

/*     // Or specify each one  */
/*     /\* Mesh *teapot = get_mesh("teapot"); *\/ */
/*     /\* Mesh *cow = get_mesh("cow"); *\/ */
/*     /\* mesh(cmd, teapot); *\/ */
/*     /\* mesh(cmd, cow); *\/ */


/*     // Draw immediate mode 3D triangle content (cubes, spheres, etc.) */
/*     renderer_draw(cmd); */

/*     // --- RENDER 3D TEXTURED GEOMETRY --- */
/*     renderer_draw_textured3D(cmd); */

/*     // --- RENDER LINES WITH LINE PIPELINE --- */
/*     if (context->graphicsPipelineLine && lineVertexCount > 0) { */
/*         vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, context->graphicsPipelineLine); */
/*         vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, context->pipelineLayout,  */
/*                                 0, 1, &descriptorSet, 0, NULL); */
/*         line_renderer_draw(cmd);  // Use the dedicated line renderer */
/*     } */

/*     // --- RENDER 2D CONTENT ON TOP (NO DEPTH TEST) --- */
/*     renderer2D_draw(cmd); */

/*     vkCmdEndRenderPass(cmd); */
/*     vkEndCommandBuffer(cmd); */
/* } */

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"


void screenshot(
                VkDevice device,
                VkPhysicalDevice physicalDevice,
                VkCommandPool commandPool,
                VkQueue queue,
                VkImage srcImage,
                VkFormat imageFormat,
                uint32_t width,
                uint32_t height,
                const char* filename)
{
    VkResult err;

    // 1. Create CPU-accessible buffer
    VkDeviceSize imageSize = width * height * 4;
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingBufferMemory;

    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = imageSize,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    err = vkCreateBuffer(device, &bufferInfo, NULL, &stagingBuffer);
    assert(err == VK_SUCCESS);

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(device, stagingBuffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memRequirements.size,
        .memoryTypeIndex = findMemoryType(physicalDevice, memRequirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };

    err = vkAllocateMemory(device, &allocInfo, NULL, &stagingBufferMemory);
    assert(err == VK_SUCCESS);
    vkBindBufferMemory(device, stagingBuffer, stagingBufferMemory, 0);

    // 2. Create command buffer for copy
    VkCommandBufferAllocateInfo cmdBufAllocInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandPool = commandPool,
        .commandBufferCount = 1,
    };

    VkCommandBuffer cmdBuf;
    vkAllocateCommandBuffers(device, &cmdBufAllocInfo, &cmdBuf);

    VkCommandBufferBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cmdBuf, &beginInfo);

    // 3. Transition image layout if necessary (skip if already TRANSFER_SRC_OPTIMAL)
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,  // adjust if different!
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = srcImage,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    vkCmdPipelineBarrier(
                         cmdBuf,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0,
                         0, NULL,
                         0, NULL,
                         1, &barrier);

    // 4. Copy image to buffer
    VkBufferImageCopy region = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .imageOffset = {0, 0, 0},
        .imageExtent = {width, height, 1},
    };

    vkCmdCopyImageToBuffer(cmdBuf, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stagingBuffer, 1, &region);

    // 5. Barrier to host read
    VkBufferMemoryBarrier bufBarrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = stagingBuffer,
        .offset = 0,
        .size = imageSize,
    };
    vkCmdPipelineBarrier(
                         cmdBuf,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT,
                         0,
                         0, NULL,
                         1, &bufBarrier,
                         0, NULL);

    vkEndCommandBuffer(cmdBuf);

    // 6. Submit and wait
    VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmdBuf,
    };
    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    // 7. Map buffer and save with stb_image_write
    void* data;
    vkMapMemory(device, stagingBufferMemory, 0, imageSize, 0, &data);

    // NOTE: Vulkan default is BGRA, convert to RGBA for PNG
    uint8_t* rgba_pixels = malloc(imageSize);
    for (uint32_t i = 0; i < width * height; ++i) {
        uint8_t* src = (uint8_t*)data + i * 4;
        uint8_t* dst = rgba_pixels + i * 4;
        dst[0] = src[2]; // R
        dst[1] = src[1]; // G
        dst[2] = src[0]; // B
        dst[3] = src[3]; // A
    }

    stbi_write_png(filename, width, height, 4, rgba_pixels, width * 4);

    free(rgba_pixels);

    vkUnmapMemory(device, stagingBufferMemory);

    // 8. Cleanup
    vkFreeCommandBuffers(device, commandPool, 1, &cmdBuf);
    vkDestroyBuffer(device, stagingBuffer, NULL);
    vkFreeMemory(device, stagingBufferMemory, NULL);
    printf("took screenshot");
}

                               

vec4 red    = {1.0f, 0.0f, 0.0f, 1.0f};
vec4 green  = {0.0f, 1.0f, 0.0f, 1.0f};
vec4 blue   = {0.0f, 0.0f, 1.0f, 1.0f};
vec4 yellow = {1.0f, 1.0f, 0.0f, 1.0f};

#include "keychords.h"


void testFunc() {
    printf("Pressed: C-x l\n");
}

void otherTestFunc() {
    printf("Pressed C-x C-l\n");
}

int main() {
    /* context.currentFrame = 0; */

    initWindow(WIDTH, HEIGHT, "OBSIDIAN ENGINE");

    /* glfwSetInputMode(context.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED); */

    /* createInstance(&context); */

    /* vec3 camera_pos = {0.0f, 3.0f, 0.0f}; // 2.0f */
    /* vec3 camera_target = {0.0f, 2.0f, 0.0f}; */
    /* camera_init(&camera, camera_pos, 90.0f, 0.0f, (float)WIDTH / (float)HEIGHT); */

    /* camera.active = true; */
    /* glfwSetInputMode(context.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED); */

    /* if (glfwCreateWindowSurface(context.instance, context.window, NULL, &context.surface) != VK_SUCCESS) { */
    /*     fprintf(stderr, "Failed to create window surface\n"); */
    /*     exit(EXIT_FAILURE); */
    /* } */

    /* pickPhysicalDevice(&context); */
    /* createLogicalDevice(&context); */
    /* createSwapChain(&context); */
    /* createImageViews(&context); */
    /* createDepthResources(&context); */

    /* createRenderPass(&context); */

    /* createUniformBuffer(&context); */
    /* createDescriptorSetLayout(&context); */

    /* createGraphicsPipeline(&context); */

    /* // 2D descriptor stuff FIRST (before 3D textured pipeline) */
    /* create2DDescriptorSetLayout(&context); */
    /* create2DDescriptorPool(&context); */

    /* // THEN 3D textured pipeline (needs 2D descriptor layout) */
    /* create3DTexturedGraphicsPipeline(&context); */

    /* // THEN rest of 2D */
    /* create2DGraphicsPipeline(&context); */
    /* createTextured2DGraphicsPipeline(&context); */
    /* createLineGraphicsPipeline(&context); */
    /* renderer2D_init(); */
    
    /* createDescriptorPool(&context); */
    /* createDescriptorSet(&context); */
    
    /* createFramebuffers(&context); */
    /* createCommandPool(&context); */
    
    /* // Initialize both 3D renderers */
    /* renderer_init( */
    /*               context.device, */
    /*               context.physicalDevice, */
    /*               context.commandPool, */
    /*               context.graphicsQueue */
    /*               ); */
    
    /* renderer_init_textured3D(); */
    
    /* line_renderer_init( */
    /*                    context.device, */
    /*                    context.physicalDevice,  */
    /*                    context.commandPool, */
    /*                    context.graphicsQueue */
    /*                    ); */
    
    /* createCommandBuffers(&context); */
    /* createSyncObjects(&context); */

    /* scene_init(&scene); */

    /* /\* load_obj("./assets/teapot.obj", "teapot",  red); *\/ */
    /* /\* load_obj("./assets/cow.obj", "cow", blue); *\/ */
    
    /* texture_pool_init(); */

    // Load textures
    int32_t tex1 = texture_pool_add(&context, "./assets/textures/puta.jpg");
    int32_t tex2 = texture_pool_add(&context, "./assets/textures/prototype/Orange/texture_01.png");
    int32_t tex3 = texture_pool_add(&context, "./assets/textures/prototype/Orange/texture_05.png");
    int32_t tex4 = texture_pool_add(&context, "./assets/textures/prototype/Dark/texture_03.png");
    int32_t tex5 = texture_pool_add(&context, "./assets/textures/pengu.png");
    
    Texture2D* texture1 = texture_pool_get(tex1);
    Texture2D* texture2 = texture_pool_get(tex2);
    Texture2D* texture3 = texture_pool_get(tex3);
    Texture2D* texture4 = texture_pool_get(tex4);
    Texture2D* texture5 = texture_pool_get(tex5);
    
    /* VkPhysicalDeviceProperties properties; */
    /* vkGetPhysicalDeviceProperties(context.physicalDevice, &properties); */
    /* float maxLineWidth = properties.limits.lineWidthRange[1]; */
    /* printf("MAX supported line width: %f\n", maxLineWidth); */
    

    load_gltf("./assets/gltf/AnimatedCube/glTF/AnimatedCube.gltf", &scene); // PASS
    load_gltf("./assets/gltf/AnimatedCube/glTF/AnimatedCube.gltf", &scene); // PASS

    /* load_gltf("./assets/gltf/AnimatedMorphSphere.glb", &scene); // FIXME ? */
    /* load_gltf("./assets/gltf/AlphaBlendModeTest.glb", &scene); // FIXME */
    /* load_gltf("./assets/gltf/UnlitTest.glb", &scene); // PASS */
    /* load_gltf("./assets/gltf/AnimatedMorphCube.glb", &scene); // PASS */
    /* load_gltf("./assets/gltf/SimpleMorph/glTF/SimpleMorph.gltf", &scene); // WHY IS THE LIGHTING LIKE THAT */
    /* load_gltf("./assets/gltf/Unicode❤♻Test.glb", &scene); // PASS */
    /* load_gltf("./assets/gltf/MaterialsVariantsShoe.glb", &scene); // FIXME */
    /* load_gltf("./assets/gltf/BoxAnimated.glb", &scene); // FIXME ANIMATION CHANNELS */
    /* load_gltf("./assets/gltf/Box.glb", &scene); // PASS */
    /* load_gltf("./assets/gltf/Corset.glb", &scene); // PASS */
    /* load_gltf("./assets/gltf/CesiumMan.glb", &scene); // FIXME RIG */
    /* load_gltf("./assets/gltf/ABeautifulGame/glTF/ABeautifulGame.gltf", &scene); // TODO Materials */
    /* load_gltf("./assets/gltf/Sponza/glTF/Sponza.gltf", &scene); // PASS */
    /* load_gltf("./assets/gltf/CarbonFibre.glb", &scene); // FIXME */
    /* load_gltf("./assets/gltf/MosquitoInAmber/glTF-Binary/MosquitoInAmber.glb", &scene); // FIXME Materials */
    /* load_gltf("./assets/gltf/MorphStressTest.glb", &scene); // PASS FIXME FLICKER ? */
    /* load_gltf("./assets/gltf/Fox.glb", &scene); // FIXME ANIMATIONS */
    /* load_gltf("./assets/gltf/RiggedFigure.glb", &scene); // FIXME */
    /* load_gltf("./assets/gltf/TextureCoordinateTest.glb", &scene); // PASS */
    /* load_gltf("./assets/gltf/Avocado.glb", &scene); // PASS */

    /* init_free_type(); */
    Font *jetbrains = load_font("./assets/fonts/JetBrainsMono-Regular.ttf", 81);
    
    registerKeyCallback(key_callback);
    registerScrollCallback(scroll_callback);
    registerCursorPosCallback(cursor_pos_callback);
    registerMouseButtonCallback(mouse_button_callback);

    /* keymap_init(&keymap); */

    keychord_bind(&keymap, "C-x L",     testFunc,           "TestFunc description", PRESS);
    keychord_bind(&keymap, "L",         testFunc,           "TestFunc description", PRESS);
    keychord_bind(&keymap, "C-c S-l",   testFunc,           "TestFunc description", PRESS);
    keychord_bind(&keymap, "C-x C-t c", otherTestFunc,      "TestFunc description", PRESS);
    keychord_bind(&keymap, "<left>",    otherTestFunc,      "TestFunc description", PRESS);
    keychord_bind(&keymap, "C-<right>", otherTestFunc,      "TestFunc description", PRESS);
    keychord_bind(&keymap, "S-<down>",  otherTestFunc,      "TestFunc description", PRESS);
    keychord_bind(&keymap, "<up>",      otherTestFunc,      "TestFunc description", PRESS);
    keychord_bind(&keymap, "TAB",       otherTestFunc,      "TestFunc description", PRESS);
    keychord_bind(&keymap, "M-x n L",   otherTestFunc,      "TestFunc description", PRESS);
    keychord_bind(&keymap, "C-g",       keymap_reset_state, "TestFunc description", PRESS);
    
    keymap_print_bindings(&keymap);
    

    while (!windowShouldClose()) {
        /* float current_frame = glfwGetTime(); */
        /* delta_time = current_frame - last_frame; */
        /* last_frame = current_frame; */
        
        /* animate_scene(&scene, current_frame); */

        
        /* camera_process_keyboard(&camera, context.window, delta_time); */
        /* camera_update(&camera); */

        beginFrame();

        
        /* if (!camera.active) { */
        /*     process_editor_movement(&camera, delta_time); */
        /* } */
        


        /* sort_meshes_by_alpha(&scene.meshes, camera.position); // HERE */
        
        
        /* // Update camera uniform buffer */
        /* UniformBufferObject ubo; */
        /* glm_mat4_mul(camera.projection_matrix, camera.view_matrix, ubo.vp); */
        /* void* data; */
        /* vkMapMemory(context.device, uniformBufferMemory, 0, sizeof(ubo), 0, &data); */
        /* memcpy(data, &ubo, sizeof(ubo)); */
        /* vkUnmapMemory(context.device, uniformBufferMemory); */
        
        /* // Clear all render buffers */
        /* renderer_clear(); */
        /* renderer_clear_textured3D(); */
        /* line_renderer_clear(); */
        /* renderer2D_clear(); */
        
        
        // 3D GEOMETRY
        /* vec3 v0 = { -0.03f, -0.03f, 0.0f }; */
        /* vec3 v1 = {  0.03f, -0.03f, 0.0f }; */
        /* vec3 v2 = {  0.03f,  0.03f, 0.0f }; */
        /* vec3 v3 = { -0.03f,  0.03f, 0.0f }; */
        /* vec3 center = { 0.0f, 0.0f, 0.0f }; */
        /* triangle(v0, center, v1, RED); */
        /* triangle(v1, center, v2, GREEN); */
        /* triangle(v2, center, v3, BLUE); */
        /* triangle(v3, center, v0, YELLOW); */
        
        
        sphere((vec3){0, 0, 30}, 5, 80, 80, YELLOW);
        
        // Rotate the cow
        /* static float cow_rotation = 0.0f; */
        /* cow_rotation += delta_time; */
        
        /* glm_mat4_identity(scene.meshes.items[0].model); */
        /* glm_rotate(scene.meshes.items[0].model, cow_rotation, (vec3){2.0f, 1.0f, 0.2f}); */
        
        
        // For the first cube, we need to: translate THEN rotate
        // So we need to rebuild the transform with translation first
        mat4 offset_transform;
        glm_mat4_identity(offset_transform);
        glm_translate(offset_transform, (vec3){5.0f, 0.0f, 0.0f});
        
        // Multiply: offset * animation = translate then rotate
        mat4 temp;
        glm_mat4_copy(scene.meshes.items[0].model, temp);
        glm_mat4_mul(offset_transform, temp, scene.meshes.items[0].model);
        
        
        
        text(jetbrains, "!\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~", 150, 50, WHITE);
        
        // Draw 3D text at world position
        text3D(jetbrains, "Hello 3D!", (vec3){0.0f, 5.0f, 10.0f}, 6, WHITE);
        text3D(jetbrains, "Press SPACE", (vec3){0.0f, 4.5f, 10.0f}, 6, RED);
        
        // 2D GEOMETRY
        quad2D((vec2){10, 10}, (vec2){50, 50}, BLUE);
        quad2D((vec2){70, 10}, (vec2){50, 50}, WHITE);
        quad2D((vec2){10, 70}, (vec2){50, 50}, RED);
        quad2D((vec2){70, 70}, (vec2){50, 50}, GREEN);
        
        
        
        texture2D((vec2){100, 200}, (vec2){200, 200}, texture1, WHITE);
        texture2D((vec2){500, 200}, (vec2){150, 150}, texture2, WHITE);
        /* texture2D((vec2){300, 300}, (vec2){600, 600}, texture2, WHITE); */
        
        
        // Render axes 
        float lineLength = 10000.0f; // Very long lines to appear "infinite"
        Color xColor = {0.937f, 0.310f, 0.420f, 1.0f}; // #EF4F6B X
        Color yColor = {0.529f, 0.839f, 0.008f, 1.0f}; // #87D602 Y
        Color zColor = {0.161f, 0.549f, 0.961f, 1.0f}; // #298CF5 Z
        line((vec3){-lineLength, 0.0f, 0.0f}, (vec3){lineLength, 0.0f, 0.0f}, xColor);
        line((vec3){0.0f, -lineLength, 0.0f}, (vec3){0.0f, lineLength, 0.0f}, yColor);
        line((vec3){0.0f, 0.0f, -lineLength}, (vec3){0.0f, 0.0f, lineLength}, zColor);
        
        
        /* // Create a properly textured plane that maintains texture aspect ratio */
        /* float floorWidth = 400.0f;   // 40 meter wide floor */
        /* float floorDepth = 200.0f;   // 20 meter deep floor */
        /* float textureWorldSize = 2.0f; // Each texture covers 2x2 meters */
        
        /* // Calculate tiling separately for X and Z to maintain aspect ratio */
        /* float tileX = floorWidth / textureWorldSize;  // 20 repeats in X */
        /* float tileZ = floorDepth / textureWorldSize;  // 10 repeats in Z */
        
        /* texturedPlane( */
        /*               (vec3){0.0f, 0.0f, 0.0f}, */
        /*               (vec2){floorWidth, floorDepth}, */
        /*               texture4, */
        /*               WHITE, */
        /*               tileX,  // Separate X tiling */
        /*               tileZ   // Separate Z tiling */
        /*               ); */
        
        
        /* // 3D TEXTURED GEOMETRY - Create a centered floor of cubes */
        /* int gridSize = 5;          // 5x5 grid (odd number to have center at 0,0) */
        /* float cubeSize = 2.0f;     // Size of each cube */
        /* float spacing = cubeSize;   // No padding - cubes touch each other */
        
        /* for (int x = -gridSize/2; x <= gridSize/2; x++) { */
        /*     for (int z = -gridSize/2; z <= gridSize/2; z++) { */
        /*         vec3 cubePos = { */
        /*             x * spacing,    // This will give positions like: -4, -2, 0, 2, 4 */
        /*             0.0f,          // All cubes on the ground */
        /*             z * spacing     // Same for Z direction */
        /*         }; */
        /*         texturedCube(cubePos, cubeSize, texture3, WHITE); */
        /*     } */
        /* } */
        
        // Billboards
        texture3D((vec3){0.0f, 2.0f, 5.0f}, (vec2){2.0f, 2.0f}, texture1, WHITE);
        texture3D((vec3){-3.0f, 1.0f, 8.0f}, (vec2){1.5f, 1.5f}, texture5, WHITE);
        texture3D((vec3){3.0f, 1.0f, 8.0f}, (vec2){1.5f, 1.5f}, texture1, WHITE);
        
        
        /* // Upload all geometry to GPU */
        /* renderer_upload(); */
        /* renderer_upload_textured3D();  // ADD THIS */
        /* line_renderer_upload(); */
        /* renderer2D_upload(); */
        
        
        
        /* // RENDER FRAME */
        /* uint32_t frameIndex = context.currentFrame; */
        /* VkFence inFlightFence = context.inFlightFences[frameIndex]; */
        /* vkWaitForFences(context.device, 1, &inFlightFence, VK_TRUE, UINT64_MAX); */
        
        /* uint32_t imageIndex; */
        /* VkResult result = vkAcquireNextImageKHR( */
        /*                                         context.device, context.swapChain, UINT64_MAX, */
        /*                                         context.imageAvailableSemaphores[frameIndex], VK_NULL_HANDLE, &imageIndex */
        /*                                         ); */
        
        /* if (result == VK_ERROR_OUT_OF_DATE_KHR) { */
        /*     continue; */
        /* } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) { */
        /*     fprintf(stderr, "Failed to acquire swap chain image\n"); */
        /*     exit(EXIT_FAILURE); */
        /* } */
        
        /* if (context.imagesInFlight[imageIndex] != VK_NULL_HANDLE) { */
        /*     vkWaitForFences(context.device, 1, &context.imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX); */
        /* } */
        /* context.imagesInFlight[imageIndex] = inFlightFence; */
        /* vkResetFences(context.device, 1, &inFlightFence); */
        
        /* // Re-record command buffer with new geometry */
        /* recordCommandBuffer(&context, imageIndex); */
        
        /* VkSemaphore waitSemaphores[] = { context.imageAvailableSemaphores[frameIndex] }; */
        /* VkSemaphore signalSemaphores[] = { context.renderFinishedSemaphores[imageIndex] }; */
        /* VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT }; */
        
        /* VkSubmitInfo submitInfo = { */
        /*     .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, */
        /*     .waitSemaphoreCount = 1, */
        /*     .pWaitSemaphores = waitSemaphores, */
        /*     .pWaitDstStageMask = waitStages, */
        /*     .commandBufferCount = 1, */
        /*     .pCommandBuffers = &context.commandBuffers[imageIndex], */
        /*     .signalSemaphoreCount = 1, */
        /*     .pSignalSemaphores = signalSemaphores */
        /* }; */
        
        /* if (vkQueueSubmit(context.graphicsQueue, 1, &submitInfo, inFlightFence) != VK_SUCCESS) { */
        /*     fprintf(stderr, "Failed to submit draw command buffer\n"); */
        /*     exit(EXIT_FAILURE); */
        /* } */
        
        /* VkPresentInfoKHR presentInfo = { */
        /*     .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, */
        /*     .waitSemaphoreCount = 1, */
        /*     .pWaitSemaphores = signalSemaphores, */
        /*     .swapchainCount = 1, */
        /*     .pSwapchains = &context.swapChain, */
        /*     .pImageIndices = &imageIndex, */
        /*     .pResults = NULL */
        /* }; */
        
        /* result = vkQueuePresentKHR(context.graphicsQueue, &presentInfo); */
        
        /* if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) { */
        /*     // Handle swapchain recreation */
        /* } else if (result != VK_SUCCESS) { */
        /*     fprintf(stderr, "Failed to present swap chain image\n"); */
        /*     exit(EXIT_FAILURE); */
        /* } */
        
        /* update_input(); */
        /* glfwPollEvents(); */
        /* context.currentFrame = (context.currentFrame + 1) % MAX_FRAMES_IN_FLIGHT; */


        endFrame();
    }
    
    vkDeviceWaitIdle(context.device);
    
    texture_pool_cleanup(&context);
    cleanup(&context);
    
    return EXIT_SUCCESS;
}

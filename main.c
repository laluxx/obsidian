#include "gltf_loader.h"
#include "font.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "vulkan_setup.h"
#include <cglm/cglm.h>
#include "camera.h"
#include "renderer.h"
#include "scene.h"
#include "context.h"
#include "common.h"
#include "input.h"
#include "window.h"
#include "keychords.h"
#include "vertico.h"
#include <stdio.h>
#include <inttypes.h>


/// TODO Stuff [0/2]
// - [ ] Panning up and down when loooking from upside down is wrong
// - [ ] ALL Textures are inversed!


double lastX = WIDTH / 2.0f, lastY = HEIGHT / 2.0f;
bool firstMouse = true;

bool shiftPressed;
bool ctrlPressed;
bool altPressed;


bool middleMousePressed = false;
bool rightMousePressed = false;
bool shiftMiddleMousePressed = false;
double lastPanX = 0.0, lastPanY = 0.0;
double lastOrbitX = 0.0, lastOrbitY = 0.0;
vec3 orbitPivot = {0.0f, 0.0f, 0.0f};  // The point we're orbiting around
float orbitDistance = 10.0f;           // Distance from pivot


bool keyW = false, keyA = false, keyS = false, keyD = false;
bool keyQ = false, keyE = false;
bool keySpace = false, keyShift = false;


void key_callback(int key, int action, int mods) {
    shiftPressed = mods & GLFW_MOD_SHIFT;
    ctrlPressed  = mods & GLFW_MOD_CONTROL;
    altPressed   = mods & GLFW_MOD_ALT;

   if (vertico.is_active) {
        if (action == PRESS || action == REPEAT)
            vertico_handle_char_input(key, mods);
        return;  // Don't process other keys when vertico is active
    }

    if (key == GLFW_KEY_Z && action == PRESS) {
        print_scene_meshes();
    }

    // Arrow keys for camera snapping
    if (key == KEY_LEFT && action == PRESS) {
        camera_snap_to_next_angle(&camera, true, false);  // Counter-clockwise
    }
    if (key == KEY_RIGHT && action == PRESS) {
        camera_snap_to_next_angle(&camera, false, false);   // Clockwise
    }
    if (key == KEY_UP && action == PRESS) {
        camera_snap_to_next_angle(&camera, true, true);   // Pitch up (more negative)
    }
    if (key == KEY_DOWN && action == PRESS) {
        camera_snap_to_next_angle(&camera, false, true);    // Pitch down (more positive)
    }

    if (key == KEY_T && action == PRESS) {
        ambientOcclusionEnabled = !ambientOcclusionEnabled;
        printf("Ambient Occlusion: %s\n", ambientOcclusionEnabled ? "ENABLED" : "DISABLED");
    }

    if (key == KEY_ESCAPE && action == PRESS) {
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

    if (key == KEY_F && action == PRESS) {
        vec3 world_origin = {0.0f, 0.0f, 0.0f};
        camera_set_look_at(&camera, world_origin);
        printf("Looking at world origin (0, 0, 0)\n");
    }

    // Track WASD key states
    if (key == KEY_W) keyW = (action != GLFW_RELEASE);
    if (key == KEY_A) keyA = (action != GLFW_RELEASE);
    if (key == KEY_S) keyS = (action != GLFW_RELEASE);
    if (key == KEY_D) keyD = (action != GLFW_RELEASE);
    if (key == KEY_Q) keyQ = (action != GLFW_RELEASE);
    if (key == KEY_E) keyE = (action != GLFW_RELEASE);
    if (key == KEY_SPACE) keySpace = (action != GLFW_RELEASE);
    if (key == KEY_LEFT_SHIFT) keyShift = (action != GLFW_RELEASE);
}

void mouse_button_callback(int button, int action, int mods) {
    // Only handle mouse buttons in editor mode (camera inactive)
    if (!camera.active) {
        // Middle mouse button - orbit/pan
        if (button == GLFW_MOUSE_BUTTON_MIDDLE) {
            if (action == PRESS) {
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
            if (action == PRESS) {
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

vec4 red    = {1.0f, 0.0f, 0.0f, 1.0f};
vec4 green  = {0.0f, 1.0f, 0.0f, 1.0f};
vec4 blue   = {0.0f, 0.0f, 1.0f, 1.0f};
vec4 yellow = {1.0f, 1.0f, 0.0f, 1.0f};

int main() {
    /* context.currentFrame = 0; */

    initWindow(WIDTH, HEIGHT, "OBSIDIAN ENGINE");

    setInputMode(context.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    /* load_obj("./assets/teapot.obj", "teapot",  red); */
    /* load_obj("./assets/cow.obj", "cow", blue); */

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

    /* load_gltf("./assets/gltf/AnimatedCube/glTF/AnimatedCube.gltf", &scene); // PASS */
    /* load_gltf("./assets/gltf/MetalRoughSpheres.glb", &scene);               // PASS */
    /* load_gltf("./assets/gltf/AnimatedMorphCube.glb", &scene);               // PASS */
    /* load_gltf("./assets/gltf/AnimatedMorphSphere.glb", &scene);             // PASS */
    /* load_gltf("./assets/gltf/AlphaBlendModeTest.glb", &scene);              // PASS */
    /* load_gltf("./assets/gltf/UnlitTest.glb", &scene);                       // PASS */
    /* load_gltf("./assets/gltf/Unicode❤♻Test.glb", &scene);                  // PASS */
    /* load_gltf("./assets/gltf/SimpleMorph/glTF/SimpleMorph.gltf", &scene);   // PASS */
    /* load_gltf("./assets/gltf/MaterialsVariantsShoe.glb", &scene);           // TODO variants in the editor */
    /* load_gltf("./assets/gltf/BoxAnimated.glb", &scene);                     // PASS */
    /* load_gltf("./assets/gltf/Box.glb", &scene);                             // PASS */
    /* load_gltf("./assets/gltf/Corset.glb", &scene);                          // PASS (but it's really small) */
    /* load_gltf("./assets/gltf/CesiumMan.glb", &scene);                       // PASS */
    /* load_gltf("./assets/gltf/RecursiveSkeletons.glb", &scene);              // FAIL */
    /* load_gltf("./assets/gltf/Sponza/glTF/Sponza.gltf", &scene);             // PASS */
    /* load_gltf("./assets/gltf/CarbonFibre.glb", &scene);                     // PASS */
    /* load_gltf("./assets/gltf/MorphStressTest.glb", &scene);                 // PASS */
    /* load_gltf("./assets/gltf/Avocado.glb", &scene);                         // PASS */
    /* load_gltf("./assets/gltf/Lantern.glb", &scene);                         // PASS */
    /* load_gltf("./assets/gltf/TextureCoordinateTest.glb", &scene);           // FAIL */
    /* load_gltf("./assets/gltf/AnimatedColorsCube.glb", &scene);              // FAIL */
    /* load_gltf("./assets/gltf/CubeVisibility.glb", &scene);                  // FAIL */
    /* load_gltf("./assets/gltf/EmissiveStrengthTest.glb", &scene);            // FAIL also the sun goes thorugh walls */
    /* load_gltf("./assets/gltf/InterpolationTest.glb", &scene);               // FAIL */
    load_gltf("./assets/gltf/DragonAttenuation.glb", &scene);               // PASS


    /* load_gltf("./assets/gltf/DispersionTest.glb", &scene); */
    /* load_gltf("./assets/gltf/DragonDispersion.glb", &scene); */
    /* load_gltf("./assets/gltf/IridescenceMetallicSpheres/glTF/IridescenceMetallicSpheres.gltf", &scene); // FAIL KHR_materials_iridescence */
    /* load_gltf("./assets/gltf/IORTestGrid.glb", &scene); */
    /* load_gltf("./assets/gltf/TransmissionTest.glb", &scene); */
    /* load_gltf("./assets/gltf/ABeautifulGame/glTF/ABeautifulGame.gltf", &scene); // TODO glass trasparent maeterial (refraction) */
    /* load_gltf("./assets/gltf/MosquitoInAmber/glTF-Binary/MosquitoInAmber.glb", &scene); // FIXME Materials */
    /* load_gltf("./assets/gltf/Fox.glb", &scene); // FIXME ANIMATIONS */
    /* load_gltf("./assets/gltf/RiggedFigure.glb", &scene); // FIXME */

    Font *jetbrains = load_font("./assets/fonts/JetBrainsMono-Regular.ttf", 81);
    vertico_init();


    // Bake and load the HDR Environment Map
    loadIBL(&context, "./assets/hdr/monochrome_studio_02_4k.hdr");
    /* loadIBL(&context, "./assets/hdr/meadow_2_4k.hdr"); */

    // Load our beautiful 4K rock material automatically!
    Material rockMat = load_pbr_material_dir("./assets/textures/rock_wall_10_4k.blend/textures");

    renderer_clear(); // Ensure clean state before loop

    registerKeyCallback(key_callback);

    registerScrollCallback(scroll_callback);
    registerCursorPosCallback(cursor_pos_callback);
    registerMouseButtonCallback(mouse_button_callback);

    keychord_bind(&keymap, "TAB",       toggle_skybox,             "Toggle the skybox",    PRESS);
    keychord_bind(&keymap, "t",         toggle_ibl_lighting,       "Toggle IBL lighting",  PRESS);
    keychord_bind(&keymap, "l",         toggle_shadows,            "Toggle shadows",       PRESS);
    keychord_bind(&keymap, "C-h c",     vertico_show_keybindings,  "Help keybindings",     PRESS);
    keychord_bind(&keymap, "C-g",       keymap_reset_state,        "TestFunc description", PRESS);
    keymap_print_bindings(&keymap);


    while (!windowShouldClose()) {
        beginFrame();

        vertico_render();

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


        // 64x64 is smooth and highly detailed, generating 24,576 dynamic vertices per frame!
        /* set_material(&rockMat); */
        /* sphere((vec3){0.0f, 0.0f, 30.0f}, 5.0f, 64, 64, WHITE); */
        /* reset_material(); */

        // Translate the cube directly in front of the camera so we can actually see it!
        /* if (scene.meshes.count > 0) { */
        /*     mat4 offset_transform; */
        /*     glm_mat4_identity(offset_transform); */
        /*     glm_translate(offset_transform, (vec3){0.0f, 3.0f, 5.0f}); // X=0, Y=3 (Camera Height), Z=5 (Forward) */

        /*     mat4 temp; */
        /*     glm_mat4_copy(scene.meshes.items[0].model, temp); */
        /*     glm_mat4_mul(offset_transform, temp, scene.meshes.items[0].model); */
        /*     markMeshesSSBODirty(&context); */
        /* } */

        // TODO
        /* set_material(&rockMat); */
        /* cube((vec3){0.0f, 0.0f, 10.0f}, 3.0f, WHITE); */
        /* reset_material(); */


        /* text(jetbrains, "!\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~", 150, 50, WHITE); */

        // Draw 3D text at world position
        /* text3D(jetbrains, "Hello 3D!", (vec3){0.0f, 5.0f, 10.0f}, 6, WHITE); */
        /* text3D(jetbrains, "Press SPACE", (vec3){0.0f, 4.5f, 10.0f}, 6, RED); */

        double time = glfwGetTime();

        // Center of the screen - use actual Vulkan swapchain dimensions
        int screenWidth = context.swapChainExtent.width;
        int screenHeight = context.swapChainExtent.height;
        float centerX = screenWidth / 2.0f;
        float centerY = screenHeight / 2.0f;

        // 2D GEOMETRY
        /* quad2D((vec2){10, 10}, (vec2){50, 50}, BLUE); */
        /* quad2D((vec2){70, 10}, (vec2){50, 50}, WHITE); */
        /* quad2D((vec2){10, 70}, (vec2){50, 50}, RED); */
        /* quad2D((vec2){70, 70}, (vec2){50, 50}, GREEN); */

        fps(jetbrains, 500, 500, RED);


        /* texture2D((vec2){100, 200}, (vec2){200, 200}, texture1, WHITE); */
        /* texture2D((vec2){500, 200}, (vec2){150, 150}, texture2, WHITE); */
        /* texture2D((vec2){300, 300}, (vec2){600, 600}, texture2, WHITE); */


        // Render axes
        float lineLength = 10000.0f; // Very long lines to appear "infinite"
        Color xColor = {0.937f, 0.310f, 0.420f, 1.0f}; // #EF4F6B X
        Color yColor = {0.529f, 0.839f, 0.008f, 1.0f}; // #87D602 Y
        Color zColor = {0.161f, 0.549f, 0.961f, 1.0f}; // #298CF5 Z
        line((vec3){-lineLength, 0.0f, 0.0f}, (vec3){lineLength, 0.0f, 0.0f}, xColor);
        line((vec3){0.0f, -lineLength, 0.0f}, (vec3){0.0f, lineLength, 0.0f}, yColor);
        line((vec3){0.0f, 0.0f, -lineLength}, (vec3){0.0f, 0.0f, lineLength}, zColor);

        endFrame();
    }

    vkDeviceWaitIdle(context.device);

    texture_pool_cleanup(&context);
    cleanup(&context);

    return EXIT_SUCCESS;
}

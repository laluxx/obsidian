#include "gltf_loader.h"
#include "font.h"
#include "theme.h"
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
#include "editor.h"
#include "gizmo.h"
#include "easing.h"
#include <stdio.h>
#include <inttypes.h>



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
float orbitDistance = 15.0f;           // Distance from pivot

bool keyW = false, keyA = false, keyS = false, keyD = false;
bool keyQ = false, keyE = false;
bool keySpace = false, keyShift = false;


// Camera framing animation state
bool is_framing = false;
vec3 frame_start_pos = {0};
vec3 frame_target_pos = {0};
float frame_anim_time = 0.0f;
const float FRAME_ANIM_DURATION = 0.4f; // 400ms for a snappy pop

// Smooth scroll wheel zooming state
bool lerp_scroll_wheel_zoom = true;
bool is_zooming = false;
vec3 zoom_start_pos = {0};
vec3 zoom_target_pos = {0};
float zoom_anim_time = 0.0f;
const float ZOOM_ANIM_DURATION = 0.15f; // Fast, tactile 150ms settle


typedef void (*TextInputCallback)(char c);
TextInputCallback active_text_input_cb = NULL;

void set_active_text_input(TextInputCallback cb) {
    active_text_input_cb = cb;
}

char translate_key_to_char(int key, int mods) {
    if (mods & (GLFW_MOD_CONTROL | GLFW_MOD_ALT | GLFW_MOD_SUPER)) return 0;
    char c = 0;
    if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z) {
        c = (mods & GLFW_MOD_SHIFT) ? key : key + 32;
    } else if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9) {
        const char shifted_nums[] = ")!@#$%^&*(";
        c = (mods & GLFW_MOD_SHIFT) ? shifted_nums[key - GLFW_KEY_0] : key;
    } else if (key == GLFW_KEY_SPACE) {
        c = ' ';
    } else if (key == GLFW_KEY_MINUS) {
        c = (mods & GLFW_MOD_SHIFT) ? '_' : '-';
    } else if (key == GLFW_KEY_EQUAL) {
        c = (mods & GLFW_MOD_SHIFT) ? '+' : '=';
    } else if (key == GLFW_KEY_LEFT_BRACKET) {
        c = (mods & GLFW_MOD_SHIFT) ? '{' : '[';
    } else if (key == GLFW_KEY_RIGHT_BRACKET) {
        c = (mods & GLFW_MOD_SHIFT) ? '}' : ']';
    } else if (key == GLFW_KEY_BACKSLASH) {
        c = (mods & GLFW_MOD_SHIFT) ? '|' : '\\';
    } else if (key == GLFW_KEY_SEMICOLON) {
        c = (mods & GLFW_MOD_SHIFT) ? ':' : ';';
    } else if (key == GLFW_KEY_APOSTROPHE) {
        c = (mods & GLFW_MOD_SHIFT) ? '"' : '\'';
    } else if (key == GLFW_KEY_COMMA) {
        c = (mods & GLFW_MOD_SHIFT) ? '<' : ',';
    } else if (key == GLFW_KEY_PERIOD) {
        c = (mods & GLFW_MOD_SHIFT) ? '>' : '.';
    } else if (key == GLFW_KEY_SLASH) {
        c = (mods & GLFW_MOD_SHIFT) ? '?' : '/';
    } else if (key == GLFW_KEY_GRAVE_ACCENT) {
        c = (mods & GLFW_MOD_SHIFT) ? '~' : '`';
    }
    return c;
}

void key_callback(int key, int action, int mods) {
    shiftPressed = mods & GLFW_MOD_SHIFT;
    ctrlPressed  = mods & GLFW_MOD_CONTROL;
    altPressed   = mods & GLFW_MOD_ALT;

    if (active_text_input_cb) {
        if (action == GLFW_PRESS || action == GLFW_REPEAT) {
            char c = translate_key_to_char(key, mods);
            if (c != 0) active_text_input_cb(c);
        }
        return;
    }
    if (key == GLFW_KEY_Z && action == PRESS) {
        print_scene_meshes();
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
        vec3 target_center = {0.0f, 0.0f, 0.0f};
        float target_dist = 10.0f;

        if (editor.inspector.selected_mesh_index >= 0 && editor.inspector.selected_mesh_index < (int)scene.meshes.count) {
            Mesh* m = &scene.meshes.items[editor.inspector.selected_mesh_index];

            // Calculate local center
            vec3 local_center;
            glm_vec3_add(m->aabbMin, m->aabbMax, local_center);
            glm_vec3_scale(local_center, 0.5f, local_center);

            // Transform to world space
            vec4 lc4 = {local_center[0], local_center[1], local_center[2], 1.0f};
            vec4 wc4;
            glm_mat4_mulv(m->model, lc4, wc4);
            glm_vec3_copy(wc4, target_center);

            // Estimate radius for distance
            vec3 extents;
            glm_vec3_sub(m->aabbMax, m->aabbMin, extents);
            float scale = glm_vec3_norm((vec3){m->model[0][0], m->model[0][1], m->model[0][2]});
            target_dist = glm_vec3_norm(extents) * scale * 1.2f;
            if (target_dist < 2.0f) target_dist = 2.0f;

            printf("Framing selected mesh %d at (%.2f, %.2f, %.2f)\n", editor.inspector.selected_mesh_index, target_center[0], target_center[1], target_center[2]);
        } else {
            printf("Framing world origin (0, 0, 0)\n");
        }

        // New camera position: target_center - (camera.front * target_dist)
        glm_vec3_copy(camera.position, frame_start_pos);
        glm_vec3_copy(camera.front, frame_target_pos);
        glm_vec3_scale(frame_target_pos, -target_dist, frame_target_pos);
        glm_vec3_add(target_center, frame_target_pos, frame_target_pos);

        // Set orbit pivot to the framed center so orbiting is perfectly centered around it!
        glm_vec3_copy(target_center, orbitPivot);
        orbitDistance = target_dist;

        is_framing = true;
        frame_anim_time = 0.0f;
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
        double mx, my;
        glfwGetCursorPos(context.window, &mx, &my);
        gizmo_mouse_button(button, action, mods, mx, my);

        // Middle mouse button - orbit/pan
        if (button == GLFW_MOUSE_BUTTON_MIDDLE) {
            if (action == PRESS) {
                middleMousePressed = true;
                glfwGetCursorPos(context.window, &lastPanX, &lastPanY);
                glfwGetCursorPos(context.window, &lastOrbitX, &lastOrbitY);

                shiftMiddleMousePressed = (mods & GLFW_MOD_SHIFT);

                // If not panning, calculate the pivot point for orbiting
                if (!shiftMiddleMousePressed) {
                    // If a mesh is selected, always orbit perfectly around its true center
                    if (editor.inspector.selected_mesh_index >= 0 && editor.inspector.selected_mesh_index < (int)scene.meshes.count) {
                        Mesh* m = &scene.meshes.items[editor.inspector.selected_mesh_index];
                        vec3 local_center;
                        glm_vec3_add(m->aabbMin, m->aabbMax, local_center);
                        glm_vec3_scale(local_center, 0.5f, local_center);
                        vec4 lc4 = {local_center[0], local_center[1], local_center[2], 1.0f};
                        vec4 wc4;
                        glm_mat4_mulv(m->model, lc4, wc4);
                        glm_vec3_copy(wc4, orbitPivot);
                    } else {
                        // Otherwise fallback to the ground plane raycast
                        vec3 hitPoint;
                        bool hitGround = raycast_to_ground(&camera, hitPoint);
                        glm_vec3_copy(hitPoint, orbitPivot);
                    }

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

        if (lerp_scroll_wheel_zoom) {
            if (!is_zooming) {
                glm_vec3_copy(camera.position, zoom_start_pos);
                glm_vec3_copy(camera.position, zoom_target_pos);
            } else {
                // If already zooming, branch off smoothly from current visual position
                glm_vec3_copy(camera.position, zoom_start_pos);
            }

            // Accumulate the target position for continuous scrolling
            glm_vec3_add(zoom_target_pos, forward, zoom_target_pos);
            is_zooming = true;
            zoom_anim_time = 0.0f;
        } else {
            glm_vec3_add(camera.position, forward, camera.position);
        }
    }
}

void cursor_pos_callback(double xpos, double ypos) {
    if (!camera.active) {
        gizmo_mouse_move(xpos, ypos);
    }

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

    initWindow(WIDTH, HEIGHT, "Obsidian Engine");

    setInputMode(context.window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);

    editor_init();
    gizmo_init();

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

    /* load_gltf("./assets/gltf/AnimatedCube/glTF/AnimatedCube.gltf", &scene);     // PASS */
    /* load_gltf("./assets/gltf/MetalRoughSpheres.glb", &scene);                   // PASS */
    /* load_gltf("./assets/gltf/AnimatedMorphCube.glb", &scene);                   // PASS */
    /* load_gltf("./assets/gltf/AnimatedMorphSphere.glb", &scene);                 // PASS */
    /* load_gltf("./assets/gltf/AlphaBlendModeTest.glb", &scene);                  // PASS */
    /* load_gltf("./assets/gltf/UnlitTest.glb", &scene);                           // PASS */
    /* load_gltf("./assets/gltf/Unicode❤♻Test.glb", &scene);                      // PASS */
    /* load_gltf("./assets/gltf/SimpleMorph/glTF/SimpleMorph.gltf", &scene);       // PASS */
    /* load_gltf("./assets/gltf/MaterialsVariantsShoe.glb", &scene);               // TODO variants in the editor */
    /* load_gltf("./assets/gltf/BoxAnimated.glb", &scene);                         // PASS */
    /* load_gltf("./assets/gltf/Box.glb", &scene);                                 // PASS */
    /* load_gltf("./assets/gltf/Corset.glb", &scene);                              // PASS (but it's really small) */
    /* load_gltf("./assets/gltf/CesiumMan.glb", &scene);                           // PASS */
    /* load_gltf("./assets/gltf/Fox.glb", &scene);                                 // PASS */
    /* load_gltf("./assets/gltf/RecursiveSkeletons.glb", &scene);                  // PASS */
    /* load_gltf("./assets/gltf/RiggedFigure.glb", &scene);                        // PASS */
    /* load_gltf("./assets/gltf/Sponza/glTF/Sponza.gltf", &scene);                 // PASS */
    /* load_gltf("./assets/gltf/CarbonFibre.glb", &scene);                         // PASS */
    /* load_gltf("./assets/gltf/MorphStressTest.glb", &scene);                     // PASS */
    /* load_gltf("./assets/gltf/Avocado.glb", &scene);                             // PASS */
    /* load_gltf("./assets/gltf/Lantern.glb", &scene);                             // PASS */
    /* load_gltf("./assets/gltf/TextureCoordinateTest.glb", &scene);               // PASS */
    /* load_gltf("./assets/gltf/AnimatedColorsCube.glb", &scene);                  // FAIL */
    /* load_gltf("./assets/gltf/CubeVisibility.glb", &scene);                      // FAIL */
    /* load_gltf("./assets/gltf/EmissiveStrengthTest.glb", &scene);                // FAIL also the sun goes thorugh walls */
    /* load_gltf("./assets/gltf/InterpolationTest.glb", &scene);                   // FAIL */
    load_gltf("./assets/gltf/DragonAttenuation.glb", &scene);                   // PASS
    /* load_gltf("./assets/gltf/ABeautifulGame/glTF/ABeautifulGame.gltf", &scene); // PASS */
    /* load_gltf("./assets/gltf/MosquitoInAmber/glTF-Binary/MosquitoInAmber.glb", &scene); // PASS */
    /* load_gltf("./assets/gltf/DragonDispersion.glb", &scene);                    // FAIL */
    /* load_gltf("./assets/gltf/DispersionTest.glb", &scene);                      // FAIL */
    /* load_gltf("./assets/gltf/IridescenceMetallicSpheres/glTF/IridescenceMetallicSpheres.gltf", &scene); // FAIL KHR_materials_iridescence */
    /* load_gltf("./assets/gltf/IORTestGrid.glb", &scene);                         // FAIL */
    /* load_gltf("./assets/gltf/TransmissionTest.glb", &scene);                    // FAIL */


    Font *jetbrains = load_font("./assets/fonts/JetBrainsMono-Regular.ttf", 81);
    vertico_init();


    // Bake and load the HDR Environment Map
    loadIBL(&context, "./assets/hdr/monochrome_studio_02_4k.hdr");
    /* loadIBL(&context, "./assets/hdr/meadow_2_4k.hdr"); */
    /* loadIBL(&context, "./assets/hdr/ferndale_studio_05_4k.hdr"); */

    // Load our beautiful 4K rock material automatically!
    Material rockMat = load_pbr_material_dir("./assets/textures/rock_wall_10_4k.blend/textures");

    renderer_clear(); // Ensure clean state before loop

    registerKeyCallback(key_callback);

    registerScrollCallback(scroll_callback);
    registerCursorPosCallback(cursor_pos_callback);
    registerMouseButtonCallback(mouse_button_callback);

    keychord_bind(&keymap, "<left>",    camera_snap_left,   "Camera snap left",     PRESS);
    keychord_bind(&keymap, "<right>",   camera_snap_right,  "Camera snap right",    PRESS);
    keychord_bind(&keymap, "<up>",      camera_snap_up,     "Camera snap up",       PRESS);
    keychord_bind(&keymap, "<down>",    camera_snap_down,   "Camera snap down",     PRESS);
    keychord_bind(&keymap, "TAB",       toggle_skybox,             "Toggle the skybox",    PRESS);
    keychord_bind(&keymap, "t",         toggle_ibl_lighting,       "Toggle IBL lighting",  PRESS);
    keychord_bind(&keymap, "l",         toggle_shadows,            "Toggle shadows",       PRESS);
    keychord_bind(&keymap, "C-h c",     vertico_show_keybindings,  "Help keybindings",     PRESS);
    keychord_bind(&keymap, "C-g",       keymap_reset_state,        "TestFunc description", PRESS);
    keymap_print_bindings(&keymap);


    ease_init(); // Initialize the easing lookup tables

    // Variables for delta time
    double lastFrameTime = glfwGetTime();

    while (!windowShouldClose()) {
        double currentFrameTime = glfwGetTime();
        float deltaTime = (float)(currentFrameTime - lastFrameTime);
        lastFrameTime = currentFrameTime;

        if (is_framing) {
            is_zooming = false; // Framing gracefully overrides manual zooming
            frame_anim_time += deltaTime;
            float t = frame_anim_time / FRAME_ANIM_DURATION;
            if (t >= 1.0f) {
                t = 1.0f;
                is_framing = false;
            }

            // EASE_EXPO_OUT gives that fast snap and smooth settle exactly like Godot
            float ease_t = ease_expo_out(t);
            glm_vec3_lerp(frame_start_pos, frame_target_pos, ease_t, camera.position);
        } else if (is_zooming) {
            zoom_anim_time += deltaTime;
            float t = zoom_anim_time / ZOOM_ANIM_DURATION;
            if (t >= 1.0f) {
                t = 1.0f;
                is_zooming = false;
            }

            // EASE_QUART_OUT is slightly less aggressive than EXPO_OUT, making consecutive mouse wheel clicks feel buttery smooth
            float ease_t = ease_quart_out(t);
            glm_vec3_lerp(zoom_start_pos, zoom_target_pos, ease_t, camera.position);
        }

        camera_update_animations(&camera, deltaTime);

        beginFrame();

        editor_update();
        editor_render();
        gizmo_render(editor.inspector.selected_mesh_index);

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

        fps(jetbrains, 200, 200, RED);


        /* texture2D((vec2){100, 200}, (vec2){200, 200}, texture1, WHITE); */
        /* texture2D((vec2){500, 200}, (vec2){150, 150}, texture2, WHITE); */
        /* texture2D((vec2){300, 300}, (vec2){600, 600}, texture2, WHITE); */

        // Render axes
        float lineLength = 10000.0f; // Very long lines to appear "infinite"
        line((vec3){-lineLength, 0.0f, 0.0f}, (vec3){lineLength, 0.0f, 0.0f}, CT.x);
        line((vec3){0.0f, -lineLength, 0.0f}, (vec3){0.0f, lineLength, 0.0f}, CT.y);
        line((vec3){0.0f, 0.0f, -lineLength}, (vec3){0.0f, 0.0f, lineLength}, CT.z);

        endFrame();
    }

    vkDeviceWaitIdle(context.device);
    editor_cleanup();
    texture_pool_cleanup(&context);
    cleanup(&context);

    return EXIT_SUCCESS;
}

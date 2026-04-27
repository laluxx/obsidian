#include "input.h"
#include "context.h"
#include "keychords.h"
#include <stdio.h>
#include <string.h>
#include "camera.h"
#include "scene.h"
#include "editor.h"
#include "gizmo.h"
#include "gltf_loader.h"
#include <cglm/cglm.h>
#include "window.h"

double lastX = 800 / 2.0f, lastY = 600 / 2.0f;
bool firstMouse = true;

bool shiftPressed;
bool ctrlPressed;
bool altPressed;

bool middleMousePressed = false;
bool rightMousePressed = false;
bool shiftMiddleMousePressed = false;
double lastPanX = 0.0, lastPanY = 0.0;
double lastOrbitX = 0.0, lastOrbitY = 0.0;
vec3 orbitPivot = {0.0f, 0.0f, 0.0f};
float orbitDistance = 15.0f;

bool keyW = false, keyA = false, keyS = false, keyD = false;
bool keyQ = false, keyE = false;
bool keySpace = false, keyShift = false;

bool is_framing = false;
vec3 frame_start_pos = {0};
vec3 frame_target_pos = {0};
float frame_anim_time = 0.0f;
const float FRAME_ANIM_DURATION = 0.4f;

bool lerp_scroll_wheel_zoom = true;
bool is_zooming = false;
vec3 zoom_start_pos = {0};
vec3 zoom_target_pos = {0};
float zoom_anim_time = 0.0f;
const float ZOOM_ANIM_DURATION = 0.15f;


#define MAX_KEYS 512
static int keys[MAX_KEYS];
static int keysPressed[MAX_KEYS];
static int keysReleased[MAX_KEYS];

#define MAX_MOUSE_BUTTONS 8
static int mouseButtons[MAX_MOUSE_BUTTONS];
static int mouseButtonsPressed[MAX_MOUSE_BUTTONS];
static int mouseButtonsReleased[MAX_MOUSE_BUTTONS];

#define MAX_GAMEPAD_BUTTONS 15
#define MAX_GAMEPAD_AXES 6
static int gamepadButtons[MAX_GAMEPAD_BUTTONS];
static int gamepadButtonsPressed[MAX_GAMEPAD_BUTTONS];
static float gamepadAxes[MAX_GAMEPAD_AXES];

bool printKeyInfo = true;

static Vec2f lastMousePosition = {0.0, 0.0};
static Vec2f currentMousePosition = {0.0, 0.0};

void init_input() {
    for (int i = 0; i < MAX_KEYS; i++) {
        keys[i] = 0;
        keysPressed[i] = 0;
        keysReleased[i] = 0;
    }
    for (int i = 0; i < MAX_MOUSE_BUTTONS; i++) {
        mouseButtons[i] = 0;
        mouseButtonsPressed[i] = 0;
        mouseButtonsReleased[i] = 0;
    }
}

static void update_mouse_buttons() {
    for (int i = 0; i < MAX_MOUSE_BUTTONS; i++) {
        int newState = glfwGetMouseButton(context.window, i);
        if (newState == GLFW_PRESS && mouseButtons[i] == 0) {
            mouseButtonsPressed[i] = 1;
            mouseButtons[i] = 1;
        } else if (newState == GLFW_RELEASE && mouseButtons[i] == 1) {
            mouseButtonsReleased[i] = 1;
            mouseButtons[i] = 0;
        } else {
            mouseButtonsPressed[i] = 0;
            mouseButtonsReleased[i] = 0;
        }
    }
}

static void gamepad_update() {
    GLFWgamepadstate state;
    if (glfwGetGamepadState(GLFW_JOYSTICK_1, &state)) {
        for (int i = 0; i < MAX_GAMEPAD_BUTTONS; i++) {
            int newState = state.buttons[i];
            if (newState == GLFW_PRESS && gamepadButtons[i] == 0) {
                gamepadButtons[i] = 1;
                gamepadButtonsPressed[i] = 1;
                if (printKeyInfo)
                    printf("Gamepad Button %d pressed\n", i);
            } else if (newState == GLFW_RELEASE && gamepadButtons[i] == 1) {
                gamepadButtons[i] = 0;
                if (printKeyInfo)
                    printf("Gamepad Button %d released\n", i);
            }
        }
        for (int i = 0; i < MAX_GAMEPAD_AXES; i++) {
            gamepadAxes[i] = state.axes[i];
        }
    }
}

void update_input() {
    glfwGetCursorPos(context.window, &currentMousePosition.x, &currentMousePosition.y);

    // Reset states
    for (int i = 0; i < MAX_KEYS; i++) {
        keysPressed[i] = 0;
        keysReleased[i] = 0;
    }
    for (int i = 0; i < MAX_MOUSE_BUTTONS; i++) {
        mouseButtonsPressed[i] = 0;
        mouseButtonsReleased[i] = 0;
    }
    for (int i = 0; i < MAX_GAMEPAD_BUTTONS; i++) {
        gamepadButtonsPressed[i] = 0;
    }

    update_mouse_buttons();
    gamepad_update();
}

int isKeyDown(int key) {
    if (key < 0 || key >= MAX_KEYS)
        return 0;
    return keys[key];
}

int isKeyPressed(int key) {
    if (key < 0 || key >= MAX_KEYS)
        return 0;
    return keysPressed[key];
}

int isKeyReleased(int key) {
    if (key < 0 || key >= MAX_KEYS)
        return 0;
    return keysReleased[key];
}

int isMouseButtonPressed(int button) {
    if (button < 0 || button >= MAX_MOUSE_BUTTONS) return 0;
    return mouseButtonsPressed[button];
}

int isMouseButtonReleased(int button) {
    if (button < 0 || button >= MAX_MOUSE_BUTTONS) return 0;
    return mouseButtonsReleased[button];
}

int isMouseButtonDown(int button) {
    if (button < 0 || button >= MAX_MOUSE_BUTTONS) return 0;
    return mouseButtons[button];
}


int isGamepadButtonPressed(int button) {
    if (button < 0 || button >= MAX_GAMEPAD_BUTTONS)
        return 0;
    return gamepadButtonsPressed[button];
}

int isGamepadButtonDown(int button) {
    if (button < 0 || button >= MAX_GAMEPAD_BUTTONS)
        return 0;
    return gamepadButtons[button];
}

void getCursorPosition(double* x, double* y) {
    if (context.window != NULL) {
        glfwGetCursorPos(context.window, x, y);
        int windowHeight;
        glfwGetWindowSize(context.window, NULL, &windowHeight);
        *y = windowHeight - *y; // NOTE Invert the y-coordinate
    } else {
        printf("NO CONTEXT\n");
        *x = 0.0;
        *y = 0.0;
    }
}

bool getMouseButton(int button) {
    if (context.window != NULL) {
        return glfwGetMouseButton(context.window, button) == GLFW_PRESS;
    } else {
        printf("NO CONTEXT\n");
        return false;
    }
}

// NOTE not a glfw wrapper
Vec2f getMouseDelta() {
    Vec2f mouseDelta = {
        currentMousePosition.x - lastMousePosition.x,
        currentMousePosition.y - lastMousePosition.y
    };

    lastMousePosition = currentMousePosition;
    return mouseDelta;
}

ScrollCallback currentScrollCallback = NULL;
void registerScrollCallback(ScrollCallback callback) {
    currentScrollCallback = callback;
}

TextCallback currentTextCallback = NULL;
void registerTextCallback(TextCallback callback) {
    currentTextCallback = callback;
}

KeyInputCallback currentKeyCallback = NULL;
void registerKeyCallback(KeyInputCallback callback) {
    currentKeyCallback = callback;
}

MouseButtonCallback currentMouseButtonCallback = NULL;
void registerMouseButtonCallback(MouseButtonCallback callback) {
    currentMouseButtonCallback = callback;
}

CursorPosCallback currentCursorPosCallback = NULL;
void registerCursorPosCallback(CursorPosCallback callback) {
    currentCursorPosCallback = callback;
}

WindowResizeCallback currentResizeCallback = NULL;
void registerWindowResizeCallback(WindowResizeCallback callback) {
    currentResizeCallback = callback;
}

WindowFocusCallback currentFocusCallback = NULL;
void registerWindowFocusCallback(WindowFocusCallback callback) {
    currentFocusCallback = callback;
}

WindowPosCallback currentWindowPosCallback = NULL;
void registerWindowPosCallback(WindowPosCallback callback) {
    currentWindowPosCallback = callback;
}

DropCallback currentDropCallback = NULL;
void registerDropCallback(DropCallback callback) {
    currentDropCallback = callback;
}



// Add a flag to track if we should skip the next char callback
static bool skip_next_char = false;

void internal_char_callback(GLFWwindow* window, unsigned int codepoint) {
    if (skip_next_char) {
        skip_next_char = false;
        return;
    }

    if (currentTextCallback != NULL) {
        currentTextCallback(codepoint);
    }
}

void internal_key_callback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    // Reset the skip flag at the start of each key event
    skip_next_char = false;

    // Update the internal state first
    if (action == GLFW_PRESS) {
        keys[key] = 1;
        keysPressed[key] = 1;
    } else if (action == GLFW_RELEASE) {
        keys[key] = 0;
        keysReleased[key] = 1;
    }

    // Process keychords with stack (local maps have priority)
    bool handled = keychord_process_key_with_stack(key, action, mods);
    if (handled) {
        skip_next_char = true;
        return;
    }

    shiftPressed = mods & GLFW_MOD_SHIFT;
    ctrlPressed  = mods & GLFW_MOD_CONTROL;
    altPressed   = mods & GLFW_MOD_ALT;

    extern bool editor_process_text_input(int key, int action, int mods);
    if (editor_process_text_input(key, action, mods)) {
        return;
    }

    // Engine-level mode toggle (Hardcoded to bypass keymap parsers)
    if (key == 256 /* GLFW_KEY_ESCAPE */ && action == PRESS) {
        extern void toggle_editor_mode(void);
        toggle_editor_mode();
        return;
    }

    // Frame selected mesh (Editor Mode Only)
    if (key == GLFW_KEY_F && action == PRESS && !camera.active) {
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

    if (key == KEY_T && action == PRESS) {
        extern bool ambientOcclusionEnabled;
        ambientOcclusionEnabled = !ambientOcclusionEnabled;
        printf("Ambient Occlusion: %s\n", ambientOcclusionEnabled ? "ENABLED" : "DISABLED");
    }

    // Then call the user's callback if it's registered and keychord didn't handle it
    if (currentKeyCallback != NULL) {
        currentKeyCallback(key, action, mods);
    }
}

void internal_mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
    if (!camera.active) {
        double mx, my;
        glfwGetCursorPos(context.window, &mx, &my);
        gizmo_mouse_button(button, action, mods, mx, my);

        if (button == GLFW_MOUSE_BUTTON_MIDDLE) {
            if (action == PRESS) {
                middleMousePressed = true;
                glfwGetCursorPos(context.window, &lastPanX, &lastPanY);
                glfwGetCursorPos(context.window, &lastOrbitX, &lastOrbitY);

                shiftMiddleMousePressed = (mods & GLFW_MOD_SHIFT);

                if (!shiftMiddleMousePressed) {
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
                        vec3 hitPoint;
                        bool hitGround = raycast_to_ground(&camera, hitPoint);
                        glm_vec3_copy(hitPoint, orbitPivot);
                    }

                    vec3 toPivot;
                    glm_vec3_sub(orbitPivot, camera.position, toPivot);
                    orbitDistance = glm_vec3_norm(toPivot);
                }

            } else if (action == GLFW_RELEASE) {
                middleMousePressed = false;
                shiftMiddleMousePressed = false;
                camera.use_look_at = false;
            }
        }

        if (button == GLFW_MOUSE_BUTTON_RIGHT) {
            if (action == PRESS) {
                rightMousePressed = true;
                glfwGetCursorPos(context.window, &lastX, &lastY);
                glfwSetInputMode(context.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            } else if (action == GLFW_RELEASE) {
                rightMousePressed = false;
                glfwSetInputMode(context.window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            }
        }
    }

    if (currentMouseButtonCallback != NULL) {
        currentMouseButtonCallback(button, action, mods);
    }
}

void internal_cursor_position_callback(GLFWwindow* window, double xpos, double ypos) {
    if (!camera.active) {
        gizmo_mouse_move(xpos, ypos);
    }

    if (firstMouse) {
        lastX = xpos;
        lastY = ypos;
        firstMouse = false;
    } else {
        double xoffset = xpos - lastX;
        double yoffset = lastY - ypos;

        if (camera.active) {
            lastX = xpos;
            lastY = ypos;
            camera_process_mouse(&camera, xoffset, yoffset);
        }
        else if (rightMousePressed) {
            lastX = xpos;
            lastY = ypos;
            camera_process_mouse(&camera, xoffset, yoffset);
        }
        else if (middleMousePressed) {
            if (shiftMiddleMousePressed) {
                if (camera.use_look_at) {
                    camera_disable_orbit_mode(&camera);
                }

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
                float orbitSpeed = 0.01f;
                camera_orbit_around_point(&camera, orbitPivot,
                                          (float)(xoffset * orbitSpeed),
                                          (float)(yoffset * orbitSpeed));
            }

            lastX = xpos;
            lastY = ypos;
        }
        else {
            lastX = xpos;
            lastY = ypos;
        }
    }

    if (currentCursorPosCallback != NULL) {
        currentCursorPosCallback(xpos, ypos);
    }
}

void internal_scroll_callback(GLFWwindow* window, double xOffset, double yOffset) {
    if (!camera.active) {
        float zoomSpeed = 0.5f;

        vec3 forward;
        glm_vec3_copy(camera.front, forward);
        glm_vec3_scale(forward, yOffset * zoomSpeed, forward);

        if (lerp_scroll_wheel_zoom) {
            if (!is_zooming) {
                glm_vec3_copy(camera.position, zoom_start_pos);
                glm_vec3_copy(camera.position, zoom_target_pos);
            } else {
                glm_vec3_copy(camera.position, zoom_start_pos);
            }

            glm_vec3_add(zoom_target_pos, forward, zoom_target_pos);
            is_zooming = true;
            zoom_anim_time = 0.0f;
        } else {
            glm_vec3_add(camera.position, forward, camera.position);
        }
    }

    if (currentScrollCallback != NULL) {
        currentScrollCallback(xOffset, yOffset);
    }
}

void internal_window_resize_callback(GLFWwindow* window, int width, int height) {
    context.framebufferResized = true;
    context.swapChainExtent.height = height;
    context.swapChainExtent.width = width;

    if (currentResizeCallback != NULL) {
        currentResizeCallback(width, height);
    }
}

void internal_window_focus_callback(GLFWwindow* window, int focused) {
    if (currentFocusCallback != NULL) {
        currentFocusCallback(focused);
    }
}

void internal_window_pos_callback(GLFWwindow* window, int xpos, int ypos) {
    if (currentWindowPosCallback != NULL) {
        currentWindowPosCallback(xpos, ypos);
    }
}

void internal_drop_callback(GLFWwindow* window, int count, const char** paths) {
    for (int i = 0; i < count; i++) {
        const char* path = paths[i];
        const char* ext = strrchr(path, '.');
        if (ext && (strcmp(ext, ".gltf") == 0 || strcmp(ext, ".glb") == 0 ||
                    strcmp(ext, ".GLTF") == 0 || strcmp(ext, ".GLB") == 0)) {
            load_gltf(path, &scene);
        } else {
            message(3, "Unsupported format!");
        }
    }

    if (currentDropCallback != NULL) {
        currentDropCallback(count, paths);
    }
}

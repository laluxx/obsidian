/// input — platform subsystem
#include "input.h"
#include "context.h"
#include "audio_api.h"

extern ObsWindow *s_event_window;
#include "camera.h"
#include "context.h"
#include "editor.h"
#include "editor_frames.h"
#include "gizmo.h"
#include "gltf_loader.h"
#include "keychords.h"
#include "middle_orbit.h"
#include "resize_policy.h"
#include "scene.h"
#include "scene_undo_input.h"
#include "sdf_appearance_clipboard.h"
#include "sdf_inspector.h"
#include "sdf_property_bridge.h"
#include "sdf_viewport.h"
#include "viewport_coordinates.h"
#include "window.h"
#include "window_api.h"
#include "vulkan_setup.h"
#include <cglm/cglm.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/// Implementation

double lastX = 800 / 2.0f, lastY = 600 / 2.0f;
bool firstMouse = true;
static ObsWindow *s_last_event_window;

bool shiftPressed;
bool ctrlPressed;
bool altPressed;

bool middleMousePressed = false;
bool rightMousePressed = false;
bool shiftMiddleMousePressed = false;
static ObsMiddleOrbitDrag middleOrbitDrag;
vec3 orbitPivot = {0.0f, 0.0f, 0.0f};
float orbitDistance = 15.0f;

bool keyW = false, keyA = false, keyS = false, keyD = false;
bool keyQ = false, keyE = false;
bool keySpace = false, keyShift = false;

bool is_framing = false;
CameraFrameTransition frame_transition = {0};
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
  glfwGetCursorPos(context.window, &currentMousePosition.x,
                   &currentMousePosition.y);

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
  /* GLFW can lose the release callback (focus change, OS pointer capture).
   * Polling the actual LEFT button closes the live SDF transaction safely.
   * Blur latches cancellation: a late release must never commit a preview. */
  if (context.window && obs_sdf_viewport_dragging()) {
    const bool left = glfwGetMouseButton(context.window,
                                         GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    const bool focused =
        glfwGetWindowAttrib(context.window, GLFW_FOCUSED) == GLFW_TRUE;
    (void)obs_sdf_viewport_reconcile(left, focused);
  }
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
  if (button < 0 || button >= MAX_MOUSE_BUTTONS)
    return 0;
  return mouseButtonsPressed[button];
}

int isMouseButtonReleased(int button) {
  if (button < 0 || button >= MAX_MOUSE_BUTTONS)
    return 0;
  return mouseButtonsReleased[button];
}

int isMouseButtonDown(int button) {
  if (button < 0 || button >= MAX_MOUSE_BUTTONS)
    return 0;
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

void getCursorPosition(double *x, double *y) {
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
  Vec2f mouseDelta = {currentMousePosition.x - lastMousePosition.x,
                      currentMousePosition.y - lastMousePosition.y};

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

static bool activate_event_window(GLFWwindow *window) {
  s_event_window = obs_window_from_glfw(window);
  if (!s_event_window) return true;
  if (!obs_editor_frames_activate_window(s_event_window)) {
    s_event_window = NULL;
    return false;
  }
  if (s_last_event_window != s_event_window) {
    firstMouse = true;
    s_last_event_window = s_event_window;
  }
  return true;
}

static void event_pointer_position(GLFWwindow *window, double *x, double *y) {
  if (s_event_window && s_event_window->pointerPositionKnown) {
    *x = s_event_window->pointerX;
    *y = s_event_window->pointerY;
  } else {
    glfwGetCursorPos(window, x, y);
  }
}

void internal_char_callback(GLFWwindow *window, unsigned int codepoint) {
  if (!activate_event_window(window)) return;
  ObsEditorFrame *active_frame = obs_editor_frame_current();
  if (obs_monad_window_is_surface(active_frame)) {
    if (obs_monad_window_char(active_frame, codepoint))
      obs_monad_audio_typed_character(codepoint);
    s_event_window = NULL;
    return;
  }
  if (skip_next_char) {
    skip_next_char = false;
    s_event_window = NULL;
    return;
  }

  if (currentTextCallback != NULL) {
    /* Character/IME input is an editor command even without a key event. */
    obs_scene_undo_on_key(true, OBS_SCENE_KEY_OTHER);
    currentTextCallback(codepoint);
    obs_monad_audio_typed_character(codepoint);
  }
  s_event_window = NULL;
}

void internal_key_callback(GLFWwindow *window, int key, int scancode,
                           int action, int mods) {
  if (!activate_event_window(window)) return;
  skip_next_char = false;
  ObsEditorFrame *active_frame = obs_editor_frame_current();
  if (obs_monad_window_is_surface(active_frame)) {
    (void)obs_monad_window_key(active_frame, key, action, mods);
    s_event_window = NULL;
    return;
  }
  if (obs_editor_frame_is_2d_only(active_frame)) {
    if (keychord_process_key_with_stack(key, action, mods))
      skip_next_char = true;
    s_event_window = NULL;
    return;
  }

  /* Defer Ctrl+Z classification until keymaps and text input have had their
   * normal chance to own it. Other key presses are command boundaries now. */
  const bool ctrl_z = key == GLFW_KEY_Z && (mods & GLFW_MOD_CONTROL);
  const bool scene_undo_shortcut =
      ctrl_z && (!(mods & GLFW_MOD_ALT) ||
                 ((mods & GLFW_MOD_ALT) && !(mods & GLFW_MOD_SHIFT)));
  const bool deferred_undo_key = action == GLFW_PRESS && scene_undo_shortcut;
  const bool modifier_key =
      key == GLFW_KEY_LEFT_SHIFT || key == GLFW_KEY_RIGHT_SHIFT ||
      key == GLFW_KEY_LEFT_CONTROL || key == GLFW_KEY_RIGHT_CONTROL ||
      key == GLFW_KEY_LEFT_ALT || key == GLFW_KEY_RIGHT_ALT ||
      key == GLFW_KEY_LEFT_SUPER || key == GLFW_KEY_RIGHT_SUPER;
  if (!deferred_undo_key)
    obs_scene_undo_on_key(action == GLFW_PRESS,
                          modifier_key ? OBS_SCENE_KEY_MODIFIER
                                       : OBS_SCENE_KEY_OTHER);

  // Update the internal state first
  if (action == GLFW_PRESS) {
    keys[key] = 1;
    keysPressed[key] = 1;
  } else if (action == GLFW_RELEASE) {
    keys[key] = 0;
    keysReleased[key] = 1;
  }

  if (obs_editor_enabled() && action == GLFW_PRESS && key == GLFW_KEY_ESCAPE &&
      editor_cancel_sdf_property_gesture()) {
    skip_next_char = true;
    s_event_window = NULL;
    return;
  }
  /* Opt-in F8 live SDF viewport tool. Prioritize Escape while dragging,
   * otherwise an editor mode toggle would strand a speculative transaction.
   * These are handled before user keybindings ONLY when tool state warrants. */
  if (action == GLFW_PRESS && obs_sdf_viewport_dragging() &&
      key == GLFW_KEY_ESCAPE) {
    (void)obs_sdf_viewport_cancel();
    if (obs_sdf_viewport_error() != OBS_SDF_EDIT_OK &&
        obs_sdf_viewport_error() != OBS_SDF_EDIT_INVALID &&
        obs_sdf_viewport_error() != OBS_SDF_EDIT_STALE)
      message(MSG_ERROR, "SDF drag cancellation refused: scene conflict");
    skip_next_char = true;
    s_event_window = NULL;
    return;
  }
  if (obs_editor_enabled() && !camera.active && action == GLFW_PRESS &&
      key == GLFW_KEY_F8) {
    const bool next = !obs_sdf_viewport_enabled();
    if (obs_sdf_viewport_set_enabled(next))
      message(MSG_INFO,
              next ? "SDF drag tool ON (F8, Esc cancel)" : "SDF drag tool OFF");
    else
      message(MSG_ERROR, "Cannot leave SDF tool: unresolved scene conflict");
    skip_next_char = true;
    s_event_window = NULL;
    return;
  }

  /* Cycles 61-62: copy/paste immutable appearance values and save/load
   * portable presets, only when no pointer-owned GUI or edit is active. */
  if (obs_editor_enabled() && !camera.active && obs_sdf_viewport_enabled() &&
      !obs_sdf_viewport_dragging() && !obs_sdf_property_active() &&
      !editor_wants_mouse() && action == GLFW_PRESS &&
      (mods & (GLFW_MOD_CONTROL | GLFW_MOD_SHIFT)) ==
          (GLFW_MOD_CONTROL | GLFW_MOD_SHIFT) &&
      !(mods & GLFW_MOD_ALT) &&
      (key == GLFW_KEY_C || key == GLFW_KEY_V || key == GLFW_KEY_S ||
       key == GLFW_KEY_L)) {
    extern int g_selected_sdf_index;
    if (key == GLFW_KEY_S || key == GLFW_KEY_L) {
      bool ok =
          key == GLFW_KEY_S
              ? obs_sdf_appearance_save_file(obs_sdf_appearance_preset_path())
              : obs_sdf_appearance_load_file(obs_sdf_appearance_preset_path());
      message(ok ? MSG_INFO : MSG_WARNING,
              ok ? (key == GLFW_KEY_S
                        ? "SDF look saved to preset file"
                        : "SDF look loaded (paste with Ctrl+Shift+V)")
                 : "SDF preset IO refused (path, data or active edit)");
    } else {
      ObsSdfEditStatus st =
          g_selected_sdf_index < 0
              ? OBS_SDF_EDIT_STALE
              : (key == GLFW_KEY_C
                     ? obs_sdf_appearance_copy((uint32_t)g_selected_sdf_index)
                     : obs_sdf_appearance_paste(
                           (uint32_t)g_selected_sdf_index));
      if (st == OBS_SDF_EDIT_OK)
        message(MSG_INFO, key == GLFW_KEY_C
                              ? "SDF appearance copied"
                              : "SDF appearance pasted (undoable)");
      else
        message(MSG_WARNING, st == OBS_SDF_EDIT_EMPTY
                                 ? "No sampled SDF appearance"
                             : st == OBS_SDF_EDIT_CONFLICT
                                 ? "Finish the current scene edit first"
                                 : "SDF appearance copy/paste refused");
    }
    skip_next_char = true;
    s_event_window = NULL;
    return;
  }

  /* A text field owns printable keys before keymaps see them. Otherwise an
   * unbound character clears the overriding map, leaving the text callback
   * active but routing later control keys to the global map. Non-printable
   * keys still reach keymaps (search cancel, cursor movement, etc.). */
  if (obs_editor_enabled()) {
    extern bool editor_process_text_input(int key, int action, int mods);
    if (editor_process_text_input(key, action, mods)) {
      skip_next_char = true;
      if (deferred_undo_key)
        obs_scene_undo_on_key(true, OBS_SCENE_KEY_OTHER);
      s_event_window = NULL;
      return;
    }
  }

  // Process keychords with stack (local maps have priority)
  bool handled = keychord_process_key_with_stack(key, action, mods);
  if (handled) {
    if (deferred_undo_key)
      obs_scene_undo_on_key(true, OBS_SCENE_KEY_OTHER);
    skip_next_char = true;
    s_event_window = NULL;
    return;
  }

  shiftPressed = mods & GLFW_MOD_SHIFT;
  ctrlPressed = mods & GLFW_MOD_CONTROL;
  altPressed = mods & GLFW_MOD_ALT;

  if (obs_editor_enabled()) {
    const bool scene_undo_owner =
        !camera.active && obs_sdf_viewport_enabled() &&
        !obs_sdf_viewport_dragging() && !obs_sdf_property_active() &&
        !editor_wants_mouse();
    obs_scene_undo_on_key(
        deferred_undo_key,
        scene_undo_owner ? OBS_SCENE_KEY_UNDO : OBS_SCENE_KEY_OTHER);

    /* Scene shortcuts run only after user keymaps and text widgets decline
     * the key, so the history boundary matches the command actually routed. */
    if (scene_undo_owner && deferred_undo_key &&
        !(mods & GLFW_MOD_ALT)) {
      ObsSdfEditStatus st = (mods & GLFW_MOD_SHIFT) ? obs_sdf_inspector_redo()
                                                    : obs_sdf_inspector_undo();
      if (st != OBS_SDF_EDIT_OK)
        message(MSG_WARNING, st == OBS_SDF_EDIT_EMPTY
                                 ? "No scene edit to undo/redo"
                                 : "Scene undo/redo refused");
      skip_next_char = true;
      s_event_window = NULL;
      return;
    }
    if (scene_undo_owner && deferred_undo_key &&
        (mods & (GLFW_MOD_CONTROL | GLFW_MOD_ALT)) ==
            (GLFW_MOD_CONTROL | GLFW_MOD_ALT) &&
        !(mods & GLFW_MOD_SHIFT)) {
      ObsSdfEditStatus st = obs_sdf_inspector_undo_selected();
      if (st != OBS_SDF_EDIT_OK)
        message(MSG_WARNING,
                st == OBS_SDF_EDIT_EMPTY
                    ? "No selected-object scene edit to undo"
                    : "Selected-object undo refused (stale object or conflict)");
      skip_next_char = true;
      s_event_window = NULL;
      return;
    }

    // Engine-level mode toggle (Hardcoded to bypass keymap parsers)
    if (key == 256 /* GLFW_KEY_ESCAPE */ && action == PRESS) {
      extern void toggle_editor_mode(void);
      toggle_editor_mode();
      s_event_window = NULL;
      return;
    }
  }
  if (deferred_undo_key && !obs_editor_enabled())
    obs_scene_undo_on_key(true, OBS_SCENE_KEY_OTHER);

  if (obs_editor_enabled() && key == KEY_T && action == PRESS) {
    extern bool ambientOcclusionEnabled;
    ambientOcclusionEnabled = !ambientOcclusionEnabled;
    printf("Ambient Occlusion: %s\n",
           ambientOcclusionEnabled ? "ENABLED" : "DISABLED");
  }

  // Then call the user's callback if it's registered and keychord didn't handle
  // it
  if (currentKeyCallback != NULL) {
    currentKeyCallback(key, action, mods);
  }
  s_event_window = NULL;
}

void internal_mouse_button_callback(GLFWwindow *window, int button, int action,
                                    int mods) {
  if (!activate_event_window(window)) return;
  ObsEditorFrame *active_frame = obs_editor_frame_current();
  if (obs_monad_window_is_surface(active_frame)) {
    double x = 0.0, y = 0.0;
    int window_width = 0, window_height = 0;
    int framebuffer_width = 0, framebuffer_height = 0;
    event_pointer_position(window, &x, &y);
    glfwGetWindowSize(window, &window_width, &window_height);
    glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
    if (window_width > 0)
      x *= (double)framebuffer_width / (double)window_width;
    if (window_height > 0)
      y *= (double)framebuffer_height / (double)window_height;
    (void)obs_monad_window_mouse_button(active_frame, button, action, mods,
                                        x, y);
    s_event_window = NULL;
    return;
  }
  if (obs_editor_frame_is_2d_only(active_frame)) {
    double x = 0.0, y = 0.0;
    int window_width = 0, window_height = 0;
    int framebuffer_width = 0, framebuffer_height = 0;
    event_pointer_position(window, &x, &y);
    glfwGetWindowSize(window, &window_width, &window_height);
    glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
    if (window_width > 0)
      x *= (double)framebuffer_width / (double)window_width;
    if (window_height > 0)
      y *= (double)framebuffer_height / (double)window_height;
    (void)obs_editor_frame_pointer_button(active_frame, button, action, x, y);
    s_event_window = NULL;
    return;
  }
  if (action == GLFW_PRESS) obs_scene_undo_on_mouse_press();
  bool editor_owns_pointer = false;
  if (obs_editor_enabled() && !camera.active) {
    double x = 0.0, y = 0.0;
    int width = 0, height = 0, framebuffer_width = 0, framebuffer_height = 0;
    event_pointer_position(window, &x, &y);
    glfwGetWindowSize(window, &width, &height);
    glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
    if (width > 0) x *= (double)framebuffer_width / width;
    if (height > 0) y *= (double)framebuffer_height / height;
    editor_mouse_move(x, y);
    editor_owns_pointer = editor_wants_mouse();
    editor_mouse_button(button, action);
  }
  if (obs_editor_enabled() && !camera.active &&
      (!editor_owns_pointer || action == GLFW_RELEASE)) {
    double mx, my;
    event_pointer_position(window, &mx, &my);
    if (!editor_wants_mouse() || action == GLFW_RELEASE)
      gizmo_mouse_button(button, action, mods, mx, my);

    if (button == GLFW_MOUSE_BUTTON_MIDDLE) {
      if (action == PRESS) {
        middleMousePressed = true;
        obs_middle_orbit_begin(&middleOrbitDrag, mx, my);
        lastX = mx;
        lastY = my;
        firstMouse = false;

        shiftMiddleMousePressed = (mods & GLFW_MOD_SHIFT);

        if (!shiftMiddleMousePressed) {
          if (editor.inspector.selected_mesh_index >= 0 &&
              editor.inspector.selected_mesh_index < (int)scene->meshes.count) {
            Mesh *m =
                &scene->meshes.items[editor.inspector.selected_mesh_index];
            vec3 local_center;
            glm_vec3_add(m->aabbMin, m->aabbMax, local_center);
            glm_vec3_scale(local_center, 0.5f, local_center);
            vec4 lc4 = {local_center[0], local_center[1], local_center[2],
                        1.0f};
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
        obs_middle_orbit_end(&middleOrbitDrag);
        shiftMiddleMousePressed = false;
        camera.use_look_at = false;
      }
    }

    if (button == GLFW_MOUSE_BUTTON_RIGHT) {
      if (action == PRESS) {
        rightMousePressed = true;
        glfwGetCursorPos(window, &lastX, &lastY);
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
      } else if (action == GLFW_RELEASE) {
        rightMousePressed = false;
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
      }
    }
  }

  if (currentMouseButtonCallback != NULL) {
    currentMouseButtonCallback(button, action, mods);
  }
  s_event_window = NULL;
}

void internal_cursor_position_callback(GLFWwindow *window, double xpos,
                                       double ypos) {
  if (!activate_event_window(window)) return;
  if (s_event_window) {
    s_event_window->pointerPositionKnown = true;
    s_event_window->pointerX = xpos;
    s_event_window->pointerY = ypos;
  }
  ObsEditorFrame *active_frame = obs_editor_frame_current();
  if (obs_monad_window_is_surface(active_frame)) {
    int width = 0, height = 0, framebuffer_width = 0, framebuffer_height = 0;
    glfwGetWindowSize(window, &width, &height);
    glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
    if (width > 0) xpos *= (double)framebuffer_width / (double)width;
    if (height > 0) ypos *= (double)framebuffer_height / (double)height;
    (void)obs_monad_window_mouse_move(active_frame, xpos, ypos);
    s_event_window = NULL;
    return;
  }
  if (obs_editor_frame_is_2d_only(active_frame)) {
    int window_width = 0, window_height = 0;
    int framebuffer_width = 0, framebuffer_height = 0;
    glfwGetWindowSize(window, &window_width, &window_height);
    glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
    if (window_width > 0)
      xpos *= (double)framebuffer_width / (double)window_width;
    if (window_height > 0)
      ypos *= (double)framebuffer_height / (double)window_height;
    (void)obs_editor_frame_pointer_move(active_frame, xpos, ypos);
    s_event_window = NULL;
    return;
  }
  bool editor_owns_pointer = false;
  if (obs_editor_enabled() && !camera.active) {
    int width = 0, height = 0, framebuffer_width = 0, framebuffer_height = 0;
    glfwGetWindowSize(window, &width, &height);
    glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
    double x = xpos, y = ypos;
    if (width > 0) x *= (double)framebuffer_width / width;
    if (height > 0) y *= (double)framebuffer_height / height;
    editor_mouse_move(x, y);
    editor_owns_pointer = editor_wants_mouse();
    if (editor_selection_box_active()) {
      editor_selection_box_update(
          (float)x, (float)framebuffer_height - (float)y);
    } else if (!editor_owns_pointer) {
      gizmo_mouse_move(xpos, ypos);
    }
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
    } else if (rightMousePressed && !editor_owns_pointer) {
      lastX = xpos;
      lastY = ypos;
      camera_process_mouse(&camera, xoffset, yoffset);
    } else if (middleMousePressed && !editor_owns_pointer) {
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
        (void)obs_middle_orbit_motion(&middleOrbitDrag, xpos, ypos, &camera,
                                      orbitPivot);
      }

      lastX = xpos;
      lastY = ypos;
    } else {
      lastX = xpos;
      lastY = ypos;
    }
  }

  if (currentCursorPosCallback != NULL) {
    currentCursorPosCallback(xpos, ypos);
  }
  s_event_window = NULL;
}

void internal_scroll_callback(GLFWwindow *window, double xOffset,
                              double yOffset) {
  if (!activate_event_window(window)) return;
  ObsEditorFrame *active_frame = obs_editor_frame_current();
  if (obs_monad_window_is_surface(active_frame)) {
    (void)obs_monad_window_scroll(active_frame, xOffset, yOffset);
    s_event_window = NULL;
    return;
  }
  if (obs_editor_frame_is_2d_only(active_frame)) {
    double x = 0.0, y = 0.0;
    int window_width = 0, window_height = 0;
    int framebuffer_width = 0, framebuffer_height = 0;
    glfwGetCursorPos(window, &x, &y);
    glfwGetWindowSize(window, &window_width, &window_height);
    glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
    if (window_width > 0)
      x *= (double)framebuffer_width / (double)window_width;
    if (window_height > 0)
      y *= (double)framebuffer_height / (double)window_height;
    (void)obs_editor_frame_scroll(active_frame, x, y, yOffset);
    s_event_window = NULL;
    return;
  }
  obs_scene_undo_on_scroll(xOffset, yOffset);
  if (obs_editor_enabled()) {
    double x = 0.0, y = 0.0;
    int window_width = 0, window_height = 0;
    int framebuffer_width = 0, framebuffer_height = 0;
    glfwGetCursorPos(window, &x, &y);
    glfwGetWindowSize(window, &window_width, &window_height);
    glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
    if (window_width > 0) x *= (double)framebuffer_width / window_width;
    if (window_height > 0) y *= (double)framebuffer_height / window_height;
    if (editor_scroll(x, y, yOffset)) {
      s_event_window = NULL;
      return;
    }
  }
  if (obs_editor_enabled() && !camera.active) {
    float zoomSpeed = 0.5f;

    vec3 forward;
    glm_vec3_copy(camera.front, forward);
    glm_vec3_scale(forward, yOffset * zoomSpeed, forward);

    if (lerp_scroll_wheel_zoom) {
      if (obs_editor_frame_current()) {
        obs_editor_frame_start_zoom(camera.position, forward);
      } else {
        if (!is_zooming) {
          glm_vec3_copy(camera.position, zoom_start_pos);
          glm_vec3_copy(camera.position, zoom_target_pos);
        } else {
          glm_vec3_copy(camera.position, zoom_start_pos);
        }

        glm_vec3_add(zoom_target_pos, forward, zoom_target_pos);
        is_zooming = true;
        zoom_anim_time = 0.0f;
      }
    } else {
      glm_vec3_add(camera.position, forward, camera.position);
    }
  }

  if (currentScrollCallback != NULL) {
    currentScrollCallback(xOffset, yOffset);
  }
  s_event_window = NULL;
}

void internal_window_resize_callback(GLFWwindow *window, int width,
                                     int height) {
  // Keep the active swapchain alive while it remains presentable. The latest
  // framebuffer size is consumed only after WSI reports OUT_OF_DATE.
  ObsWindow *w = obs_window_from_glfw(window);
  if (w && !obs_editor_frames_activate_window(w)) return;
  if (w) {
    w->framebufferResized = true;
    int framebuffer_width = 0, framebuffer_height = 0;
    glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
    w->resizeFramebufferWidth =
        framebuffer_width > 0 ? (uint32_t)framebuffer_width : 0;
    w->resizeFramebufferHeight =
        framebuffer_height > 0 ? (uint32_t)framebuffer_height : 0;
    w->resizeLastEventTime = glfwGetTime();
    if (obs_resize_framebuffer_is_empty(framebuffer_width,
                                        framebuffer_height)) {
      w->suspended = true;
      w->swapchainOutOfDate = true;
    }
  }
  (void)width;
  (void)height;

  if (currentResizeCallback != NULL) {
    s_event_window = w;
    currentResizeCallback(width, height);
    s_event_window = NULL;
  }
}

void internal_window_focus_callback(GLFWwindow *window, int focused) {
  if (!activate_event_window(window)) return;
  if (!focused) obs_scene_undo_on_focus_lost();
  if (!focused && obs_editor_enabled())
    (void)editor_cancel_sdf_property_gesture();
  if (!focused && s_last_event_window == s_event_window) {
    middleMousePressed = false;
    rightMousePressed = false;
    shiftMiddleMousePressed = false;
    obs_middle_orbit_end(&middleOrbitDrag);
    camera.use_look_at = false;
    firstMouse = true;
  }
  if (!focused && obs_sdf_viewport_dragging()) {
    (void)obs_sdf_viewport_reconcile(false, false);
    if (obs_sdf_viewport_dragging())
      message(MSG_ERROR, "SDF drag conflict on focus loss; Escape to retry");
  }
  if (currentFocusCallback != NULL) {
    currentFocusCallback(focused);
  }
  s_event_window = NULL;
}

void internal_window_close_callback(GLFWwindow *window) {
  s_event_window = obs_window_from_glfw(window);
  if (s_event_window && obs_editor_frames_request_window_close(s_event_window)) {
    s_event_window = NULL;
    return;
  }
  if (window)
    glfwSetWindowShouldClose(window, GLFW_TRUE);
  s_event_window = NULL;
}

void internal_window_pos_callback(GLFWwindow *window, int xpos, int ypos) {
  if (!activate_event_window(window)) return;
  if (currentWindowPosCallback != NULL) {
    currentWindowPosCallback(xpos, ypos);
  }
  s_event_window = NULL;
}

static bool is_dropped_texture_extension(const char *extension) {
  static const char *const extensions[] = {
      ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".hdr", ".exr", ".dds",
      ".psd", ".gif", ".pic", ".pnm", ".ppm", ".pgm"};
  if (!extension) return false;
  for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); ++i)
    if (strcasecmp(extension, extensions[i]) == 0) return true;
  return false;
}

void internal_drop_paths_at(double x, double y, int count,
                            const char **paths) {
  for (int i = 0; i < count; i++) {
    const char *path = paths[i];
    const char *ext = strrchr(path, '.');
    if (obs_editor_enabled() && is_dropped_texture_extension(ext)) {
      int mesh_index = x >= 0.0 && y >= 0.0
                           ? gizmo_pick_mesh(x, y)
                           : -1;
      if (mesh_index >= 0 && mesh_index < (int)scene->meshes.count) {
        int32_t texture_index = texture_pool_add(&context, path);
        if (texture_index >= 0) {
          Mesh *mesh = &scene->meshes.items[mesh_index];
          mesh->textureIndex = texture_index;
          mesh->texture = texture_pool_get(texture_index);
          markMeshesSSBODirty(&context);
          message(MSG_SUCCESS, "Albedo texture assigned");
        } else {
          message(MSG_ERROR, "Failed to load dropped texture");
        }
      }
      continue;
    }
    if (ext && (strcasecmp(ext, ".gltf") == 0 ||
                strcasecmp(ext, ".glb") == 0)) {
      if (obs_editor_enabled() && !editor_async_gltf_request(path))
        message(MSG_ERROR, "Could not queue asynchronous glTF load");
    } else if (ext && strcasecmp(ext, ".obj") == 0) {
      if (obs_editor_enabled() && !editor_async_obj_request(path))
        message(MSG_ERROR, "Could not queue asynchronous OBJ load");
    } else {
      if (obs_editor_enabled())
        message(MSG_ERROR, "Unsupported dropped file format");
    }
  }

  if (currentDropCallback != NULL) {
    currentDropCallback(count, paths);
  }
}

void internal_drop_callback(GLFWwindow *window, int count,
                            const char **paths) {
  if (!activate_event_window(window)) return;
  double x = 0.0, y = 0.0;
  int window_width = 0, window_height = 0;
  ObsViewportPixel pixel = {0};
  glfwGetCursorPos(window, &x, &y);
  glfwGetWindowSize(window, &window_width, &window_height);
  bool inside = obs_viewport_cursor_pixel(
      x, y, window_width, window_height, context.drawExtent.width,
      context.drawExtent.height, false, &pixel);
  internal_drop_paths_at(inside ? pixel.x : -1.0,
                         inside ? pixel.y : -1.0, count, paths);
  s_event_window = NULL;
}

#include "input.h"
#include "context.h"
#include "keychords.h"
#include <stdio.h>


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


void internal_char_callback(GLFWwindow* window, unsigned int codepoint) {
    if (currentTextCallback != NULL) {
        currentTextCallback(codepoint);
    }
}

void internal_key_callback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    // Update the internal state first
    if (action == GLFW_PRESS) {
        keys[key] = 1;
        keysPressed[key] = 1;
    } else if (action == GLFW_RELEASE) {
        keys[key] = 0;
        keysReleased[key] = 1;
    }
    
    // Process keychords - if a keychord is triggered, don't call user callback
    if (keychord_process_key(&keymap, key, action, mods)) {
        return;  // Keychord handled the input, stop here
    }
    
    // Then call the user's callback if it's registered and keychord didn't handle it
    if (currentKeyCallback != NULL) {
        currentKeyCallback(key, action, mods);
    }
}

void internal_mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
    if (currentMouseButtonCallback != NULL) {
        currentMouseButtonCallback(button, action, mods);
    }
}

void internal_cursor_position_callback(GLFWwindow* window, double xpos, double ypos) {
    if (currentCursorPosCallback != NULL) {
        currentCursorPosCallback(xpos, ypos);
    }
}

void internal_scroll_callback(GLFWwindow* window, double xOffset, double yOffset) {
    if (currentScrollCallback != NULL) {
        currentScrollCallback(xOffset, yOffset);
    }
}



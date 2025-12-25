#pragma once

#include "context.h"
#include "camera.h"

#define WIDTH 800
#define HEIGHT 600

#define CURSOR_NORMAL   GLFW_CURSOR_NORMAL
#define CURSOR_HIDDEN   GLFW_CURSOR_HIDDEN
#define CURSOR_DISABLED GLFW_CURSOR_DISABLED


#define ARROW_CURSOR     GLFW_ARROW_CURSOR
#define IBEAM_CURSOR     GLFW_IBEAM_CURSOR
#define CROSSHAIR_CURSOR GLFW_CROSSHAIR_CURSOR
#define HAND_CURSOR      GLFW_HAND_CURSOR
#define HRESIZE_CURSOR   GLFW_HRESIZE_CURSOR
#define VRESIZE_CURSOR   GLFW_VRESIZE_CURSOR

GLFWcursor* createStandardCursor(int shape);
void setCursor(GLFWcursor* cursor);


extern float delta_time;
extern float last_frame;


GLFWwindow* initWindow(int width, int height, const char* title);
int windowShouldClose();

void beginFrame();
void endFrame();

double getTime();

void setWindowPos(GLFWwindow* window, int x, int y);
void getWindowPos(GLFWwindow* window, int* x, int* y);

void setWindowSize(GLFWwindow* window, int width, int height);
void getWindowSize(GLFWwindow* window, int* width, int* height);

void pollEvents(void);

const char* getClipboardString();
void setClipboardString(const char *text);

void setInputMode(GLFWwindow* window, int mode, int value);
void getCursorPos(GLFWwindow* window, double* xpos, double* ypos);


void setCursorMode(int mode);
int getCursorMode();
void showCursor();
void hideCursor();
void disableCursor();

/* void setWindowResizeIncrements(int char_width, int char_height); */
void setWindowResizeIncrements(int char_width, int char_height, int min_width, int min_height);


void toggle_editor_mode();
void process_editor_movement(Camera* cam, float deltaTime);

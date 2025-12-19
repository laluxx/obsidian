#pragma once

#define WIDTH 800
#define HEIGHT 600

#define CURSOR_NORMAL   GLFW_CURSOR_NORMAL
#define CURSOR_HIDDEN   GLFW_CURSOR_HIDDEN
#define CURSOR_DISABLED GLFW_CURSOR_DISABLED

#include "context.h"
#include "camera.h"

extern float delta_time;
extern float last_frame;


GLFWwindow* initWindow(int width, int height, const char* title);
int windowShouldClose();

void beginFrame();
void endFrame();

double getTime();

void setWindowPos(GLFWwindow* window, int x, int y);
void getWindowPos(GLFWwindow* window, int* x, int* y);
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



void toggle_editor_mode();

void process_editor_movement(Camera* cam, float deltaTime);

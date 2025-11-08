#pragma once

#define WIDTH 800
#define HEIGHT 600

#include "context.h"
#include "camera.h"

extern float delta_time;
extern float last_frame;


GLFWwindow* initWindow(int width, int height, const char* title);
int windowShouldClose();

void beginFrame();
void endFrame();

double getTime();
void setInputMode(GLFWwindow* window, int mode, int value);
void getCursorPos(GLFWwindow* window, double* xpos, double* ypos);

void toggle_editor_mode();

void process_editor_movement(Camera* cam, float deltaTime);

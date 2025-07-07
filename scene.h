#pragma once

#include "renderer.h"

typedef struct {
    Meshes meshes;
} Scene;

extern Scene scene;

void scene_init(Scene *s);

#pragma once

#include "renderer.h"
#include <cglm/cglm.h>
#include <vulkan/vulkan.h>

/* Mesh load_obj(const char* path, vec4 color); */
Mesh load_obj(const char* path, char* name, vec4 color);

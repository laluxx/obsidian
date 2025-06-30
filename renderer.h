#pragma once

#include <vulkan/vulkan.h>
#include <cglm/cglm.h>

#define MAX_VERTICES 65536

typedef struct {
    vec3 pos;
    vec4 color;
} Vertex;

void renderer_init(
    VkDevice device,
    VkPhysicalDevice physicalDevice,
    VkCommandPool commandPool,
    VkQueue graphicsQueue
);

void renderer_shutdown(void);

void renderer_upload(void);
void renderer_draw(VkCommandBuffer cmd);
void renderer_clear(void);

// Primitives
void vertex(vec3 pos, vec4 color);
void line(vec3 a, vec3 b, vec4 color);
void triangle(vec3 a, vec3 b, vec3 c, vec4 color);
void plane(vec3 origin, vec2 size, vec4 color);
void cube(vec3 origin, float size, vec4 color);

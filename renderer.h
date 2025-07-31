#pragma once

#include <vulkan/vulkan.h>
#include <cglm/cglm.h>

#define MAX_VERTICES 65536

typedef struct {
    vec3 pos;
    vec4 color;
} Vertex;

typedef struct {
    VkBuffer vertexBuffer;
    VkDeviceMemory vertexBufferMemory;
    uint32_t vertexCount;
    mat4 model; 
} Mesh;

typedef struct {
    Mesh* items;
    size_t count;
    size_t capacity;
} Meshes;


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
void sphere(vec3 center, float radius, int latDiv, int longDiv, vec4 color);

void mesh(VkCommandBuffer cmd, Mesh* mesh);
void mesh_destroy(VkDevice device, Mesh* mesh);


void meshes_init(Meshes* meshes);
void meshes_add(Meshes* meshes, Mesh mesh);
void meshes_remove(Meshes* meshes, size_t index);
void meshes_destroy(VkDevice device, Meshes* meshes);
void meshes_draw(VkCommandBuffer cmd, Meshes* meshes);


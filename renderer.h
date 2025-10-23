#pragma once

#include "context.h"
#include "common.h"
#include <vulkan/vulkan.h>
#include <cglm/cglm.h>

#define MAX_VERTICES 65536 * 4
#define MAX_TEXTURES 256  // Maximum number of textures we can handle

extern VkDescriptorSet descriptorSet;

typedef struct {
    vec3 pos;
    vec4 color;
    vec3 normal;
    vec2 texCoord;
    uint32_t textureIndex;  // 0 = no texture, >0 = texture index
} Vertex;

typedef struct {
    vec2 pos;
    Color color;
    vec2 texCoord;
    uint32_t textureIndex;  // Which texture to use
} Vertex2D;

void renderer2D_init();
void renderer2D_clear(void);
void quad2D(vec2 position, vec2 size, Color color);
void renderer2D_upload();
void renderer2D_draw(VkCommandBuffer cmd);

typedef struct {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkSampler sampler;
    VkDescriptorSet descriptorSet;  // Each texture has its own descriptor set
    uint32_t width, height;
    bool loaded;
} Texture2D;


typedef struct {
    Texture2D* texture;
    uint32_t startVertex;
    uint32_t vertexCount;
} Texture3DBatch;

static Vertex vertices3D_textured[MAX_VERTICES];
static uint32_t vertex_count_3D_textured = 0;

static Texture3DBatch texture3DBatches[MAX_TEXTURES];
static uint32_t texture3DBatchCount = 0;

static VkBuffer vertexBuffer3D_textured;
static VkDeviceMemory vertexBufferMemory3D_textured;


void renderer_init_textured3D();
void renderer_upload_textured3D();
void renderer_draw_textured3D(VkCommandBuffer cmd);
void renderer_clear_textured3D();

// Texture management
bool load_texture(VulkanContext* context, const char* filename, Texture2D* texture);
void destroy_texture(VulkanContext* context, Texture2D* texture);
void texture2D(vec2 position, vec2 size, Texture2D* texture, Color tint);
void texture3D(vec3 position, vec2 size, Texture2D* texture, Color tint);

// Texture pool management
void texture_pool_init();
void texture_pool_cleanup(VulkanContext* context);
int32_t texture_pool_add(VulkanContext* context, const char* filename);
Texture2D* texture_pool_get(int32_t index);

typedef struct {
    mat4 model;
    int ambientOcclusionEnabled;
    int padding[3];
} PushConstants;

extern PushConstants pushConstants;

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
void vertex_with_normal(vec3 pos, Color color, vec3 normal);
void vertex(vec3 pos, vec4 color);
/* void line(vec3 a, vec3 b, Color color); */
/* void line(vec3 start, vec3 end, Color color, float thickness); */
void triangle(vec3 a, vec3 b, vec3 c, Color color);
void plane(vec3 origin, vec2 size, Color color);
/* void texturedPlane(vec3 origin, vec2 size, Texture2D* texture, Color tint, float tileFactor); */
void texturedPlane(vec3 origin, vec2 size, Texture2D* texture, Color tint, float tileX, float tileZ);
void cube(vec3 origin, float size, Color color);
void texturedCube(vec3 position, float size, Texture2D* texture, Color tint);
void sphere(vec3 center, float radius, int latDiv, int longDiv, Color color);

void mesh(VkCommandBuffer cmd, Mesh* mesh);
void mesh_destroy(VkDevice device, Mesh* mesh);

void meshes_init(Meshes* meshes);
void meshes_add(Meshes* meshes, Mesh mesh);
void meshes_remove(Meshes* meshes, size_t index);
void meshes_destroy(VkDevice device, Meshes* meshes);
void meshes_draw(VkCommandBuffer cmd, Meshes* meshes);


/// LINE

extern uint32_t lineVertexCount;


void line_renderer_init(VkDevice dev, VkPhysicalDevice physDev, VkCommandPool cmdPool, VkQueue queue);
void line(vec3 start, vec3 end, Color color);
void line_renderer_upload();
void line_renderer_draw(VkCommandBuffer cmd);
void line_renderer_clear();
void line_renderer_shutdown();

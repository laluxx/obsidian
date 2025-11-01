#pragma once

#include "context.h"
#include "common.h"
#include <vulkan/vulkan.h>
#include <cglm/cglm.h>

#define MAX_VERTICES 65536 * 32
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

typedef struct {
    Texture2D* texture;
    uint32_t startVertex;
    uint32_t vertexCount;
} TextureBatch;


extern Vertex2D vertices2D[MAX_VERTICES];
extern uint32_t vertexCount2D;
extern uint32_t coloredVertexCount;
extern uint32_t textureBatchCount;
extern TextureBatch textureBatches[MAX_TEXTURES];


extern Vertex vertices3D_textured[MAX_VERTICES];
extern uint32_t vertex_count_3D_textured;
extern Texture3DBatch texture3DBatches[MAX_TEXTURES];
extern uint32_t texture3DBatchCount;


static VkBuffer vertexBuffer3D_textured;
static VkDeviceMemory vertexBufferMemory3D_textured;


void renderer_init_textured3D();
void renderer_upload_textured3D();
void renderer_draw_textured3D(VkCommandBuffer cmd);
void renderer_clear_textured3D();

// Texture management
bool load_texture_from_rgba(VulkanContext* context, unsigned char* rgba_data, uint32_t width, uint32_t height, Texture2D* texture);
bool load_texture_from_memory(VulkanContext* context, unsigned char* data, size_t data_size, Texture2D* texture);
int32_t texture_pool_add_from_memory(unsigned char* data, size_t data_size);

bool load_texture(VulkanContext* context, const char* filename, Texture2D* texture);
void destroy_texture(VulkanContext* context, Texture2D* texture);
void texture2D(vec2 position, vec2 size, Texture2D* texture, Color tint);
void texture3D(vec3 position, vec2 size, Texture2D* texture, Color tint);

// Texture pool management
void texture_pool_init();
void texture_pool_cleanup(VulkanContext* context);
int32_t texture_pool_add(VulkanContext* context, const char* filename);
Texture2D* texture_pool_get(int32_t index);

/* typedef struct { */
/*     mat4 model; */
/*     int ambientOcclusionEnabled; */
/*     int isUnlit; */
/*     int padding[2]; */
/* } PushConstants; */

typedef struct {
    mat4 model;
    int ambientOcclusionEnabled;
    int isUnlit;
    int alphaMode;
    float alphaCutoff;
} PushConstants;



extern PushConstants pushConstants;

typedef struct {
    vec3* positions;     // Morph target position deltas
    vec3* normals;       // Morph target normal deltas (optional)
    size_t vertex_count;
} MorphTarget;

typedef struct {
    MorphTarget* targets;
    size_t target_count;
    float* weights;           // Current weights for each target
    Vertex* base_vertices;    // Original vertices before morphing
    size_t base_vertex_count;
    uint32_t* index_map;      // Maps expanded vertex index to original vertex 
} MorphData;

typedef struct {
    VkBuffer vertexBuffer;
    VkDeviceMemory vertexBufferMemory;
    uint32_t vertexCount;
    mat4 model;              // World transform
    mat4 local_transform;    // Local transform (for animation)
    void* node;              // cgltf_node* (stored as void* to avoid header dependency)
    char *name;
    int32_t textureIndex;    // Index into texture pool (-1 = no texture)
    Texture2D* texture;      // Direct pointer for convenience
    int32_t materialIndex;   // Index into material pool (-1 = no material)
    MorphData *morph_data;
    bool is_unlit;           // 1 = skip lighting, 0 = apply lighting

    int alpha_mode;          // 0 = OPAQUE, 1 = MASK, 2 = BLEND
    float alpha_cutoff;      // For MASK mode
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
void triangle(vec3 a, vec3 b, vec3 c, Color color);
void plane(vec3 origin, vec2 size, Color color);
void texturedPlane(vec3 origin, vec2 size, Texture2D* texture, Color tint, float tileX, float tileZ);
void cube(vec3 origin, float size, Color color);
void texturedCube(vec3 position, float size, Texture2D* texture, Color tint);
void sphere(vec3 center, float radius, int latDiv, int longDiv, Color color);


void sort_meshes_by_alpha(Meshes *meshes, vec3 cameraPos);

void mesh(VkCommandBuffer cmd, Mesh* mesh);
void mesh_update_morph(Mesh* mesh);
void mesh_destroy(VkDevice device, Mesh* mesh);

void meshes_init(Meshes* meshes);
void meshes_add(Meshes* meshes, Mesh mesh);
void meshes_remove(Meshes* meshes, size_t index);
void meshes_destroy(VkDevice device, Meshes* meshes);
void meshes_draw(VkCommandBuffer cmd, Meshes* meshes);
Mesh* get_mesh(const char* name);

/// LINE

extern uint32_t lineVertexCount;

void line_renderer_init(VkDevice dev, VkPhysicalDevice physDev, VkCommandPool cmdPool, VkQueue queue);
void line(vec3 start, vec3 end, Color color);
void line_renderer_upload();
void line_renderer_draw(VkCommandBuffer cmd);
void line_renderer_clear();
void line_renderer_shutdown();

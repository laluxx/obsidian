#pragma once

#include "context.h"
#include "common.h"
#include <vulkan/vulkan.h>
#include <cglm/cglm.h>

#define MAX_VERTICES 65536 * 32
#define MAX_TEXTURES 256

// Last 3 slots in the bindless array are reserved for IBL
#define IBL_IRRADIANCE_SLOT  (MAX_TEXTURES - 3)
#define IBL_PREFILTER_SLOT   (MAX_TEXTURES - 2)
#define IBL_BRDF_LUT_SLOT    (MAX_TEXTURES - 1)
#define MAX_POINT_LIGHTS     8

typedef struct {
    vec4  direction;        // xyz=dir, w=unused
    vec4  color;            // xyz=color, w=intensity
} DirectionalLight;

typedef struct {
    vec4  position;         // xyz=pos, w=radius
    vec4  color;            // xyz=color, w=intensity
} PointLight;

#define SHADOW_CASCADE_COUNT 4

typedef struct {
    DirectionalLight sun;
    PointLight       pointLights[MAX_POINT_LIGHTS];
    int              pointLightCount;
    float            ambientIntensity;
    int              iblEnabled;
    int              _pad;
    vec4             cameraPos;   // xyz=pos, w=unused
    mat4             cascadeSpace[SHADOW_CASCADE_COUNT];
    vec4             cascadeSplits;
} LightingData;

typedef struct {
    vec3     pos;           // +0
    float    _pad0;         // +12
    vec4     color;         // +16
    vec3     normal;        // +32
    vec2     texCoord;      // +44
    float    _pad1[3];      // +52
    vec4     tangent;       // +64  (xyz=tangent, w=bitangent sign)
    uint32_t textureIndex;  // +80
    uint32_t _pad2[3];      // +84
    vec4     weights;       // +96  (Bone weights)
    uint32_t joints[4];     // +112 (Bone indices)
} Vertex; // Total: 128 bytes (Exactly 32 floats for perfect shader alignment)

#define MAX_DYNAMIC_MESHES 4096
#define MAX_DYNAMIC_VERTICES (1024 * 1024)

void emit_draw(uint32_t firstVertex, uint32_t count, mat4 model);
uint32_t append_vertices(const Vertex* verts, uint32_t count);

typedef struct {
    vec2 pos;
    Color color;
    vec2 texCoord;
    int32_t textureIndex;  // -1 = color, -2 = exQuad, >= 0 = texture
    vec2 size;
    vec4 cornerRadius;     // TL, TR, BR, BL (Exactly fills the old padding!)
    float borderThickness;
    Color borderColor;
} Vertex2D;

void renderer2D_clear(void);
void quad2D(vec2 position, vec2 size, Color color);
void exQuad2D(vec2 position, vec2 size, vec4 radii, float borderThickness, Color borderColor, Color color);
void renderer2D_upload();
void renderer2D_init();


void renderer2D_draw(VkCommandBuffer cmd);

typedef struct {
    VkImage         image;
    VkDeviceMemory  memory;
    VkImageView     view;
    VkSampler       sampler;
    VkDescriptorSet descriptorSet;   // kept for legacy 2D per-batch path
    uint32_t        bindlessSlot;    // index into the bindless texture array
    uint32_t        width, height;
    bool            loaded;
} Texture2D;

extern Vertex2D vertices2D[MAX_VERTICES];
extern uint32_t vertexCount2D;
extern uint64_t dynamicVertexBufferAddr;

// Texture management
bool load_texture_from_rgba_with_format(VulkanContext* context, unsigned char* rgba_data, uint32_t width, uint32_t height, Texture2D* texture, VkFormat format);
bool load_texture_from_rgba(VulkanContext* context, unsigned char* rgba_data, uint32_t width, uint32_t height, Texture2D* texture);
bool update_texture_from_rgba(VulkanContext* context, Texture2D* texture, unsigned char* rgba_data, int width, int height);
bool load_texture_from_memory(VulkanContext* context, unsigned char* data, size_t data_size, Texture2D* texture);
int32_t texture_pool_add_from_memory(unsigned char* data, size_t data_size);

bool load_texture(VulkanContext* context, const char* filename, Texture2D* texture);
void destroy_texture(VulkanContext* context, Texture2D* texture);
void texture2D(vec2 position, vec2 size, Texture2D* texture, Color tint);

// Texture pool management
void texture_pool_init();
void texture_pool_cleanup(VulkanContext* context);
int32_t texture_pool_add(VulkanContext* context, const char* filename);
int32_t texture_pool_add_svg(VulkanContext* context, const char* filename, int width, int height);
Texture2D* texture_pool_get(int32_t index);

typedef struct {
    int      ambientOcclusionEnabled;
    int      iblEnabled;
    int      meshIndex;           // -1 = indirect (gl_BaseInstanceARB), >=0 = direct
    int      cascadeIndex;
    uint64_t meshBufferAddr;      // BDA: MeshGPUData array
    uint64_t vertexBufferAddr;    // BDA: mega vertex buffer
    uint64_t jointBufferAddr;     // BDA: global joint SSBO
    uint64_t morphBufferAddr;     // BDA: megaMorphBuffer (permanent deltas)
    uint64_t morphWeightAddr;     // BDA: per-frame morph weights
} PushConstants;

/* One entry per mesh in the SSBO — read by the vertex+fragment shader via gl_DrawID */
typedef struct {
    mat4  model;

    // Bindless texture slots (-1 = not present)
    int   albedoIndex;        // base color / albedo map
    int   normalMapIndex;     // tangent-space normal map
    int   metallicRoughIndex; // R=metallic, G=roughness (glTF spec)
    int   aoIndex;            // ambient occlusion (R channel)

    int   emissiveIndex;      // emissive map
    int   isUnlit;
    int   alphaMode;          // 0=opaque 1=mask 2=blend
    float alphaCutoff;

    // Material constant factors (multiplied with texture samples)
    vec4  baseColorFactor;    // rgba
    float metallicFactor;
    float roughnessFactor;
    float emissiveStrength;
    int   displacementIndex;

    vec3  emissiveFactor;
    float displacementScale;

    vec4  aabbMin;
    vec4  aabbMax;

    int   jointOffset;        // -1 = no skinning, >=0 = offset into Global Joint SSBO
    int   morphDeltaOffset;   // Offset into Mega Morph Buffer
    int   morphWeightOffset;  // Offset into dynamic Morph Weight Buffer
    int   morphCount;         // Number of active morph targets

    float transmissionFactor;
    float ior;
    float thicknessFactor;
    int   transmissionIndex;
    int   thicknessIndex;
    float attenuationColorR;
    float attenuationColorG;
    float attenuationColorB;
    float attenuationDistance;
    float dispersion;
    float _pad0;
    float _pad1;
} MeshGPUData;

extern PushConstants pushConstants;

typedef struct {
    vec4 pos_delta;    // xyz = delta, w = unused (alignment)
    vec4 normal_delta; // xyz = delta, w = unused (alignment)
} MorphDelta;

// Upload morph deltas permanently to megaMorphBuffer.
// Returns the base delta index (mesh.morphDeltaOffset). UINT32_MAX = overflow.
uint32_t megaMorphBufferAllocate(VulkanContext* ctx, MorphDelta* deltas, uint32_t deltaCount);

// We no longer need CPU-side morph targets, they will live permanently on the GPU!
typedef struct {
    float* weights;           // Current weights for each target (uploaded dynamically)
    size_t target_count;
} MorphData;

typedef struct {
    /* If megaBaseVertex != UINT32_MAX the mesh is static and lives in the mega buffer.
   Otherwise it's dynamic and gets appended to the dynamic mega buffer region each frame. */
    uint32_t         megaBaseVertex;
    uint32_t         megaBaseIndex;
    uint32_t         dynamicBaseVertex;
    uint32_t         vertexCount;
    uint32_t         indexCount;
    vec3             aabbMin;
    vec3             aabbMax;
    mat4  model;
    mat4  local_transform;
    void* node;
    char* name;
    int32_t  textureIndex;
    Texture2D* texture;
    int32_t  materialIndex;
    MorphData* morph_data;
    bool is_unlit;
    int  alpha_mode;
    float alpha_cutoff;

    int  jointOffset;   // Offset into the global joint matrix array
    int  jointCount;    // Number of bones affecting this mesh

    int  morphDeltaOffset;  // Base offset in megaMorphBuffer
    int  morphWeightOffset; // Base offset in morphWeightBuffer
    int  morphCount;        // Number of morph targets

    /* PBR material texture slots (bindless indices, -1 = not present) */
    int32_t  normalMapIndex;      // tangent-space normal map
    int32_t  metallicRoughIndex;  // R=metallic, G=roughness
    int32_t  aoIndex;             // ambient occlusion
    int32_t  emissiveIndex;       // emissive map

    /* PBR constant factors */
    vec4  baseColorFactor;        // default {1,1,1,1}
    float metallicFactor;         // default 1.0
    float roughnessFactor;        // default 1.0
    float emissiveStrength;       // default 1.0
    vec3  emissiveFactor;         // default {0,0,0}

    float transmissionFactor;
    float ior;
    float thicknessFactor;
    int   transmissionIndex;
    int   thicknessIndex;
    vec3  attenuationColor;
    float attenuationDistance;
    float dispersion;
} Mesh;

typedef struct {
    Mesh* items;
    size_t count;
    size_t capacity;
} Meshes;

void renderer_init(VkDevice device,
                   VkPhysicalDevice physicalDevice,
                   VkCommandPool commandPool,
                   VkQueue graphicsQueue);
void renderer_shutdown(void);
void renderer_clear(void);
uint32_t get_dynamic_vertex_count(void);
Vertex* get_dynamic_vertices(void);

/* ── Immediate-mode material API ─────────────────────────────────────
   Call imm_set_material() before any draw call to set PBR properties.
   All draws until the next imm_set_material() share that material.    */
typedef struct {
    vec4  baseColorFactor;   /* default {1,1,1,1} */
    float metallicFactor;    /* default 0.0       */
    float roughnessFactor;   /* default 0.5       */
    float emissiveStrength;  /* default 1.0        */
    int   isUnlit;           /* default 0          */
    int   alphaMode;         /* default 2 (BLEND)  */
    vec3  emissiveFactor;    /* default {0,0,0}    */
    int   albedoIndex;       /* default -1        */
    int   normalMapIndex;    /* default -1        */
    int   metallicRoughIndex;/* default -1        */
    int   aoIndex;           /* default -1        */
    int   emissiveIndex;     /* default -1        */
    int   displacementIndex;
    float displacementScale;

    float transmissionFactor;
    float ior;
    float thicknessFactor;
    int   transmissionIndex;
    int   thicknessIndex;
    vec3  attenuationColor;
    float attenuationDistance;
    float dispersion;
} Material;

void set_material(const Material* mat);
void reset_material(void);
Material load_pbr_material(const char* albedoPath, const char* normalPath, const char* roughnessPath);
Material load_pbr_material_dir(const char* dirPath);
int alloc_slot(mat4 model);
void emit_draw_with_slot(uint32_t firstVertex, uint32_t count, int slot);

void begin_frame(void);

// Primitives
void vertex_with_normal(vec3 pos, Color color, vec3 normal);
void vertex(vec3 pos, vec4 color);
void triangle(vec3 a, vec3 b, vec3 c, Color color);
void plane(vec3 origin, vec2 size, Color color);
void cube(vec3 origin, float size, Color color);
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
void line_set_width(float width);
void line_renderer_upload();
void line_renderer_draw(VkCommandBuffer cmd);
void line_renderer_clear();
void line_renderer_shutdown();



void circle2D(vec2 center, float radius, Color color);
void line2D(vec2 start, vec2 end, Color color);
void triangle_col(vec2 p0, Color c0, vec2 p1, Color c1, vec2 p2, Color c2);
void shaderQuad2D(vec2 position, vec2 size, int shaderId, vec4 customParams);

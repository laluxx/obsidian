#pragma once

#include "common.h"
#include "context.h"
#include <cglm/cglm.h>
#include <vulkan/vulkan.h>
#include <stdatomic.h>

/// Public interface

#define MAX_VERTICES 65536 * 32
#define MAX_LINE_VERTICES MAX_VERTICES
#define MAX_TEXTURES 8192

// Last 3 slots in the bindless array are reserved for IBL
#define IBL_IRRADIANCE_SLOT (MAX_TEXTURES - 3)
#define IBL_PREFILTER_SLOT (MAX_TEXTURES - 2)
#define IBL_BRDF_LUT_SLOT (MAX_TEXTURES - 1)
#define MAX_POINT_LIGHTS 8
#define MAX_MESH_BONE_OVERRIDES 256u

typedef struct {
  vec4 direction; // xyz=dir, w=unused
  vec4 color;     // xyz=color, w=intensity
} DirectionalLight;

typedef struct {
  vec4 position; // xyz=pos, w=radius
  vec4 color;    // xyz=color, w=intensity
} PointLight;

#define SHADOW_CASCADE_COUNT 4

typedef struct {
  DirectionalLight sun;
  PointLight pointLights[MAX_POINT_LIGHTS];
  int pointLightCount;
  float ambientIntensity;
  int iblEnabled;
  int shadowsEnabled;
  vec4 cameraPos; // xyz=pos, w=unused
  mat4 cascadeSpace[SHADOW_CASCADE_COUNT];
  vec4 cascadeSplits;
  mat4 cullSpace;     // Used for frozen/active frustum culling
  vec4 cullCameraPos; // Used for cone culling
  mat4 prev_vp;       // AAA Optimization: HZB Reprojection
  int freezeCulling;  // 1 = frozen, 0 = active
  int hzbValid;       // Previous-frame HZB is initialized and readable
  int _pad2[2];
} LightingData;

static inline bool
obs_lighting_has_directional_sun(const LightingData *lighting) {
  return lighting && lighting->sun.color[3] > 0.0f;
}

static inline bool
obs_lighting_needs_shadow_map(const LightingData *lighting,
                              bool shadows_enabled) {
  return shadows_enabled && obs_lighting_has_directional_sun(lighting);
}

typedef struct {
  vec3 pos;              // +0
  float _pad0;           // +12
  vec4 color;            // +16
  vec3 normal;           // +32
  vec2 texCoord;         // +44
  float _pad1[3];        // +52
  vec4 tangent;          // +64  (xyz=tangent, w=bitangent sign)
  uint32_t textureIndex; // +80
  uint32_t _pad2[3];     // +84
  vec4 weights;          // +96  (Bone weights)
  uint32_t joints[4];    // +112 (Bone indices)
} Vertex; // Total: 128 bytes (Exactly 32 floats for perfect shader alignment)

#define MAX_DYNAMIC_MESHES 4096
#define MAX_DYNAMIC_VERTICES (1024 * 1024)
#define OBS_DYNAMIC_VERTEX_STRIDE(ctx) \
  ((uint32_t)((ctx)->dynamicStagingSize / \
              (MAX_FRAMES_IN_FLIGHT * sizeof(Vertex))))

#define MAX_SDF_PRIMITIVES 1024

#define SDF_TYPE_SPHERE 0
#define SDF_TYPE_BOX 1
#define SDF_TYPE_TERRAIN 2

#define SDF_OP_UNION 0
#define SDF_OP_SMOOTH_UNION 1
#define SDF_OP_SMOOTH_SUBTRACT 2

typedef struct __attribute__((aligned(32))) {
  mat4 inverseTransform;
  vec4 size;
  vec4 color;    // RGB = Albedo, A = Alpha (unused for now)
  vec3 emissive; // RGB = Emissive Color
  float metallic;
  float roughness;
  float emissiveStrength;
  float smoothness; // Blend factor
  int type;         // SDF_TYPE_SPHERE, SDF_TYPE_BOX, SDF_TYPE_TERRAIN
  int operation;    // SDF_OP_UNION, SDF_OP_SMOOTH_UNION
  int _pad1;
  int _pad2;
  int _pad3;
  int _pad4; // Pad to exactly 160 bytes (multiple of 32)
  int _pad5;
  int _pad6;
  int _pad7;
} SdfPrimitive;

void emit_draw(uint32_t firstVertex, uint32_t count, mat4 model);
uint32_t append_vertices(const Vertex *verts, uint32_t count);

/* Packed to remove the 16 bytes of interior padding in the old 192-byte
 * interleaved vertex. Every field remains at a 4-byte-compatible offset;
 * Vulkan attribute offsets below are derived with offsetof. Keep the record
 * 16-byte aligned and its compact stride covered by the capacity test. */
typedef struct __attribute__((packed, aligned(16))) {
  vec2 pos;
  Color color;
  vec2 texCoord;
  int32_t textureIndex; // -1 = color, -2 = exQuad, >= 0 = texture
  vec2 size;
  vec4 cornerRadius; // TL, TR, BR, BL (Exactly fills the old padding!)
  float borderThickness;
  Color borderColor;
  uint32_t faceData0[4];
  uint32_t faceData1[4];     // run_offset, codepoint, flags, event_kind
  uint32_t faceEventData[4]; // event_offset, event_count, serial, anchor
  vec4 faceParams;
  vec4 faceEvent; // event_time, duration, reserved, reserved
  vec4 clipRect;  // x, y, width, height; zero width disables clipping
} Vertex2D;

_Static_assert(sizeof(Vertex2D) == 176,
               "Vertex2D layout must stay compact and shader-compatible");

typedef struct {
  uint32_t shader_id;
  uint32_t buffer_pos;
  uint32_t line;
  uint32_t column;
  uint32_t run_offset;
  uint32_t codepoint;
  uint32_t flags;
  uint32_t event_kind;
  uint32_t event_offset;
  uint32_t event_count;
  uint32_t event_serial;
  uint32_t event_anchor;
  vec4 params;
  vec4 event;
} ObsFaceShaderData;

#define OBS_FACE_SHADER_EVENT_NONE 0u
#define OBS_FACE_SHADER_EVENT_INSERT 1u
#define OBS_FACE_SHADER_EVENT_DELETE 2u
#define OBS_FACE_SHADER_TARGET_FOREGROUND 0x2u
#define OBS_FACE_SHADER_TARGET_BACKGROUND 0x4u

/* Shader-zero glyphs and quads need no face metadata in the vertex stream.
 * Keep this predicate shared so the renderer and its fast-path tests agree. */
static inline bool
obs_face_shader_data_requires_payload(const ObsFaceShaderData *data) {
  return data && (data->shader_id != 0 ||
                  data->event_kind != OBS_FACE_SHADER_EVENT_NONE);
}

typedef struct {
  mat4 projection;
  vec4 frame;
  vec4 pointer; // screen-space x/y; negative x disables proximity effects
} Obs2DPushConstants;

void renderer2D_clear(void);
void renderer2D_set_pointer_position(float x, float y);
void renderer2D_set_clip_rect(float x, float y, float width, float height);
void renderer2D_clear_clip_rect(void);
void quad2D(vec2 position, vec2 size, Color color);
void quad2D_with_face(vec2 position, vec2 size, Color color,
                      const ObsFaceShaderData *face_data);
void exQuad2D(vec2 position, vec2 size, vec4 radii, float borderThickness,
              Color borderColor, Color color);
void renderer2D_upload();
void renderer2D_init();

void renderer2D_draw(VkCommandBuffer cmd);

typedef enum {
  TEXTURE_STATUS_EMPTY = 0,
  TEXTURE_STATUS_QUEUED_FOR_COOKING = 1,
  TEXTURE_STATUS_COOKING = 2,
  TEXTURE_STATUS_READY_FOR_UPLOAD = 3,
  TEXTURE_STATUS_UPLOADING = 4,
  TEXTURE_STATUS_READY = 5,
  TEXTURE_STATUS_FAILED = 6
} TextureStatus;

typedef struct {
  VkImage image;
  VkDeviceMemory memory;
  VkImageView view;
  VkSampler sampler;
  VkDescriptorSet descriptorSet; // kept for legacy 2D per-batch path
  uint32_t bindlessSlot;         // index into the bindless texture array
  uint32_t width, height;
  bool loaded;                   // Legacy flag, will be phased out for status
  _Atomic TextureStatus status; // Published between cooker and owner threads
} Texture2D;

extern Vertex2D *vertices2D;
bool renderer2D_reserve(uint32_t additional_vertices);
extern vec4 renderer2D_clip_rect;
extern uint32_t vertexCount2D;
extern VkPipeline graphicsPipelineWireframe;

// Texture management
bool load_texture_from_rgba_with_format(VulkanContext *context,
                                        unsigned char *rgba_data,
                                        uint32_t width, uint32_t height,
                                        Texture2D *texture, VkFormat format);
bool load_texture_from_rgba(VulkanContext *context, unsigned char *rgba_data,
                            uint32_t width, uint32_t height,
                            Texture2D *texture);
bool update_texture_from_rgba(VulkanContext *context, Texture2D *texture,
                              unsigned char *rgba_data, int width, int height);
bool load_texture_from_memory(VulkanContext *context, unsigned char *data,
                              size_t data_size, Texture2D *texture);
int32_t texture_pool_add_embedded(VulkanContext *ctx,
                                  const char *virtual_filename,
                                  unsigned char *data, size_t data_size);

bool load_texture_ex(VulkanContext *ctx, const char *filename,
                     unsigned char *mem_data, size_t mem_size,
                     Texture2D *texture);
bool load_texture(VulkanContext *context, const char *filename,
                  Texture2D *texture);
void destroy_texture(VulkanContext *context, Texture2D *texture);
void texture2D(vec2 position, vec2 size, Texture2D *texture, Color tint);

// Texture pool management
void texture_pool_init();
/* Submit and drain uploads that may still reference standalone engine
 * textures (font atlases are one example) without destroying the pool. */
void texture_pool_wait_for_uploads(VulkanContext *context);
void texture_pool_cleanup(VulkanContext *context);
int32_t texture_pool_add(VulkanContext *context, const char *filename);
/* Retained pool API: deduplicates managed paths, balanced acquire/release.
 * No descriptor/slot reuse; retirement requires explicit GPU quiescence.
 * Owner thread only. Do not release indices from texture_pool_add(). */
bool texture_pool_acquire(VulkanContext *context, const char *filename,
                          int32_t *out_index);
bool texture_pool_release(int32_t index);
uint32_t texture_pool_retirement_pending(void);
uint32_t texture_pool_collect_retired(VulkanContext *context);
bool texture_pool_lease_acquire(void *user, const char *path,
                                int32_t *out_index);
void texture_pool_lease_release(void *user, int32_t index);
int32_t texture_pool_add_svg(VulkanContext *context, const char *filename,
                             int width, int height);
Texture2D *texture_pool_get(int32_t index);

typedef struct {
  uint32_t vertex_offset;
  uint32_t triangle_offset;
  uint32_t vertex_count;
  uint32_t triangle_count;
} Meshlet;

typedef struct {
  uint32_t joints[4];
  float weights[4];
} MeshletSkinData;

typedef struct {
  float center[3];
  float radius;
  float cone_apex[3];
  float cone_axis[3];
  float cone_cutoff;
} MeshletBounds;

typedef struct {
  int ambientOcclusionEnabled;
  int iblEnabled;
  int meshIndex; // -1 = indirect (gl_BaseInstanceARB), >=0 = direct
  int cascadeIndex;
  uint64_t meshBufferAddr;      // BDA: MeshGPUData array
  uint64_t vertexBufferAddr;    // BDA: mega vertex buffer
  uint64_t jointBufferAddr;     // BDA: global joint SSBO
  uint64_t morphBufferAddr;     // BDA: megaMorphBuffer (permanent deltas)
  uint64_t morphWeightAddr;     // BDA: per-frame morph weights
  uint64_t meshletBufferAddr;   // BDA: Meshlet array
  uint64_t meshletBoundsAddr;   // BDA: Meshlet static bounds
  uint64_t meshletSkinAddr;     // BDA: Meshlet skin data
  uint64_t dynamicBoundsAddr;   // BDA: Meshlet dynamic bounds
  uint64_t meshletVertexAddr;   // BDA: Meshlet local vertices
  uint64_t meshletTriangleAddr; // BDA: Meshlet local triangles
  uint64_t sdfBufferAddr;       // BDA: SdfPrimitive array
  int sdfCount;                 // Number of active SDF primitives
  int _pad1;
} PushConstants;

/* One entry per mesh in the SSBO — read by the vertex+fragment shader via
 * gl_DrawID */
typedef struct {
  mat4 model;

  // Bindless texture slots (-1 = not present)
  int albedoIndex;        // base color / albedo map
  int normalMapIndex;     // tangent-space normal map
  int metallicRoughIndex; // R=metallic, G=roughness (glTF spec)
  int aoIndex;            // ambient occlusion (R channel)

  int emissiveIndex; // emissive map
  int isUnlit;
  int alphaMode; // 0=opaque 1=mask 2=blend
  float alphaCutoff;

  // Material constant factors (multiplied with texture samples)
  vec4 baseColorFactor; // rgba
  float metallicFactor;
  float roughnessFactor;
  float emissiveStrength;
  int displacementIndex;

  vec3 emissiveFactor;
  float displacementScale;

  vec4 aabbMin;
  vec4 aabbMax;

  int jointOffset;      // -1 = no skinning, >=0 = offset into Global Joint SSBO
  int morphDeltaOffset; // Offset into Mega Morph Buffer
  int morphWeightOffset; // Offset into dynamic Morph Weight Buffer
  int morphCount;        // Number of active morph targets

  int meshletOffset; // AAA Task Shader integration
  int meshletCount;
  int meshletVertexOffset;
  int meshletTriangleOffset;

  float transmissionFactor;
  float ior;
  float thicknessFactor;
  int transmissionIndex;
  int thicknessIndex;
  float attenuationColorR;
  float attenuationColorG;
  float attenuationColorB;
  float attenuationDistance;
  float dispersion;
  int isVisible;
  int isWireframe;
  int vertexOffset;
  int matcapMode;
  float aoStrength;       // glTF occlusionTexture.strength
  int _pad3;
} MeshGPUData;

extern PushConstants pushConstants;

typedef struct {
  vec4 pos_delta;    // xyz = delta, w = unused (alignment)
  vec4 normal_delta; // xyz = delta, w = unused (alignment)
} MorphDelta;

// Upload morph deltas permanently to megaMorphBuffer.
// Returns the base delta index (mesh.morphDeltaOffset). UINT32_MAX = overflow.
uint32_t megaMorphBufferAllocate(VulkanContext *ctx, MorphDelta *deltas,
                                 uint32_t deltaCount);

// We no longer need CPU-side morph targets, they will live permanently on the
// GPU!
typedef struct {
  float *weights; // Current weights for each target (uploaded dynamically)
  size_t target_count;
} MorphData;

typedef struct {
  mat4 world_offset;
  bool active;
} MeshBoneOverride;

typedef struct {
  /* If megaBaseVertex != UINT32_MAX the mesh is static and lives in the mega
 buffer. Otherwise it's dynamic and gets appended to the dynamic mega buffer
 region each frame. */
  /* Stable host identity, lazily assigned by the scene registry. */
  uint64_t foreign_identity;
  uint32_t megaBaseVertex;
  uint32_t megaBaseIndex;
  uint32_t dynamicBaseVertex;
  uint32_t vertexCount;
  uint32_t indexCount;
  uint32_t meshletCount;
  uint32_t megaBaseMeshlet;
  uint32_t megaBaseMeshletVertex;
  uint32_t megaBaseMeshletTriangle;
  vec3 aabbMin;
  vec3 aabbMax;
  mat4 model;
  mat4 local_transform;
  void *node;
  char *name;
  int32_t textureIndex;
  Texture2D *texture;
  int32_t materialIndex;
  MorphData *morph_data;
  bool is_unlit;
  int matcap_mode; /* 0=PBR, 1=studio matcap, 2=view-normal diagnostic */
  bool visible;
  bool wireframe;
  int alpha_mode;
  float alpha_cutoff;

  int jointOffset; // Offset into the global joint matrix array
  int jointCount;  // Number of bones affecting this mesh

  int morphDeltaOffset;  // Base offset in megaMorphBuffer
  int morphWeightOffset; // Base offset in morphWeightBuffer
  int morphCount;        // Number of morph targets

  /* Allocated only when an editor operation first overrides a joint. */
  MeshBoneOverride *bone_overrides;
  uint32_t bone_override_capacity;

  /* PBR material texture slots (bindless indices, -1 = not present) */
  int32_t normalMapIndex;     // tangent-space normal map
  int32_t metallicRoughIndex; // R=metallic, G=roughness
  int32_t aoIndex;            // ambient occlusion
  float aoStrength;           // [0,1], 0 disables the map
  int32_t emissiveIndex;      // emissive map

  /* PBR constant factors */
  vec4 baseColorFactor;   // default {1,1,1,1}
  float metallicFactor;   // default 1.0
  float roughnessFactor;  // default 1.0
  float emissiveStrength; // default 1.0
  vec3 emissiveFactor;    // default {0,0,0}

  float transmissionFactor;
  float ior;
  float thicknessFactor;
  int transmissionIndex;
  int thicknessIndex;
  vec3 attenuationColor;
  float attenuationDistance;
  float dispersion;

  // Physics
  int collider_type; // 0 = None, 1 = Box, 2 = Sphere
  float mass;        // 0.0 = Static, > 0.0 = Dynamic
  float friction;
  float restitution;
  uint32_t physics_body_id;
} Mesh;

typedef struct {
  Mesh *items;
  uint32_t *draw_indices;
  size_t count;
  size_t capacity;
} Meshes;

void renderer_init(VkDevice device, VkPhysicalDevice physicalDevice,
                   VkCommandPool commandPool, VkQueue graphicsQueue);
void renderer_shutdown(void);
void renderer_clear(void);
uint32_t get_dynamic_vertex_count(void);
Vertex *get_dynamic_vertices(void);

/* ── Immediate-mode material API ─────────────────────────────────────
   Call imm_set_material() before any draw call to set PBR properties.
   All draws until the next imm_set_material() share that material.    */
typedef struct {
  vec4 baseColorFactor;   /* default {1,1,1,1} */
  float metallicFactor;   /* default 0.0       */
  float roughnessFactor;  /* default 0.5       */
  float emissiveStrength; /* default 1.0        */
  int isUnlit;            /* default 0          */
  int alphaMode;          /* default 2 (BLEND)  */
  vec3 emissiveFactor;    /* default {0,0,0}    */
  int albedoIndex;        /* default -1        */
  int normalMapIndex;     /* default -1        */
  int metallicRoughIndex; /* default -1        */
  int aoIndex;            /* default -1        */
  float aoStrength;       /* default 1.0       */
  int emissiveIndex;      /* default -1        */
  int displacementIndex;
  float displacementScale;

  float transmissionFactor;
  float ior;
  float thicknessFactor;
  int transmissionIndex;
  int thicknessIndex;
  vec3 attenuationColor;
  float attenuationDistance;
  float dispersion;
  int isWireframe;
} Material;

void set_material(const Material *mat);
void reset_material(void);
Material load_pbr_material(const char *albedoPath, const char *normalPath,
                           const char *roughnessPath);
Material load_pbr_material_dir(const char *dirPath);
int alloc_slot(mat4 model);
void emit_draw_with_slot(uint32_t firstVertex, uint32_t count, int slot);

void begin_frame(void);
void begin_frame_2d(void);

// Primitives — spawn a persistent Mesh into the scene hierarchy
void plane(vec3 origin, vec2 size, Color color);
void cube(vec3 origin, float size, Color color);
void circle(vec3 origin, float radius, Color color);
void cylinder(vec3 origin, float height, float radius, Color color);
void cone(vec3 origin, float height, float radius, Color color);
void uv_sphere(vec3 origin, float radius, int lats, int lons, Color color);
void torus(vec3 origin, float major_radius, float minor_radius, int major_segs,
           int minor_segs, Color color);

// Low-level immediate helpers used internally
void vertex_with_normal(vec3 pos, Color color, vec3 normal);
void vertex(vec3 pos, vec4 color);
void triangle(vec3 a, vec3 b, vec3 c, Color color);
void bone(vec3 start, vec3 end, Color color);

void sort_meshes_by_alpha(Meshes *meshes, vec3 cameraPos);

void mesh(VkCommandBuffer cmd, Mesh *mesh);
void mesh_update_morph(Mesh *mesh);
void mesh_destroy(VkDevice device, Mesh *mesh);
MeshBoneOverride *mesh_bone_override_get(Mesh *mesh, uint32_t local_joint,
                                         bool create);
void mesh_bone_overrides_clear(Mesh *mesh);

void meshes_init(Meshes *meshes);
bool meshes_reserve(Meshes *meshes, size_t needed);
bool meshes_append_prepared(Meshes *meshes, Mesh *prepared, size_t count);
void meshes_add(Meshes *meshes, Mesh mesh);
void meshes_remove(Meshes *meshes, size_t index);
void meshes_destroy(VkDevice device, Meshes *meshes);
void meshes_draw(VkCommandBuffer cmd, Meshes *meshes);
Mesh *get_mesh(const char *name);

/// LINE

extern uint32_t lineVertexCount;

void line_renderer_init(VkDevice dev, VkPhysicalDevice physDev,
                        VkCommandPool cmdPool, VkQueue queue);
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

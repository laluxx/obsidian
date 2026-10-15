/// Contract seam: real glTF decoding and private cook, fake engine types only
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "cache_textures.h"
#include "geometry_upload_batch.h"
#include "cgltf.h"
typedef float vec3[3], vec4[4], mat4[4][4];
typedef struct {
    vec3 pos; float _pad0; vec4 color; vec3 normal; float texCoord[2];
    float _pad1[3]; vec4 tangent; uint32_t textureIndex, _pad2[3];
    vec4 weights; uint32_t joints[4];
} Vertex;
typedef struct { float pos[3], normal[3], tangent[3]; } MorphDelta;
typedef struct { uint32_t vertex_offset, triangle_offset, vertex_count, triangle_count; } Meshlet;
typedef struct { uint32_t joints[4]; float weights[4]; } MeshletSkinData;
typedef struct { float center[3], radius, cone_apex[3], cone_axis[3], cone_cutoff; } MeshletBounds;
typedef struct {
    char name[256]; vec3 aabbMin,aabbMax; uint32_t vertexCount,indexCount;
    bool visible; uint32_t morphCount,vertexOffset,indexOffset,morphOffset,
        meshletCount,meshletOffset,meshletVertexOffset,meshletTriangleOffset;
    vec4 baseColorFactor; float metallicFactor,roughnessFactor,emissiveStrength,
        aoStrength;
    vec3 emissiveFactor; int alpha_mode; float alpha_cutoff; bool is_unlit;
    float transmissionFactor,ior,thicknessFactor; vec3 attenuationColor;
    float attenuationDistance,dispersion;
    char tex_albedo[512],tex_normal[512],tex_metallicRoughness[512],
      tex_occlusion[512],tex_emissive[512],tex_transmission[512],tex_thickness[512];
} OmdlMeshMeta;
typedef struct {
    char name[64]; int32_t parent; vec3 translation; vec4 rotation;
    vec3 scale; mat4 base_matrix; int32_t mesh_idx,skin_idx; bool expanded;
} OmdlNode;
typedef struct {
    uint32_t node_count; OmdlNode *nodes; mat4 *world_transforms, *local_transforms;
    vec3 *script_translations; uint32_t *topology_order;
    uint32_t skin_count,anim_count; void *skins,*skin_joints,*skin_ibms,*anims,*channels,*anim_floats;
} OmdlSceneGraph;
typedef struct {
    char *name; vec3 aabbMin,aabbMax; vec4 baseColorFactor;
    vec3 emissiveFactor,attenuationColor;
    float metallicFactor,roughnessFactor,emissiveStrength,alpha_cutoff,
          transmissionFactor,ior,thicknessFactor,attenuationDistance,dispersion,
          aoStrength;
    int alpha_mode,materialIndex;
    bool is_unlit,visible;
    uint32_t vertexCount,indexCount,meshletCount,megaBaseVertex,megaBaseIndex,
             megaBaseMeshlet,megaBaseMeshletVertex,megaBaseMeshletTriangle,
             morphDeltaOffset,dynamicBaseVertex;
    int textureIndex,normalMapIndex,metallicRoughIndex,aoIndex,emissiveIndex,
        transmissionIndex,thicknessIndex,jointOffset;
    mat4 model,local_transform;
    void *node;
} Mesh;
typedef struct {
    char *source_path; OmdlSceneGraph *graph; OmdlMeshMeta *metadata;
    Mesh *meshes; size_t mesh_count; ObsCacheTexturePlan textures;
    void *streams[OBS_GEOM_POOL_COUNT]; uint32_t counts[OBS_GEOM_POOL_COUNT];
    ObsGeometrySourceSpan spans[OBS_GEOM_POOL_COUNT];
} ObsGltfCacheCandidate;
void obs_gltf_cache_destroy(ObsGltfCacheCandidate *candidate);
bool obs_gltf_cook_static_cpu(const char *source, ObsGltfCacheCandidate **out);

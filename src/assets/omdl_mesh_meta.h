/// omdl_mesh_meta — cached native mesh schema shared by loader and worker
#pragma once
#include "renderer.h"

typedef struct {
    char name[256];
    vec3 aabbMin;
    vec3 aabbMax;
    uint32_t vertexCount;
    uint32_t indexCount;
    bool visible;
    uint32_t morphCount;
    uint32_t vertexOffset;
    uint32_t indexOffset;
    uint32_t morphOffset;
    uint32_t meshletCount;
    uint32_t meshletOffset;
    uint32_t meshletVertexOffset;
    uint32_t meshletTriangleOffset;

    vec4 baseColorFactor;
    float metallicFactor;
    float roughnessFactor;
    float emissiveStrength;
    float aoStrength;
    vec3 emissiveFactor;
    int alpha_mode;
    float alpha_cutoff;
    bool is_unlit;

    float transmissionFactor;
    float ior;
    float thicknessFactor;
    vec3 attenuationColor;
    float attenuationDistance;
    float dispersion;

    char tex_albedo[512];
    char tex_normal[512];
    char tex_metallicRoughness[512];
    char tex_occlusion[512];
    char tex_emissive[512];
    char tex_transmission[512];
    char tex_thickness[512];
} OmdlMeshMeta;

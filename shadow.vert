#version 450
#extension GL_ARB_shader_draw_parameters : require

// Padding perfectly aligns to skip the point lights and grab the shadow matrices
layout(set = 3, binding = 0) uniform LightingUBO {
    vec4 pad_to_cascade[20];
    mat4 cascadeSpace[4];
    vec4 cascadeSplits;
} lighting;

struct MeshData {
    mat4  model;
    mat4  normalMatrix;
    int   albedoIndex;
    int   normalMapIndex;
    int   metallicRoughIndex;
    int   aoIndex;
    int   emissiveIndex;
    int   isUnlit;
    int   alphaMode;
    float alphaCutoff;
    vec4  baseColorFactor;
    float metallicFactor;
    float roughnessFactor;
    float emissiveStrength;
    int   _pad0;
    vec3  emissiveFactor;
    int   _pad1;
    vec4  aabbMin;
    vec4  aabbMax;
};

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer MeshBuffer {
    MeshData meshes[];
};

layout(push_constant) uniform PC {
    int ambientOcclusionEnabled;
    int iblEnabled;
    int meshIndex;
    int cascadeIndex;
    MeshBuffer meshData;
} pc;

layout(location = 0) in vec3 inPos;

void main() {
    uint idx = (pc.meshIndex >= 0) ? uint(pc.meshIndex) : uint(gl_BaseInstanceARB);
    mat4 model = pc.meshData.meshes[idx].model;
    gl_Position = lighting.cascadeSpace[pc.cascadeIndex] * model * vec4(inPos, 1.0);
}

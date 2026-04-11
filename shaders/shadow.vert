#version 450
#extension GL_ARB_shader_draw_parameters : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require

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

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer MeshBuffer {
    MeshData meshes[];
};

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer VertexBuffer {
    float data[];
};

layout(push_constant) uniform PC {
    int ambientOcclusionEnabled;
    int iblEnabled;
    int meshIndex;
    int cascadeIndex;
    MeshBuffer meshData;
    VertexBuffer vertexData;
} pc;

void main() {
    uint idx = (pc.meshIndex >= 0) ? uint(pc.meshIndex) : uint(gl_BaseInstanceARB);
    mat4 model = pc.meshData.meshes[idx].model;

    // Pull the vec3 position manually (stride is 24 floats / 96 bytes)
    uint base = gl_VertexIndex * 24;
    vec3 pos = vec3(pc.vertexData.data[base+0], pc.vertexData.data[base+1], pc.vertexData.data[base+2]);

    gl_Position = lighting.cascadeSpace[pc.cascadeIndex] * model * vec4(pos, 1.0);
}

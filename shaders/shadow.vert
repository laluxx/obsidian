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
    int   displacementIndex;
    vec3  emissiveFactor;
    float displacementScale;
    vec4  aabbMin;
    vec4  aabbMax;

    // AAA: Must perfectly mirror renderer.h MeshGPUData layout
    int   jointOffset;
    int   _pad0;
    int   _pad1;
    int   _pad2;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer MeshBuffer {
    MeshData meshes[];
};

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer VertexBuffer {
    float data[];
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer JointBuffer {
    mat4 matrices[];
};

layout(push_constant) uniform PC {
    int ambientOcclusionEnabled;
    int iblEnabled;
    int meshIndex;
    int cascadeIndex;
    MeshBuffer meshData;
    VertexBuffer vertexData;
    JointBuffer jointData;
} pc;

void main() {
    uint idx = (pc.meshIndex >= 0) ? uint(pc.meshIndex) : uint(gl_BaseInstanceARB);
    mat4 model = pc.meshData.meshes[idx].model;
    int jointOffset = pc.meshData.meshes[idx].jointOffset;

    // ── PERFECT 128-BYTE (32 FLOAT) CGLM STRIDE ──
    uint base = gl_VertexIndex * 32;
    vec3 inPos = vec3(pc.vertexData.data[base+0], pc.vertexData.data[base+1], pc.vertexData.data[base+2]);

    // ── HARDWARE SKELETAL SKINNING FOR SHADOWS ──
    mat4 skinMat = mat4(1.0);
    if (jointOffset >= 0) {
        vec4 inWeights = vec4(pc.vertexData.data[base+24], pc.vertexData.data[base+25], pc.vertexData.data[base+26], pc.vertexData.data[base+27]);
        uvec4 inJoints = uvec4(
            floatBitsToUint(pc.vertexData.data[base+28]),
            floatBitsToUint(pc.vertexData.data[base+29]),
            floatBitsToUint(pc.vertexData.data[base+30]),
            floatBitsToUint(pc.vertexData.data[base+31])
        );

        skinMat =
            inWeights.x * pc.jointData.matrices[jointOffset + inJoints.x] +
            inWeights.y * pc.jointData.matrices[jointOffset + inJoints.y] +
            inWeights.z * pc.jointData.matrices[jointOffset + inJoints.z] +
            inWeights.w * pc.jointData.matrices[jointOffset + inJoints.w];
    }

    vec4 localPos = skinMat * vec4(inPos, 1.0);
    vec4 worldPos = model * localPos;

    gl_Position = lighting.cascadeSpace[pc.cascadeIndex] * worldPos;
}

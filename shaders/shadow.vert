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
    int   morphDeltaOffset;
    int   morphWeightOffset;
    int   morphCount;

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
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer MeshBuffer {
    MeshData meshes[];
};

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer VertexBuffer {
    float data[];
};

struct PackedJoint {
    vec4 row0;
    vec4 row1;
    vec4 row2;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer JointBuffer {
    PackedJoint joints[];
};

struct MorphDelta {
    vec4 pos_delta;
    vec4 normal_delta;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer MorphBuffer {
    MorphDelta deltas[];
};

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer WeightBuffer {
    float weights[];
};

mat4 unpackJoint(PackedJoint j) {
    return mat4(
        vec4(j.row0.x, j.row1.x, j.row2.x, 0.0),
        vec4(j.row0.y, j.row1.y, j.row2.y, 0.0),
        vec4(j.row0.z, j.row1.z, j.row2.z, 0.0),
        vec4(j.row0.w, j.row1.w, j.row2.w, 1.0)
    );
}

layout(push_constant) uniform PC {
    int ambientOcclusionEnabled;
    int iblEnabled;
    int meshIndex;
    int cascadeIndex;
    MeshBuffer meshData;
    VertexBuffer vertexData;
    JointBuffer jointData;
    MorphBuffer morphData;
    WeightBuffer weightData;
} pc;

void main() {
    uint idx = (pc.meshIndex >= 0) ? uint(pc.meshIndex) : uint(gl_BaseInstanceARB);
    MeshData m = pc.meshData.meshes[idx];
    mat4 model = m.model;
    int jointOffset = m.jointOffset;

    // ── PERFECT 128-BYTE (32 FLOAT) CGLM STRIDE ──
    uint base = gl_VertexIndex * 32;
    vec3 inPos = vec3(pc.vertexData.data[base+0], pc.vertexData.data[base+1], pc.vertexData.data[base+2]);

    // ── HARDWARE MORPH TARGETS FOR SHADOWS ──
    if (m.morphCount > 0) {
        uint vertBase = uint(m.morphDeltaOffset) + gl_VertexIndex * uint(m.morphCount);
        for (int i = 0; i < m.morphCount; i++) {
            float w = pc.weightData.weights[m.morphWeightOffset + i];
            inPos += pc.morphData.deltas[vertBase + uint(i)].pos_delta.xyz * w;
        }
    }

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
            inWeights.x * unpackJoint(pc.jointData.joints[jointOffset + inJoints.x]) +
            inWeights.y * unpackJoint(pc.jointData.joints[jointOffset + inJoints.y]) +
            inWeights.z * unpackJoint(pc.jointData.joints[jointOffset + inJoints.z]) +
            inWeights.w * unpackJoint(pc.jointData.joints[jointOffset + inJoints.w]);
    }
    vec4 localPos = skinMat * vec4(inPos, 1.0);
    vec4 worldPos = model * localPos;

    gl_Position = lighting.cascadeSpace[pc.cascadeIndex] * worldPos;
}

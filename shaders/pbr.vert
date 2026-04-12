#version 450
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_ARB_shader_draw_parameters : require
#extension GL_EXT_nonuniform_qualifier : enable

layout(set = 0, binding = 0) uniform UBO {
    mat4  vp;
    mat4  view;
    mat4  proj;
    vec4  cameraPos;
    float time;
} ubo;

layout(set = 1, binding = 0) uniform sampler2D textures[];

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

// Reading floats prevents C vs GLSL struct padding disasters.
// Vertex size is now exactly 128 bytes (32 floats) to accommodate bone joints and weights.
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer VertexBuffer {
    float data[];
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer JointBuffer {
    mat4 matrices[];
};

layout(push_constant) uniform PC {
    int  ambientOcclusionEnabled;
    int  iblEnabled;
    int  meshIndex;
    int  cascadeIndex;
    MeshBuffer meshData;
    VertexBuffer vertexData;
    JointBuffer jointData;
} pc;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec2 outTexCoord;
layout(location = 2) out vec4 outColor;
layout(location = 3) out mat3 outTBN;
layout(location = 6) out flat int outMeshIndex;

void main() {
    int mIdx = pc.meshIndex;
    if (mIdx < 0) {
        mIdx = gl_BaseInstanceARB;
    }
    outMeshIndex = mIdx;

    MeshData m = pc.meshData.meshes[mIdx];

    // ── PERFECT 128-BYTE (32 FLOAT) CGLM STRIDE ──
    uint base = gl_VertexIndex * 32;
    vec3 inPos      = vec3(pc.vertexData.data[base+0], pc.vertexData.data[base+1], pc.vertexData.data[base+2]);
    vec4 inColor    = vec4(pc.vertexData.data[base+4], pc.vertexData.data[base+5], pc.vertexData.data[base+6], pc.vertexData.data[base+7]);
    vec3 inNormal   = vec3(pc.vertexData.data[base+8], pc.vertexData.data[base+9], pc.vertexData.data[base+10]);
    vec2 inTexCoord = vec2(pc.vertexData.data[base+11], pc.vertexData.data[base+12]);
    vec4 inTangent  = vec4(pc.vertexData.data[base+16], pc.vertexData.data[base+17], pc.vertexData.data[base+18], pc.vertexData.data[base+19]);

    vec4 inWeights  = vec4(pc.vertexData.data[base+24], pc.vertexData.data[base+25], pc.vertexData.data[base+26], pc.vertexData.data[base+27]);
    uvec4 inJoints  = uvec4(
        floatBitsToUint(pc.vertexData.data[base+28]),
        floatBitsToUint(pc.vertexData.data[base+29]),
        floatBitsToUint(pc.vertexData.data[base+30]),
        floatBitsToUint(pc.vertexData.data[base+31])
    );

    // ── HARDWARE SKELETAL SKINNING ──
    mat4 skinMat = mat4(1.0);
    if (m.jointOffset >= 0) {
        skinMat =
            inWeights.x * pc.jointData.matrices[m.jointOffset + inJoints.x] +
            inWeights.y * pc.jointData.matrices[m.jointOffset + inJoints.y] +
            inWeights.z * pc.jointData.matrices[m.jointOffset + inJoints.z] +
            inWeights.w * pc.jointData.matrices[m.jointOffset + inJoints.w];
    }

    vec4 localPos = skinMat * vec4(inPos, 1.0);
    mat3 skinNormalMat = mat3(skinMat);
    vec3 localNormal = normalize(skinNormalMat * inNormal);
    vec3 localTangent = normalize(skinNormalMat * inTangent.xyz);

    // ── AAA VERTEX DISPLACEMENT ──
    if (m.displacementIndex >= 0) {
        float disp = textureLod(textures[nonuniformEXT(m.displacementIndex)], inTexCoord, 0.0).r;
        disp = disp - 0.5;
        localPos.xyz += localNormal * (disp * m.displacementScale);
    }

    vec4 worldPos = m.model * localPos;
    outWorldPos = worldPos.xyz;
    outTexCoord = inTexCoord;
    outColor    = inColor;

    vec3 N = normalize(mat3(m.normalMatrix) * localNormal);
    vec3 T = normalize(mat3(m.normalMatrix) * localTangent);

    T = normalize(T - dot(T, N) * N);
    vec3 B = cross(N, T) * inTangent.w;

    outTBN = mat3(T, B, N);

    gl_Position = ubo.vp * worldPos;
}

#version 450
#extension GL_EXT_nonuniform_qualifier      : enable
#extension GL_ARB_shader_draw_parameters    : require

layout(set = 0, binding = 0) uniform UBO {
    mat4 vp;
    mat4 view;
    mat4 proj;
    vec4 cameraPos;
    float time;
} ubo;

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

layout(set = 2, binding = 0) readonly buffer MeshSSBO {
    MeshData meshes[];
};

layout(push_constant) uniform PC {
    int ambientOcclusionEnabled;
    int iblEnabled;
    int meshIndex;
    int _pad;
} pc;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inTexCoord;
layout(location = 4) in vec4 inTangent;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec2 outTexCoord;
layout(location = 2) out vec4 outColor;
layout(location = 3) out mat3 outTBN;
layout(location = 6) out flat int outMeshIndex;

void main() {
    uint idx = (pc.meshIndex >= 0) ? uint(pc.meshIndex) : uint(gl_BaseInstanceARB);
    MeshData m = meshes[idx];

    vec4 worldPos = m.model * vec4(inPos, 1.0);
    outWorldPos   = worldPos.xyz;
    outTexCoord   = inTexCoord;
    outColor      = inColor;
    outMeshIndex  = int(idx);

    // TBN matrix for normal mapping
    vec3 N = normalize(mat3(m.normalMatrix) * inNormal);
    vec3 T = normalize(mat3(m.model)        * inTangent.xyz);
    T = normalize(T - dot(T, N) * N);        // Gram-Schmidt re-orthogonalize
    vec3 B = cross(N, T) * inTangent.w;
    outTBN = mat3(T, B, N);

    gl_Position = ubo.vp * worldPos;
}

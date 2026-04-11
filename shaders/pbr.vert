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
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer MeshBuffer {
    MeshData meshes[];
};

// Reading floats prevents C vs GLSL struct padding disasters.
// CGLM aligns vec4 to 16 bytes, making the true Vertex size 96 bytes (24 floats).
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer VertexBuffer {
    float data[];
};

layout(push_constant) uniform PC {
    int  ambientOcclusionEnabled;
    int  iblEnabled;
    int  meshIndex;
    int  cascadeIndex;
    MeshBuffer meshData;
    VertexBuffer vertexData;
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

    // ── PERFECT 96-BYTE (24 FLOAT) CGLM STRIDE ──
    uint base = gl_VertexIndex * 24;
    vec3 inPos      = vec3(pc.vertexData.data[base+0], pc.vertexData.data[base+1], pc.vertexData.data[base+2]);
    // floats 3 are padding for 16-byte alignment
    vec4 inColor    = vec4(pc.vertexData.data[base+4], pc.vertexData.data[base+5], pc.vertexData.data[base+6], pc.vertexData.data[base+7]);
    vec3 inNormal   = vec3(pc.vertexData.data[base+8], pc.vertexData.data[base+9], pc.vertexData.data[base+10]);
    vec2 inTexCoord = vec2(pc.vertexData.data[base+11], pc.vertexData.data[base+12]);
    // floats 13, 14, 15 are padding for 16-byte alignment
    vec4 inTangent  = vec4(pc.vertexData.data[base+16], pc.vertexData.data[base+17], pc.vertexData.data[base+18], pc.vertexData.data[base+19]);

    // ── AAA VERTEX DISPLACEMENT ──
    if (m.displacementIndex >= 0) {
        // Vertex Shaders cannot compute implicit LOD gradients, so we explicitly read Mip 0.
        float disp = textureLod(textures[nonuniformEXT(m.displacementIndex)], inTexCoord, 0.0).r;

        // Center the displacement so it carves inward (cracks) AND extrudes outward (rocks).
        disp = disp - 0.5;

        inPos += inNormal * (disp * m.displacementScale);
    }

    vec4 worldPos = m.model * vec4(inPos, 1.0);
    outWorldPos = worldPos.xyz;
    outTexCoord = inTexCoord;
    outColor    = inColor;

    vec3 N = normalize(mat3(m.normalMatrix) * inNormal);
    vec3 T = normalize(mat3(m.normalMatrix) * inTangent.xyz);

    T = normalize(T - dot(T, N) * N);
    vec3 B = cross(N, T) * inTangent.w;

    outTBN = mat3(T, B, N);

    gl_Position = ubo.vp * worldPos;
}

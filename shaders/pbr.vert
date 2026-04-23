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
    int   jointOffset;
    int   morphDeltaOffset;
    int   morphWeightOffset;
    int   morphCount;
    int   meshletOffset;
    int   meshletCount;
    int   meshletVertexOffset;
    int   meshletTriangleOffset;
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
    int   isVisible;
    int   isWireframe;
    int   vertexOffset;
    int   _pad1;
    int   _pad2;
    int   _pad3;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer MeshBuffer { MeshData meshes[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer VertexBuffer { float data[]; };
struct PackedJoint { vec4 row0; vec4 row1; vec4 row2; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer JointBuffer { PackedJoint joints[]; };
struct MorphDelta { vec4 pos_delta; vec4 normal_delta; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer MorphBuffer { MorphDelta deltas[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer WeightBuffer { float weights[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer DummyBuffer { uint dummy[]; };

mat4 unpackJoint(PackedJoint j) {
    return mat4(
        vec4(j.row0.x, j.row1.x, j.row2.x, 0.0),
        vec4(j.row0.y, j.row1.y, j.row2.y, 0.0),
        vec4(j.row0.z, j.row1.z, j.row2.z, 0.0),
        vec4(j.row0.w, j.row1.w, j.row2.w, 1.0)
    );
}

layout(push_constant) uniform PC {
    int          ambientOcclusionEnabled;
    int          iblEnabled;
    int          meshIndex;
    int          cascadeIndex;
    MeshBuffer   meshData;
    VertexBuffer vertexData;
    JointBuffer  jointData;
    MorphBuffer  morphData;
    WeightBuffer weightData;
    DummyBuffer  meshletData;
    DummyBuffer  boundsData;
    DummyBuffer  meshletVertexAddr;
    DummyBuffer  meshletTriangleAddr;
} pc;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec2 outTexCoord;
layout(location = 2) out vec4 outColor;
layout(location = 3) out mat3 outTBN;
layout(location = 6) out flat int outMeshIndex;

void main() {
    int mIdx = pc.meshIndex;
    if (mIdx < 0) mIdx = gl_BaseInstanceARB;
    outMeshIndex = mIdx;

    MeshData m = pc.meshData.meshes[mIdx];

    if (m.isVisible == 0) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }

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

    if (m.morphCount > 0) {
        uint vertBase = uint(m.morphDeltaOffset) + gl_VertexIndex * uint(m.morphCount);
        for (int i = 0; i < m.morphCount; i++) {
            float w = pc.weightData.weights[m.morphWeightOffset + i];
            uint deltaIdx = vertBase + uint(i);
            inPos    += pc.morphData.deltas[deltaIdx].pos_delta.xyz    * w;
            inNormal += pc.morphData.deltas[deltaIdx].normal_delta.xyz * w;
        }
    }

    mat4 skinMat = mat4(1.0);
    if (m.jointOffset >= 0) {
        skinMat =
            inWeights.x * unpackJoint(pc.jointData.joints[m.jointOffset + inJoints.x]) +
            inWeights.y * unpackJoint(pc.jointData.joints[m.jointOffset + inJoints.y]) +
            inWeights.z * unpackJoint(pc.jointData.joints[m.jointOffset + inJoints.z]) +
            inWeights.w * unpackJoint(pc.jointData.joints[m.jointOffset + inJoints.w]);
    }

    vec4 localPos = skinMat * vec4(inPos, 1.0);
    mat3 skinNormalMat = mat3(skinMat);
    vec3 localNormal = normalize(skinNormalMat * inNormal);
    vec3 localTangent = normalize(skinNormalMat * inTangent.xyz);

    if (m.displacementIndex >= 0) {
        float disp = textureLod(textures[nonuniformEXT(m.displacementIndex)], inTexCoord, 0.0).r;
        disp = disp - 0.5;
        localPos.xyz += localNormal * (disp * m.displacementScale);
    }

    vec4 worldPos = m.model * localPos;
    outWorldPos = worldPos.xyz;
    outTexCoord = inTexCoord;
    outColor    = inColor;

    mat3 normalMatrix = transpose(inverse(mat3(m.model)));
    vec3 N = normalize(normalMatrix * localNormal);
    vec3 T = normalize(normalMatrix * localTangent);

    vec3 T_ortho = T - dot(T, N) * N;
    if (dot(T_ortho, T_ortho) > 0.0001) {
        T = normalize(T_ortho);
    } else {
        vec3 up = abs(N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        T = normalize(cross(up, N));
    }

    vec3 B = cross(N, T) * inTangent.w;
    outTBN = mat3(T, B, N);

    gl_Position = ubo.vp * worldPos;
}

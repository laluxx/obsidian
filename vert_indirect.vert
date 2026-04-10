#version 450
#extension GL_ARB_shader_draw_parameters : enable
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inTexCoord;

layout(set = 0, binding = 0) uniform UniformBufferObject {
    mat4 vp;
} ubo;

/* Must match MeshGPUData in renderer.h exactly — including aabbMin/aabbMax */
struct MeshData {
    mat4  model;
    int   textureIndex;
    int   isUnlit;
    int   alphaMode;
    float alphaCutoff;
    vec4  aabbMin;
    vec4  aabbMax;
};

layout(set = 2, binding = 0) readonly buffer MeshSSBO {
    MeshData meshes[];
} meshSSBO;

layout(location = 0) out vec4        fragColor;
layout(location = 1) out vec3        fragNormal;
layout(location = 2) out vec3        fragWorldPos;
layout(location = 3) out flat int    fragAmbientOcclusionEnabled;
layout(location = 4) out vec2        fragTexCoord;
layout(location = 5) out flat int    fragIsUnlit;
layout(location = 6) out flat int    fragAlphaMode;
layout(location = 7) out flat float  fragAlphaCutoff;
layout(location = 8) out flat int    fragTextureIndex;

void main() {
    /* gl_BaseInstanceARB == firstInstance == mesh index set in updateMeshSSBOAndIndirect */
    MeshData mesh = meshSSBO.meshes[gl_BaseInstanceARB];

    vec4 worldPos   = mesh.model * vec4(inPosition, 1.0);
    gl_Position     = ubo.vp * worldPos;

    mat3 normalMatrix = mat3(transpose(inverse(mesh.model)));
    fragNormal      = normalize(normalMatrix * inNormal);
    fragWorldPos    = worldPos.xyz;
    fragColor       = inColor;
    fragTexCoord    = inTexCoord;

    fragTextureIndex            = mesh.textureIndex;
    fragIsUnlit                 = mesh.isUnlit;
    fragAlphaMode               = mesh.alphaMode;
    fragAlphaCutoff             = mesh.alphaCutoff;
    fragAmbientOcclusionEnabled = 1; /* AO always on for indirect pass */
}

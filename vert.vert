#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inTexCoord;

layout(binding = 0) uniform UniformBufferObject {
    mat4 vp;
} ubo;

layout(push_constant) uniform PushConstants {
    mat4 model;
    int ambientOcclusionEnabled;
} pc;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec3 fragWorldPos;
layout(location = 3) out flat int fragAmbientOcclusionEnabled;
layout(location = 4) out vec2 fragTexCoord;

void main() {
    vec4 worldPos = pc.model * vec4(inPosition, 1.0);
    gl_Position = ubo.vp * worldPos;
    
    // Transform normal to world space
    mat3 normalMatrix = mat3(transpose(inverse(pc.model)));
    fragNormal = normalize(normalMatrix * inNormal);
    
    fragWorldPos = worldPos.xyz;
    fragColor = inColor;
    fragAmbientOcclusionEnabled = pc.ambientOcclusionEnabled;
    fragTexCoord = inTexCoord;  // Pass through texture coordinates
}

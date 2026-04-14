#version 450

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in int inTextureIndex;
layout(location = 4) in vec2 inSize;
layout(location = 5) in vec4 inCornerRadius;
layout(location = 6) in float inBorderThickness;
layout(location = 7) in vec4 inBorderColor;

layout(push_constant) uniform PushConstants2D {
    mat4 projection;
} pc;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec2 fragTexCoord;
layout(location = 2) flat out int fragTextureIndex;
layout(location = 3) out vec2 fragSize;
layout(location = 4) out vec4 fragCornerRadius;
layout(location = 5) out float fragBorderThickness;
layout(location = 6) out vec4 fragBorderColor;

void main() {
    gl_Position = pc.projection * vec4(inPos, 0.0, 1.0);
    fragColor = inColor;
    fragTexCoord = inTexCoord;
    fragTextureIndex = inTextureIndex;
    fragSize = inSize;
    fragCornerRadius = inCornerRadius;
    fragBorderThickness = inBorderThickness;
    fragBorderColor = inBorderColor;
}

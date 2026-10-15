#version 450
/// Shader declarations

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in int inTextureIndex;
layout(location = 4) in vec2 inSize;
layout(location = 5) in vec4 inCornerRadius;
layout(location = 6) in float inBorderThickness;
layout(location = 7) in vec4 inBorderColor;
layout(location = 8) in uvec4 inFaceData0;
layout(location = 9) in uvec4 inFaceData1;
layout(location = 10) in vec4 inFaceParams;
layout(location = 11) in uvec4 inFaceEventData;
layout(location = 12) in vec4 inFaceEvent;
layout(location = 13) in vec4 inClipRect;

layout(push_constant) uniform PushConstants2D {
    mat4 projection;
    vec4 frame;
    vec4 pointer;
} pc;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec2 fragTexCoord;
layout(location = 2) flat out int fragTextureIndex;
layout(location = 3) out vec2 fragSize;
layout(location = 4) out vec4 fragCornerRadius;
layout(location = 5) out float fragBorderThickness;
layout(location = 6) out vec4 fragBorderColor;
layout(location = 7) flat out uvec4 fragFaceData0;
layout(location = 8) flat out uvec4 fragFaceData1;
layout(location = 9) flat out vec4 fragFaceParams;
layout(location = 10) flat out uvec4 fragFaceEventData;
layout(location = 11) flat out vec4 fragFaceEvent;
layout(location = 12) out vec2 fragPosition;
layout(location = 13) flat out vec4 fragClipRect;

/// Shader entry
void main() {
    gl_Position = pc.projection * vec4(inPos, 0.0, 1.0);
    fragColor = inColor;
    fragTexCoord = inTexCoord;
    fragTextureIndex = inTextureIndex;
    fragSize = inSize;
    fragCornerRadius = inCornerRadius;
    fragBorderThickness = inBorderThickness;
    fragBorderColor = inBorderColor;
    fragFaceData0 = inFaceData0;
    fragFaceData1 = inFaceData1;
    fragFaceParams = inFaceParams;
    fragFaceEventData = inFaceEventData;
    fragFaceEvent = inFaceEvent;
    fragPosition = inPos;
    fragClipRect = inClipRect;
}

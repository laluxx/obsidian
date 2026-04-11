#version 450
layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) flat in int fragTextureIndex;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D textures[256];

void main() {
    if (fragTextureIndex >= 0) {
        vec4 texColor = texture(textures[fragTextureIndex], fragTexCoord);
        outColor = texColor * fragColor;
    } else {
        outColor = fragColor;
    }
}

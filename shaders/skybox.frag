#version 450
layout(location = 0) in vec3 inUVW;
layout(location = 0) out vec4 outColor;

layout(set = 1, binding = 4) uniform samplerCube skyboxMap;

void main() {
    // Vulkan Y-flip for Cubemaps
    vec3 uvw = inUVW;
    uvw.y *= -1.0;
    vec3 color = textureLod(skyboxMap, uvw, 0.0).rgb;

    // Match the exposure of the PBR shader
    float exposure = 0.6;
    color = color * exposure;

    // ACES Tonemapping
    color = (color * (2.51 * color + 0.03)) / (color * (2.51 * color + 0.59) + 0.06);
    color = clamp(color, 0.0, 1.0);

    outColor = vec4(color, 1.0);
}

#version 450

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragTexCoord;

layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D texSampler;

void main() {
    // Sample the distance field
    float distance = texture(texSampler, fragTexCoord).a;

    // Convert from 0-1 range to signed distance
    // 0.5 = edge, >0.5 = inside, <0.5 = outside
    float signedDist = distance - 0.5;

    // CRITICAL: Use screen-space derivatives for adaptive antialiasing
    // This automatically adjusts based on how zoomed in/out you are
    float distanceChange = fwidth(signedDist);

    // Smooth antialiasing across the edge
    // The edge is where signedDist crosses zero
    float alpha = smoothstep(-distanceChange, distanceChange, signedDist);

    // Apply color with calculated alpha
    outColor = vec4(fragColor.rgb, fragColor.a * alpha);
}

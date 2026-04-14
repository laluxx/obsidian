#version 450
layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) flat in int fragTextureIndex;
layout(location = 3) in vec2 fragSize;
layout(location = 4) in vec4 fragCornerRadius;
layout(location = 5) in float fragBorderThickness;
layout(location = 6) in vec4 fragBorderColor;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D textures[256];

void main() {
    if (fragTextureIndex >= 0) {
        vec4 texColor = texture(textures[fragTextureIndex], fragTexCoord);
        outColor = texColor * fragColor;
    } else if (fragTextureIndex == -2) {
        vec2 halfSize = fragSize * 0.5;

        // UVs are mapped with Y=0 at the bottom coordinate, so Y > 0 is TOP.
        vec2 pixelPos = (fragTexCoord - 0.5) * fragSize;

        // Map user input (TL, TR, BR, BL) to continuous SDF array: (TR, BR, TL, BL)
        vec4 r = vec4(fragCornerRadius.y, fragCornerRadius.z, fragCornerRadius.x, fragCornerRadius.w);

        // Isolate quadrant
        r.xy = (pixelPos.x > 0.0) ? r.xy : r.zw;
        r.x  = (pixelPos.y > 0.0) ? r.x  : r.y;

        // Continuous mathematically perfect outer SDF
        vec2 d = abs(pixelPos) - halfSize + r.x;
        float outerDist = min(max(d.x, d.y), 0.0) + length(max(d, 0.0)) - r.x;

        float fw = fwidth(outerDist);
        float outerAlpha = 1.0 - smoothstep(-fw, fw, outerDist);

        if (outerAlpha <= 0.0) discard;

        // Calculate inner SDF to separate border from fill
        float innerDist = outerDist + fragBorderThickness;
        float innerAlpha = 1.0 - smoothstep(-fw, fw, innerDist);

        // Mix between border color and fill color
        vec4 mixedColor = mix(fragBorderColor, fragColor, innerAlpha);

        // Apply final transparency
        outColor = vec4(mixedColor.rgb, mixedColor.a * outerAlpha);
    } else {
        outColor = fragColor;
    }
}

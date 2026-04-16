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

// Extremely fast hardware-level HSV to RGB conversion
vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

void main() {
    if (fragTextureIndex >= 0) {
        vec4 texColor = texture(textures[fragTextureIndex], fragTexCoord);
        outColor = texColor * fragColor;
    } else if (fragTextureIndex == -2) {
        vec2 halfSize = fragSize * 0.5;
        vec2 pixelPos = (fragTexCoord - 0.5) * fragSize;
        vec4 r = vec4(fragCornerRadius.y, fragCornerRadius.z, fragCornerRadius.x, fragCornerRadius.w);

        r.xy = (pixelPos.x > 0.0) ? r.xy : r.zw;
        r.x  = (pixelPos.y > 0.0) ? r.x  : r.y;

        vec2 d = abs(pixelPos) - halfSize + r.x;
        float outerDist = min(max(d.x, d.y), 0.0) + length(max(d, 0.0)) - r.x;

        float fw = fwidth(outerDist);
        float outerAlpha = 1.0 - smoothstep(-fw, fw, outerDist);

        if (outerAlpha <= 0.0) discard;

        float innerDist = outerDist + fragBorderThickness;
        float innerAlpha = 1.0 - smoothstep(-fw, fw, innerDist);

        vec4 mixedColor = mix(fragBorderColor, fragColor, innerAlpha);
        outColor = vec4(mixedColor.rgb, mixedColor.a * outerAlpha);
    } else if (fragTextureIndex == -3) {
        // --- 1. PERFECT SDF HUE RING ---
        vec2 p = fragTexCoord * 2.0 - 1.0;
        float d = length(p);
        float innerFrac = fragCornerRadius.x;
        float outerFrac = fragCornerRadius.y;
        float alpha = fragCornerRadius.w;

        float fw = fwidth(d);
        float mask = smoothstep(outerFrac + fw, outerFrac - fw, d) * smoothstep(innerFrac - fw, innerFrac + fw, d);
        if (mask <= 0.0) discard;

        float angle = atan(p.y, p.x);
        float h = angle / 6.28318530718;
        if (h < 0.0) h += 1.0;

        outColor = vec4(hsv2rgb(vec3(h, 1.0, 1.0)), alpha * mask);
    } else if (fragTextureIndex == -4) {
        // --- 2. BARYCENTRIC SV TRIANGLE ---
        vec2 p = fragTexCoord * 2.0 - 1.0;
        float hue = fragCornerRadius.x;
        float radiusFrac = fragCornerRadius.y;
        float alpha = fragCornerRadius.w;

        float baseAngle = hue * (3.14159265359 / 180.0);
        float a0 = baseAngle;
        float a1 = baseAngle + 2.09439510239; // +120 deg
        float a2 = baseAngle - 2.09439510239; // -120 deg

        vec2 v0 = vec2(cos(a0), sin(a0)) * radiusFrac;
        vec2 v1 = vec2(cos(a1), sin(a1)) * radiusFrac;
        vec2 v2 = vec2(cos(a2), sin(a2)) * radiusFrac;

        // Exact barycentric coordinate calculation
        float denom = (v1.y - v2.y)*(v0.x - v2.x) + (v2.x - v1.x)*(v0.y - v2.y);
        float u = ((v1.y - v2.y)*(p.x - v2.x) + (v2.x - v1.x)*(p.y - v2.y)) / denom;
        float v = ((v2.y - v0.y)*(p.x - v2.x) + (v0.x - v2.x)*(p.y - v2.y)) / denom;
        float w = 1.0 - u - v;

        // Anti-aliasing mask using the closest edge distance
        float minBary = min(min(u, v), w);
        float fw = max(fwidth(minBary), 0.001); // Prevent zero-fwidth artifacts
        float mask = smoothstep(-fw, fw, minBary);
        if (mask <= 0.0) discard;

        // Convert the barycentric weights into pure Saturation and Value
        float val = clamp(u + w, 0.0, 1.0);
        float sat = (val > 1e-6) ? clamp(u / val, 0.0, 1.0) : 0.0;

        outColor = vec4(hsv2rgb(vec3(hue / 360.0, sat, val)), alpha * mask);
    } else {
        outColor = fragColor;
    }
}

#version 450

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec3 fragWorldPos;
layout(location = 3) in flat int fragAmbientOcclusionEnabled;
layout(location = 4) in vec2 fragTexCoord;
layout(location = 5) in flat int fragIsUnlit;
layout(location = 6) in flat int fragAlphaMode;
layout(location = 7) in flat float fragAlphaCutoff;

layout(location = 0) out vec4 outColor;

layout(set = 1, binding = 0) uniform sampler2D texSampler;

// Simple directional-based AO approximation
float cheapAO(vec3 normal) {
    float upFactor = dot(normal, vec3(0.0, 1.0, 0.0)) * 0.5 + 0.5;
    return mix(0.2, 1.0, upFactor);
}

void main() {
    // Sample the signed distance field (assumes [0,1] range, 0.5 = zero contour)
    float sdfValue = texture(texSampler, fragTexCoord).a;
    float signedDist = sdfValue - 0.5;

    // Compute screen-space derivative of UV to estimate pixel coverage
    // This accounts for perspective, distance, and scaling
    vec2 dx = dFdx(fragTexCoord);
    vec2 dy = dFdy(fragTexCoord);
    float screenSDFScale = max(length(dx), length(dy));

    // One SDF unit = 1 texel; edge width in SDF units ≈ screen coverage
    // Use √2/2 (~0.707) as conservative diagonal pixel coverage factor
    float distanceChange = max(screenSDFScale * 0.7071, 0.0001);

    // Optional: slight sharpening bias (reduce blur)
    distanceChange *= 0.8;

    // Smooth antialiasing over the estimated edge width
    float alpha = smoothstep(-distanceChange, distanceChange, signedDist);

    // Apply base color with computed alpha
    vec4 baseColor = vec4(fragColor.rgb, fragColor.a * alpha);

    // Early discard for performance and correct blending
    if (baseColor.a < 0.01) {
        discard;
    }

    // Handle unlit materials (e.g., UI, emissive text)
    if (fragIsUnlit != 0) {
        outColor = baseColor;
        return;
    }

    // --- Lighting (same as your other shaders) ---
    vec3 N = normalize(fragNormal);
    vec3 lightDir = normalize(vec3(0.3, 0.8, 0.5));
    float diff = max(dot(N, lightDir), 0.0);

    vec3 skyColor = vec3(0.3, 0.5, 0.7);
    vec3 groundColor = vec3(0.2, 0.15, 0.1);
    float hemiBlend = dot(N, vec3(0.0, 1.0, 0.0)) * 0.5 + 0.5;
    float ao = (fragAmbientOcclusionEnabled != 0) ? cheapAO(N) : 1.0;
    vec3 ambient = mix(groundColor, skyColor, hemiBlend) * ao;

    vec3 sunColor = vec3(1.0, 0.95, 0.8);
    vec3 direct = sunColor * diff;

    vec3 litColor = baseColor.rgb * (ambient + direct);

    // Distance fog (unchanged)
    float dist = length(fragWorldPos);
    float fogFactor = exp(-dist * 0.01);
    vec3 finalColor = mix(skyColor * 0.5, litColor, fogFactor);

    outColor = vec4(finalColor, baseColor.a);
}

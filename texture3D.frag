#version 450

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec3 fragWorldPos;
layout(location = 3) in flat int fragAmbientOcclusionEnabled;
layout(location = 4) in vec2 fragTexCoord;

layout(location = 0) out vec4 outColor;

// Change to set 1, binding 0
layout(set = 1, binding = 0) uniform sampler2D texSampler;

// Simple directional-based AO approximation
float cheapAO(vec3 normal) {
    // Surfaces facing up are brighter, down are darker
    float upFactor = dot(normal, vec3(0.0, 1.0, 0.0)) * 0.5 + 0.5;
    
    // Map to AO range (0.2 = very dark, 1.0 = no occlusion)
    return mix(0.2, 1.0, upFactor);
}

void main() {
    // Sample the texture
    vec4 texColor = texture(texSampler, fragTexCoord);
    
    // Apply color tint
    vec4 baseColor = fragColor * texColor;
    
    // Apply simple lighting (same as regular 3D shader)
    vec3 N = normalize(fragNormal);
    vec3 lightDir = normalize(vec3(0.3, 0.8, 0.5));
    float diff = max(dot(N, lightDir), 0.0);
    
    vec3 skyColor = vec3(0.3, 0.5, 0.7);
    vec3 groundColor = vec3(0.2, 0.15, 0.1);
    
    float hemiBlend = dot(N, vec3(0, 1, 0)) * 0.5 + 0.5;
    
    // APPLY AMBIENT OCCLUSION HERE - THIS IS WHAT'S MISSING!
    float ao = fragAmbientOcclusionEnabled != 0 ? cheapAO(N) : 1.0;
    vec3 ambient = mix(groundColor, skyColor, hemiBlend) * ao;
    
    vec3 sunColor = vec3(1.0, 0.95, 0.8);
    vec3 direct = sunColor * diff;
    
    vec3 finalColor = baseColor.rgb * (ambient + direct);
    
    // Distance fog
    float dist = length(fragWorldPos);
    float fogFactor = exp(-dist * 0.01);
    finalColor = mix(skyColor * 0.5, finalColor, fogFactor);
    
    outColor = vec4(finalColor, baseColor.a);
}

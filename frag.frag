#version 450

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec3 fragWorldPos;
layout(location = 3) in flat int fragAmbientOcclusionEnabled; // New input

layout(location = 0) out vec4 outColor;

// Simple directional-based AO approximation
float cheapAO(vec3 normal) {
    // Surfaces facing up are brighter, down are darker
    float upFactor = dot(normal, vec3(0.0, 1.0, 0.0)) * 0.5 + 0.5;
    
    // Map to AO range (0.2 = very dark, 1.0 = no occlusion)
    return mix(0.2, 1.0, upFactor);
}

void main() {
    vec3 N = normalize(fragNormal);
    
    // Calculate simple AO only if enabled
    float ao = fragAmbientOcclusionEnabled != 0 ? cheapAO(N) : 1.0;
    
    // Sun lighting
    vec3 lightDir = normalize(vec3(0.3, 0.8, 0.5));
    float diff = max(dot(N, lightDir), 0.0);
    
    // Sky color (blue ambient)
    vec3 skyColor = vec3(0.3, 0.5, 0.7);
    
    // Ground color (brownish)
    vec3 groundColor = vec3(0.2, 0.15, 0.1);
    
    // Hemisphere lighting (surfaces facing up get sky, down get ground)
    float hemiBlend = dot(N, vec3(0, 1, 0)) * 0.5 + 0.5;
    vec3 ambient = mix(groundColor, skyColor, hemiBlend) * ao;
    
    // Direct lighting
    vec3 sunColor = vec3(1.0, 0.95, 0.8);
    vec3 direct = sunColor * diff;
    
    // Combine
    vec3 finalColor = fragColor.rgb * (ambient + direct);
    
    // Simple distance fog
    float dist = length(fragWorldPos);
    float fogFactor = exp(-dist * 0.01);
    finalColor = mix(skyColor * 0.5, finalColor, fogFactor);
    
    outColor = vec4(finalColor, fragColor.a);
}



/* #version 450 */

/* layout(location = 0) in vec4 fragColor; */
/* layout(location = 1) in vec3 fragNormal; */
/* layout(location = 2) in vec3 fragWorldPos; */

/* layout(location = 0) out vec4 outColor; */

/* // Simple directional-based AO approximation */
/* float cheapAO(vec3 normal) { */
/*     // Surfaces facing up are brighter, down are darker */
/*     float upFactor = dot(normal, vec3(0.0, 1.0, 0.0)) * 0.5 + 0.5; */
    
/*     // Map to AO range (0.2 = very dark, 1.0 = no occlusion) */
/*     return mix(0.2, 1.0, upFactor); */
/* } */

/* void main() { */
/*     vec3 N = normalize(fragNormal); */
    
/*     // Calculate simple AO */
/*     float ao = cheapAO(N); */
    
/*     // Sun lighting */
/*     vec3 lightDir = normalize(vec3(0.3, 0.8, 0.5)); */
/*     float diff = max(dot(N, lightDir), 0.0); */
    
/*     // Sky color (blue ambient) */
/*     vec3 skyColor = vec3(0.3, 0.5, 0.7); */
    
/*     // Ground color (brownish) */
/*     vec3 groundColor = vec3(0.2, 0.15, 0.1); */
    
/*     // Hemisphere lighting (surfaces facing up get sky, down get ground) */
/*     float hemiBlend = dot(N, vec3(0, 1, 0)) * 0.5 + 0.5; */
/*     vec3 ambient = mix(groundColor, skyColor, hemiBlend) * ao; */
    
/*     // Direct lighting */
/*     vec3 sunColor = vec3(1.0, 0.95, 0.8); */
/*     vec3 direct = sunColor * diff; */
    
/*     // Combine */
/*     vec3 finalColor = fragColor.rgb * (ambient + direct); */
    
/*     // Simple distance fog */
/*     float dist = length(fragWorldPos); */
/*     float fogFactor = exp(-dist * 0.01); */
/*     finalColor = mix(skyColor * 0.5, finalColor, fogFactor); */
    
/*     outColor = vec4(finalColor, fragColor.a); */
/* } */




/* #version 450 */

/* layout(location = 0) in vec4 fragColor; */
/* layout(location = 0) out vec4 outColor; */

/* void main() { */
/*     outColor = fragColor; */
/* } */

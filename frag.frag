#version 450
layout(location = 0) in vec4  fragColor;
layout(location = 1) in vec3  fragNormal;
layout(location = 2) in vec3  fragWorldPos;
layout(location = 3) in flat int   fragAmbientOcclusionEnabled;
layout(location = 4) in vec2  fragTexCoord;
layout(location = 5) in flat int   fragIsUnlit;
layout(location = 6) in flat int   fragAlphaMode;
layout(location = 7) in flat float fragAlphaCutoff;
layout(location = 8) in flat int   fragTextureIndex;

layout(location = 0) out vec4 outColor;

float cheapAO(vec3 normal) {
    float upFactor = dot(normal, vec3(0.0, 1.0, 0.0)) * 0.5 + 0.5;
    return mix(0.2, 1.0, upFactor);
}

void main() {
    if (fragIsUnlit != 0) {
        outColor = fragColor;
        return;
    }

    vec3 N   = normalize(fragNormal);
    float ao = fragAmbientOcclusionEnabled != 0 ? cheapAO(N) : 1.0;

    vec3 lightDir    = normalize(vec3(0.3, 0.8, 0.5));
    float diff       = max(dot(N, lightDir), 0.0);
    vec3 skyColor    = vec3(0.3, 0.5, 0.7);
    vec3 groundColor = vec3(0.2, 0.15, 0.1);
    float hemiBlend  = dot(N, vec3(0, 1, 0)) * 0.5 + 0.5;
    vec3 ambient     = mix(groundColor, skyColor, hemiBlend) * ao;
    vec3 sunColor    = vec3(1.0, 0.95, 0.8);
    vec3 direct      = sunColor * diff;
    vec3 finalColor  = fragColor.rgb * (ambient + direct);

    outColor = vec4(finalColor, fragColor.a);
}

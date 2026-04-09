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

layout(set = 1, binding = 0) uniform sampler2D texSamplers[256];

layout(push_constant) uniform PushConstants {
    mat4  model;
    int   ambientOcclusionEnabled;
    int   isUnlit;
    int   alphaMode;
    float alphaCutoff;
    int   textureIndex;
} pushConsts;

float cheapAO(vec3 normal) {
    float upFactor = dot(normal, vec3(0.0, 1.0, 0.0)) * 0.5 + 0.5;
    return mix(0.2, 1.0, upFactor);
}

void main() {
    /* sample from bindless array using per-mesh index from SSBO via vertex shader */
    int texIdx = fragTextureIndex >= 0 ? fragTextureIndex : 0;
    vec4 texColor  = texture(texSamplers[texIdx], fragTexCoord);
    if (fragTextureIndex < 0) texColor = vec4(1.0);

    vec4 baseColor = fragColor * texColor;

    /* alpha modes */
    if (fragAlphaMode == 1 && baseColor.a < fragAlphaCutoff) discard;

    vec3 N = normalize(fragNormal);

    /* unlit path — skip all lighting */
    if (fragIsUnlit != 0) {
        outColor = baseColor;
        return;
    }

    vec3 lightDir    = normalize(vec3(0.3, 0.8, 0.5));
    float diff       = max(dot(N, lightDir), 0.0);
    vec3 skyColor    = vec3(0.3, 0.5, 0.7);
    vec3 groundColor = vec3(0.2, 0.15, 0.1);
    float hemiBlend  = dot(N, vec3(0, 1, 0)) * 0.5 + 0.5;
    float ao         = fragAmbientOcclusionEnabled != 0 ? cheapAO(N) : 1.0;
    vec3 ambient     = mix(groundColor, skyColor, hemiBlend) * ao;
    vec3 sunColor    = vec3(1.0, 0.95, 0.8);
    vec3 direct      = sunColor * diff;
    vec3 finalColor  = baseColor.rgb * (ambient + direct);

    float dist      = length(fragWorldPos);
    float fogFactor = exp(-dist * 0.01);
    finalColor      = mix(skyColor * 0.5, finalColor, fogFactor);

    outColor = vec4(finalColor, baseColor.a);
}

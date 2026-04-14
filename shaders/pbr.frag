#version 450
#extension GL_EXT_nonuniform_qualifier : enable

// ── Descriptor sets ──────────────────────────────────────────────────────────
layout(set = 0, binding = 0) uniform UBO {
    mat4  vp;
    mat4  view;
    mat4  proj;
    vec4  cameraPos;
    float time;
} ubo;

layout(set = 1, binding = 0) uniform sampler2D textures[];

struct PointLight {
    vec4 position;   // xyz=pos, w=radius
    vec4 color;      // xyz=color, w=intensity
};
struct DirLight {
    vec4 direction;
    vec4 color;      // w=intensity
};
layout(set = 3, binding = 0) uniform LightingUBO {
    DirLight  sun;
    PointLight pointLights[8];
    int       pointLightCount;
    float     ambientIntensity;
    int       iblEnabled;
    int       _pad;
    vec4      cameraPos;
    mat4      cascadeSpace[4];
    vec4      cascadeSplits;
} lighting;

// Using sampler2DShadow unlocks hardware-accelerated bilinear depth comparison!
layout(set = 3, binding = 5) uniform sampler2DShadow shadowMap;

struct MeshData {
    mat4  model;
    int   albedoIndex;
    int   normalMapIndex;
    int   metallicRoughIndex;
    int   aoIndex;
    int   emissiveIndex;
    int   isUnlit;
    int   alphaMode;
    float alphaCutoff;
    vec4  baseColorFactor;
    float metallicFactor;
    float roughnessFactor;
    float emissiveStrength;
    int   displacementIndex;
    vec3  emissiveFactor;
    float displacementScale;
    vec4  aabbMin;
    vec4  aabbMax;

    // AAA: Must perfectly mirror renderer.h MeshGPUData layout
    int   jointOffset;
    int   morphDeltaOffset;
    int   morphWeightOffset;
    int   morphCount;

    float transmissionFactor;
    float ior;
    float thicknessFactor;
    int   transmissionIndex;
    int   thicknessIndex;
    float attenuationColorR;
    float attenuationColorG;
    float attenuationColorB;
    float attenuationDistance;
    float dispersion;
    float _pad0;
    float _pad1;
};

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer MeshBuffer {
    MeshData meshes[];
};

// We define a dummy buffer reference here so the Push Constant size matches the Vertex Shader
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer VertexBuffer {
    float data[];
};

struct PackedJoint {
    vec4 row0;
    vec4 row1;
    vec4 row2;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer JointBuffer {
    PackedJoint joints[];
};

layout(push_constant) uniform PC {
    int  ambientOcclusionEnabled;
    int  iblEnabled;
    int  meshIndex;
    int  cascadeIndex;
    MeshBuffer meshData;
    VertexBuffer vertexData;
    JointBuffer jointData; // Matches pbr.vert exactly
} pc;

// ── Inputs from vertex shader ─────────────────────────────────────────────────
layout(location = 0) in vec3  inWorldPos;
layout(location = 1) in vec2  inTexCoord;
layout(location = 2) in vec4  inColor;
layout(location = 3) in mat3  inTBN;      // locations 3,4,5
layout(location = 6) in flat int inMeshIndex;

layout(location = 0) out vec4 outColor;

// ── Constants ────────────────────────────────────────────────────────────────
const float PI = 3.14159265358979;
const float INV_PI = 1.0 / PI;

// ── PBR BRDF ─────────────────────────────────────────────────────────────────

// GGX / Trowbridge-Reitz Normal Distribution Function
float D_GGX(float NdotH, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float d  = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

// Smith-Schlick-GGX Geometry Function (combined masking + shadowing)
float G_SmithGGX(float NdotV, float NdotL, float roughness) {
    float r  = roughness + 1.0;
    float k  = (r * r) / 8.0;   // Disney remapping for direct light
    float gV = NdotV / (NdotV * (1.0 - k) + k);
    float gL = NdotL / (NdotL * (1.0 - k) + k);
    return gV * gL;
}

// Fresnel-Schlick with roughness attenuation (for IBL ambient)
vec3 F_Schlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}
vec3 F_SchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0)
                * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// One directional/point light contribution
void lightContrib(vec3 L, vec3 lightColor, float attenuation,
                  vec3 N, vec3 V, vec3 albedo, float metallic, float roughness, vec3 F0,
                  inout vec3 outDiffuse, inout vec3 outSpecular) {
    vec3 H_dir = V + L;
    vec3 H = dot(H_dir, H_dir) < 0.0001 ? N : normalize(H_dir);
    float NdotL = max(dot(N, L), 0.0001);
    float NdotV = max(dot(N, V), 0.0001);
    float NdotH = max(dot(N, H), 0.0);
    float HdotV = max(dot(H, V), 0.0);

    float D = D_GGX(NdotH, roughness);
    float G = G_SmithGGX(NdotV, NdotL, roughness);
    vec3  F = F_Schlick(HdotV, F0);

    vec3 spec = (D * G * F) / max(4.0 * NdotV * NdotL, 0.001);
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    vec3 diff = kD * albedo * INV_PI;

    vec3 radiance = lightColor * attenuation * NdotL;
    outDiffuse  += diff * radiance;
    outSpecular += spec * radiance;
}

// ── IBL diffuse + specular ────────────────────────────────────────────────────
layout(set = 3, binding = 1) uniform samplerCube irradianceMap;
layout(set = 3, binding = 2) uniform samplerCube prefilterMap;
layout(set = 3, binding = 3) uniform sampler2D   brdfLUT;
layout(set = 3, binding = 4) uniform samplerCube skyboxMap;
layout(set = 3, binding = 6) uniform sampler2D   opaqueScreenMap;

#define IBL_MAX_LOD 4.0

void iblAmbient(vec3 N, vec3 V, vec3 albedo, float metallic, float roughness, vec3 F0, float ao, float visibility,
                inout vec3 outDiffuse, inout vec3 outSpecular) {
    vec3 F = F_SchlickRoughness(max(dot(N, V), 0.0), F0, roughness);
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);

    vec3 sampleN = N; sampleN.y *= -1.0;
    vec3 irradiance = texture(irradianceMap, sampleN).rgb;
    outDiffuse += kD * irradiance * albedo * ao;

    vec3 R = reflect(-V, N);
    vec3 sampleR = R; sampleR.y *= -1.0;
    vec3 prefilteredColor = textureLod(prefilterMap, sampleR, roughness * IBL_MAX_LOD).rgb;
    vec2 brdfLUT_val = texture(brdfLUT, vec2(max(dot(N, V), 0.0), roughness)).rg;

    float specOcclusion = mix(1.0, visibility, smoothstep(0.5, 1.0, max(dot(R, normalize(-lighting.sun.direction.xyz)), 0.0)) * (1.0 - roughness));
    outSpecular += prefilteredColor * (F * brdfLUT_val.x + brdfLUT_val.y) * ao * specOcclusion;
}

// ── Cascaded Shadow Mapping (Atlas) ──────────────────────────────────────────
float ShadowCalculation(vec3 worldPos, vec3 N, vec3 L, float NdotL) {
    vec4 viewPos = ubo.view * vec4(worldPos, 1.0);
    float z = abs(viewPos.z);

    int cascadeIndex = 0;
    for(int i = 0; i < 3; ++i) {
        if(z > lighting.cascadeSplits[i]) cascadeIndex = i + 1;
    }

    float cascadeScale = (cascadeIndex == 0) ? 1.0 : (cascadeIndex == 1) ? 2.0 : (cascadeIndex == 2) ? 4.0 : 8.0;
    float normalBias = cascadeScale * mix(0.05, 0.15, 1.0 - NdotL);
    vec3 biasedWorldPos = worldPos + N * normalBias;

    vec4 fragPosLightSpace = lighting.cascadeSpace[cascadeIndex] * vec4(biasedWorldPos, 1.0);
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    projCoords.xy = projCoords.xy * 0.5 + 0.5;

    if(projCoords.z > 1.0 || projCoords.z < 0.0 || projCoords.x < 0.0 || projCoords.x > 1.0 || projCoords.y < 0.0 || projCoords.y > 1.0)
        return 1.0;

    float bias = mix(0.001, 0.004, 1.0 - NdotL);
    if (cascadeIndex == 1) bias *= 1.2;
    else if (cascadeIndex == 2) bias *= 1.5;
    else if (cascadeIndex == 3) bias *= 2.0;

    vec2 atlasOffsets[4] = vec2[](
        vec2(0.0, 0.0), vec2(0.5, 0.0),
        vec2(0.0, 0.5), vec2(0.5, 0.5)
    );
    vec2 shadowUV = (projCoords.xy * 0.5) + atlasOffsets[cascadeIndex];

    float visibility = 0.0;
    vec2 texelSize = 1.0 / textureSize(shadowMap, 0).xy;
    float depth = projCoords.z - bias;

    vec2 poissonDisk[16] = vec2[](
        vec2( -0.94201624,  -0.39906216 ), vec2(  0.94558609, -0.76890725 ),
        vec2( -0.094184101, -0.92938870 ), vec2(  0.34495938,  0.29387760 ),
        vec2( -0.91588581,   0.45771432 ), vec2( -0.81544232, -0.87912464 ),
        vec2( -0.38277543,   0.27676845 ), vec2(  0.97484398,  0.75648379 ),
        vec2(  0.44323325,  -0.97511554 ), vec2(  0.53742981, -0.47373420 ),
        vec2( -0.26496911,  -0.41893023 ), vec2(  0.79197514,  0.19090188 ),
        vec2( -0.24188840,   0.99706507 ), vec2( -0.81409955,  0.91437590 ),
        vec2(  0.19984126,   0.78641367 ), vec2(  0.14383161, -0.14100467 )
    );

    float filterRadius = 1.5;
    for (int i = 0; i < 16; i++) {
        visibility += texture(shadowMap, vec3(shadowUV + poissonDisk[i] * texelSize * filterRadius, depth));
    }
    visibility /= 16.0;

    return visibility;
}

// Inverse ACES: recovers scene-linear HDR from a tonemapped value.
// Solves: x = (t*(2.51t+0.03))/(t*(2.51t+0.59)+0.06) for t.
// Rearranged: t^2*2.51*(x-1) + t*(0.59x-0.03) + 0.06x = 0
vec3 InverseACES(vec3 x) {
    x = clamp(x, 0.0001, 0.9999);
    vec3 a = 2.51 * (x - 1.0);
    vec3 b = 0.59 * x - 0.03;
    vec3 c = 0.06 * x;
    vec3 disc = max(b*b - 4.0*a*c, vec3(0.0));
    return (-b - sqrt(disc)) / (2.0 * a);
}

// AAA Unified Refraction Function (Supports 3-Tap Dispersion & Frosted Glass)
vec3 getTransmittedColor(vec3 viewPos, vec3 viewN, vec3 viewV, float ior, float pathLength, float maxDistortion, float roughness) {
    vec3 viewR = refract(-viewV, viewN, 1.0 / ior);
    if (dot(viewR, viewR) < 0.001) viewR = reflect(-viewV, viewN); // Total Internal Reflection

    vec2 uvOffset = vec2(0.0);
    if (pathLength > 0.0) {
        float maxZ = -0.01;
        float safePathLength = pathLength;
        if (viewPos.z + viewR.z * pathLength > maxZ) {
            if (viewR.z > 0.0001) safePathLength = (maxZ - viewPos.z) / viewR.z;
            else safePathLength = 0.0;
        }
        vec3 exitViewPos = viewPos + viewR * safePathLength;
        vec4 clipEnter = ubo.proj * vec4(viewPos, 1.0);
        vec4 clipExit  = ubo.proj * vec4(exitViewPos, 1.0);
        vec2 ndcEnter = clipEnter.xy / clipEnter.w;
        vec2 ndcExit  = clipExit.xy / clipExit.w;
        uvOffset = (ndcExit - ndcEnter) * 0.5;
    } else {
        vec2 refrDir = viewN.xy * (1.0 - 1.0 / ior);
        uvOffset = refrDir * 0.05;
    }

    if (length(uvOffset) > maxDistortion) {
        uvOffset = normalize(uvOffset) * maxDistortion;
    }

    vec2 screenSize = vec2(textureSize(opaqueScreenMap, 0));
    vec2 screenUV = gl_FragCoord.xy / screenSize;
    vec2 refrUV = screenUV + uvOffset;

    if (refrUV.x >= 0.0 && refrUV.x <= 1.0 && refrUV.y >= 0.0 && refrUV.y <= 1.0) {
        vec3 sampledColor;
        // AAA Frosted Glass: High-performance 5-tap cross blur
        if (roughness > 0.05) {
            vec2 t = (roughness * 10.0) / screenSize;
            sampledColor = textureLod(opaqueScreenMap, refrUV, 0.0).rgb * 0.3333;
            sampledColor += textureLod(opaqueScreenMap, refrUV + vec2(t.x, t.y), 0.0).rgb * 0.1666;
            sampledColor += textureLod(opaqueScreenMap, refrUV + vec2(-t.x, t.y), 0.0).rgb * 0.1666;
            sampledColor += textureLod(opaqueScreenMap, refrUV + vec2(t.x, -t.y), 0.0).rgb * 0.1666;
            sampledColor += textureLod(opaqueScreenMap, refrUV + vec2(-t.x, -t.y), 0.0).rgb * 0.1666;
        } else {
            sampledColor = textureLod(opaqueScreenMap, refrUV, 0.0).rgb;
        }

        // transmissionImage is SRGB format: hardware linearizes on sample automatically.
        // sampledColor is now linear, but has been through ACES tonemap + exposure.
        // Reconstruct scene-linear HDR by inverting the exact pipeline used in main():
        //   pipeline: scene_linear * exposure -> ACES -> stored as SRGB
        // inverse:  SRGB hw-linearize (done) -> InverseACES -> / exposure
        const float exposure = 0.6;
        // InverseACES: solve x = (t*(2.51*t+0.03))/(t*(2.51*t+0.59)+0.06) for t given x
        // Quadratic: 2.51*t^2 + (0.03 - x*(2.51*t+0.59))*... simplified:
        // a=2.51, b=0.59-2.51*x (wait — standard derivation):
        // x*(2.51t^2 + 0.59t + 0.06) = 2.51t^2 + 0.03t
        // t^2*(2.51x-2.51) + t*(0.59x-0.03) + 0.06x = 0
        // t^2*2.51*(x-1) + t*(0.59x-0.03) + 0.06x = 0
        vec3 t = clamp(sampledColor, 0.0001, 0.9999);
        vec3 a = 2.51 * (t - 1.0);
        vec3 b = 0.59 * t - 0.03;
        vec3 c = 0.06 * t;
        vec3 disc = max(b*b - 4.0*a*c, vec3(0.0));
        vec3 scene_linear = (-b - sqrt(disc)) / (2.0 * a);
        return max(scene_linear / exposure, vec3(0.0));
    } else {
        // Off-screen fallback: Read raw HDR directly from the environment
        vec3 worldR = inverse(mat3(ubo.view)) * viewR;
        worldR.y *= -1.0;
        return textureLod(prefilterMap, worldR, roughness * 4.0).rgb;
    }
}

void main() {
    MeshData m = pc.meshData.meshes[inMeshIndex];
    vec2 texCoord = inTexCoord;

    vec4 albedoSample = (m.albedoIndex >= 0) ? texture(textures[nonuniformEXT(m.albedoIndex)], texCoord) : vec4(1.0);
    albedoSample.rgb = pow(albedoSample.rgb, vec3(2.2));
    vec4 baseColor = albedoSample * m.baseColorFactor * inColor;

    if (m.alphaMode == 1 && baseColor.a < m.alphaCutoff) discard;
    vec3 albedo = baseColor.rgb;

    vec3 N;
    if (m.normalMapIndex >= 0) {
        vec3 nSample = texture(textures[nonuniformEXT(m.normalMapIndex)], texCoord).rgb;
        nSample = nSample * 2.0 - 1.0;
        nSample.y = -nSample.y;
        N = normalize(inTBN * nSample);
    } else {
        N = normalize(inTBN[2]);
    }

    float metallic  = m.metallicFactor;
    float roughness = m.roughnessFactor;
    if (m.metallicRoughIndex >= 0) {
        vec4 mr = texture(textures[nonuniformEXT(m.metallicRoughIndex)], texCoord);
        metallic  *= mr.b;
        roughness *= mr.g;
    }
    roughness = clamp(roughness, 0.04, 1.0);
    metallic  = clamp(metallic,  0.0,  1.0);

    float ao = 1.0;
    if (m.aoIndex >= 0 && pc.ambientOcclusionEnabled != 0) {
        ao = texture(textures[nonuniformEXT(m.aoIndex)], texCoord).r;
    }

    if (m.isUnlit != 0) {
        outColor = vec4(baseColor.rgb, baseColor.a);
        return;
    }

    vec3 V = normalize(ubo.cameraPos.xyz - inWorldPos);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    vec3 diffuseLight = vec3(0.0);
    vec3 specularLight = vec3(0.0);

    vec3 sunL = normalize(-lighting.sun.direction.xyz);
    float sunNdotL = max(dot(N, sunL), 0.0);
    float visibility = ShadowCalculation(inWorldPos, N, sunL, sunNdotL);

    vec3 lc = lighting.sun.color.rgb * lighting.sun.color.w;
    lightContrib(sunL, lc, visibility, N, V, albedo, metallic, roughness, F0, diffuseLight, specularLight);

    for (int i = 0; i < lighting.pointLightCount; i++) {
        vec3 lpos = lighting.pointLights[i].position.xyz;
        float radius = lighting.pointLights[i].position.w;
        vec3 pl_c = lighting.pointLights[i].color.rgb * lighting.pointLights[i].color.w;

        vec3 L = lpos - inWorldPos;
        float dist = length(L);
        L = L / dist;

        float atten = 1.0 / (dist * dist + 0.0001);
        float falloff = clamp(1.0 - pow(dist / max(radius, 0.001), 4.0), 0.0, 1.0);
        atten *= falloff * falloff;

        lightContrib(L, pl_c, atten, N, V, albedo, metallic, roughness, F0, diffuseLight, specularLight);
    }

    if (lighting.iblEnabled != 0 && pc.iblEnabled != 0) {
        iblAmbient(N, V, albedo, metallic, roughness, F0, ao, visibility, diffuseLight, specularLight);
        diffuseLight *= lighting.ambientIntensity;
        specularLight *= lighting.ambientIntensity;
    } else {
        diffuseLight += vec3(lighting.ambientIntensity) * albedo * ao * mix(0.5, 1.0, visibility);
    }

    vec3 emissive = m.emissiveFactor * m.emissiveStrength;
    if (m.emissiveIndex >= 0) {
        vec3 eMap = texture(textures[nonuniformEXT(m.emissiveIndex)], inTexCoord).rgb;
        emissive *= pow(eMap, vec3(2.2));
    }

    float transmission = m.transmissionFactor;
    if (m.transmissionIndex >= 0) {
        transmission *= texture(textures[nonuniformEXT(m.transmissionIndex)], texCoord).r;
    }

    vec3 color;
    if (transmission > 0.0 && lighting.iblEnabled != 0 && pc.iblEnabled != 0) {
      // World scale: average of all three axes to handle non-uniform scaling correctly.
      float scaleX = length(vec3(m.model[0][0], m.model[1][0], m.model[2][0]));
      float scaleY = length(vec3(m.model[0][1], m.model[1][1], m.model[2][1]));
      float scaleZ = length(vec3(m.model[0][2], m.model[1][2], m.model[2][2]));
      float worldScale = (scaleX + scaleY + scaleZ) / 3.0;

      float thickness = m.thicknessFactor;
      if (m.thicknessIndex >= 0) {
        thickness *= texture(textures[nonuniformEXT(m.thicknessIndex)], texCoord).g;
      }
      // Guard: if no thickness data, use a small default so absorption is visible but not total.
      float pathLength = (thickness > 0.0) ? thickness * worldScale : 0.0;

      vec4 viewPos = ubo.view * vec4(inWorldPos, 1.0);
      vec3 viewN = normalize(mat3(ubo.view) * N);
      vec3 viewV = normalize(-viewPos.xyz);

      float ior = max(m.ior, 1.001);
      vec3 transmittedLight;

      // AAA Chromatic Dispersion (KHR_materials_dispersion)
      if (m.dispersion > 0.0) {
        // Correct Abbe Number implementation
        float dispAmt = (ior - 1.0) / (2.0 * max(m.dispersion, 0.001));
        float iorR = max(1.001, ior - dispAmt);
        float iorG = max(1.001, ior);
        float iorB = max(1.001, ior + dispAmt);

        float r = getTransmittedColor(viewPos.xyz, viewN, viewV, iorR, pathLength, 0.2, roughness).r;
        float g = getTransmittedColor(viewPos.xyz, viewN, viewV, iorG, pathLength, 0.2, roughness).g;
        float b = getTransmittedColor(viewPos.xyz, viewN, viewV, iorB, pathLength, 0.2, roughness).b;
        transmittedLight = vec3(r, g, b);
      } else {
        transmittedLight = getTransmittedColor(viewPos.xyz, viewN, viewV, ior, pathLength, 0.2, roughness);
      }

      // AAA Fresnel Blend: Edges should reflect environment, not purely refract!
      float R0 = pow((1.0 - ior) / (1.0 + ior), 2.0);
      float F_glass = R0 + (1.0 - R0) * pow(clamp(1.0 - dot(viewN, viewV), 0.0, 1.0), 5.0);
      vec3 worldRefl = reflect(-V, N);
      worldRefl.y *= -1.0;
      vec3 reflectionLight = textureLod(prefilterMap, worldRefl, roughness * 4.0).rgb;
      transmittedLight = mix(transmittedLight, reflectionLight, F_glass);

      vec3 absorption = vec3(1.0);
      if (pathLength > 0.0 && m.attenuationDistance > 0.0 && m.attenuationDistance < 99999.0) {
        // glTF spec: absorption coefficient from attenuation color and distance.
        // attenuationColor is the color at attenuationDistance thickness.
        // We clamp away from 0 and 1 to keep log finite.
        vec3 attColor = clamp(
                              vec3(m.attenuationColorR, m.attenuationColorG, m.attenuationColorB),
                              vec3(0.001), vec3(0.999));
        vec3 sigma_a = -log(attColor) / max(m.attenuationDistance, 0.0001);
        // Clamp pathLength so sigma_a * pathLength never exceeds ~10 (exp(-10) ~ 0.00005).
        // Beyond that absorption is total black regardless — no need to compute further.
        float clampedPath = min(pathLength, m.attenuationDistance * 4.0);
        absorption = exp(-sigma_a * clampedPath);
      }

      vec3 transmissionColor = transmittedLight * albedo * absorption;

      // HDR Linear mix, single tonemap execution!
      vec3 localLinear = mix(diffuseLight, transmissionColor, transmission * (1.0 - metallic)) + specularLight + emissive;
      float exposure = 0.6;
      vec3 tonemapped = localLinear * exposure;
      color = (tonemapped * (2.51 * tonemapped + 0.03)) / (tonemapped * (2.51 * tonemapped + 0.59) + 0.06);

    } else {
        vec3 localLinear = diffuseLight + specularLight + emissive;
        float exposure = 0.6;
        vec3 tonemapped = localLinear * exposure;
        color = (tonemapped * (2.51 * tonemapped + 0.03)) / (tonemapped * (2.51 * tonemapped + 0.59) + 0.06);
    }
    color = clamp(color, 0.0, 1.0);

    float finalAlpha = baseColor.a;
    if (m.alphaMode == 0 || m.alphaMode == 1) finalAlpha = 1.0;

    // For transmission, we manually blended the background, so overwrite dest entirely
    if (transmission > 0.0) finalAlpha = 1.0;

    outColor = vec4(color, finalAlpha);
}

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
} lighting;

struct MeshData {
    mat4  model;
    mat4  normalMatrix;
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
    int   _pad0;
    vec3  emissiveFactor;
    int   _pad1;
    vec4  aabbMin;
    vec4  aabbMax;
};
layout(set = 2, binding = 0) readonly buffer MeshSSBO {
    MeshData meshes[];
};

layout(push_constant) uniform PC {
    int  ambientOcclusionEnabled;
    int  iblEnabled;
    int  meshIndex;
    int  _pad;
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
vec3 lightContrib(vec3 L, vec3 lightColor, float attenuation,
                  vec3 N, vec3 V,
                  vec3 albedo, float metallic, float roughness, vec3 F0) {
    vec3  H     = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0001);
    float NdotV = max(dot(N, V), 0.0001);
    float NdotH = max(dot(N, H), 0.0);
    float HdotV = max(dot(H, V), 0.0);

    float D  = D_GGX(NdotH, roughness);
    float G  = G_SmithGGX(NdotV, NdotL, roughness);
    vec3  F  = F_Schlick(HdotV, F0);

    // Cook-Torrance specular
    vec3  spec  = (D * G * F) / (4.0 * NdotV * NdotL);

    // Lambertian diffuse — energy conserving: (1-F)(1-metallic)
    vec3  kD    = (vec3(1.0) - F) * (1.0 - metallic);
    vec3  diff  = kD * albedo * INV_PI;

    return (diff + spec) * lightColor * attenuation * NdotL;
}

// ── IBL diffuse + specular ────────────────────────────────────────────────────
layout(set = 3, binding = 1) uniform samplerCube irradianceMap;
layout(set = 3, binding = 2) uniform samplerCube prefilterMap;
layout(set = 3, binding = 3) uniform sampler2D   brdfLUT;

#define IBL_MAX_LOD 5.0

vec3 iblAmbient(vec3 N, vec3 V, vec3 albedo, float metallic, float roughness, vec3 F0, float ao) {
    vec3 F    = F_SchlickRoughness(max(dot(N, V), 0.0), F0, roughness);
    vec3 kD   = (vec3(1.0) - F) * (1.0 - metallic);

    // Diffuse: Sample the irradiance cubemap directly using the normal
    vec3 irradiance = texture(irradianceMap, N).rgb;
    vec3 diffuse    = kD * irradiance * albedo;

    // Specular: Sample the prefiltered cubemap and BRDF lookup table
    vec3 R                = reflect(-V, N);
    vec3 prefilteredColor = textureLod(prefilterMap, R, roughness * IBL_MAX_LOD).rgb;
    vec2 brdfLUT_val      = texture(brdfLUT, vec2(max(dot(N, V), 0.0), roughness)).rg;
    vec3 specular         = prefilteredColor * (F * brdfLUT_val.x + brdfLUT_val.y);

    return (diffuse + specular) * ao;
}

// ── Main ──────────────────────────────────────────────────────────────────────
void main() {
    MeshData m = meshes[inMeshIndex];

    // ── Sample albedo ─────────────────────────────────────────────────────────
    vec4 albedoSample = (m.albedoIndex >= 0)
        ? texture(textures[nonuniformEXT(m.albedoIndex)], inTexCoord)
        : vec4(1.0);

    // Convert SRGB texture to Linear space IMMEDIATELY.
    // We load all textures as UNORM to preserve raw data for Normal/Metallic maps,
    // which means we must manually decode SRGB for color maps.
    albedoSample.rgb = pow(albedoSample.rgb, vec3(2.2));

    // m.baseColorFactor and inColor are already linear
    vec4 baseColor = albedoSample * m.baseColorFactor * inColor;

    // Alpha handling
    if (m.alphaMode == 1 && baseColor.a < m.alphaCutoff) discard;

    vec3 albedo = baseColor.rgb;

    // ── Normal mapping ────────────────────────────────────────────────────────
    vec3 N;
    if (m.normalMapIndex >= 0) {
        vec3 nSample = texture(textures[nonuniformEXT(m.normalMapIndex)], inTexCoord).rgb;
        nSample = nSample * 2.0 - 1.0;          // unpack [0,1] -> [-1,1]
        N = normalize(inTBN * nSample);
    } else {
        N = normalize(inTBN[2]);                 // use geometry normal
    }

    // ── Metallic / Roughness ──────────────────────────────────────────────────
    float metallic  = m.metallicFactor;
    float roughness = m.roughnessFactor;
    if (m.metallicRoughIndex >= 0) {
        vec4 mr = texture(textures[nonuniformEXT(m.metallicRoughIndex)], inTexCoord);
        metallic  *= mr.b;   // glTF: B channel = metallic
        roughness *= mr.g;   // glTF: G channel = roughness
    }
    roughness = clamp(roughness, 0.04, 1.0);
    metallic  = clamp(metallic,  0.0,  1.0);

    // ── Ambient Occlusion ─────────────────────────────────────────────────────
    float ao = 1.0;
    if (m.aoIndex >= 0 && pc.ambientOcclusionEnabled != 0) {
        ao = texture(textures[nonuniformEXT(m.aoIndex)], inTexCoord).r;
    }

    // ── Unlit path ────────────────────────────────────────────────────────────
    if (m.isUnlit != 0) {
        // Base color is now strictly linear. Output it directly.
        // Vulkan's SRGB swapchain will handle the Linear -> SRGB conversion.
        outColor = vec4(baseColor.rgb, baseColor.a);
        return;
    }

    // ── PBR shading ───────────────────────────────────────────────────────────
    vec3 V  = normalize(ubo.cameraPos.xyz - inWorldPos);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);  // dielectric F0=0.04, metal F0=albedo

    vec3 Lo = vec3(0.0);

    // Directional light (sun)
    {
        vec3  L    = normalize(-lighting.sun.direction.xyz);
        vec3  lc   = lighting.sun.color.rgb * lighting.sun.color.w;
        Lo += lightContrib(L, lc, 1.0, N, V, albedo, metallic, roughness, F0);
    }

    // Point lights
    for (int i = 0; i < lighting.pointLightCount; i++) {
        vec3  lpos  = lighting.pointLights[i].position.xyz;
        float radius= lighting.pointLights[i].position.w;
        vec3  lc    = lighting.pointLights[i].color.rgb * lighting.pointLights[i].color.w;

        vec3  L     = lpos - inWorldPos;
        float dist  = length(L);
        L = L / dist;

        // Inverse-square attenuation with smooth radius falloff
        float atten = 1.0 / (dist * dist + 0.0001);
        float falloff = clamp(1.0 - pow(dist / max(radius, 0.001), 4.0), 0.0, 1.0);
        atten *= falloff * falloff;

        Lo += lightContrib(L, lc, atten, N, V, albedo, metallic, roughness, F0);
    }

    // ── Ambient (IBL or flat) ─────────────────────────────────────────────────
    vec3 ambient;
    if (lighting.iblEnabled != 0 && pc.iblEnabled != 0) {
        ambient = iblAmbient(N, V, albedo, metallic, roughness, F0, ao);
        ambient *= lighting.ambientIntensity;
    } else {
        ambient = vec3(lighting.ambientIntensity) * albedo * ao;
    }

    // ── Emissive ──────────────────────────────────────────────────────────────
    vec3 emissive = m.emissiveFactor * m.emissiveStrength;
    if (m.emissiveIndex >= 0) {
        vec3 eMap = texture(textures[nonuniformEXT(m.emissiveIndex)], inTexCoord).rgb;
        emissive *= pow(eMap, vec3(2.2));
    }

    vec3 color = Lo + ambient + emissive;

    // ── Tone mapping (ACES filmic) ────────────────────────────────────────────
    // ACES fitted approximation by Krzysztof Narkowicz
    color = (color * (2.51 * color + 0.03)) / (color * (2.51 * color + 0.59) + 0.06);

    // NOTE: We write to an SRGB swapchain (VK_FORMAT_B8G8R8A8_SRGB),
    // so Vulkan hardware automatically applies gamma correction.
    // Doing pow(..., 1/2.2) here would double-gamma correct and wash out colors!
    color = clamp(color, 0.0, 1.0);

    outColor = vec4(color, baseColor.a);
}

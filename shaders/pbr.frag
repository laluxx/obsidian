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
    int   displacementIndex;
    vec3  emissiveFactor;
    float displacementScale;
    vec4  aabbMin;
    vec4  aabbMax;

    // AAA: Must perfectly mirror renderer.h MeshGPUData layout
    int   jointOffset;
    int   _pad0;
    int   _pad1;
    int   _pad2;
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

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer JointBuffer {
    mat4 matrices[];
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

// We allocated 5 mip levels (0, 1, 2, 3, 4). The maximum valid LOD is 4.0!
#define IBL_MAX_LOD 4.0

vec3 iblAmbient(vec3 N, vec3 V, vec3 L, vec3 albedo, float metallic, float roughness, vec3 F0, float ao, float visibility) {
    vec3 F    = F_SchlickRoughness(max(dot(N, V), 0.0), F0, roughness);
    vec3 kD   = (vec3(1.0) - F) * (1.0 - metallic);

    // Vulkan Y-flip for Cubemaps
    vec3 sampleN = N; sampleN.y *= -1.0;

    // Diffuse: Sample the irradiance cubemap directly using the normal
    vec3 irradiance = texture(irradianceMap, sampleN).rgb;
    vec3 diffuse    = kD * irradiance * albedo;

    // Specular: Sample the prefiltered cubemap and BRDF lookup table
    vec3 R                = reflect(-V, N);
    vec3 sampleR          = R; sampleR.y *= -1.0;
    vec3 prefilteredColor = textureLod(prefilterMap, sampleR, roughness * IBL_MAX_LOD).rgb;
    vec2 brdfLUT_val      = texture(brdfLUT, vec2(max(dot(N, V), 0.0), roughness)).rg;

    // ── AAA SPECULAR OCCLUSION ──
    // The IBL prefilter map contains the blazing HDR sun. We must prevent shiny objects
    // from reflecting the sun when they are standing in a shadow!
    float RdotL = max(dot(R, L), 0.0);
    // Mask out the reflection if it points towards the occluded sun.
    // We factor in roughness because rough materials blur the sun out.
    float specOcclusion = mix(1.0, visibility, smoothstep(0.5, 1.0, RdotL) * (1.0 - roughness));

    vec3 specular = prefilteredColor * (F * brdfLUT_val.x + brdfLUT_val.y) * specOcclusion;

    // ── AAA SKY OCCLUSION ──
    // A shadow blocks the sun, but an object casting a shadow usually blocks
    // part of the sky too! We gently darken the IBL in shadowed areas.
    float skyOcclusion = mix(0.5, 1.0, visibility);

    return (diffuse + specular) * ao * skyOcclusion;
}

// ── Cascaded Shadow Mapping (Atlas) ──────────────────────────────────────────
float ShadowCalculation(vec3 worldPos, vec3 N, vec3 L, float NdotL) {
    vec4 viewPos = ubo.view * vec4(worldPos, 1.0);
    float z = abs(viewPos.z);

    int cascadeIndex = 0;
    for(int i = 0; i < 3; ++i) {
        if(z > lighting.cascadeSplits[i]) cascadeIndex = i + 1;
    }

    // AAA Normal Offset Bias
    // Push the shadow sample point out along the normal to stop self-shadowing.
    // We scale by cascade index because distant cascades have larger world-space texels.
    float cascadeScale = (cascadeIndex == 0) ? 1.0 : (cascadeIndex == 1) ? 2.0 : (cascadeIndex == 2) ? 4.0 : 8.0;

    // CRITICAL FIX: The bias MUST NOT drop to 0.0 when facing the sun, or you get a checkerboard pattern!
    // We use mix to guarantee a minimum normal bias of 0.05.
    float normalBias = cascadeScale * mix(0.05, 0.15, 1.0 - NdotL);
    vec3 biasedWorldPos = worldPos + N * normalBias;

    vec4 fragPosLightSpace = lighting.cascadeSpace[cascadeIndex] * vec4(biasedWorldPos, 1.0);
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    projCoords.xy = projCoords.xy * 0.5 + 0.5;

    if(projCoords.z > 1.0 || projCoords.z < 0.0 || projCoords.x < 0.0 || projCoords.x > 1.0 || projCoords.y < 0.0 || projCoords.y > 1.0)
        return 0.0;

    // Base depth bias. Also cannot be allowed to drop to microscopic levels (0.0001)
    // because IEEE 32-bit floats will cause Z-fighting against the depth map.
    float bias = mix(0.001, 0.004, 1.0 - NdotL);
    if (cascadeIndex == 1) bias *= 1.2;
    else if (cascadeIndex == 2) bias *= 1.5;
    else if (cascadeIndex == 3) bias *= 2.0;

    // Atlas Offsets: 4 quadrants in a 2x2 grid
    vec2 atlasOffsets[4] = vec2[](
        vec2(0.0, 0.0), vec2(0.5, 0.0),
        vec2(0.0, 0.5), vec2(0.5, 0.5)
    );

    // Scale down UVs by half to fit in one quadrant, then shift to the correct slot
    vec2 shadowUV = (projCoords.xy * 0.5) + atlasOffsets[cascadeIndex];

    // sampler2DShadow returns 1.0 when LIT and 0.0 when SHADOWED!
    float visibility = 0.0;
    vec2 texelSize = 1.0 / textureSize(shadowMap, 0).xy;
    float depth = projCoords.z - bias;

    // AAA Poisson Disk Filter (16-Tap)
    // These coordinates are organically scattered in a circle to eliminate pixelated, blocky edges.
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

    // Increase the filter radius to make the shadow edge softer and more cinematic
    float filterRadius = 1.5;

    for (int i = 0; i < 16; i++) {
        visibility += texture(shadowMap, vec3(shadowUV + poissonDisk[i] * texelSize * filterRadius, depth));
    }
    visibility /= 16.0;

    return visibility;
}

// ── Main ──────────────────────────────────────────────────────────────────────
void main() {
    MeshData m = pc.meshData.meshes[inMeshIndex];

    vec2 texCoord = inTexCoord;

    // ── Sample albedo ─────────────────────────────────────────────────────────
    vec4 albedoSample = (m.albedoIndex >= 0)
        ? texture(textures[nonuniformEXT(m.albedoIndex)], texCoord)
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
        vec3 nSample = texture(textures[nonuniformEXT(m.normalMapIndex)], texCoord).rgb;
        nSample = nSample * 2.0 - 1.0;          // unpack [0,1] -> [-1,1]

        // Invert Y to match Vulkan's +Y DOWN coordinate system
        nSample.y = -nSample.y;

        N = normalize(inTBN * nSample);
    } else {
        N = normalize(inTBN[2]);                 // use geometry normal
    }

    // ── Metallic / Roughness ──────────────────────────────────────────────────
    float metallic  = m.metallicFactor;
    float roughness = m.roughnessFactor;
    if (m.metallicRoughIndex >= 0) {
        vec4 mr = texture(textures[nonuniformEXT(m.metallicRoughIndex)], texCoord);
        metallic  *= mr.b;   // glTF: B channel = metallic
        roughness *= mr.g;   // glTF: G channel = roughness
    }
    roughness = clamp(roughness, 0.04, 1.0);
    metallic  = clamp(metallic,  0.0,  1.0);

    // ── Ambient Occlusion ─────────────────────────────────────────────────────
    float ao = 1.0;
    if (m.aoIndex >= 0 && pc.ambientOcclusionEnabled != 0) {
        ao = texture(textures[nonuniformEXT(m.aoIndex)], texCoord).r;
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

    // Evaluate Directional Shadow globally so we can pass it to the IBL
    vec3  sunL       = normalize(-lighting.sun.direction.xyz);
    float sunNdotL   = max(dot(N, sunL), 0.0);
    float visibility = ShadowCalculation(inWorldPos, N, sunL, sunNdotL);

    // Directional light (sun)
    {
        vec3 lc = lighting.sun.color.rgb * lighting.sun.color.w;
        Lo += lightContrib(sunL, lc, 1.0, N, V, albedo, metallic, roughness, F0) * visibility;
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
        ambient = iblAmbient(N, V, sunL, albedo, metallic, roughness, F0, ao, visibility);
        ambient *= lighting.ambientIntensity;
    } else {
        ambient = vec3(lighting.ambientIntensity) * albedo * ao * mix(0.5, 1.0, visibility);
    }

    // ── Emissive ──────────────────────────────────────────────────────────────
    vec3 emissive = m.emissiveFactor * m.emissiveStrength;
    if (m.emissiveIndex >= 0) {
        vec3 eMap = texture(textures[nonuniformEXT(m.emissiveIndex)], inTexCoord).rgb;
        emissive *= pow(eMap, vec3(2.2));
    }

    vec3 color = Lo + ambient + emissive;

    // ── Tone mapping (ACES filmic) ─────────────────────────────────
    float exposure = 0.6;
    color = color * exposure;

    // ACES fitted approximation by Krzysztof Narkowicz
    color = (color * (2.51 * color + 0.03)) / (color * (2.51 * color + 0.59) + 0.06);

    // NOTE: We write to an SRGB swapchain (VK_FORMAT_B8G8R8A8_SRGB),
    // so Vulkan hardware automatically applies gamma correction.
    // Doing pow(..., 1/2.2) here would double-gamma correct and wash out colors!
    color = clamp(color, 0.0, 1.0);

    float finalAlpha = baseColor.a;

    // Force alpha behavior based on glTF alphaMode to override the hardware blender
    if (m.alphaMode == 0) {
        // OPAQUE: Completely ignore texture alpha
        finalAlpha = 1.0;
    } else if (m.alphaMode == 1) {
        // MASK: Discard already happened above, surviving pixels must be solid
        finalAlpha = 1.0;
    }
    // BLEND (mode 2) leaves finalAlpha as baseColor.a

    outColor = vec4(color, finalAlpha);
}

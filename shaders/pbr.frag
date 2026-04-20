#version 450
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_EXT_buffer_reference     : require
#extension GL_EXT_buffer_reference2    : require

// ── Descriptor sets ───────────────────────────────────────────────────────────

layout(set = 0, binding = 0) uniform UBO {
    mat4  vp;
    mat4  view;
    mat4  proj;
    vec4  cameraPos;
    float time;
} ubo;

layout(set = 1, binding = 0) uniform sampler2D textures[];

struct PointLight {
    vec4 position; // xyz=pos, w=radius
    vec4 color;    // xyz=color, w=intensity
};
struct DirLight {
    vec4 direction;
    vec4 color; // w=intensity
};
layout(set = 3, binding = 0) uniform LightingUBO {
    DirLight   sun;
    PointLight pointLights[8];
    int        pointLightCount;
    float      ambientIntensity;
    int        iblEnabled;
    int        _pad;
    vec4       cameraPos;
    mat4       cascadeSpace[4];
    vec4       cascadeSplits;
} lighting;

layout(set = 3, binding = 1) uniform samplerCube irradianceMap;
layout(set = 3, binding = 2) uniform samplerCube prefilterMap;
layout(set = 3, binding = 3) uniform sampler2D   brdfLUT;
layout(set = 3, binding = 4) uniform samplerCube skyboxMap;
layout(set = 3, binding = 5) uniform sampler2DShadow shadowMap;
layout(set = 3, binding = 6) uniform sampler2D        opaqueScreenMap;

// ── Mesh data (buffer device address) ────────────────────────────────────────

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

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer MeshBuffer {
    MeshData meshes[];
};
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer VertexBuffer {
    float data[];
};
struct PackedJoint { vec4 row0; vec4 row1; vec4 row2; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer JointBuffer {
    PackedJoint joints[];
};

layout(push_constant) uniform PC {
    int          ambientOcclusionEnabled;
    int          iblEnabled;
    int          meshIndex;
    int          cascadeIndex;
    MeshBuffer   meshData;
    VertexBuffer vertexData;
    JointBuffer  jointData;
} pc;

// ── Vertex shader outputs ─────────────────────────────────────────────────────

layout(location = 0) in vec3        inWorldPos;
layout(location = 1) in vec2        inTexCoord;
layout(location = 2) in vec4        inColor;
layout(location = 3) in mat3        inTBN;     // occupies locations 3, 4, 5
layout(location = 6) in flat int    inMeshIndex;

layout(location = 0) out vec4 outColor;

// ── Constants ─────────────────────────────────────────────────────────────────

const float PI          = 3.14159265358979;
const float INV_PI      = 1.0 / PI;
const float IBL_MAX_LOD = 4.0;
const float EXPOSURE    = 0.6;

// ── BRDF ──────────────────────────────────────────────────────────────────────

// GGX normal distribution function
float D_GGX(float NdotH, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float d  = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

// Smith-GGX masking-shadowing term for direct lights (Disney k remapping)
float G_SmithGGX(float NdotV, float NdotL, float roughness) {
    float r  = roughness + 1.0;
    float k  = (r * r) * 0.125;
    float gV = NdotV / (NdotV * (1.0 - k) + k);
    float gL = NdotL / (NdotL * (1.0 - k) + k);
    return gV * gL;
}

vec3 F_Schlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Fresnel with roughness attenuation — used for IBL where we don't have a half-vector
vec3 F_SchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0)
              * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Accumulates one direct light contribution into separate diffuse and specular terms.
// Keeping them separate lets the transmission path blend only the diffuse component.
void lightContrib(
    vec3 L, vec3 lightColor, float attenuation,
    vec3 N, vec3 V, vec3 albedo, float metallic, float roughness, vec3 F0,
    inout vec3 outDiffuse, inout vec3 outSpecular)
{
    vec3  H_dir = V + L;
    // Guard against degenerate half-vector when L and V are exactly opposite
    vec3  H     = dot(H_dir, H_dir) < 1e-4 ? N : normalize(H_dir);
    float NdotL = max(dot(N, L), 1e-4);
    float NdotV = max(dot(N, V), 1e-4);
    float NdotH = max(dot(N, H), 0.0);
    float HdotV = max(dot(H, V), 0.0);

    float D   = D_GGX(NdotH, roughness);
    float G   = G_SmithGGX(NdotV, NdotL, roughness);
    vec3  F   = F_Schlick(HdotV, F0);
    vec3  kD  = (vec3(1.0) - F) * (1.0 - metallic);
    vec3  rad = lightColor * attenuation * NdotL;

    outDiffuse  += kD * albedo * INV_PI * rad;
    outSpecular += (D * G * F) / max(4.0 * NdotV * NdotL, 1e-3) * rad;
}

// ── IBL ───────────────────────────────────────────────────────────────────────

void iblAmbient(
    vec3 N, vec3 V, vec3 albedo, float metallic, float roughness,
    vec3 F0, float ao, float visibility,
    inout vec3 outDiffuse, inout vec3 outSpecular)
{
    vec3 F  = F_SchlickRoughness(max(dot(N, V), 0.0), F0, roughness);
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);

    // Cubemaps are sampled with Y flipped to match Vulkan's clip-space convention
    vec3 sampleN = vec3(N.x, -N.y, N.z);
    outDiffuse  += kD * texture(irradianceMap, sampleN).rgb * albedo * ao;

    vec3 R       = reflect(-V, N);
    vec3 sampleR = vec3(R.x, -R.y, R.z);
    vec3 prefilteredColor = textureLod(prefilterMap, sampleR, roughness * IBL_MAX_LOD).rgb;
    vec2 brdf         = texture(brdfLUT, vec2(max(dot(N, V), 0.0), roughness)).rg;

    // Reduce specular contribution when the reflection vector points toward a shadow.
    // Rough surfaces spread the reflection lobe enough that the sun disc is diffused out,
    // so we only occlude for low roughness.
    float RdotSun     = max(dot(R, normalize(-lighting.sun.direction.xyz)), 0.0);
    float specOcclusion = mix(1.0, visibility, smoothstep(0.5, 1.0, RdotSun) * (1.0 - roughness));
    outSpecular += prefilteredColor * (F * brdf.x + brdf.y) * ao * specOcclusion;
}

// ── Cascaded shadow map (16-tap Poisson PCF on a 2×2 atlas) ──────────────────

float shadowVisibility(vec3 worldPos, vec3 N, vec3 L, float NdotL) {
    float z = abs((ubo.view * vec4(worldPos, 1.0)).z);

    int cascade = 0;
    for (int i = 0; i < 3; i++)
        if (z > lighting.cascadeSplits[i]) cascade = i + 1;

    // Normal-offset bias scaled per cascade because distant cascades have
    // larger world-space texels.
    float cascadeScale = float(1 << cascade); // 1, 2, 4, 8
    vec3  biasedPos    = worldPos + N * (cascadeScale * mix(0.05, 0.15, 1.0 - NdotL));

    vec4  fragLS    = lighting.cascadeSpace[cascade] * vec4(biasedPos, 1.0);
    vec3  proj      = fragLS.xyz / fragLS.w;
    proj.xy         = proj.xy * 0.5 + 0.5;

    // Fragments outside the cascade frustum are considered fully lit
    if (proj.z > 1.0 || proj.z < 0.0 ||
        proj.x < 0.0 || proj.x > 1.0 ||
        proj.y < 0.0 || proj.y > 1.0)
        return 1.0;

    float bias = mix(0.001, 0.004, 1.0 - NdotL)
               * ((cascade == 0) ? 1.0 : (cascade == 1) ? 1.2 : (cascade == 2) ? 1.5 : 2.0);

    const vec2 atlasOffsets[4] = vec2[](
        vec2(0.0, 0.0), vec2(0.5, 0.0),
        vec2(0.0, 0.5), vec2(0.5, 0.5)
    );
    vec2 shadowUV = proj.xy * 0.5 + atlasOffsets[cascade];
    vec2 texelSize = 1.0 / vec2(textureSize(shadowMap, 0));
    float depth = proj.z - bias;

    // 16-tap Poisson disk — offsets chosen to minimise low-frequency banding
    const vec2 poisson[16] = vec2[](
        vec2(-0.94201624, -0.39906216), vec2( 0.94558609, -0.76890725),
        vec2(-0.09418410, -0.92938870), vec2( 0.34495938,  0.29387760),
        vec2(-0.91588581,  0.45771432), vec2(-0.81544232, -0.87912464),
        vec2(-0.38277543,  0.27676845), vec2( 0.97484398,  0.75648379),
        vec2( 0.44323325, -0.97511554), vec2( 0.53742981, -0.47373420),
        vec2(-0.26496911, -0.41893023), vec2( 0.79197514,  0.19090188),
        vec2(-0.24188840,  0.99706507), vec2(-0.81409955,  0.91437590),
        vec2( 0.19984126,  0.78641367), vec2( 0.14383161, -0.14100467)
    );

    float vis = 0.0;
    for (int i = 0; i < 16; i++)
        vis += texture(shadowMap, vec3(shadowUV + poisson[i] * texelSize * 1.5, depth));
    return vis * (1.0 / 16.0);
}

// ── ACES tonemap and its analytic inverse ─────────────────────────────────────

vec3 tonemapACES(vec3 x) {
    return (x * (2.51 * x + 0.03)) / (x * (2.51 * x + 0.59) + 0.06);
}

// Solves x = ACES(t) for t analytically.
// Rearranged quadratic: 2.51*(x-1)*t^2 + (0.59*x - 0.03)*t + 0.06*x = 0
// We always want the smaller (physically meaningful) root, so we use -sqrt.
vec3 inverseACES(vec3 x) {
    x       = clamp(x, 1e-4, 0.9999);
    vec3 a  = 2.51 * (x - 1.0);
    vec3 b  = 0.59 * x - 0.03;
    vec3 c  = 0.06 * x;
    vec3 disc = max(b * b - 4.0 * a * c, vec3(0.0));
    return (-b - sqrt(disc)) / (2.0 * a);
}

// ── Screen-space refraction ───────────────────────────────────────────────────

// Returns scene-linear HDR color seen through a refracting surface.
// viewPos, viewN, viewV are all in view space.
// roughness > 0 produces a cheap 5-tap cross blur for frosted glass.
vec3 getTransmittedColor(
    vec3 viewPos, vec3 viewN, vec3 viewV,
    float ior, float pathLength, float maxDistortion, float roughness)
{
    vec3 viewR = refract(-viewV, viewN, 1.0 / ior);
    // Total internal reflection — fall back to reflection direction
    if (dot(viewR, viewR) < 1e-3) viewR = reflect(-viewV, viewN);

    vec2 uvOffset;
    if (pathLength > 0.0) {
        // Clamp exit point so it never crosses the near plane (view-space z < -0.01)
        float safeLen = pathLength;
        if (viewPos.z + viewR.z * pathLength > -0.01) {
            safeLen = (viewR.z > 1e-4) ? (-0.01 - viewPos.z) / viewR.z : 0.0;
        }
        vec3 exitPos  = viewPos + viewR * safeLen;
        vec2 ndcEnter = (ubo.proj * vec4(viewPos,  1.0)).xy / (ubo.proj * vec4(viewPos,  1.0)).w;
        vec2 ndcExit  = (ubo.proj * vec4(exitPos,  1.0)).xy / (ubo.proj * vec4(exitPos,  1.0)).w;
        uvOffset      = (ndcExit - ndcEnter) * 0.5;
    } else {
        // No thickness data — use a simple normal-based screen-space nudge
        uvOffset = viewN.xy * (1.0 - 1.0 / ior) * 0.05;
    }

    if (length(uvOffset) > maxDistortion)
        uvOffset = normalize(uvOffset) * maxDistortion;

    vec2 screenUV = gl_FragCoord.xy / vec2(textureSize(opaqueScreenMap, 0));
    vec2 refrUV   = screenUV + uvOffset;

    if (refrUV.x >= 0.0 && refrUV.x <= 1.0 && refrUV.y >= 0.0 && refrUV.y <= 1.0) {
        vec3 sampled;
        if (roughness > 0.05) {
            // 5-tap cross blur — cheap approximation of a frosted glass BSDF
            vec2 r = (roughness * 10.0) / vec2(textureSize(opaqueScreenMap, 0));
            sampled  = textureLod(opaqueScreenMap, refrUV,                        0.0).rgb * 0.3333;
            sampled += textureLod(opaqueScreenMap, refrUV + vec2( r.x,  r.y), 0.0).rgb * 0.1667;
            sampled += textureLod(opaqueScreenMap, refrUV + vec2(-r.x,  r.y), 0.0).rgb * 0.1667;
            sampled += textureLod(opaqueScreenMap, refrUV + vec2( r.x, -r.y), 0.0).rgb * 0.1667;
            sampled += textureLod(opaqueScreenMap, refrUV + vec2(-r.x, -r.y), 0.0).rgb * 0.1667;
        } else {
            sampled = textureLod(opaqueScreenMap, refrUV, 0.0).rgb;
        }

        // opaqueScreenMap is SRGB — the hardware has already linearised on sample.
        // The stored value went through: scene_linear * EXPOSURE -> ACES -> gamma.
        // Invert to recover scene_linear so we can apply absorption in linear space.
        return max(inverseACES(sampled) / EXPOSURE, vec3(0.0));
    } else {
        // Off-screen: sample the prefiltered environment instead.
        // view transpose == view inverse for orthonormal matrices.
        vec3 worldR = transpose(mat3(ubo.view)) * viewR;
        worldR.y   *= -1.0;
        return textureLod(prefilterMap, worldR, roughness * IBL_MAX_LOD).rgb;
    }
}

// ── Main ──────────────────────────────────────────────────────────────────────

void main() {
    MeshData m = pc.meshData.meshes[inMeshIndex];

    // ── Albedo ────────────────────────────────────────────────────────────────
    vec4 albedoSample = (m.albedoIndex >= 0)
        ? texture(textures[nonuniformEXT(m.albedoIndex)], inTexCoord)
        : vec4(1.0);
    // Textures are loaded as UNORM; decode gamma manually for colour maps
    albedoSample.rgb = pow(albedoSample.rgb, vec3(2.2));
    vec4 baseColor   = albedoSample * m.baseColorFactor * inColor;

    if (m.alphaMode == 1 && baseColor.a < m.alphaCutoff) discard;

    // ── Normal ────────────────────────────────────────────────────────────────
    vec3 N;
    if (m.normalMapIndex >= 0) {
        vec3 n  = texture(textures[nonuniformEXT(m.normalMapIndex)], inTexCoord).rgb * 2.0 - 1.0;
        n.y     = -n.y; // OpenGL-convention normal maps need Y flipped for Vulkan
        N       = normalize(inTBN * n);
    } else {
        N = normalize(inTBN[2]);
    }

    // ── Metallic / Roughness ──────────────────────────────────────────────────
    float metallic  = m.metallicFactor;
    float roughness = m.roughnessFactor;
    if (m.metallicRoughIndex >= 0) {
        vec4 mr   = texture(textures[nonuniformEXT(m.metallicRoughIndex)], inTexCoord);
        metallic  *= mr.b; // glTF packs metallic in B, roughness in G
        roughness *= mr.g;
    }
    roughness = clamp(roughness, 0.04, 1.0);
    metallic  = clamp(metallic,  0.00, 1.0);

    // ── AO ────────────────────────────────────────────────────────────────────
    float ao = 1.0;
    if (m.aoIndex >= 0 && pc.ambientOcclusionEnabled != 0)
        ao = texture(textures[nonuniformEXT(m.aoIndex)], inTexCoord).r;

    // ── Unlit early-out ───────────────────────────────────────────────────────
    if (m.isUnlit != 0) {
        outColor = vec4(baseColor.rgb, baseColor.a);
        return;
    }

    // ── Lighting accumulation ─────────────────────────────────────────────────
    vec3 albedo = baseColor.rgb;
    vec3 V      = normalize(ubo.cameraPos.xyz - inWorldPos);
    vec3 F0     = mix(vec3(0.04), albedo, metallic);

    vec3 diffuseLight  = vec3(0.0);
    vec3 specularLight = vec3(0.0);

    vec3  sunL     = normalize(-lighting.sun.direction.xyz);
    float sunNdotL = max(dot(N, sunL), 0.0);
    float vis      = shadowVisibility(inWorldPos, N, sunL, sunNdotL);

    vec3 sunColor = lighting.sun.color.rgb * lighting.sun.color.w;
    lightContrib(sunL, sunColor, vis, N, V, albedo, metallic, roughness, F0,
                 diffuseLight, specularLight);

    for (int i = 0; i < lighting.pointLightCount; i++) {
        vec3  lpos   = lighting.pointLights[i].position.xyz;
        float radius = lighting.pointLights[i].position.w;
        vec3  lc     = lighting.pointLights[i].color.rgb * lighting.pointLights[i].color.w;
        vec3  Lv     = lpos - inWorldPos;
        float dist   = length(Lv);
        Lv /= dist;

        float atten   = 1.0 / (dist * dist + 1e-4);
        float falloff = clamp(1.0 - pow(dist / max(radius, 1e-3), 4.0), 0.0, 1.0);
        atten        *= falloff * falloff;

        lightContrib(Lv, lc, atten, N, V, albedo, metallic, roughness, F0,
                     diffuseLight, specularLight);
    }

    bool useIBL = (lighting.iblEnabled != 0 && pc.iblEnabled != 0);
    if (useIBL) {
        iblAmbient(N, V, albedo, metallic, roughness, F0, ao, vis,
                   diffuseLight, specularLight);
        diffuseLight  *= lighting.ambientIntensity;
        specularLight *= lighting.ambientIntensity;
    } else {
        diffuseLight += lighting.ambientIntensity * albedo * ao * mix(0.5, 1.0, vis);
    }

    // ── Emissive ──────────────────────────────────────────────────────────────
    vec3 emissive = m.emissiveFactor * m.emissiveStrength;
    if (m.emissiveIndex >= 0)
        emissive *= pow(texture(textures[nonuniformEXT(m.emissiveIndex)], inTexCoord).rgb, vec3(2.2));

    // ── Transmission (KHR_materials_transmission + volume + dispersion) ───────
    float transmission = m.transmissionFactor;
    if (m.transmissionIndex >= 0)
        transmission *= texture(textures[nonuniformEXT(m.transmissionIndex)], inTexCoord).r;

    vec3 localLinear;

    if (transmission > 0.0 && useIBL) {
        // Extract uniform scale from the model matrix to convert object-space
        // thickness to world-space path length
        float scaleX = length(m.model[0].xyz);
        float scaleY = length(m.model[1].xyz);
        float scaleZ = length(m.model[2].xyz);
        float worldScale = (scaleX + scaleY + scaleZ) * (1.0 / 3.0);

        float thickness = m.thicknessFactor;
        if (m.thicknessIndex >= 0)
            thickness *= texture(textures[nonuniformEXT(m.thicknessIndex)], inTexCoord).g;
        float pathLength = thickness * worldScale; // 0 if no thickness data

        vec4  viewPos4 = ubo.view * vec4(inWorldPos, 1.0);
        vec3  viewPos  = viewPos4.xyz;
        vec3  viewN    = normalize(mat3(ubo.view) * N);
        vec3  viewV    = normalize(-viewPos);
        float ior      = max(m.ior, 1.001);

        vec3 transmittedLight;
        if (m.dispersion > 0.0) {
            // glTF spec: dispersion is the IOR difference between the red and blue
            // wavelengths, split symmetrically around the base IOR
            float half_d = m.dispersion * 0.5;
            transmittedLight = vec3(
                getTransmittedColor(viewPos, viewN, viewV, max(1.001, ior - half_d), pathLength, 0.2, roughness).r,
                getTransmittedColor(viewPos, viewN, viewV, ior,                       pathLength, 0.2, roughness).g,
                getTransmittedColor(viewPos, viewN, viewV, max(1.001, ior + half_d), pathLength, 0.2, roughness).b
            );
        } else {
            transmittedLight = getTransmittedColor(viewPos, viewN, viewV, ior, pathLength, 0.2, roughness);
        }

        // Fresnel edge reflection — grazing angles should reflect the environment
        // rather than showing a pure refraction
        float R0      = pow((1.0 - ior) / (1.0 + ior), 2.0);
        float F_glass = R0 + (1.0 - R0) * pow(clamp(1.0 - dot(viewN, viewV), 0.0, 1.0), 5.0);
        vec3  worldRefl = vec3(reflect(-V, N).x, -reflect(-V, N).y, reflect(-V, N).z);
        transmittedLight = mix(transmittedLight,
                               textureLod(prefilterMap, worldRefl, roughness * IBL_MAX_LOD).rgb,
                               F_glass);

        // Beer-Lambert volume absorption (KHR_materials_volume)
        vec3 absorption = vec3(1.0);
        if (pathLength > 0.0 && m.attenuationDistance > 0.0 && m.attenuationDistance < 99999.0) {
            vec3 attColor = clamp(
                vec3(m.attenuationColorR, m.attenuationColorG, m.attenuationColorB),
                vec3(1e-3), vec3(0.999));
            vec3  sigma_a    = -log(attColor) / max(m.attenuationDistance, 1e-4);
            float clampedPath = min(pathLength, m.attenuationDistance * 4.0);
            absorption        = exp(-sigma_a * clampedPath);
        }

        vec3 transmissionColor = transmittedLight * albedo * absorption;

        // Mix diffuse and transmission in linear space; specular and emissive always add on top
        localLinear = mix(diffuseLight, transmissionColor, transmission * (1.0 - metallic))
                    + specularLight + emissive;
    } else {
        localLinear = diffuseLight + specularLight + emissive;
    }

    // ── Tonemap ───────────────────────────────────────────────────────────────
    vec3 color = clamp(tonemapACES(localLinear * EXPOSURE), 0.0, 1.0);

    // ── Alpha ─────────────────────────────────────────────────────────────────
    // OPAQUE and MASK modes: alpha is always 1. BLEND keeps baseColor.a.
    // Transmission overwrites the background via refraction, so also force alpha=1.
    float finalAlpha = (m.alphaMode == 0 || m.alphaMode == 1 || transmission > 0.0)
                     ? 1.0
                     : baseColor.a;

    outColor = vec4(color, finalAlpha);
}

#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

const float PI          = 3.14159265358979;
const float INV_PI      = 1.0 / PI;
const float IBL_MAX_LOD = 4.0;
const float EXPOSURE    = 0.6;

struct SdfPrimitive {
    mat4  inverseTransform;
    vec4  size;
    vec4  color;
    vec3  emissive;
    float metallic;
    float roughness;
    float emissiveStrength;
    float smoothness;
    int   type;
    int   operation;
    int   _pad1;
    int   _pad2;
    int   _pad3;
};

// Zero-latency physical pointer to our math structures
layout(buffer_reference, scalar) readonly buffer SdfBuffer {
    SdfPrimitive primitives[];
};

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer DummyBuffer { uint dummy[]; };

layout(push_constant) uniform PC {
    int          ambientOcclusionEnabled;
    int          iblEnabled;
    int          meshIndex;
    int          cascadeIndex;
    DummyBuffer  meshBufferAddr;
    DummyBuffer  vertexBufferAddr;
    DummyBuffer  jointBufferAddr;
    DummyBuffer  morphBufferAddr;
    DummyBuffer  morphWeightAddr;
    DummyBuffer  meshletBufferAddr;
    DummyBuffer  meshletBoundsAddr;
    DummyBuffer  meshletSkinAddr;
    DummyBuffer  dynamicBoundsAddr;
    DummyBuffer  meshletVertexAddr;
    DummyBuffer  meshletTriangleAddr;
    SdfBuffer    sdfBufferAddr;
    int          sdfCount;
    int          _pad1;
} pc;

layout(set = 0, binding = 0) uniform UniformBufferObject {
    mat4 vp;
    mat4 view;
    mat4 proj;
    vec4 cameraPos;
    float time;
} ubo;

layout(set = 1, binding = 0) uniform sampler2D textures[];

struct PointLight { vec4 position; vec4 color; };
struct DirLight { vec4 direction; vec4 color; };
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
    mat4       cullSpace;
    vec4       cullCameraPos;
    int        freezeCulling;
    int        _pad2[3];
} lighting;

layout(set = 3, binding = 1) uniform samplerCube irradianceMap;
layout(set = 3, binding = 2) uniform samplerCube prefilterMap;
layout(set = 3, binding = 3) uniform sampler2D   brdfLUT;
layout(set = 3, binding = 4) uniform samplerCube skyboxMap;
layout(set = 3, binding = 5) uniform sampler2DShadow shadowMap;

// ── PBR Lighting Functions ──────────────────────────────────────────────────

float D_GGX(float NdotH, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float d  = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

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

vec3 F_SchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

void lightContrib(
    vec3 L, vec3 lightColor, float attenuation,
    vec3 N, vec3 V, vec3 albedo, float metallic, float roughness, vec3 F0,
    inout vec3 outDiffuse, inout vec3 outSpecular)
{
    vec3  H_dir = V + L;
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

void iblAmbient(
    vec3 N, vec3 V, vec3 albedo, float metallic, float roughness,
    vec3 F0, float ao, float visibility,
    inout vec3 outDiffuse, inout vec3 outSpecular)
{
    vec3 F  = F_SchlickRoughness(max(dot(N, V), 0.0), F0, roughness);
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);

    vec3 sampleN = vec3(N.x, -N.y, N.z);
    outDiffuse  += kD * texture(irradianceMap, sampleN).rgb * albedo * ao;

    vec3 R       = reflect(-V, N);
    vec3 sampleR = vec3(R.x, -R.y, R.z);
    vec3 prefilteredColor = textureLod(prefilterMap, sampleR, roughness * IBL_MAX_LOD).rgb;
    vec2 brdf         = texture(brdfLUT, vec2(max(dot(N, V), 0.0), roughness)).rg;

    float RdotSun     = max(dot(R, normalize(-lighting.sun.direction.xyz)), 0.0);
    float specOcclusion = mix(1.0, visibility, smoothstep(0.5, 1.0, RdotSun) * (1.0 - roughness));
    outSpecular += prefilteredColor * (F * brdf.x + brdf.y) * ao * specOcclusion;
}

float shadowVisibility(vec3 worldPos, vec3 N, vec3 L, float NdotL) {
    float z = abs((ubo.view * vec4(worldPos, 1.0)).z);
    int cascade = 0;
    for (int i = 0; i < 3; i++)
        if (z > lighting.cascadeSplits[i]) cascade = i + 1;

    float cascadeScale = float(1 << cascade);
    vec3  biasedPos    = worldPos + N * (cascadeScale * mix(0.05, 0.15, 1.0 - NdotL));

    vec4  fragLS    = lighting.cascadeSpace[cascade] * vec4(biasedPos, 1.0);
    vec3  proj      = fragLS.xyz / fragLS.w;
    proj.xy         = proj.xy * 0.5 + 0.5;

    if (proj.z > 1.0 || proj.z < 0.0 || proj.x < 0.0 || proj.x > 1.0 || proj.y < 0.0 || proj.y > 1.0)
        return 1.0;

    float bias = mix(0.001, 0.004, 1.0 - NdotL) * ((cascade == 0) ? 1.0 : (cascade == 1) ? 1.2 : (cascade == 2) ? 1.5 : 2.0);

    const vec2 atlasOffsets[4] = vec2[](vec2(0.0, 0.0), vec2(0.5, 0.0), vec2(0.0, 0.5), vec2(0.5, 0.5));
    vec2 shadowUV = proj.xy * 0.5 + atlasOffsets[cascade];
    vec2 texelSize = 1.0 / vec2(textureSize(shadowMap, 0));
    float depth = proj.z - bias;

    const vec2 poisson[16] = vec2[](
        vec2(-0.94201624, -0.39906216), vec2( 0.94558609, -0.76890725), vec2(-0.09418410, -0.92938870), vec2( 0.34495938,  0.29387760),
        vec2(-0.91588581,  0.45771432), vec2(-0.81544232, -0.87912464), vec2(-0.38277543,  0.27676845), vec2( 0.97484398,  0.75648379),
        vec2( 0.44323325, -0.97511554), vec2( 0.53742981, -0.47373420), vec2(-0.26496911, -0.41893023), vec2( 0.79197514,  0.19090188),
        vec2(-0.24188840,  0.99706507), vec2(-0.81409955,  0.91437590), vec2( 0.19984126,  0.78641367), vec2( 0.14383161, -0.14100467)
    );

    float vis = 0.0;
    for (int i = 0; i < 16; i++) vis += texture(shadowMap, vec3(shadowUV + poisson[i] * texelSize * 1.5, depth));
    return vis * (1.0 / 16.0);
}

vec3 tonemapACES(vec3 x) {
    return (x * (2.51 * x + 0.03)) / (x * (2.51 * x + 0.59) + 0.06);
}

// ── SDF Evaluator ───────────────────────────────────────────────────────────
float sdSphere(vec3 p, float s) { return length(p) - s; }
float sdBox(vec3 p, vec3 b) {
    vec3 q = abs(p) - b;
    return length(max(q,0.0)) + min(max(q.x,max(q.y,q.z)),0.0);
}

// Inigo Quilez Polynomial Smooth Min - The magic behind the "clay" blending
vec2 smin(float d1, float d2, float k, float color1, float color2) {
    float h = clamp(0.5 + 0.5*(d2 - d1)/k, 0.0, 1.0);
    float d = mix(d2, d1, h) - k*h*(1.0-h);
    float c = mix(color2, color1, h); // Smoothly interpolate material/color
    return vec2(d, c);
}

// Evaluate the entire scene distance field!
vec2 map(vec3 pos) {
    if (pc.sdfCount == 0) return vec2(9999.0, -1.0);
    SdfBuffer sdfs = SdfBuffer(pc.sdfBufferAddr);

    float res_d = 9999.0;
    float res_mat = -1.0;

    for (int i = 0; i < pc.sdfCount; i++) {
        SdfPrimitive prim = sdfs.primitives[i];

        // Put the ray position into the local space of the shape
        vec3 localPos = (prim.inverseTransform * vec4(pos, 1.0)).xyz;

        float d = 9999.0;
        if (prim.type == 0) d = sdSphere(localPos, prim.size.x);
        else if (prim.type == 1) d = sdBox(localPos, prim.size.xyz);

        if (i == 0) {
            res_d = d;
            res_mat = float(i);
        } else {
            if (prim.operation == 1) { // Smooth Union
                vec2 blend = smin(res_d, d, prim.smoothness, res_mat, float(i));
                res_d = blend.x;
                res_mat = blend.y;
            } else { // Union
                if (d < res_d) {
                    res_d = d;
                    res_mat = float(i);
                }
            }
        }
    }
    return vec2(res_d, res_mat);
}

// Calculate normal by sampling the distance field around the hit point
vec3 calcNormal(vec3 pos) {
    vec2 e = vec2(1.0,-1.0)*0.5773*0.0005;
    return normalize( e.xyy*map( pos + e.xyy ).x +
                      e.yyx*map( pos + e.yyx ).x +
                      e.yxy*map( pos + e.yxy ).x +
                      e.xxx*map( pos + e.xxx ).x );
}

void main() {
    if (pc.sdfCount == 0) discard;

    // ── Perfect Ray Generation ──
    // 1. Get NDC coordinates from the fullscreen triangle
    vec2 ndc = inUV * 2.0 - 1.0;

    // 2. Un-project to View Space
    vec4 viewTarget = inverse(ubo.proj) * vec4(ndc.x, ndc.y, 1.0, 1.0);
    vec3 viewDir = normalize(viewTarget.xyz / viewTarget.w);

    // 3. Transform View Direction to World Direction
    vec3 rayDir = normalize((inverse(ubo.view) * vec4(viewDir, 0.0)).xyz);
    vec3 rayOri = ubo.cameraPos.xyz;

    float t = 0.0;
    float max_dist = 10000.0;
    vec2 res = vec2(9999.0, -1.0);

    // ── Raymarch Loop ──
    for(int i = 0; i < 256; i++) {
        vec3 p = rayOri + rayDir * t;
        res = map(p);
        // Break immediately if we are inside or touching the surface
        if (res.x < 0.001) break;
        t += res.x;
        if (t > max_dist) break;
    }

    if (t > max_dist) discard;

    // ── Integration & Depth ──
    vec3 hitPos = rayOri + rayDir * t;
    vec4 clipPos = ubo.proj * ubo.view * vec4(hitPos, 1.0);
    gl_FragDepth = clipPos.z / clipPos.w; // Absolute flawless integration with standard 3D!

    vec3 N = calcNormal(hitPos);
    vec3 V = normalize(ubo.cameraPos.xyz - hitPos);

    // ── Materials ──
    SdfBuffer sdfs = SdfBuffer(pc.sdfBufferAddr);
    int mat_idx = int(round(res.y));
    SdfPrimitive prim = sdfs.primitives[mat_idx];

    // Decode SRGB color
    vec3 albedo = pow(prim.color.rgb, vec3(2.2));
    float metallic = clamp(prim.metallic, 0.0, 1.0);
    float roughness = clamp(prim.roughness, 0.04, 1.0);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    float ao = 1.0; // Dynamic raymarched AO can be added later

    // ── Lighting Accumulation ──
    vec3 diffuseLight  = vec3(0.0);
    vec3 specularLight = vec3(0.0);

    vec3  sunL     = normalize(-lighting.sun.direction.xyz);
    float sunNdotL = max(dot(N, sunL), 0.0);
    float vis      = shadowVisibility(hitPos, N, sunL, sunNdotL);

    vec3 sunColor = lighting.sun.color.rgb * lighting.sun.color.w;
    lightContrib(sunL, sunColor, vis, N, V, albedo, metallic, roughness, F0, diffuseLight, specularLight);

    for (int i = 0; i < lighting.pointLightCount; i++) {
        vec3  lpos   = lighting.pointLights[i].position.xyz;
        float radius = lighting.pointLights[i].position.w;
        vec3  lc     = lighting.pointLights[i].color.rgb * lighting.pointLights[i].color.w;
        vec3  Lv     = lpos - hitPos;
        float dist   = length(Lv);
        Lv /= dist;

        float atten   = 1.0 / (dist * dist + 1e-4);
        float falloff = clamp(1.0 - pow(dist / max(radius, 1e-3), 4.0), 0.0, 1.0);
        atten        *= falloff * falloff;

        lightContrib(Lv, lc, atten, N, V, albedo, metallic, roughness, F0, diffuseLight, specularLight);
    }

    if (lighting.iblEnabled != 0 && pc.iblEnabled != 0) {
        iblAmbient(N, V, albedo, metallic, roughness, F0, ao, vis, diffuseLight, specularLight);
        diffuseLight  *= lighting.ambientIntensity;
        specularLight *= lighting.ambientIntensity;
    } else {
        diffuseLight += lighting.ambientIntensity * albedo * ao * mix(0.5, 1.0, vis);
    }

    vec3 emissive = prim.emissive * prim.emissiveStrength;
    vec3 localLinear = diffuseLight + specularLight + emissive;
    vec3 finalColor = clamp(tonemapACES(localLinear * EXPOSURE), 0.0, 1.0);

    outColor = vec4(finalColor, 1.0);
}

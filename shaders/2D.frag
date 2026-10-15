#version 450
/// Shader declarations
layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) flat in int fragTextureIndex;
layout(location = 3) in vec2 fragSize;
layout(location = 4) in vec4 fragCornerRadius;
layout(location = 5) in float fragBorderThickness;
layout(location = 6) in vec4 fragBorderColor;
layout(location = 7) flat in uvec4 fragFaceData0;
layout(location = 8) flat in uvec4 fragFaceData1;
layout(location = 9) flat in vec4 fragFaceParams;
layout(location = 10) flat in uvec4 fragFaceEventData;
layout(location = 11) flat in vec4 fragFaceEvent;
layout(location = 12) in vec2 fragPosition;
layout(location = 13) flat in vec4 fragClipRect;

layout(push_constant) uniform PushConstants2D {
    mat4 projection;
    vec4 frame;
    vec4 pointer;
} pc;

layout(location = 0) out vec4 outColor;

/* Face shader ABI
 *
 * Every glyph carries these values (ordinary quads carry zeroes):
 *   fragFaceData0 = uint4(shader_id, buffer_position, line, column)
 *   fragFaceData1 = uint4(face_run_offset, codepoint, flags, event_kind)
 *   fragFaceEventData = uint4(event_offset, event_count, serial, anchor)
 *   fragFaceParams = four values from the face's :shader-params attribute
 *   fragFaceEvent.x = edit time in seconds, .y = effect duration
 *   pc.frame.x = monotonic time in seconds
 *
 * The buffer position is recomputed from the live buffer on every draw.  It
 * is therefore deliberately a logical position, not a pixel coordinate or a
 * persistent GPU index: insertions and deletions naturally retime effects.
 * Add dispatch cases below (or replace this block) when writing a custom
 * face shader. Shader id 1 is prism_flow, id 2 is warning_bloom, and id 3
 * is ember_dissolve, id 4 is soft_bloom, and id 5 is glass_proximity.
 * The pointer push constant uses the renderer's screen-space coordinates;
 * a negative x disables pointer-proximity effects.
 */

layout(set = 0, binding = 0) uniform sampler2D textures[256];

// Extremely fast hardware-level HSV to RGB conversion
vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

float hash12(vec2 p) {
    p = fract(p * vec2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return fract(p.x * p.y);
}

float event_progress() {
    return clamp(max(pc.frame.x - fragFaceEvent.x, 0.0) /
                 max(fragFaceEvent.y, 0.001), 0.0, 1.0);
}

float glyph_halo() {
    if (fragTextureIndex < 0) return 0.0;
    vec2 texel = 1.0 / vec2(textureSize(textures[fragTextureIndex], 0));
    float center = texture(textures[fragTextureIndex], fragTexCoord).a;
    float halo = 0.0;
    for (int yy = -2; yy <= 2; yy++) {
        for (int xx = -2; xx <= 2; xx++) {
            if (xx == 0 && yy == 0) continue;
            vec2 uv = clamp(fragTexCoord + vec2(xx, yy) * texel * 2.0,
                            vec2(0.0), vec2(1.0));
            halo = max(halo, texture(textures[fragTextureIndex], uv).a);
        }
    }
    return max(halo - center, 0.0);
}

/// Shader entry
void main() {
    if (fragClipRect.z > 0.0 && fragClipRect.w > 0.0 &&
        (fragPosition.x < fragClipRect.x ||
         fragPosition.x > fragClipRect.x + fragClipRect.z ||
         fragPosition.y < fragClipRect.y ||
         fragPosition.y > fragClipRect.y + fragClipRect.w))
        discard;

    if (fragTextureIndex >= 0) {
        vec4 texColor = texture(textures[fragTextureIndex], fragTexCoord);
        outColor = texColor * fragColor;
    } else if (fragTextureIndex == -2) {
        vec2 halfSize = fragSize * 0.5;
        vec2 pixelPos = (fragTexCoord - 0.5) * fragSize;
        vec4 r = vec4(fragCornerRadius.y, fragCornerRadius.z, fragCornerRadius.x, fragCornerRadius.w);

        r.xy = (pixelPos.x > 0.0) ? r.xy : r.zw;
        r.x  = (pixelPos.y > 0.0) ? r.x  : r.y;

        vec2 d = abs(pixelPos) - halfSize + r.x;
        float outerDist = min(max(d.x, d.y), 0.0) + length(max(d, 0.0)) - r.x;

        float fw = fwidth(outerDist);
        float outerAlpha = 1.0 - smoothstep(-fw, fw, outerDist);

        if (outerAlpha <= 0.0) discard;

        float innerDist = outerDist + fragBorderThickness;
        float innerAlpha = 1.0 - smoothstep(-fw, fw, innerDist);

        vec4 mixedColor = mix(fragBorderColor, fragColor, innerAlpha);
        outColor = vec4(mixedColor.rgb, mixedColor.a * outerAlpha);
    } else if (fragTextureIndex == -3) {
        // --- 1. PERFECT SDF HUE RING ---
        vec2 p = fragTexCoord * 2.0 - 1.0;
        float d = length(p);
        float innerFrac = fragCornerRadius.x;
        float outerFrac = fragCornerRadius.y;
        float alpha = fragCornerRadius.w;

        float fw = fwidth(d);
        float mask = smoothstep(outerFrac + fw, outerFrac - fw, d) * smoothstep(innerFrac - fw, innerFrac + fw, d);
        if (mask <= 0.0) discard;

        float angle = atan(p.y, p.x);
        float h = angle / 6.28318530718;
        if (h < 0.0) h += 1.0;

        outColor = vec4(hsv2rgb(vec3(h, 1.0, 1.0)), alpha * mask);
    } else if (fragTextureIndex == -4) {
        // --- 2. BARYCENTRIC SV TRIANGLE ---
        vec2 p = fragTexCoord * 2.0 - 1.0;
        float hue = fragCornerRadius.x;
        float radiusFrac = fragCornerRadius.y;
        float alpha = fragCornerRadius.w;

        float baseAngle = hue * (3.14159265359 / 180.0);
        float a0 = baseAngle;
        float a1 = baseAngle + 2.09439510239; // +120 deg
        float a2 = baseAngle - 2.09439510239; // -120 deg

        vec2 v0 = vec2(cos(a0), sin(a0)) * radiusFrac;
        vec2 v1 = vec2(cos(a1), sin(a1)) * radiusFrac;
        vec2 v2 = vec2(cos(a2), sin(a2)) * radiusFrac;

        // Exact barycentric coordinate calculation
        float denom = (v1.y - v2.y)*(v0.x - v2.x) + (v2.x - v1.x)*(v0.y - v2.y);
        float u = ((v1.y - v2.y)*(p.x - v2.x) + (v2.x - v1.x)*(p.y - v2.y)) / denom;
        float v = ((v2.y - v0.y)*(p.x - v2.x) + (v0.x - v2.x)*(p.y - v2.y)) / denom;
        float w = 1.0 - u - v;

        // Anti-aliasing mask using the closest edge distance
        float minBary = min(min(u, v), w);
        float fw = max(fwidth(minBary), 0.001); // Prevent zero-fwidth artifacts
        float mask = smoothstep(-fw, fw, minBary);
        if (mask <= 0.0) discard;

        // Convert the barycentric weights into pure Saturation and Value
        float val = clamp(u + w, 0.0, 1.0);
        float sat = (val > 1e-6) ? clamp(u / val, 0.0, 1.0) : 0.0;

        outColor = vec4(hsv2rgb(vec3(hue / 360.0, sat, val)), alpha * mask);
    } else {
        outColor = fragColor;
    }

    bool face_foreground = (fragFaceData1.z & 0x2u) != 0u;
    bool face_background = (fragFaceData1.z & 0x4u) != 0u;
    bool shader_target_matches = (fragTextureIndex >= 0 && face_foreground) ||
                                 (fragTextureIndex == -1 && face_background);
    float event_age = max(pc.frame.x - fragFaceEvent.x, 0.0);

    if (shader_target_matches && fragFaceData0.x == 1u) {
        // prism_flow: iridescent spectral bands with a soft glyph rim.
        float speed = abs(fragFaceParams.x) > 0.001 ? fragFaceParams.x : 1.0;
        float spacing = abs(fragFaceParams.y) > 0.001 ? fragFaceParams.y : 0.12;
        float amount = abs(fragFaceParams.z) > 0.001 ? abs(fragFaceParams.z) : 0.72;
        float phase = pc.frame.x * speed + float(fragFaceData0.y) * spacing;
        float hue = fract(0.56 + 0.18 * sin(phase * 0.7 + float(fragFaceData0.z) * 0.08));
        vec3 prism = hsv2rgb(vec3(hue, 0.78, 1.0));
        float shimmer = 0.5 + 0.5 * sin(phase + float(fragFaceData1.x) * 0.11);
        float halo = glyph_halo();
        outColor.rgb = mix(outColor.rgb, prism, clamp(amount * (0.45 + 0.55 * shimmer), 0.0, 1.0));
        if (fragTextureIndex >= 0)
            outColor = vec4(mix(outColor.rgb, prism, halo * 0.6),
                            max(outColor.a, halo * 0.45));
    }

    if (shader_target_matches && fragFaceData0.x == 2u) {
        // warning_bloom: warning-coloured local bloom and a bright insertion
        // pulse. Params xyz are the warning colour; w controls intensity.
        vec3 warning = length(fragFaceParams.xyz) > 0.01
            ? clamp(fragFaceParams.xyz, 0.0, 1.0)
            : vec3(1.0, 0.58, 0.08);
        float intensity = abs(fragFaceParams.w) > 0.01 ? abs(fragFaceParams.w) : 0.9;
        float pulse = (fragFaceData1.w == 1u)
            ? 1.0 - smoothstep(0.0, fragFaceEvent.y, event_age) : 0.0;
        float halo = glyph_halo();
        float strength = clamp((0.58 + 0.20 * pulse) * intensity, 0.0, 1.0);
        outColor.rgb = mix(outColor.rgb, warning, strength);
        if (fragTextureIndex >= 0)
            outColor = vec4(mix(outColor.rgb, warning, halo * (0.9 + pulse)),
                            max(outColor.a, halo * (0.42 + 0.35 * pulse)));
        else {
            vec2 p = fragTexCoord - 0.5;
            float radial = 1.0 - smoothstep(0.05, 0.72, length(p));
            outColor.rgb = mix(outColor.rgb, warning, radial * 0.18 * (1.0 + pulse));
        }
    }

    if (shader_target_matches && fragFaceData0.x == 3u) {
        // ember_dissolve: deleted glyphs burn from a noisy edge into ember
        // sparks; the same face remains a warm red preview when idle.
        vec3 ember = length(fragFaceParams.xyz) > 0.01
            ? clamp(fragFaceParams.xyz, 0.0, 1.0)
            : vec3(0.95, 0.12, 0.03);
        float warm = 0.5 + 0.5 * sin(pc.frame.x * 3.0 + float(fragFaceData0.y) * 0.08);
        vec3 ember_color = mix(ember, vec3(1.0, 0.62, 0.04), warm * 0.55);
        float fade = 1.0;
        if (fragFaceData1.w == 2u) {
            float progress = event_progress();
            fade = 1.0 - smoothstep(0.0, 1.0, progress);
            if (fragTextureIndex >= 0) {
                vec2 cell = floor(fragTexCoord * vec2(42.0, 24.0)) +
                            vec2(float(fragFaceEventData.x), float(fragFaceEventData.z));
                float grain = hash12(cell);
                float keep = 1.0 - smoothstep(progress * 1.18 - 0.08,
                                               progress * 1.18 + 0.08, grain);
                float spark = smoothstep(0.72, 1.0, grain) * (1.0 - progress);
                outColor.a *= fade * keep;
                outColor.rgb = mix(outColor.rgb, ember_color, 0.78 + 0.2 * spark);
                outColor.rgb += ember_color * spark * 0.9;
            } else {
                vec2 p = fragTexCoord - 0.5;
                float plume = 1.0 - smoothstep(0.08, 0.76, length(p));
                outColor.rgb = mix(outColor.rgb, ember_color, plume * 0.8);
                outColor.a *= fade * plume;
            }
        } else {
            outColor.rgb = mix(outColor.rgb, ember_color, 0.76 + 0.1 * warm);
        }
    }

    if (shader_target_matches && fragFaceData0.x == 4u) {
        // Glyph-local bloom: the glyph atlas halo remains bounded to this
        // glyph quad; this is not a full-frame post-processing bloom pass.
        vec3 bloom = length(fragFaceParams.xyz) > 0.01
            ? clamp(fragFaceParams.xyz, 0.0, 1.0)
            : vec3(0.48, 0.72, 1.0);
        float strength = abs(fragFaceParams.w) > 0.01
            ? clamp(abs(fragFaceParams.w), 0.0, 1.0) : 0.75;
        float halo = glyph_halo();
        outColor.rgb = mix(outColor.rgb, bloom, strength * 0.28);
        if (fragTextureIndex >= 0) {
            outColor.rgb += bloom * halo * strength * 0.8;
            outColor.a = max(outColor.a, halo * strength * 0.55);
        }
    }

    if (shader_target_matches && fragFaceData0.x == 5u &&
        pc.pointer.x >= 0.0) {
        // Glass-inspired pointer lens: a bounded red tint, narrow specular
        // edge and a small texture shift as the pointer nears the glyph.
        float radius = abs(fragFaceParams.x) > 1.0
            ? clamp(abs(fragFaceParams.x), 8.0, 240.0) : 72.0;
        float strength = abs(fragFaceParams.y) > 0.01
            ? clamp(abs(fragFaceParams.y), 0.0, 1.0) : 0.82;
        float distance_to_pointer = distance(fragPosition, pc.pointer.xy);
        float proximity = 1.0 - smoothstep(0.0, radius, distance_to_pointer);
        if (proximity > 0.001) {
            vec3 glass_red = vec3(1.0, 0.12, 0.18);
            float lens = proximity * strength;
            outColor.rgb = mix(outColor.rgb, glass_red, lens * 0.62);
            float highlight = exp(-abs(distance_to_pointer - radius * 0.42) /
                                  max(radius * 0.07, 1.0));
            outColor.rgb = mix(outColor.rgb, vec3(1.0, 0.82, 0.84),
                               highlight * lens * 0.42);
            if (fragTextureIndex >= 0) {
                vec2 toward_pointer = (pc.pointer.xy - fragPosition) /
                                      max(distance_to_pointer, 0.001);
                vec2 offset = toward_pointer * (lens * 0.006);
                float refracted = texture(textures[fragTextureIndex],
                                          clamp(fragTexCoord + offset,
                                                vec2(0.0), vec2(1.0))).a;
                outColor.a = max(outColor.a, refracted * lens * 0.2);
            }
        }
    }

}

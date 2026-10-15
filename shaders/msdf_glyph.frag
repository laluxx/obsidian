#version 460
/// MSDF glyph sampling (standalone reference pipeline)
/* Texture must be VK_FORMAT_R8G8B8A8_UNORM, not SRGB; the RGB channels encode
 * distances, not colors. Alpha is unused. The engine's existing 2D shader
 * and bitmap glyph atlas are NOT replaced without a dedicated texture kind,
 * atlas layout, sampler setup, and pipeline contract. */
layout(location=0) in vec2 glyphUV;
layout(location=1) in vec4 glyphColor;
layout(location=0) out vec4 outColor;
layout(set=0,binding=0) uniform sampler2D glyphAtlas;
layout(push_constant) uniform GlyphParams { float pxRange; } params;

/// Median decode and anti-aliasing
float median3(vec3 v) {
    return max(min(v.r, v.g), min(max(v.r, v.g), v.b));
}
/// Shader entry
void main() {
    vec3 msd = texture(glyphAtlas, glyphUV).rgb;
    float signedDistance = median3(msd)-0.5;
    vec2 unitRange = vec2(params.pxRange)/vec2(textureSize(glyphAtlas,0));
    vec2 texelFootprint = max(fwidth(glyphUV), vec2(1.0e-6));
    float screenPxRange = max(0.5*dot(unitRange,1.0/texelFootprint),1.0);
    float alpha = clamp(signedDistance*screenPxRange+0.5,0.0,1.0);
    outColor = vec4(glyphColor.rgb,glyphColor.a*alpha);
}

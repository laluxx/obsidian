#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec3 inNormal;

layout(binding = 0) uniform UniformBufferObject {
    mat4 vp;
} ubo;

layout(push_constant) uniform PushConstants {
    mat4 model;
    int ambientOcclusionEnabled;
} pc;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec3 fragWorldPos;
layout(location = 3) out flat int fragAmbientOcclusionEnabled; // New output

void main() {
    vec4 worldPos = pc.model * vec4(inPosition, 1.0);
    gl_Position = ubo.vp * worldPos;
    
    // Transform normal to world space
    mat3 normalMatrix = mat3(transpose(inverse(pc.model)));
    fragNormal = normalize(normalMatrix * inNormal);
    
    fragWorldPos = worldPos.xyz;
    fragColor = inColor;
    fragAmbientOcclusionEnabled = pc.ambientOcclusionEnabled; // Pass to fragment shader
}




/* #version 450 */

/* layout(location = 0) in vec3 inPosition; */
/* layout(location = 1) in vec4 inColor; */
/* layout(location = 2) in vec3 inNormal; */

/* layout(binding = 0) uniform UniformBufferObject { */
/*     mat4 vp; */
/* } ubo; */

/* layout(push_constant) uniform PushConstants { */
/*     mat4 model; */
/* } pc; */

/* layout(location = 0) out vec4 fragColor; */
/* layout(location = 1) out vec3 fragNormal; */
/* layout(location = 2) out vec3 fragWorldPos; */

/* void main() { */
/*     vec4 worldPos = pc.model * vec4(inPosition, 1.0); */
/*     gl_Position = ubo.vp * worldPos; */
    
/*     // Transform normal to world space */
/*     mat3 normalMatrix = mat3(transpose(inverse(pc.model))); */
/*     fragNormal = normalize(normalMatrix * inNormal); */
    
/*     fragWorldPos = worldPos.xyz; */
/*     fragColor = inColor; */
/* } */


// RANDOM COLORS
/* #version 450 */
/* layout(location = 0) in vec3 inPosition; */
/* layout(location = 1) in vec4 inColor; */

/* // Camera/view-projection */
/* layout(binding = 0) uniform UniformBufferObject { */
/*     mat4 vp; */
/* } ubo; */

/* // Per-mesh model matrix */
/* layout(push_constant) uniform PushConstants { */
/*     mat4 model; */
/* } pc; */

/* layout(location = 0) out vec4 fragColor; */

/* // Hash function for pseudo-random numbers */
/* float hash(float n) { */
/*     return fract(sin(n) * 43758.5453); */
/* } */

/* void main() { */
/*     gl_Position = ubo.vp * pc.model * vec4(inPosition, 1.0); */
    
/*     // Generate a random darkness factor for each triangle */
/*     int triangleID = gl_VertexIndex / 3; */
/*     float seed = float(triangleID) * 12.9898; */
    
/*     // Create a darkness factor between 0.3 and 1.0 */
/*     // (0.3 = darkest, 1.0 = original color) */
/*     float darknessFactor = 0.3 + hash(seed) * 0.7; */
    
/*     // Apply darkness to the original vertex color */
/*     vec3 color = inColor.rgb * darknessFactor; */
    
/*     fragColor = vec4(color, inColor.a); */
/* } */

/* #version 450 */
/* layout(location = 0) in vec3 inPosition; */
/* layout(location = 1) in vec4 inColor; */

/* // Camera/view-projection */
/* layout(binding = 0) uniform UniformBufferObject { */
/*     mat4 vp; */
/* } ubo; */

/* // Per-mesh model matrix */
/* layout(push_constant) uniform PushConstants { */
/*     mat4 model; */
/* } pc; */

/* layout(location = 0) out vec4 fragColor; */

/* // Hash function for pseudo-random numbers */
/* float hash(float n) { */
/*     return fract(sin(n) * 43758.5453); */
/* } */

/* vec3 randomColor(float seed) { */
/*     return vec3( */
/*         hash(seed), */
/*         hash(seed + 1.0), */
/*         hash(seed + 2.0) */
/*     ); */
/* } */

/* void main() { */
/*     gl_Position = ubo.vp * pc.model * vec4(inPosition, 1.0); */
    
/*     // Generate same random color for all vertices of the same triangle */
/*     int triangleID = gl_VertexIndex / 3; */
/*     float seed = float(triangleID) * 12.9898; */
/*     vec3 color = randomColor(seed); */
    
/*     fragColor = vec4(color, 1.0); */
/* } */



/* #version 450 */

/* layout(location = 0) in vec3 inPosition; */
/* layout(location = 1) in vec4 inColor; */

/* // Camera/view-projection */
/* layout(binding = 0) uniform UniformBufferObject { */
/*     mat4 vp; */
/* } ubo; */

/* // Per-mesh model matrix */
/* layout(push_constant) uniform PushConstants { */
/*     mat4 model; */
/* } pc; */

/* layout(location = 0) out vec4 fragColor; */

/* void main() { */
/*     gl_Position = ubo.vp * pc.model * vec4(inPosition, 1.0); */
/*     fragColor = inColor; */
/* } */



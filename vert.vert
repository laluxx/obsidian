#version 450
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;

// Camera/view-projection
layout(binding = 0) uniform UniformBufferObject {
    mat4 vp;
} ubo;

// Per-mesh model matrix
layout(push_constant) uniform PushConstants {
    mat4 model;
} pc;

layout(location = 0) out vec4 fragColor;

// Hash function for pseudo-random numbers
float hash(float n) {
    return fract(sin(n) * 43758.5453);
}

vec3 randomColor(float seed) {
    return vec3(
        hash(seed),
        hash(seed + 1.0),
        hash(seed + 2.0)
    );
}

void main() {
    gl_Position = ubo.vp * pc.model * vec4(inPosition, 1.0);
    
    // Generate same random color for all vertices of the same triangle
    int triangleID = gl_VertexIndex / 3;
    float seed = float(triangleID) * 12.9898;
    vec3 color = randomColor(seed);
    
    fragColor = vec4(color, 1.0);
}


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

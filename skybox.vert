#version 450
layout(set = 0, binding = 0) uniform UBO {
    mat4 vp;
    mat4 view;
    mat4 proj;
    vec4 cameraPos;
    float time;
} ubo;

layout(location = 0) out vec3 outUVW;

void main() {
    vec3 positions[8] = vec3[8](
        vec3(-1.0, -1.0,  1.0), vec3( 1.0, -1.0,  1.0), vec3( 1.0,  1.0,  1.0), vec3(-1.0,  1.0,  1.0),
        vec3(-1.0, -1.0, -1.0), vec3( 1.0, -1.0, -1.0), vec3( 1.0,  1.0, -1.0), vec3(-1.0,  1.0, -1.0)
    );
    int indices[36] = int[36](
        0, 1, 2, 2, 3, 0, 1, 5, 6, 6, 2, 1, 7, 6, 5, 5, 4, 7, 4, 0, 3, 3, 7, 4, 4, 5, 1, 1, 0, 4, 3, 2, 6, 6, 7, 3
    );
    outUVW = positions[indices[gl_VertexIndex]];

    // Drop translation from the view matrix so the skybox follows the camera perfectly
    mat4 rotView = mat4(mat3(ubo.view));
    vec4 pos = ubo.proj * rotView * vec4(outUVW, 1.0);

    // Force depth to exactly 1.0 (the back of the depth buffer)
    gl_Position = pos.xyww;
}

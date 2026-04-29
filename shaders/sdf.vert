#version 460
layout(location = 0) out vec2 outUV;

void main() {
    // Generate a full-screen triangle natively on the GPU without uploading vertex buffers!
    outUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(outUV * 2.0f - 1.0f, 0.0f, 1.0f);
}

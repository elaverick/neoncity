#version 450
layout(location = 0) out vec2 vUV;

void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vUV = p;
    // z = 0: the far plane under reverse-Z, so the sky only fills empty pixels.
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}

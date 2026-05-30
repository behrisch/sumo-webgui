#version 440

// QRhi vertex shader for PersonLayerRhi. Persons render as small upright
// squares (no rotation) — same model as the OpenGL PersonLayer.

layout(location = 0) in vec2 a_local;   // quad vertex in person frame
layout(location = 1) in vec2 a_pos;     // per-instance world XY (float32)
layout(location = 2) in vec4 a_color;   // per-instance rgba (normalised)

layout(std140, binding = 0) uniform Ubuf {
    mat4 proj;
} u;

layout(location = 0) out vec4 v_color;

void main() {
    gl_Position = u.proj * vec4(a_pos + a_local, 0.0, 1.0);
    v_color = a_color;
}

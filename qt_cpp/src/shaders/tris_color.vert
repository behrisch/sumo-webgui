#version 440

// Per-vertex 2D position + rgba color, shared projection in UBO.
// Used by: Detector, StoppingPlace, EdgeColor, Polygon (fill).

layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec4 a_color;

layout(std140, binding = 0) uniform Ubo {
    mat4 proj;
} ubo;

layout(location = 0) out vec4 v_color;

void main() {
    v_color = a_color;
    gl_Position = ubo.proj * vec4(a_pos, 0.0, 1.0);
}

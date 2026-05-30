#version 440

// POI: instanced quad rendered as a shaded disc.
// Binding 0: local quad XY (PerVertex)
// Binding 1: world XY (PerInstance, float2)
// Binding 2: rgba8 (PerInstance, UNormByte4)

layout(location = 0) in vec2 a_local;
layout(location = 1) in vec2 a_pos;
layout(location = 2) in vec4 a_color;

layout(std140, binding = 0) uniform Ubo {
    mat4  proj;
    float half_size;
} ubo;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_local;

void main() {
    gl_Position = ubo.proj * vec4(a_pos + a_local, 0.0, 1.0);
    v_color = a_color;
    v_local = a_local / ubo.half_size;
}

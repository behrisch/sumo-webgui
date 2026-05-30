#version 440

// TLS bar: instanced quad oriented by per-instance lane tangent + width.
// The local quad's x is pre-scaled by kBarLen in the buffer upload, so the
// VS just multiplies a_local.x by a_tan and a_local.y by perp*a_width.
//
// Binding 0: a_local (PerVertex)             [pre-scaled by kBarLen on x]
// Binding 1: a_pos   (PerInstance, float2)
// Binding 2: a_tan   (PerInstance, float2)
// Binding 3: a_width (PerInstance, float)
// Binding 4: a_color (PerInstance, UNormByte4)

layout(location = 0) in vec2  a_local;
layout(location = 1) in vec2  a_pos;
layout(location = 2) in vec2  a_tan;
layout(location = 3) in float a_width;
layout(location = 4) in vec4  a_color;

layout(std140, binding = 0) uniform Ubo {
    mat4 proj;
} ubo;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_local;

void main() {
    vec2 perp = vec2(-a_tan.y, a_tan.x);
    vec2 p = a_pos + a_local.x * a_tan + a_local.y * perp * a_width;
    gl_Position = ubo.proj * vec4(p, 0.0, 1.0);
    v_color = a_color;
    v_local = a_local;
}

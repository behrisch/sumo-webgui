#version 440

// QRhi vertex shader for VehicleLayerRhi. Compiled via qsb to SPIR-V then
// cross-translated to MSL / HLSL / GLSL at build time.

layout(location = 0) in vec2  a_local;       // quad vertex in vehicle frame
layout(location = 1) in vec2  a_pos;         // per-instance world XY (float32)
layout(location = 2) in float a_angle_navi;  // per-instance navi-degrees (CW from north)
layout(location = 3) in vec4  a_color;       // per-instance rgba (normalised)

layout(std140, binding = 0) uniform Ubuf {
    mat4 proj;
} u;

layout(location = 0) out vec4 v_color;

void main() {
    // Convert SUMO navi-degrees (CW from north) to math-frame radians (CCW
    // from +x) on the GPU so we don't pay it per vehicle on the CPU and
    // don't have to upload a separate cos/sin buffer.
    float rad = radians(90.0 - a_angle_navi);
    float c = cos(rad);
    float s = sin(rad);
    vec2 rotated = vec2(c * a_local.x - s * a_local.y,
                        s * a_local.x + c * a_local.y);
    vec2 world = a_pos + rotated;
    gl_Position = u.proj * vec4(world, 0.0, 1.0);
    v_color = a_color;
}

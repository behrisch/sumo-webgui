#version 440

// QRhi vertex shader for VehicleLayerRhi. Compiled via qsb to SPIR-V then
// cross-translated to MSL / HLSL / GLSL at build time.

layout(location = 0) in vec2  a_local;       // unit mesh: x in [-1,0], y in [-1,+1]
layout(location = 1) in vec2  a_pos;         // per-instance world XY (float32)
layout(location = 2) in float a_angle_navi;  // per-instance navi-degrees (CW from north)
layout(location = 3) in vec4  a_color;       // per-instance rgba (normalised)
layout(location = 4) in float a_length;      // per-instance body length (meters)
layout(location = 5) in vec4  a_tint;        // per-vertex tint multiplier (UNorm)
layout(location = 6) in float a_width;       // per-instance body width (meters)

layout(std140, binding = 0) uniform Ubuf {
    mat4 proj;
    vec4 min_size;  // x = min length (m), y = min width (m), z/w = pad
} u;

layout(location = 0) out vec4 v_color;

void main() {
    // Convert SUMO navi-degrees (CW from north) to math-frame radians (CCW
    // from +x) on the GPU so we don't pay it per vehicle on the CPU and
    // don't have to upload a separate cos/sin buffer.
    float rad = radians(90.0 - a_angle_navi);
    float c = cos(rad);
    float s = sin(rad);
    // Scale the unit local mesh: x along heading by per-instance length, y by
    // per-instance half-width. Clamped to u.min_size so vehicles stay legible
    // at low zoom (mirrors ecal `vehicleMinPixels`; caller computes meters
    // per N pixels from the camera). a_pos is the centre of the FRONT bumper
    // so x runs from -length (rear) to 0 (front).
    float L = max(a_length, u.min_size.x);
    float W = max(a_width,  u.min_size.y);
    vec2 scaled = vec2(a_local.x * L, a_local.y * W * 0.5);
    vec2 rotated = vec2(c * scaled.x - s * scaled.y,
                        s * scaled.x + c * scaled.y);
    vec2 world = a_pos + rotated;
    gl_Position = u.proj * vec4(world, 0.0, 1.0);
    // Per-vertex tint scales the per-instance color (so a 0-tint vertex is
    // always black, a 255-tint vertex shows the full body color). Alpha is
    // taken straight from the instance.
    v_color = vec4(a_color.rgb * a_tint.r, a_color.a);
}

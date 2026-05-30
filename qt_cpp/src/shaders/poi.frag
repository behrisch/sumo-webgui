#version 440

layout(location = 0) in  vec4 v_color;
layout(location = 1) in  vec2 v_local;
layout(location = 0) out vec4 frag;

void main() {
    float r = length(v_local);
    if (r > 1.0) discard;
    float edge = smoothstep(1.0, 0.9, r);
    float rim  = smoothstep(0.95, 0.82, r);
    vec3  c    = mix(vec3(0.0), v_color.rgb, rim);
    frag = vec4(c, v_color.a * edge);
}

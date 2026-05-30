#version 440

layout(location = 0) in  vec4 v_color;
layout(location = 1) in  vec2 v_local;
layout(location = 0) out vec4 frag;

void main() {
    // Local quad spans roughly [-0.5,0.5]^2 across (a_local is pre-scaled
    // along x by kBarLen — but its sign/magnitude near 0/±something is still
    // dominated by ±kBarLen/2 on x and ±0.5 on y). A coarse rim shading is
    // enough to differentiate adjacent bars.
    float r   = max(abs(v_local.x), abs(v_local.y)) * 2.0;
    float rim = smoothstep(1.0, 0.85, r);
    vec3  c   = mix(v_color.rgb * 0.55, v_color.rgb, rim);
    frag = vec4(c, v_color.a);
}

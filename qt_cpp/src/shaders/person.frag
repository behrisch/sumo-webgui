#version 440

layout(location = 0) in  vec4 v_color;
layout(location = 0) out vec4 frag;

void main() {
    frag = v_color;
}

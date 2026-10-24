#version 300 es
precision highp float;

uniform mat4 view_matrix;
uniform mat4 world_matrix;
uniform vec4 mult_color;
uniform vec4 add_color;

in vec4 frag_color;
out vec4 out_color;

void main() {
    out_color = frag_color;
}

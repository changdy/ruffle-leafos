#version 300 es
precision highp float;

in vec2 position;
in vec2 uv;

out vec2 frag_uv;
out vec2 shifted_uv;

uniform float dir_x;
uniform float dir_y;
uniform float full_size;
uniform float m;
uniform float m2;
uniform float first_weight;
uniform float last_offset;
uniform float last_weight;

void main() {
    frag_uv = uv;
    vec2 direction = vec2(dir_x, dir_y);
    shifted_uv = uv - direction * m;
    gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);
    gl_Position.y = 1.0 - gl_Position.y;
}

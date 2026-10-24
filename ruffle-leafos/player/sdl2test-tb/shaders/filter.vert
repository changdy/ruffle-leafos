#version 300 es
precision highp float;

uniform vec2 u_position;
uniform vec2 u_uv;

out vec2 frag_uv;

void main() {
    frag_uv = u_uv;
    gl_Position = vec4(u_position.x * 2.0 - 1.0, 1.0 - u_position.y * 2.0, 0.0, 1.0);
}

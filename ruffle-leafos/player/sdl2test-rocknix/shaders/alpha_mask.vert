#version 300 es
precision highp float;

in vec2 position;
in vec2 uv;

out vec2 frag_uv;

void main() {
    frag_uv = uv;
    gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);
    gl_Position.y = 1.0 - gl_Position.y;
}

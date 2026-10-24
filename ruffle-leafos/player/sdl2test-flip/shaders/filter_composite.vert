#version 300 es
precision highp float;

// Generic filter vertex shader: takes position and UV as uniforms
uniform vec2 u_position;
uniform vec2 uv0;
uniform vec2 uv1;
uniform vec2 uv2;

out vec2 frag_uv;
out vec2 frag_uv1;
out vec2 frag_uv2;

void main() {
    frag_uv = uv0;
    frag_uv1 = uv1;
    frag_uv2 = uv2;
    gl_Position = vec4(u_position.x * 2.0 - 1.0, 1.0 - u_position.y * 2.0, 0.0, 1.0);
}

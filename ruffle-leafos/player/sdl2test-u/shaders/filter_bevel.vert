#version 300 es
precision highp float;

in vec2 position;
in vec2 source_uv;
in vec2 blur_uv_left;
in vec2 blur_uv_right;

out vec2 frag_source_uv;
out vec2 frag_blur_uv_left;
out vec2 frag_blur_uv_right;

void main() {
    frag_source_uv = source_uv;
    frag_blur_uv_left = blur_uv_left;
    frag_blur_uv_right = blur_uv_right;
    gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);
    gl_Position.y = 1.0 - gl_Position.y;
}

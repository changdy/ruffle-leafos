#version 300 es
precision highp float;

uniform mat4 view_matrix;
uniform mat4 world_matrix;
uniform vec4 mult_color;
uniform vec4 add_color;

in vec2 position;
in vec4 color;
out vec4 frag_color;

void main() {
    frag_color = clamp(color * mult_color + add_color, 0.0, 1.0);
    float alpha = clamp(frag_color.a, 0.0, 1.0);
    frag_color = vec4(frag_color.rgb * alpha, alpha);
    gl_Position = view_matrix * world_matrix * vec4(position, 0.0, 1.0);
}

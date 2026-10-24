#version 300 es
precision highp float;

uniform mat4 view_matrix;
uniform mat4 world_matrix;
uniform vec4 mult_color;
uniform vec4 add_color;
uniform mat3 u_matrix;

uniform sampler2D u_texture;

in vec2 frag_uv;
out vec4 out_color;

void main() {
    vec4 color = texture(u_texture, frag_uv);

    // Unmultiply alpha before apply color transform.
    if (color.a > 0.0) {
        color.rgb /= color.a;
        color = clamp(mult_color * color + add_color, 0.0, 1.0);
        float alpha = clamp(color.a, 0.0, 1.0);
        color = vec4(color.rgb * alpha, alpha);
    }

    out_color = color;
}

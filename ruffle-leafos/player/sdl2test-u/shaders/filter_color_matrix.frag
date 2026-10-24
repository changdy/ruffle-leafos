#version 300 es
precision highp float;

in vec2 frag_uv;

out vec4 frag_color;

uniform sampler2D texture_sampler;
uniform float color_matrix[20];

void main() {
    vec4 src = texture(texture_sampler, frag_uv);
    if (src.a == 0.0) {
        frag_color = vec4(0.0);
        return;
    }

    vec3 unpremultiplied = src.rgb / src.a;
    vec4 color;
    color.r = clamp(
        color_matrix[0] * unpremultiplied.r +
        color_matrix[1] * unpremultiplied.g +
        color_matrix[2] * unpremultiplied.b +
        color_matrix[3] * src.a +
        color_matrix[4] / 255.0,
        0.0,
        1.0
    );
    color.g = clamp(
        color_matrix[5] * unpremultiplied.r +
        color_matrix[6] * unpremultiplied.g +
        color_matrix[7] * unpremultiplied.b +
        color_matrix[8] * src.a +
        color_matrix[9] / 255.0,
        0.0,
        1.0
    );
    color.b = clamp(
        color_matrix[10] * unpremultiplied.r +
        color_matrix[11] * unpremultiplied.g +
        color_matrix[12] * unpremultiplied.b +
        color_matrix[13] * src.a +
        color_matrix[14] / 255.0,
        0.0,
        1.0
    );
    color.a = clamp(
        color_matrix[15] * unpremultiplied.r +
        color_matrix[16] * unpremultiplied.g +
        color_matrix[17] * unpremultiplied.b +
        color_matrix[18] * src.a +
        color_matrix[19] / 255.0,
        0.0,
        1.0
    );
    frag_color = vec4(color.rgb * color.a, color.a);
}

#version 300 es
precision highp float;

in vec2 frag_source_uv;
in vec2 frag_blur_uv;

out vec4 frag_color;

uniform sampler2D source_texture;
uniform sampler2D blur_texture;
uniform vec4 glow_color;
uniform float strength;
uniform int inner;
uniform int knockout;
uniform int composite_source;

void main() {
    float blur = texture(blur_texture, frag_blur_uv).a;
    vec4 dest = texture(source_texture, frag_source_uv);

    if (frag_blur_uv.x < 0.0 || frag_blur_uv.x > 1.0 || frag_blur_uv.y < 0.0 || frag_blur_uv.y > 1.0) {
        blur = 0.0;
    }

    vec4 color = vec4(glow_color.rgb, 1.0);
    if (inner == 1) {
        float alpha = glow_color.a * clamp((1.0 - blur) * strength, 0.0, 1.0);
        if (knockout == 1) {
            color = color * alpha * dest.a;
        } else if (composite_source == 1) {
            color = color * alpha * dest.a + dest * (1.0 - alpha);
        } else {
            color = color * alpha * dest.a;
        }
    } else {
        float alpha = glow_color.a * clamp(blur * strength, 0.0, 1.0);
        if (knockout == 1) {
            color = color * alpha * (1.0 - dest.a);
        } else if (composite_source == 1) {
            color = color * alpha * (1.0 - dest.a) + dest;
        } else {
            color = color * alpha;
        }
    }
    frag_color = color;
}

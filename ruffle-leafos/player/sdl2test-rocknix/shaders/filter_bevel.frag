#version 300 es
precision highp float;

in vec2 frag_source_uv;
in vec2 frag_blur_uv_left;
in vec2 frag_blur_uv_right;

out vec4 frag_color;

uniform sampler2D source_texture;
uniform sampler2D blur_texture;
uniform vec4 highlight_color;
uniform vec4 shadow_color;
uniform float strength;
uniform int bevel_type;
uniform int knockout;
uniform int composite_source;

void main() {
    float blur_left = texture(blur_texture, frag_blur_uv_left).a;
    float blur_right = texture(blur_texture, frag_blur_uv_right).a;
    vec4 dest = texture(source_texture, frag_source_uv);

    if (frag_blur_uv_left.x < 0.0 || frag_blur_uv_left.x > 1.0 || frag_blur_uv_left.y < 0.0 || frag_blur_uv_left.y > 1.0) {
        blur_left = 0.0;
    }
    if (frag_blur_uv_right.x < 0.0 || frag_blur_uv_right.x > 1.0 || frag_blur_uv_right.y < 0.0 || frag_blur_uv_right.y > 1.0) {
        blur_right = 0.0;
    }

    float highlight_alpha = clamp((blur_left - blur_right) * strength, 0.0, 1.0);
    float shadow_alpha = clamp((blur_right - blur_left) * strength, 0.0, 1.0);
    vec4 glow = highlight_color * highlight_alpha + shadow_color * shadow_alpha;

    bool outer = bevel_type == 0 || bevel_type == 2;
    bool inner = bevel_type == 1 || bevel_type == 2;

    if (inner && outer) {
        if (knockout == 1) {
            frag_color = glow;
        } else {
            frag_color = dest - dest * glow.a + glow;
        }
    } else if (inner) {
        if (knockout == 1) {
            frag_color = glow * dest.a;
        } else {
            frag_color = glow * dest.a + dest * (1.0 - glow.a);
        }
    } else {
        if (knockout == 1) {
            frag_color = glow - glow * dest.a;
        } else {
            frag_color = dest + glow - glow * dest.a;
        }
    }
}

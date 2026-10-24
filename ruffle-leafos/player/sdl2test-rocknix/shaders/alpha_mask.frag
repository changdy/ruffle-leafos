#version 300 es
precision highp float;

in vec2 frag_uv;

out vec4 frag_color;

uniform sampler2D maskee_texture;
uniform sampler2D mask_texture;

void main() {
    vec4 dst = texture(maskee_texture, frag_uv);
    vec4 src = texture(mask_texture, frag_uv);
    frag_color = vec4(dst.rgb * src.a, dst.a * src.a);
}

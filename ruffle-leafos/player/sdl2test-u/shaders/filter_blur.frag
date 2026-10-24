#version 300 es
precision highp float;

in vec2 frag_uv;
in vec2 shifted_uv;

out vec4 frag_color;

uniform sampler2D texture_sampler;
uniform float dir_x;
uniform float dir_y;
uniform float full_size;
uniform float m2;
uniform float first_weight;
uniform float last_offset;
uniform float last_weight;

void main() {
    vec2 direction = vec2(dir_x, dir_y);
    vec4 total = vec4(0.0);

    total += texture(texture_sampler, shifted_uv - direction) * first_weight;

    vec4 center = vec4(0.0);
    for (float i = 0.5; i < m2; i += 2.0) {
        center += texture(texture_sampler, shifted_uv + direction * i);
    }
    total += center * 2.0;

    vec2 last_location = shifted_uv + direction * (m2 + last_offset);
    total += texture(texture_sampler, last_location) * last_weight;

    vec4 result = total / full_size;
    frag_color = floor(result * 255.0) / 255.0;
}

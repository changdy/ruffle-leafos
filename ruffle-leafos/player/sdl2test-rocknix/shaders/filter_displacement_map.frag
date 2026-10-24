#version 300 es
precision highp float;

in vec2 frag_uv;

out vec4 frag_color;

uniform sampler2D source_texture;
uniform sampler2D map_texture;
uniform vec4 filter_color;
uniform uint components_packed;
uniform uint mode;
uniform float scale_x;
uniform float scale_y;
uniform float source_width;
uniform float source_height;
uniform float map_width;
uniform float map_height;
uniform float offset_x;
uniform float offset_y;
uniform float viewscale_x;
uniform float viewscale_y;

uint unpack_component_x() { return (components_packed >> 8u) & 0xFFu; }
uint unpack_component_y() { return components_packed & 0x0Fu; }

float get_component(vec4 map, uint component) {
    if (component == 1u) return map.r * 255.0;
    if (component == 2u) return map.g * 255.0;
    if (component == 4u) return map.b * 255.0;
    if (component == 8u) return map.a * 255.0;
    return 128.0;
}

void main() {
    vec2 source_pos = vec2(frag_uv.x * source_width, frag_uv.y * source_height);
    vec2 map_uv = vec2(
        (source_pos.x - offset_x) / viewscale_x / map_width,
        (source_pos.y - offset_y) / viewscale_y / map_height
    );

    vec4 map = texture(map_texture, map_uv);
    if (map_uv.x < 0.0 || map_uv.x > 1.0 || map_uv.y < 0.0 || map_uv.y > 1.0) {
        map = vec4(0.5);
    }

    uint comp_x = unpack_component_x();
    uint comp_y = unpack_component_y();
    vec2 displaced = vec2(
        source_pos.x + (get_component(map, comp_x) - 128.0) * viewscale_x * scale_x / 256.0,
        source_pos.y + (get_component(map, comp_y) - 128.0) * viewscale_y * scale_y / 256.0
    );
    vec2 displaced_uv = displaced / vec2(source_width, source_height);

    bool out_of_bounds = displaced_uv.x < 0.0 || displaced_uv.x > 1.0 || displaced_uv.y < 0.0 || displaced_uv.y > 1.0;
    vec2 final_uv = displaced_uv;

    if (mode == 1u) {
        final_uv = clamp(displaced_uv, 0.0, 1.0);
    } else if (mode == 2u && out_of_bounds) {
        final_uv = frag_uv;
    }

    vec4 result = texture(source_texture, final_uv);
    if (mode == 3u && out_of_bounds) {
        result = vec4(filter_color.rgb, 1.0) * filter_color.a;
    }
    frag_color = result;
}

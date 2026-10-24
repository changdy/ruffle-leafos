#include "probe_input.h"

static uint32_t read32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

void probe_input_init(struct probe_input *s, uint32_t delay) {
    volatile unsigned char *bytes = (volatile unsigned char *)s;
    for (unsigned i = 0; i < sizeof(*s); ++i) bytes[i] = 0;
    s->arm_delay_ms = delay;
}

void probe_input_presented(struct probe_input *s, uint64_t now) {
    if (!s->presented) {
        s->presented = 1;
        s->presented_at = now;
    }
}

int probe_input_ready(const struct probe_input *s, uint64_t now) {
    return s->presented && now >= s->presented_at &&
           now - s->presented_at >= s->arm_delay_ms;
}

enum probe_action probe_input_event(struct probe_input *s,
                                    const unsigned char event[56], uint64_t now) {
    uint32_t type = read32(event), family = 0, device = 0, code = 0;
    int down = 0, repeat = 0;
    switch (type) {
    case 0x300: case 0x301: /* SDL_KEYDOWN / SDL_KEYUP */
        family = 1; code = read32(event + 16);
        down = type == 0x300; repeat = event[13] != 0;
        break;
    case 0x401: case 0x402: /* SDL_MOUSEBUTTONDOWN / UP */
        family = 2; device = read32(event + 12); code = event[16];
        down = type == 0x401;
        break;
    case 0x603: case 0x604: /* SDL_JOYBUTTONDOWN / UP */
        family = 3; device = read32(event + 8); code = event[12];
        down = type == 0x603;
        break;
    case 0x651: case 0x652: /* SDL_CONTROLLERBUTTONDOWN / UP */
        family = 4; device = read32(event + 8); code = event[12];
        down = type == 0x651;
        break;
    case 0x602: /* SDL_JOYHATMOTION: the Pocket 1 D-pad */
        family = 5; device = read32(event + 8); code = event[12];
        down = event[13] != 0;
        break;
    case 0x302: case 0x303: case 0x304: case 0x305:
    case 0x400: case 0x403: case 0x600: case 0x601: case 0x650:
        /* Text, pointer movement, wheel, stick drift: never reach the game. */
        return PROBE_CONSUME;
    default:
        return PROBE_PASS; /* Window, hotplug, and quit events still work. */
    }
    struct probe_button *slot = 0;
    for (unsigned i = 0; i < 256; ++i) {
        struct probe_button *b = &s->buttons[i];
        if (!b->used) {
            b->used = 1; b->family = family; b->device = device; b->code = code;
            slot = b; break;
        }
        if (b->family == family && b->device == device && b->code == code) {
            slot = b; break;
        }
    }
    if (!slot) return PROBE_CONSUME;
    int exit = down && !slot->down && !repeat && probe_input_ready(s, now);
    slot->down = (unsigned char)down;
    return exit ? PROBE_QUIT : PROBE_CONSUME;
}

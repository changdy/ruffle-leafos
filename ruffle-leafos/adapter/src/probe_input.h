#ifndef RUFFLE_PROBE_INPUT_H
#define RUFFLE_PROBE_INPUT_H

#include <stdint.h>

/* SDL2's public event ABI, not SDL3. SDL_Event is 56 bytes on this target. */
enum probe_action { PROBE_PASS, PROBE_CONSUME, PROBE_QUIT };
struct probe_button {
    uint32_t family, device, code;
    unsigned char used, down;
};
struct probe_input {
    uint64_t presented_at;
    uint32_t arm_delay_ms;
    unsigned char presented;
    struct probe_button buttons[256];
};

void probe_input_init(struct probe_input *state, uint32_t arm_delay_ms);
void probe_input_presented(struct probe_input *state, uint64_t now_ms);
int probe_input_ready(const struct probe_input *state, uint64_t now_ms);
enum probe_action probe_input_event(struct probe_input *state,
                                    const unsigned char event[56], uint64_t now_ms);
#endif

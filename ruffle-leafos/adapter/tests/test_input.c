#include "probe_input.h"
#define assert(condition) do { if (!(condition)) return __LINE__; } while (0)

static unsigned char event[56];
static void put32(unsigned at, uint32_t value) {
    for (unsigned n = 0; n < 4; ++n) event[at+n] = (unsigned char)(value >> (8*n));
}
static void make_event(uint32_t type, uint32_t device, unsigned code, unsigned value) {
    for (unsigned i = 0; i < sizeof(event); ++i) event[i] = 0;
    put32(0, type); put32(8, device);
    event[12] = (unsigned char)code; event[13] = (unsigned char)value;
}
int probe_tests(void) {
    struct probe_input s;
    probe_input_init(&s, 500);
    make_event(0x603, 7, 1, 0); /* A used to start the game. */
    assert(probe_input_event(&s, event, 1) == PROBE_CONSUME);
    probe_input_presented(&s, 100);
    probe_input_presented(&s, 200); /* Must not reset the first-frame clock. */
    assert(!probe_input_ready(&s, 599));
    assert(probe_input_ready(&s, 600));
    assert(probe_input_event(&s, event, 700) == PROBE_CONSUME); /* Held A. */
    make_event(0x604, 7, 1, 0);
    assert(probe_input_event(&s, event, 701) == PROBE_CONSUME);
    make_event(0x603, 7, 1, 0);
    assert(probe_input_event(&s, event, 702) == PROBE_QUIT);
    probe_input_init(&s, 0); probe_input_presented(&s, 0);
    make_event(0x602, 4, 0, 1);
    assert(probe_input_event(&s, event, 1) == PROBE_QUIT); /* D-pad. */
    probe_input_init(&s, 500);
    make_event(0x602, 4, 0, 1);
    assert(probe_input_event(&s, event, 1) == PROBE_CONSUME);
    probe_input_presented(&s, 100);
    assert(probe_input_event(&s, event, 700) == PROBE_CONSUME);
    make_event(0x602, 4, 0, 0);
    assert(probe_input_event(&s, event, 701) == PROBE_CONSUME);
    make_event(0x602, 4, 0, 2);
    assert(probe_input_event(&s, event, 702) == PROBE_QUIT);
    probe_input_init(&s, 0); probe_input_presented(&s, 0);
    make_event(0x300, 0, 0, 1); put32(16, 40); /* Repeated keyboard Enter. */
    assert(probe_input_event(&s, event, 1) == PROBE_CONSUME);
    make_event(0x301, 0, 0, 0); put32(16, 40);
    assert(probe_input_event(&s, event, 2) == PROBE_CONSUME);
    make_event(0x300, 0, 0, 0); put32(16, 40);
    assert(probe_input_event(&s, event, 3) == PROBE_QUIT);
    make_event(0x600, 3, 0, 0); event[16] = 0xff;
    assert(probe_input_event(&s, event, 4) == PROBE_CONSUME); /* Stick drift. */
    make_event(0x651, 42, 4, 0);
    assert(probe_input_event(&s, event, 5) == PROBE_QUIT); /* Controller shoulder. */
    make_event(0x401, 0, 0, 0); event[16] = 1;
    assert(probe_input_event(&s, event, 6) == PROBE_QUIT); /* External mouse. */
    make_event(0x200, 0, 0, 0);
    assert(probe_input_event(&s, event, 7) == PROBE_PASS); /* Window lifecycle. */
    make_event(0x100, 0, 0, 0);
    assert(probe_input_event(&s, event, 8) == PROBE_PASS); /* Normal quit. */
    probe_input_init(&s, 500);
    make_event(0x603, 99, 1, 0);
    assert(probe_input_event(&s, event, 100000) == PROBE_CONSUME); /* No GL frame. */
    return 0;
}

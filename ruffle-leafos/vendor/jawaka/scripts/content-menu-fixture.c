/* Qualification only. Never included in Leaf or a published pak.
   SDL events and independent evdev reads prove which roster device saw Guide.
   A blue/green screen toggles on SDL's logical Guide; SIGTERM is logged and
   deliberately ignored so the daemon's continuous-hold escalation is testable. */
#define _GNU_SOURCE
#include <SDL.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t terms;
static void term(int sig) { (void)sig; terms++; }
static unsigned long long now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    /* Evdev injection is useful for repeat/queued-edge qualification too.
       Invoke from adb outside the child's private input namespace. */
    if (argc == 6 && strcmp(argv[1], "--send") == 0) {
        int fd = open(argv[2], O_WRONLY);
        struct input_event ev = {0};
        ev.type = atoi(argv[3]); ev.code = atoi(argv[4]); ev.value = atoi(argv[5]);
        if (fd < 0 || write(fd, &ev, sizeof(ev)) != sizeof(ev)) return 2;
        printf("%llu SEND path=%s type=%d code=%d value=%d\n", now(), argv[2],
               ev.type, ev.code, ev.value);
        ev.type = EV_SYN; ev.code = SYN_REPORT; ev.value = 0;
        if (write(fd, &ev, sizeof(ev)) != sizeof(ev)) return 2;
        close(fd);
        return 0;
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC) < 0) {
        fprintf(stderr, "SDL init: %s\n", SDL_GetError()); return 2;
    }
    struct sigaction action = {0};
    action.sa_handler = term; sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    SDL_Window *window = SDL_CreateWindow("Menu qualification", 0, 0, 960, 720,
                                          SDL_WINDOW_FULLSCREEN);
    SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE) : NULL;
    printf("%llu START pid=%d core=%s roster=%s\n", now(), getpid(),
           getenv("JAWAKA_GAME_CORE_ID"), getenv("SDL_JOYSTICK_DEVICE"));
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        SDL_Joystick *joy = SDL_JoystickOpen(i);
        if (!joy) continue;
        char guid[40]; SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joy), guid, sizeof(guid));
        printf("%llu SDL device=%d instance=%d name=%s guid=%s buttons=%d axes=%d hats=%d\n",
               now(), i, SDL_JoystickInstanceID(joy), SDL_JoystickName(joy), guid,
               SDL_JoystickNumButtons(joy), SDL_JoystickNumAxes(joy), SDL_JoystickNumHats(joy));
        SDL_GameController *controller = SDL_GameControllerOpen(i);
        if (controller) {
            char *mapping = SDL_GameControllerMapping(controller);
            printf("%llu MAPPING device=%d %s\n", now(), i, mapping ? mapping : "none");
            SDL_free(mapping);
        }
        if (SDL_JoystickIsHaptic(joy)) {
            SDL_Haptic *haptic = SDL_HapticOpenFromJoystick(joy);
            if (haptic && SDL_HapticRumbleInit(haptic) == 0)
                printf("%llu RUMBLE device=%d rc=%d\n", now(), i, SDL_HapticRumblePlay(haptic, 0.3f, 150));
        }
    }
    int fds[64];
    for (int i = 0; i < 64; i++) {
        char path[64]; snprintf(path, sizeof(path), "/dev/input/event%d", i);
        fds[i] = open(path, O_RDONLY | O_NONBLOCK);
        if (fds[i] < 0) continue;
        char name[128] = {0}; struct input_id id = {0};
        ioctl(fds[i], EVIOCGNAME(sizeof(name)), name); ioctl(fds[i], EVIOCGID, &id);
        int clock = CLOCK_MONOTONIC; ioctl(fds[i], EVIOCSCLOCKID, &clock);
        printf("%llu EVDEV path=%s name=%s id=%04x:%04x:%04x:%04x\n", now(), path,
               name, id.bustype, id.vendor, id.product, id.version);
    }
    bool menu = false;
    sig_atomic_t logged_terms = 0;
    for (;;) {
        if (logged_terms != terms) {
            logged_terms = terms;
            printf("%llu SIGTERM count=%d\n", now(), (int)logged_terms);
        }
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_JOYBUTTONDOWN || ev.type == SDL_JOYBUTTONUP)
                printf("%llu JOY instance=%d button=%d value=%d\n", now(), ev.jbutton.which,
                       ev.jbutton.button, ev.jbutton.state);
            if (ev.type == SDL_CONTROLLERBUTTONDOWN || ev.type == SDL_CONTROLLERBUTTONUP) {
                printf("%llu CONTROLLER instance=%d button=%s value=%d\n", now(), ev.cbutton.which,
                       SDL_GameControllerGetStringForButton(ev.cbutton.button), ev.cbutton.state);
                if (ev.type == SDL_CONTROLLERBUTTONDOWN && ev.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE) {
                    menu = !menu; printf("%llu MENU toggled=%d\n", now(), menu);
                }
            }
        }
        for (int i = 0; i < 64; i++) {
            if (fds[i] < 0) continue;
            struct input_event input;
            while (read(fds[i], &input, sizeof(input)) == sizeof(input)) {
                if (input.type == EV_KEY || input.type == EV_ABS)
                    printf("%llu RAW event%d edge=%lld.%06lld type=%d code=%d value=%d\n",
                           now(), i, (long long)input.time.tv_sec, (long long)input.time.tv_usec,
                           input.type, input.code, input.value);
            }
        }
        if (renderer) {
            SDL_SetRenderDrawColor(renderer, 16, menu ? 110 : 30, menu ? 40 : 120, 255);
            SDL_RenderClear(renderer); SDL_RenderPresent(renderer);
        }
        SDL_Delay(5);
    }
}

#include "internal/launcher/menu_escape.h"
#include "internal/launcher/standalone_policy.h"

#include <stdio.h>
#include <stdlib.h>

static void expect(const char *name, bool ok) {
    if (!ok) { fprintf(stderr, "Menu escape: %s\n", name); exit(1); }
}

int main(void) {
    jw_standalone_policy provider = jw_standalone_policy_resolve(
        "ppsspp", "/sd/Apps/mlp1/Fixture.pak/emulators/PPSSPP", "mlp1/Fixture.pak");
    expect("built-in true forwards", jw_standalone_policy_menu(&provider, true, false)
           == JW_STANDALONE_MENU_FORWARD);
    expect("external true handled", jw_standalone_policy_menu(&provider, true, true)
           == JW_STANDALONE_MENU_EXTERNAL_HANDLED);
    for (int external = 0; external < 2; external++) {
        expect("false asks before quitting",
               jw_standalone_policy_menu(&provider, false, external)
               == JW_STANDALONE_MENU_CONFIRM_QUIT);
        jw_standalone_policy release = jw_standalone_policy_resolve("ppsspp", NULL, NULL);
        expect("release route unchanged", jw_standalone_policy_menu(&release, true, external)
               == JW_STANDALONE_MENU_RELEASE);
    }

    jw_menu_escape escape = {0};
    expect("no hold", !jw_menu_escape_kill(&escape, 1, true, 9000));
    /* SIGTERM really sent at 4100 after a busy tick, not the nominal 3000
       deadline. Its full grace starts at 4100. */
    jw_menu_escape_sent(&escape, 1, 7, 4100);
    expect("target session and hold", escape.session == 1 && escape.hold == 7);
    expect("not nominal deadline", !jw_menu_escape_kill(&escape, 1, true, 5000));
    expect("full grace", !jw_menu_escape_kill(&escape, 1, true, 6099));
    expect("uninterrupted hold kills", jw_menu_escape_kill(&escape, 1, true, 6100));
    expect("one kill", !jw_menu_escape_kill(&escape, 1, true, 9000));

    /* End notification covers release and every cancellation source; an old
       hold's end cannot cancel a later hold. */
    jw_menu_escape_sent(&escape, 1, 8, 10000);
    jw_menu_escape_end(&escape, 8);
    expect("release/cancel after TERM", !jw_menu_escape_kill(&escape, 1, true, 15000));
    jw_menu_escape_sent(&escape, 1, 9, 20000);
    jw_menu_escape_end(&escape, 8);
    expect("fresh hold gets its grace", !jw_menu_escape_kill(&escape, 1, true, 21999));
    expect("fresh hold kills at own deadline", jw_menu_escape_kill(&escape, 1, true, 22000));
    jw_menu_escape_sent(&escape, 1, 10, 30000);
    jw_menu_escape_end(&escape, 10); /* queued release drained before deadline */
    expect("queued release", !jw_menu_escape_kill(&escape, 1, true, 35000));
    jw_menu_escape_sent(&escape, 1, 11, 40000);
    jw_menu_escape_cancel(&escape); /* sleep/flush/shutdown */
    expect("sleep", !jw_menu_escape_kill(&escape, 1, true, 45000));
    jw_menu_escape_sent(&escape, 1, 12, 50000);
    expect("child exit", !jw_menu_escape_kill(&escape, 1, false, 55000));
    jw_menu_escape_sent(&escape, 1, 13, 60000);
    expect("replacement even with same PID", !jw_menu_escape_kill(&escape, 2, true, 65000));
    puts("Menu routing and escape checks passed");
    return 0;
}

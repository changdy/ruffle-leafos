# shellcheck shell=bash
# Start and stop jawakad for the daemon smokes. Source it; it only defines
# functions.
#
# Every smoke has to be able to reap the daemon it started. The old
# `( cd "$ROOT_DIR"; VAR=... jawakad ... ) &` could not: a subshell that runs
# two commands forks jawakad instead of exec'ing it, so $! was the subshell
# and killing it orphaned the daemon, which kept its jawaka-osd (a window on a
# desktop) alive and respawned it. smoke_daemon_start execs, so DAEMON_PID is
# jawakad itself.

# smoke_daemon_start DIR LOG [NAME=VALUE ...] DAEMON [ARG ...]
#   Runs DAEMON from DIR with the NAME=VALUE pairs in its environment,
#   appends its output to LOG and sets DAEMON_PID. JAWAKA_OSD=0 goes first,
#   so no smoke opens an OSD. One that needs OSD requests on passes
#   JAWAKA_OSD=1 and runs a copy of jawakad with no jawaka-osd beside it
#   (see life1-game-check-ipc-smoke.sh).
smoke_daemon_start() {
    local dir="$1" log="$2"
    shift 2
    (cd "$dir" && exec env JAWAKA_OSD=0 "$@") >>"$log" 2>&1 &
    DAEMON_PID=$!
}

# smoke_daemon_alive PID: true while PID runs. A zombie is not running.
smoke_daemon_alive() {
    kill -0 "$1" 2>/dev/null || return 1
    case "$(ps -o stat= -p "$1" 2>/dev/null)" in
        Z*) return 1 ;;
    esac
    return 0
}

# smoke_daemon_stop [SIGNAL]
#   Sends SIGNAL (TERM by default) to DAEMON_PID and gives it 5 s to exit,
#   then SIGKILLs it and its children. Clears DAEMON_PID, and does nothing
#   when there is no daemon. Returns 1, saying so, if jawakad is somehow
#   still running after that.
smoke_daemon_stop() {
    local pid="${DAEMON_PID:-}" signal="${1:-TERM}" _
    DAEMON_PID=""
    [ -n "$pid" ] || return 0
    kill -"$signal" "$pid" 2>/dev/null || true
    for _ in $(seq 1 50); do
        smoke_daemon_alive "$pid" || break
        sleep 0.1
    done
    if smoke_daemon_alive "$pid"; then
        echo "smoke: jawakad pid $pid still running 5 s after SIG$signal; killing it" >&2
        # Stopped, it cannot respawn a child while its children go first.
        kill -STOP "$pid" 2>/dev/null || true
        pkill -KILL -P "$pid" 2>/dev/null || true
        kill -KILL "$pid" 2>/dev/null || true
        for _ in $(seq 1 20); do
            smoke_daemon_alive "$pid" || break
            sleep 0.1
        done
    fi
    if smoke_daemon_alive "$pid"; then
        echo "FAIL smoke: jawakad pid $pid survived SIGKILL" >&2
        return 1
    fi
    wait "$pid" 2>/dev/null || true
    return 0
}

#!/usr/bin/env bash
# Leaf#48 Branch C: a stop signal must not cost the session its settings.
#
# Drives the real jawaka-retroarch-runner against a stand-in RetroArch and
# asserts the three exits that matter: RetroArch quitting on its own, a stop
# signal that RetroArch answers, and a stop signal it ignores.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_REL="${BUILD:-build}"
RUNNER="$ROOT_DIR/$BUILD_REL/bin/jawaka-retroarch-runner"
FAKE_RA="$ROOT_DIR/scripts/fake-retroarch.py"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/jw-ra-runner.XXXXXX")"

cleanup() {
    status=$?
    set +e
    [ -n "${RUNNER_PID:-}" ] && kill -KILL "$RUNNER_PID" 2>/dev/null
    [ -n "${FAKE_PID:-}" ] && kill -KILL "$FAKE_PID" 2>/dev/null
    # Matched on this run's temp dir, which every fixture path contains, so a
    # concurrent build's fake RetroArch is never caught in the sweep.
    pkill -f "$TMP_DIR" 2>/dev/null
    rm -rf "$TMP_DIR"
    exit "$status"
}
trap cleanup EXIT

fail() { echo "retroarch-runner-stop-smoke: $1" >&2; exit 1; }

# A stray listener would swallow the QUIT and make this test lie about the
# runner. The port is the same one the device uses; there is no override.
if python3 - <<'PY'
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
try:
    s.bind(("127.0.0.1", 55355))
except OSError:
    sys.exit(0)
sys.exit(1)
PY
then
    fail "UDP 127.0.0.1:55355 is already in use; cannot drive the runner"
fi

make -C "$ROOT_DIR" jawaka-retroarch-runner >/dev/null

CARD="$TMP_DIR/card"
PLATFORM="$CARD/platform"
INTERNAL="$CARD/internal"
RUNTIME="$TMP_DIR/runtime"
RA_DIR="$INTERNAL/retroarch"
SHARED="$RA_DIR/retroarch.cfg"
mkdir -p "$PLATFORM/defaults" "$PLATFORM/cores" "$INTERNAL" "$RUNTIME" "$RA_DIR"

printf '%s\n' 'video_vsync = "true"' >"$PLATFORM/defaults/retroarch.cfg"

export SDCARD_PATH="$CARD"
export UMRK_PLATFORM_PATH="$PLATFORM"
export UMRK_INTERNAL_DATA_PATH="$INTERNAL"
export UMRK_RUNTIME_PATH="$RUNTIME"
export UMRK_RETROARCH_BIN="$TMP_DIR/retroarch"

# The runner execv()s this path, so it has to be an executable file.
cat >"$TMP_DIR/retroarch" <<EOF
#!/usr/bin/env bash
exec python3 "$FAKE_RA" "\$@"
EOF
chmod 755 "$TMP_DIR/retroarch"

seed_shared() {
    printf '%s\n' \
        'menu_driver = "rgui"' \
        'rewind_enable = "false"' \
        'cheevos_password = "hunter2"' >"$SHARED"
}

working_files() {
    find "$RUNTIME" -maxdepth 1 -name 'retroarch-current-*.cfg' 2>/dev/null
}

start_runner() {
    rm -f "$TMP_DIR/ready" "$TMP_DIR/quits"
    FAKE_RA_MODE="$1" FAKE_RA_READY="$TMP_DIR/ready" \
        FAKE_RA_QUIT_LOG="$TMP_DIR/quits" \
        "$RUNNER" --menu >"$TMP_DIR/runner.out" 2>&1 &
    RUNNER_PID=$!
}

wait_ready() {
    for _ in $(seq 1 100); do
        [ -f "$TMP_DIR/ready" ] && return 0
        sleep 0.1
    done
    fail "$1: stand-in RetroArch never started"
}

# ---------------------------------------------------------------- exits alone
seed_shared
start_runner exit-now
set +e
wait "$RUNNER_PID"; rc=$?
set -e
RUNNER_PID=
[ "$rc" -eq 0 ] || fail "clean exit: runner status=$rc (want 0)"
grep -q 'fake_retroarch_saved = "yes"' "$SHARED" ||
    fail "clean exit: RetroArch's save did not reach the durable config"
grep -q 'rewind_enable = "true"' "$SHARED" ||
    fail "clean exit: the changed setting did not persist"
grep -q 'cheevos_password' "$SHARED" &&
    fail "clean exit: the achievement password reached the durable config"
[ -z "$(working_files)" ] || fail "clean exit: working config left behind"

# ------------------------------------------------ stop signal, RetroArch quits
seed_shared
start_runner quit
wait_ready "stop signal"
kill -TERM "$RUNNER_PID"
set +e
wait "$RUNNER_PID"; rc=$?
set -e
RUNNER_PID=
[ "$rc" -eq 0 ] || fail "stop signal: runner status=$rc (want 0); $(cat "$TMP_DIR/runner.out")"
grep -q 'fake_retroarch_saved = "yes"' "$SHARED" ||
    fail "stop signal: RetroArch's save did not reach the durable config"
# One QUIT, not a storm: the handler only sets a flag, and the request is made
# once on the normal path.
quits=$(grep -c '^QUIT$' "$TMP_DIR/quits" 2>/dev/null || true)
[ "$quits" = "1" ] || fail "stop signal: sent $quits QUIT commands (want 1)"
[ -z "$(working_files)" ] || fail "stop signal: working config left behind"

# --------------------------------------------- stop signal, RetroArch is wedged
seed_shared
start_runner deaf
wait_ready "wedged"
FAKE_PID="$(cat "$TMP_DIR/ready")"
started=$SECONDS
kill -TERM "$RUNNER_PID"
set +e
wait "$RUNNER_PID"; rc=$?
set -e
RUNNER_PID=
elapsed=$((SECONDS - started))
[ "$elapsed" -le 15 ] ||
    fail "wedged: escalation took ${elapsed}s; it is supposed to be bounded"
[ "$rc" -eq 90 ] ||
    fail "wedged: runner status=$rc (want 90, settings not saved); $(cat "$TMP_DIR/runner.out")"
# The pre-quit promotion still ran, so the durable config must be a complete
# config -- never truncated, and never carrying the raw file's secret.
grep -q 'rewind_enable' "$SHARED" ||
    fail "wedged: durable config lost the user's setting"
grep -q 'cheevos_password' "$SHARED" &&
    fail "wedged: the achievement password reached the durable config"
kill -0 "$FAKE_PID" 2>/dev/null &&
    fail "wedged: RetroArch survived the runner"
FAKE_PID=
[ -z "$(working_files)" ] ||
    fail "wedged: the secret-bearing working config outlived RetroArch"

# --------------------------------------- stop signal that lands before the wait
# The runner installs its handlers before it prepares the config and forks, so a
# stop signal can arrive while it is still doing that work -- before it ever
# reaches the wait. Relying on EINTR alone loses that signal: the flag is set,
# no interrupt is ever delivered, and the runner sleeps until RetroArch exits on
# its own. The stand-in here never exits by itself within the test's lifetime,
# so a runner that misses the signal simply never returns.
# Repeated: exactly where the signal lands inside the runner's startup is a
# race, so one pass can miss the window and prove nothing.
for attempt in 1 2 3; do
seed_shared
start_runner quit
# Signal as soon as the runner has demonstrably begun its work -- the working
# config exists, so handlers are installed and prepare has run -- but without
# waiting for RetroArch to come up. That lands in the window this is about,
# rather than in the irreducible one before main() starts.
for _ in $(seq 1 200); do
    [ -n "$(working_files)" ] && break
    sleep 0.05
done
kill -TERM "$RUNNER_PID"
started=$SECONDS
set +e
wait "$RUNNER_PID"; rc=$?
set -e
RUNNER_PID=
elapsed=$((SECONDS - started))
[ "$elapsed" -le 20 ] ||
    fail "early stop (attempt $attempt): runner took ${elapsed}s; the signal was missed before the wait"
# Either exit is legitimate -- the race decides whether RetroArch was up in time
# to answer QUIT -- but the runner must come back promptly either way.
case "$rc" in
    0|90) ;;
    *) fail "early stop (attempt $attempt): runner status=$rc (want 0 or 90); $(cat "$TMP_DIR/runner.out")" ;;
esac
[ -z "$(working_files)" ] || fail "early stop (attempt $attempt): working config left behind"
done

echo "retroarch-runner-stop-smoke: ok"

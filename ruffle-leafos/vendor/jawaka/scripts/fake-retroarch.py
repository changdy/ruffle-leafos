#!/usr/bin/env python3
"""Stand-in for RetroArch in the retroarch-runner-stop, power-hold-save and
retroarch-frontend smoke tests.

Speaks just enough of the pinned v1.22.2 contract the runner depends on: it
binds the network command port, and on QUIT it writes its --config file the way
save-on-exit does and exits. FAKE_RA_MODE selects which failure the runner has
to cope with.

With FAKE_RA_SYNC_SAVE_BYTES set it also speaks the MLP1 command-menu patch's
synchronous save (retroarch-builds, protocol 1): GET_STATE_SAVE_INFO answers
"1 <bytes> 0" and SAVE_STATE_SYNC writes <bytes> of data to
FAKE_RA_STATES_DIR/<rom stem>.state<slot>.tmp-<id>, fsyncs it and replies
TMP_READY with that path. Without the variable both commands go unanswered,
like a RetroArch built without the patch.

FAKE_RA_FRONTEND stands in for a content pak's own RetroArch (LEAF-RA-1) and
decides how it answers jawakad's capability probe, GET_INFO then VERSION:
"answer" speaks both, "no-version" answers GET_INFO and leaves VERSION
unanswered, "unpatched" is upstream RetroArch built without Leaf's command
patch (VERSION answered, GET_INFO silent), and "silent" answers neither. The
VERSION reply is real RetroArch's: the bare version and a newline, no command
name; FAKE_RA_VERSION sets it (default 1.22.2). Every other command keeps its
usual behavior, and with the variable set GET_INFO and VERSION follow it
instead of the other modes.

FAKE_RA_LOAD_STATE adds the boot resume's side: GET_INFO answers "0 0 0" (a
core with states), and LOAD_STATE_SYNC <id> <slot> answers "OK <size>" for
FAKE_RA_STATES_DIR/<rom stem>.state<slot> ("ok"; ERROR OPEN when it is
missing), "ERROR <value>" for any other value, or nothing at all ("silent",
a RetroArch from before the command). Slot -1, the capability probe, is
refused with BAD_ARGS by any build that has the command.
"""
import os
import re
import signal
import socket
import sys
import time

PORT = 55355
REQUEST_ID = re.compile(r"^[A-Za-z0-9_-]{1,32}$")


def config_path(argv):
    for i, arg in enumerate(argv):
        if arg == "--config" and i + 1 < len(argv):
            return argv[i + 1]
    raise SystemExit("fake-retroarch: no --config")


def save(path):
    """What save-on-exit does: rewrite the whole file, one line per key."""
    with open(path, "r", encoding="utf-8", errors="replace") as fp:
        lines = fp.read().splitlines()
    out = []
    seen = False
    for line in lines:
        if line.startswith("rewind_enable"):
            if not seen:
                out.append('rewind_enable = "true"')
                seen = True
            continue
        out.append(line)
    if not seen:
        out.append('rewind_enable = "true"')
    out.append('fake_retroarch_saved = "yes"')
    tmp = path + ".fake"
    with open(tmp, "w", encoding="utf-8") as fp:
        fp.write("\n".join(out) + "\n")
    os.replace(tmp, path)


# Returned by frontend_reply() for a command it does not speak.
NOT_SPOKEN = object()


def frontend_reply(text):
    """A LEAF-RA-1 frontend's GET_INFO and VERSION replies. NOT_SPOKEN when
    FAKE_RA_FRONTEND is unset or the command is neither; None for silence."""
    mode = os.environ.get("FAKE_RA_FRONTEND")
    if not mode or text not in ("GET_INFO", "VERSION"):
        return NOT_SPOKEN
    if text == "GET_INFO":
        return "GET_INFO 0 0 0\n" if mode in ("answer", "no-version") else None
    if mode in ("answer", "unpatched"):
        return os.environ.get("FAKE_RA_VERSION", "1.22.2") + "\n"
    return None


def sync_save_reply(text, argv):
    """Protocol-1 replies for the power-hold save, or None when not spoken."""
    size = os.environ.get("FAKE_RA_SYNC_SAVE_BYTES")
    if not size:
        return None
    if text == "GET_STATE_SAVE_INFO":
        return "GET_STATE_SAVE_INFO 1 %d 0" % int(size)
    if not text.startswith("SAVE_STATE_SYNC"):
        return None
    parts = text.split()
    if len(parts) != 5 or not REQUEST_ID.match(parts[1]):
        return "SAVE_STATE_SYNC - ERROR BAD_ARGS"
    request_id, slot, max_bytes, start_by = parts[1], int(parts[2]), int(parts[3]), int(parts[4])
    if slot < 0 or slot > 999:
        return "SAVE_STATE_SYNC %s ERROR BAD_ARGS" % request_id
    error = os.environ.get("FAKE_RA_SYNC_SAVE_ERROR")
    if error:
        return "SAVE_STATE_SYNC %s ERROR %s" % (request_id, error)
    if time.monotonic() * 1000 > start_by:
        return "SAVE_STATE_SYNC %s ERROR LATE" % request_id
    data = b"S" * int(size)
    if len(data) > max_bytes:
        return "SAVE_STATE_SYNC %s ERROR TOO_LARGE" % request_id
    states_dir = os.environ.get("FAKE_RA_STATES_DIR")
    if not states_dir:
        return "SAVE_STATE_SYNC %s ERROR PATH" % request_id
    stem = os.path.splitext(os.path.basename(argv[-1]))[0]
    name = "%s.state%s" % (stem, "" if slot == 0 else slot)
    path = os.path.join(states_dir, "%s.tmp-%s" % (name, request_id))
    os.makedirs(states_dir, exist_ok=True)
    try:
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
    except FileExistsError:
        return "SAVE_STATE_SYNC %s ERROR OPEN" % request_id
    with os.fdopen(fd, "wb") as fp:
        fp.write(data)
        fp.flush()
        os.fsync(fp.fileno())
    return "SAVE_STATE_SYNC %s TMP_READY %d %s" % (request_id, len(data), path)


def load_state_reply(text, argv):
    """The boot resume's GET_INFO and LOAD_STATE_SYNC, or None when not spoken."""
    mode = os.environ.get("FAKE_RA_LOAD_STATE")
    if not mode:
        return None
    if text == "GET_INFO":
        return "GET_INFO 0 0 0"
    if not text.startswith("LOAD_STATE_SYNC") or mode == "silent":
        return None
    parts = text.split()
    if len(parts) != 3 or not REQUEST_ID.match(parts[1]):
        return "LOAD_STATE_SYNC - ERROR BAD_ARGS"
    request_id = parts[1]
    try:
        slot = int(parts[2])
    except ValueError:
        return "LOAD_STATE_SYNC - ERROR BAD_ARGS"
    if slot < 0 or slot > 999:
        return "LOAD_STATE_SYNC %s ERROR BAD_ARGS" % request_id
    if mode != "ok":
        return "LOAD_STATE_SYNC %s ERROR %s" % (request_id, mode)
    states_dir = os.environ.get("FAKE_RA_STATES_DIR", "")
    stem = os.path.splitext(os.path.basename(argv[-1]))[0]
    path = os.path.join(states_dir, "%s.state%s" % (stem, "" if slot == 0 else slot))
    try:
        size = os.path.getsize(path)
    except OSError:
        return "LOAD_STATE_SYNC %s ERROR OPEN" % request_id
    return "LOAD_STATE_SYNC %s OK %d" % (request_id, size)


def main():
    cfg = config_path(sys.argv)
    mode = os.environ.get("FAKE_RA_MODE", "quit")
    quit_log = os.environ.get("FAKE_RA_QUIT_LOG")
    ready = os.environ.get("FAKE_RA_READY")

    if mode == "deaf":
        # A wedged emulator: no command handling, and SIGTERM ignored so the
        # runner has to escalate all the way to SIGKILL.
        signal.signal(signal.SIGTERM, signal.SIG_IGN)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("127.0.0.1", PORT))
    sock.settimeout(0.2)

    if ready:
        with open(ready, "w", encoding="utf-8") as fp:
            fp.write(str(os.getpid()))

    if mode == "exit-now":
        save(cfg)
        return 0

    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        try:
            data, sender = sock.recvfrom(1024)
        except socket.timeout:
            continue
        text = data.decode("utf-8", "replace").strip()
        if quit_log and text:
            with open(quit_log, "a", encoding="utf-8") as fp:
                fp.write(text + "\n")
        reply = frontend_reply(text)
        if reply is None:
            continue
        if reply is NOT_SPOKEN:
            reply = sync_save_reply(text, sys.argv)
            if reply is None:
                reply = load_state_reply(text, sys.argv)
        if reply is not None:
            sock.sendto(reply.encode("utf-8"), sender)
            continue
        if text == "QUIT" and mode == "quit":
            save(cfg)
            return 0
    return 3


if __name__ == "__main__":
    sys.exit(main())

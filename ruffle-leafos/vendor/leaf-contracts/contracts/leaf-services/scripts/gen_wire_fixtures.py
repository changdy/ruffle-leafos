#!/usr/bin/env python3
"""Generate cross-language wire fixtures for CTL-1 and LIFE-1.

Each fixture is a frame: a 4-byte big-endian (network order) length
prefix followed by the UTF-8 JSON payload, exactly as
internal/ipc/ipc.c frames it (contracts.md "Wire format for CTL-1 and
LIFE-1"). The point of this fixture set is that a framing disagreement
between the C server and the Go client is not catchable by JSON
fixtures alone -- both implementations must decode these exact bytes
and, on re-encoding the same logical message, reproduce these exact
bytes again (the canonical form is compact JSON, keys in the order
given here, no trailing newline inside the payload).

Writes <name>.bin (the frame) and <name>.json (the logical message,
pretty-printed for humans, plus expected byte-length metadata) for each
case, and a MANIFEST.json summarizing all of them with a sha256 of the
frame bytes so a consuming repo's CI can spot-check without re-deriving
from JSON.
"""
from __future__ import annotations

import hashlib
import json
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), "wire-fixtures")


def frame(payload_obj_ordered_json: str) -> bytes:
    payload = payload_obj_ordered_json.encode("utf-8")
    return struct.pack(">I", len(payload)) + payload


def compact(pairs: list[tuple[str, object]]) -> str:
    """Compact JSON with an explicit, canonical key order (dict preserves
    insertion order in Python 3.7+, and json.dumps respects it when
    sort_keys is not set)."""
    obj = dict(pairs)
    return json.dumps(obj, separators=(",", ":"), ensure_ascii=False)


CASES = []

SEMANTIC_CEILING_BYTES = 65536


def add(name: str, description: str, pairs: list[tuple[str, object]],
        expect_semantic_ceiling_violation: bool = False) -> None:
    CASES.append((name, description, pairs, expect_semantic_ceiling_violation))


def logs_response_of_size(request_id: str, target_payload_bytes: int) -> list[tuple[str, object]]:
    """A schema-valid logs_response (v, id, lines, redacted) whose compact
    JSON encoding is exactly target_payload_bytes long, achieved by sizing
    the single log line -- no extra/unknown field is introduced, so this
    stays valid against the closed logs_response schema. Each ASCII 'A'
    costs exactly one UTF-8 byte, so the size is exact, not approximate."""
    def build(padlen: int) -> list[tuple[str, object]]:
        return [("v", 1), ("id", request_id), ("lines", ["A" * padlen]), ("redacted", False)]

    base_len = len(compact(build(0)).encode("utf-8"))
    pad_needed = target_payload_bytes - base_len
    if pad_needed < 0:
        raise ValueError(f"target {target_payload_bytes} too small (needs >= {base_len})")
    return build(pad_needed)


# ---- CTL-1 --------------------------------------------------------------

add("ctl1_list_request", "CTL-1 list request, no service_id needed.",
    [("v", 1), ("op", "list"), ("id", "1")])

add("ctl1_list_response", "CTL-1 list response with one service.",
    [("v", 1), ("id", "1"), ("services", [
        {"service_id": "org.umrk.syncthing", "desired_enabled": False, "effective_state": "disabled"},
    ])])

add("ctl1_status_request", "CTL-1 status request for a specific service.",
    [("v", 1), ("op", "status"), ("id", "2"), ("service_id", "org.umrk.syncthing")])

add("ctl1_status_response", "CTL-1 status response, running + subscribed.",
    [("v", 1), ("id", "2"), ("service_id", "org.umrk.syncthing"),
     ("desired_enabled", True), ("effective_state", "running"),
     ("coordination", "subscribed")])

add("ctl1_run_request", "CTL-1 session run request.",
    [("v", 1), ("op", "run"), ("id", "3"), ("service_id", "org.umrk.syncthing")])

add("ctl1_ack_response", "CTL-1 generic ack (e.g. for run/stop/enable/disable).",
    [("v", 1), ("id", "3"), ("ok", True)])

add("ctl1_error_unsupported_version", "CTL-1 error: client sent a v this server cannot serve.",
    [("v", 1), ("id", "4"),
     ("error", {"code": "unsupported-version", "message": "server supports v1 only", "supported_versions": [1]})])

add("ctl1_capabilities_request", "CTL-1 capabilities request.",
    [("v", 1), ("op", "capabilities"), ("id", "5")])

add("ctl1_capabilities_response", "CTL-1 capabilities response.",
    [("v", 1), ("id", "5"), ("capabilities", ["app-services-v1", "control-ipc-v1", "game-coordination-v1"])])

add("ctl1_logs_response_at_semantic_ceiling",
    f"CTL-1 logs response payload at exactly the {SEMANTIC_CEILING_BYTES}-byte "
    "(64 KiB) CTL-1/LIFE-1 semantic ceiling -- valid; a conforming server must "
    "accept this.",
    logs_response_of_size("9", SEMANTIC_CEILING_BYTES))

add("ctl1_logs_response_over_semantic_ceiling",
    f"CTL-1 logs response payload one byte over the {SEMANTIC_CEILING_BYTES}-byte "
    "semantic ceiling. The transport (16 MiB JW_IPC_MAX_FRAME) happily carries "
    "this frame; contracts.md is explicit that CTL-1/LIFE-1 treat anything past "
    "64 KiB as a protocol error regardless. This is not a JSON-shape defect -- "
    "the payload is otherwise a perfectly valid logs_response -- so an "
    "implementation that only schema-validates shape and never checks payload "
    "size would wrongly accept it.",
    logs_response_of_size("9", SEMANTIC_CEILING_BYTES + 1),
    expect_semantic_ceiling_violation=True)

# ---- LIFE-1 ---------------------------------------------------------------

add("life1_subscribe_request", "LIFE-1 subscribe, default notify/250ms/0ms.",
    [("v", 1), ("op", "subscribe"), ("id", "1"), ("events", ["game"]),
     ("service_id", "org.umrk.syncthing"), ("mode", "notify"), ("ack_ms", 250), ("wait_ms", 0)])

add("life1_subscribe_check_request", "LIFE-1 stop subscription with check-before-stop capability.",
    [("v", 1), ("op", "subscribe"), ("id", "1"), ("events", ["game"]),
     ("service_id", "org.umrk.syncthing"), ("mode", "stop"), ("ack_ms", 250),
     ("wait_ms", 15000), ("check_before_stop", True)])

add("life1_subscribe_ack", "LIFE-1 subscribe ack.",
    [("v", 1), ("id", "1"), ("ok", True)])

add("life1_subscribe_reject_stale_generation", "LIFE-1 subscribe rejected: stale-generation peer.",
    [("v", 1), ("id", "2"),
     ("error", {"code": "stale-generation-peer", "message": "subscriber pid is not a member of the current generation's reserved process group"})])

add("life1_game_start_event", "LIFE-1 game.start event, unsolicited (no id).",
    [("v", 1), ("event", "game.start"), ("launch_id", "launch-0001"),
     ("source_id", "secondary_sd"), ("saves_path", "/media/sdcard1/Saves/nes"),
     ("states_path", "/media/sdcard1/States/nes"), ("wait_budget_ms", 15000)])

add("life1_game_check_event", "LIFE-1 game.check event before a mandatory verified stop.",
    [("v", 1), ("event", "game.check"), ("launch_id", "launch-0001"),
     ("source_id", "secondary_sd"), ("saves_path", "/media/sdcard1/Saves/nes"),
     ("states_path", "/media/sdcard1/States/nes"), ("wait_budget_ms", 15000)])

add("life1_waiting_status", "LIFE-1 waiting status reply (zero or more before ready/error).",
    [("v", 1), ("status", "waiting"), ("launch_id", "launch-0001"), ("pending_items", 3)])

add("life1_check_waiting_status", "LIFE-1 check waiting status with bounded pending items and bytes.",
    [("v", 1), ("status", "waiting"), ("launch_id", "launch-0001"),
     ("pending_items", 3), ("pending_bytes", 49152)])

add("life1_ready_status", "LIFE-1 ready status reply, terminal.",
    [("v", 1), ("status", "ready"), ("launch_id", "launch-0001")])

add("life1_stop_status", "LIFE-1 check terminal requesting Jawaka's verified stop.",
    [("v", 1), ("status", "stop"), ("launch_id", "launch-0001")])

add("life1_error_status", "LIFE-1 error status reply, terminal.",
    [("v", 1), ("status", "error"), ("launch_id", "launch-0001"), ("reason", "upstream-rest-api-timeout")])

add("life1_error_status_unicode",
    "LIFE-1 error status reply whose reason contains multi-byte UTF-8 characters "
    "(three 2-byte code points and one 4-byte code point) -- catches an implementation that counts "
    "Unicode characters instead of encoded UTF-8 bytes for the length prefix.",
    [("v", 1), ("status", "error"), ("launch_id", "launch-0001"), ("reason", "upstream café déjà-vu \U0001f525")])

add("life1_game_cancel_event", "LIFE-1 game.cancel event (Start now / budget expiry).",
    [("v", 1), ("event", "game.cancel"), ("launch_id", "launch-0001")])

add("life1_game_abort_event", "LIFE-1 game.abort event for a known-not-started launch.",
    [("v", 1), ("event", "game.abort"), ("launch_id", "launch-0001")])

add("life1_game_finish_event", "LIFE-1 game.finish event, fire-and-forget.",
    [("v", 1), ("event", "game.finish"), ("launch_id", "launch-0001")])

add("life1_game_state_request", "LIFE-1 game.state query.",
    [("v", 1), ("op", "game.state"), ("id", "7")])

add("life1_game_state_response_active", "LIFE-1 game.state response, an authoritative launch is active.",
    [("v", 1), ("id", "7"), ("active", True), ("launch_id", "launch-0001"),
     ("source_id", "secondary_sd"), ("saves_path", "/media/sdcard1/Saves/nes"),
     ("states_path", "/media/sdcard1/States/nes")])

add("life1_game_state_response_inactive", "LIFE-1 game.state response, no active launch.",
    [("v", 1), ("id", "7"), ("active", False)])


def main() -> None:
    # Only remove files this generator owns: <name>.bin/<name>.json for each
    # entry in CASES, plus MANIFEST.json. Everything else under wire-fixtures/
    # (e.g. the hand-authored ctl1_logs_redaction_case.json) is left alone --
    # a prior version of this script deleted the whole directory contents and
    # silently dropped that hand-authored fixture on every regeneration.
    owned = {"MANIFEST.json"}
    for name, _description, _pairs, _ceiling in CASES:
        owned.add(f"{name}.bin")
        owned.add(f"{name}.json")

    if os.path.isdir(OUT):
        for f in os.listdir(OUT):
            if f in owned:
                os.remove(os.path.join(OUT, f))
    else:
        os.makedirs(OUT)

    manifest = []
    for name, description, pairs, expect_ceiling_violation in CASES:
        payload_json = compact(pairs)
        frame_bytes = frame(payload_json)
        length_prefix_value = len(payload_json.encode("utf-8"))

        bin_path = os.path.join(OUT, f"{name}.bin")
        with open(bin_path, "wb") as f:
            f.write(frame_bytes)

        json_path = os.path.join(OUT, f"{name}.json")
        with open(json_path, "w") as f:
            json.dump({
                "description": description,
                "message": dict(pairs),
                "canonical_payload_utf8": payload_json,
                "frame_total_bytes": len(frame_bytes),
                "length_prefix_value": length_prefix_value,
                "semantic_ceiling_bytes": SEMANTIC_CEILING_BYTES,
                "expect_semantic_ceiling_violation": expect_ceiling_violation,
            }, f, indent=2)
            f.write("\n")

        manifest.append({
            "name": name,
            "description": description,
            "frame_total_bytes": len(frame_bytes),
            "length_prefix_value": length_prefix_value,
            "expect_semantic_ceiling_violation": expect_ceiling_violation,
            "sha256_of_frame": hashlib.sha256(frame_bytes).hexdigest(),
        })

    with open(os.path.join(OUT, "MANIFEST.json"), "w") as f:
        json.dump({
            "$comment": "Cross-language wire fixtures for CTL-1/LIFE-1. Each <name>.bin is "
                        "a complete frame: 4-byte big-endian length prefix + UTF-8 JSON "
                        "payload. A conforming C or Go implementation must decode <name>.bin "
                        "into the message described in <name>.json, and re-encoding that same "
                        "logical message must reproduce <name>.bin byte-for-byte "
                        "(sha256_of_frame is the byte-for-byte check). This is what a JSON-only "
                        "fixture cannot catch: a framing or byte-order disagreement between the "
                        "two languages (B-IPC-01). expect_semantic_ceiling_violation marks the "
                        "two payload-size boundary cases at/over the 64 KiB CTL-1/LIFE-1 semantic "
                        "ceiling (distinct from the 16 MiB transport ceiling); "
                        "life1_error_status_unicode is a non-ASCII-payload case that catches an "
                        "implementation counting Unicode characters instead of UTF-8 bytes.",
            "frame_format": "4-byte unsigned big-endian length prefix, then that many bytes of UTF-8 JSON payload.",
            "fixtures": manifest,
        }, f, indent=2)
        f.write("\n")

    print(f"Generated {len(manifest)} wire fixtures in {OUT}")


if __name__ == "__main__":
    main()

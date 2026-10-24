#!/usr/bin/env python3
import json
import socket
import sqlite3
import struct
import sys
import time


def request(socket_path, payload):
    body = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode()
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.connect(socket_path)
        sock.sendall(struct.pack("!I", len(body)) + body)
        size = struct.unpack("!I", sock.recv(4))[0]
        data = bytearray()
        while len(data) < size:
            chunk = sock.recv(size - len(data))
            if not chunk:
                raise RuntimeError("short IPC response")
            data.extend(chunk)
    return json.loads(data)


def status(socket_path):
    return request(socket_path, {"type": "library-status"})


def wait_idle(socket_path, minimum_generation, timeout=15.0):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        last = status(socket_path)
        if (last.get("generation", 0) >= minimum_generation
                and not last.get("scan_running", False)
                and not last.get("pending_rescan", False)):
            return last
        time.sleep(0.02)
    raise RuntimeError(f"scan did not become idle: {last}")


def effective_name(db, rom_path):
    row = db.execute(
        """SELECT COALESCE(NULLIF(manual.value,''), NULLIF(imported.value,''), g.name)
           FROM games g
           LEFT JOIN game_settings manual
             ON manual.game_id=g.id AND manual.key='display_name'
           LEFT JOIN game_settings imported
             ON imported.game_id=g.id AND imported.key='imported_display_name'
           WHERE g.rom_path=?""",
        (rom_path,),
    ).fetchone()
    return row[0] if row else None


def main():
    socket_path, db_path, primary, secondary, outside = sys.argv[1:]
    baseline = wait_idle(socket_path, 0)["generation"]

    ps_paths = [f"{primary}/Roms/PS/game.cue", f"{primary}/Roms/PS/track.bin"]
    p8_paths = [f"{secondary}/Roms/PICO8/cart-a.p8", f"{secondary}/Roms/PICO8/cart-b.p8"]
    first = request(socket_path, {
        "type": "scan-library",
        "title_groups": [{
            "provider": "org.umrk.itchio",
            "title": "Black Jewel Reborn",
            "rom_paths": ps_paths,
        }],
    })
    second = request(socket_path, {
        "type": "scan-library",
        "title_groups": [{
            "provider": "org.umrk.itchio",
            "title": "Pocket Collection",
            "rom_paths": p8_paths,
        }],
    })
    third = request(socket_path, {
        "type": "scan-library",
        "title_groups": [{
            "provider": "org.umrk.itchio",
            "title": "Newest Pocket Collection",
            "rom_paths": p8_paths,
        }],
    })
    if first.get("type") != "ok" or first.get("title_hints_accepted") != 2:
        raise RuntimeError(f"primary title request failed: {first}")
    if second.get("type") != "ok" or second.get("title_hints_accepted") != 2:
        raise RuntimeError(f"secondary title request failed: {second}")
    if third.get("type") != "ok" or third.get("title_hints_accepted") != 2:
        raise RuntimeError(f"newest queued title request failed: {third}")
    if second.get("action") != "scan-library queued" or third.get("action") != "scan-library queued":
        raise RuntimeError(f"title requests did not exercise queued merge: {second}, {third}")
    wait_idle(socket_path, baseline + 2)

    db = sqlite3.connect(db_path)
    primary_name = effective_name(db, "Roms/PS/game.cue")
    if primary_name != "Black Jewel Reborn":
        rows = db.execute("SELECT rom_path,name FROM games WHERE rom_path LIKE '%game.cue'").fetchall()
        raise RuntimeError(f"primary CUE title={primary_name!r} rows={rows!r}")
    secondary_a = effective_name(db, f"{secondary}/Roms/PICO8/cart-a.p8")
    secondary_b = effective_name(db, f"{secondary}/Roms/PICO8/cart-b.p8")
    if secondary_a != "Newest Pocket Collection — cart-a":
        raise RuntimeError(f"secondary multi-ROM title A={secondary_a!r} was not disambiguated")
    if secondary_b != "Newest Pocket Collection — cart-b":
        raise RuntimeError(f"secondary multi-ROM title B={secondary_b!r} was not disambiguated")
    bin_imports = db.execute(
        """SELECT COUNT(*) FROM game_settings s JOIN games g ON g.id=s.game_id
           WHERE g.rom_path='Roms/PS/track.bin' AND s.key='imported_display_name'"""
    ).fetchone()[0]
    if bin_imports != 0:
        raise RuntimeError("CUE companion BIN received imported title metadata")

    game_id = db.execute("SELECT id FROM games WHERE rom_path='Roms/PS/game.cue'").fetchone()[0]
    db.execute(
        "INSERT INTO game_settings(game_id,key,value,updated_at) VALUES(?,?,?,strftime('%s','now'))",
        (game_id, "display_name", "My Manual Name"),
    )
    db.commit()
    if effective_name(db, "Roms/PS/game.cue") != "My Manual Name":
        raise RuntimeError("manual display name did not win")
    db.execute("DELETE FROM game_settings WHERE game_id=? AND key='display_name'", (game_id,))
    db.commit()
    if effective_name(db, "Roms/PS/game.cue") != "Black Jewel Reborn":
        raise RuntimeError("manual reset did not restore imported title")
    db.close()

    rejected = request(socket_path, {
        "type": "scan-library",
        "title_groups": [{
            "provider": "org.umrk.itchio",
            "title": "Outside",
            "rom_paths": [outside],
        }],
    })
    if rejected.get("type") != "error":
        raise RuntimeError(f"outside-ROM path was accepted: {rejected}")
    malformed = request(socket_path, {
        "type": "scan-library",
        "title_groups": [{
            "provider": "not valid!",
            "title": "Bad",
            "rom_paths": ps_paths[:1],
        }],
    })
    if malformed.get("type") != "error":
        raise RuntimeError(f"malformed provider was accepted: {malformed}")

    print("PASS imported-title-ipc-smoke")


if __name__ == "__main__":
    main()

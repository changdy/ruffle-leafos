#!/usr/bin/env python3
"""Exercise deletion IPC against a real daemon and disposable ROMs only."""

import json
import os
from pathlib import Path
import signal
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time


PREPARING, READY, COMMITTING, DONE, ERROR = range(5)


class Client:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(3)
        self.sock.connect(str(path))

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.sock.close()

    def receive(self, size):
        result = bytearray()
        while len(result) < size:
            chunk = self.sock.recv(size - len(result))
            if not chunk:
                raise RuntimeError("daemon closed the retained deletion connection")
            result.extend(chunk)
        return bytes(result)

    def request(self, kind, **fields):
        data = json.dumps({"type": kind, **fields}, separators=(",", ":")).encode()
        self.sock.sendall(struct.pack("!I", len(data)) + data)
        size = struct.unpack("!I", self.receive(4))[0]
        assert size <= 16 * 1024 * 1024, size
        return json.loads(self.receive(size))

    def settle(self, reply):
        deadline = time.monotonic() + 15
        while reply.get("phase") == COMMITTING:
            assert time.monotonic() < deadline, reply
            time.sleep(0.02)
            reply = self.request("rom-delete-status")
        return reply

    def preview(self, path, source="primary"):
        return self.request("rom-delete-preview", source_id=source, rom_relpath=path)

    def delete(self, path, digest, source="primary"):
        return self.settle(self.request("rom-delete", source_id=source, rom_relpath=path,
                                        digest=digest))


def rejected(reply):
    assert reply.get("phase") == ERROR or reply.get("type") == "error", reply


def ready(reply):
    assert reply.get("phase") == READY and len(reply.get("digest", "")) == 16, reply
    return reply["digest"]


def files(reply):
    return {item["rom_relpath"] for item in reply["files"]}


def write(path, contents):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(contents if isinstance(contents, bytes) else contents.encode())


class Fixture:
    def __init__(self, root, daemon):
        self.root, self.daemon = root, daemon
        self.primary, self.secondary = root / "primary", root / "secondary"
        self.runtime, self.state, self.platform = root / "runtime", root / "state", root / "platform"
        self.socket, self.db_path = self.runtime / "d.sock", self.state / "library.db"
        self.log, self.proc, self.log_file = root / "daemon.log", None, None
        self.readonly_marker = root / "readonly-source"
        self.failure_marker = root / "fail-after"
        for directory in (self.runtime, self.state, self.platform / "defaults",
                          self.primary / "Apps/shared", self.secondary / "Roms/GBA"):
            directory.mkdir(parents=True)
        for name in ("Delete", "Keep", "Cancel", "Stale", "ReadOnly", "LostResponse", "Serialize", "Restart"):
            write(self.primary / f"Roms/GBA/{name}.gba", f"{name} disposable payload\n")
        write(self.secondary / "Roms/GBA/Delete.gba", "Other card must survive\n")
        write(self.secondary / "Roms/GBA/ReadOnly.gba", "Readonly card must survive\n")
        write(self.primary / "Roms/PS/Owner.m3u", "Disc.cue\n")
        write(self.primary / "Roms/PS/Disc.cue", 'FILE "Track.bin" BINARY\n  TRACK 01 MODE2/2352\n')
        write(self.primary / "Roms/PS/Track.bin", b"track payload\n")
        write(self.primary / "Roms/PS/Partial.m3u", "PartialDisc.cue\n")
        write(self.primary / "Roms/PS/PartialDisc.cue", 'FILE "PartialTrack.bin" BINARY\n  TRACK 01 MODE2/2352\n')
        write(self.primary / "Roms/PS/PartialTrack.bin", b"partial payload\n")
        # Two "(Disc N)" images the scanner groups into a generated playlist.
        for disc in (1, 2):
            write(self.primary / f"Roms/PS/Twin (Disc {disc}).cue", f'FILE "Twin (Disc {disc}).bin" BINARY\n  TRACK 01 MODE2/2352\n')
            write(self.primary / f"Roms/PS/Twin (Disc {disc}).bin", f"twin disc {disc}\n")
        systems = []
        for code, extensions, playlists in (("GBA", ["gba", "zip", "7z"], []),
                                            ("PS", ["cue", "img", "pbp", "chd", "toc"], ["m3u"])):
            systems.append({
                "id": code, "name": code, "patterns": [code], "extensions": extensions,
                "archive_extensions": [], "archive_inner_extensions": extensions,
                "archive_mode": "pass_through", "file_names": [], "ignore_file_names": [],
                "playlist_extensions": playlists, "m3u_generation": "manual" if playlists else "none",
                "default_core": "fixture", "alternate_cores": [],
                "rom_root": f"Roms/{code}", "image_root": f"Images/{code}",
            })
        write(self.platform / "defaults/systems.json", json.dumps({"platform": "mac", "systems": systems}))
        write(self.platform / "defaults/cores.json", json.dumps({"platform": "mac", "cores": [{
            "id": "fixture", "display_name": "Fixture", "type": "retroarch",
            "file_name": "fixture_libretro.dylib", "config_folder": "Fixture", "status": "packaged",
        }]}))

    def start(self):
        self.socket.unlink(missing_ok=True)
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(("UMRK_", "JAWAKA_", "SDCARD_PATH", "ROMS_PATH",
                                      "USERDATA_PATH", "APPS_PATH", "IMAGES_PATH", "LOGS_PATH"))}
        env.update(PLATFORM="mac", SDCARD_PATH=str(self.primary),
                   SDCARD_PATHS=f"{self.primary}:{self.secondary}",
                   UMRK_RUNTIME_PATH=str(self.runtime), UMRK_DAEMON_SOCKET=str(self.socket),
                   UMRK_INTERNAL_DATA_PATH=str(self.state), UMRK_PLATFORM_PATH=str(self.platform),
                   USERDATA_PATH=str(self.primary / ".userdata/mac"),
                   JAWAKA_OSD="0", JAWAKA_SDCARD_ROOT=str(self.primary),
                   JAWAKA_TEST_DELETE_READONLY_FILE=str(self.readonly_marker),
                   JAWAKA_TEST_DELETE_FAIL_AFTER_FILE=str(self.failure_marker),
                   JAWAKA_TEST_DELETE_COMMIT_DELAY_MS="500")
        self.log_file = self.log.open("ab")
        self.proc = subprocess.Popen([str(self.daemon), "--daemon-only"], env=env,
                                     cwd=Path(__file__).resolve().parents[1], stdout=self.log_file,
                                     stderr=subprocess.STDOUT, start_new_session=True)
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            assert self.proc.poll() is None, "daemon exited during startup"
            if self.socket.exists():
                try:
                    with Client(self.socket) as client:
                        status = client.request("library-status")
                    if status.get("generation", 0) > 0 and not status.get("scan_running", False):
                        assert "using compatibility scanner" not in self.log.read_text()
                        return status["generation"]
                except (ConnectionError, FileNotFoundError):
                    pass
            time.sleep(0.02)
        raise RuntimeError("daemon scan did not become idle")

    def stop(self):
        if self.proc and self.proc.poll() is None:
            os.killpg(self.proc.pid, signal.SIGTERM)
            try:
                self.proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                os.killpg(self.proc.pid, signal.SIGKILL)
                self.proc.wait(timeout=3)
                raise RuntimeError("daemon did not stop cleanly")
        if self.log_file:
            self.log_file.close()
        self.proc = None


def wait_idle(fixture, minimum_generation):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        with Client(fixture.socket) as client:
            status = client.request("library-status")
        if status.get("generation", 0) >= minimum_generation and not status.get("scan_running", False):
            return status["generation"]
        time.sleep(0.02)
    raise RuntimeError(f"queued scan did not finish: {status}")


def assert_serialized(fixture, owner, phase):
    for kind, fields in (
        ("library-relocate-prepare", {"operation_id": "deletion-busy", "expected_generation": 0,
                                     "items": [{"old": {"source_id": "primary", "rom_relpath": "GBA/Cancel.gba"},
                                                "new": {"source_id": "secondary_sd", "rom_relpath": "GBA/Cancel.gba"}}]}),
        ("storage-action", {"source": "secondary_sd", "action": "safe-unmount"}),
        ("launch-game", {"system": "GBA", "rom_path": "Roms/GBA/Cancel.gba"}),
    ):
        with Client(fixture.socket) as client:
            reply = client.request(kind, **fields)
        assert reply.get("type") == "error" and "ROM deletion" in reply.get("message", ""), reply
    with Client(fixture.socket) as client:
        queued = client.request("scan-library")
    assert queued.get("action") == "scan-library queued", queued
    with Client(fixture.socket) as client:
        status = client.request("library-status")
    assert not status.get("scan_running"), status
    assert owner.request("rom-delete-status").get("phase") == phase
    return status["generation"]


def test_serialization(fixture):
    with Client(fixture.socket) as client:
        digest = ready(client.preview("GBA/Serialize.gba"))
        committing = client.request("rom-delete", source_id="primary", rom_relpath="GBA/Serialize.gba",
                                    digest=digest)
        assert committing.get("phase") == COMMITTING, committing
        generation = assert_serialized(fixture, client, COMMITTING)
        with Client(fixture.socket) as other:
            rejected(other.preview("GBA/Cancel.gba"))  # one deletion at a time
        result = client.settle(committing)
        assert result.get("phase") == DONE, result
    wait_idle(fixture, generation + 2)  # One deletion publication, then the queued scan.
    assert not (fixture.primary / "Roms/GBA/Serialize.gba").exists()


def test_partial_failure(fixture):
    primary = fixture.primary
    parent_path = primary / "Roms/PS/Partial.m3u"
    original_playlist = parent_path.read_bytes()
    with sqlite3.connect(fixture.db_path) as db:
        parent = db.execute("SELECT id FROM games WHERE source_id='primary' AND rom_relpath='PS/Partial.m3u'").fetchone()[0]
        keep = db.execute("SELECT id FROM games WHERE source_id='primary' AND rom_relpath='GBA/Keep.gba'").fetchone()[0]
        db.execute("INSERT INTO game_settings(game_id,key,value,updated_at) VALUES(?,'display_name','Keep parent metadata',1)", (parent,))
        db.execute("INSERT INTO favorites VALUES('game',?,1)", (parent,))
        db.execute("INSERT INTO recents VALUES('game',?,1,20)", (parent,))
        db.execute("INSERT OR REPLACE INTO settings VALUES('five_game_ids',?)", (f"{parent},{keep}",))
        for path, member in (("PS/PartialTrack.bin", ""), ("PS/PartialTrack.bin", "member"),
                             ("PS/PartialDisc.cue", ""), ("PS/Partial.m3u", "")):
            db.execute("INSERT INTO hidden_roms VALUES('primary',?,?)", (path, member))
    write(fixture.failure_marker, "1")
    with Client(fixture.socket) as client:
        digest = ready(client.preview("PS/Partial.m3u"))
        result = client.delete("PS/Partial.m3u", digest)
        rejected(result)
        assert result.get("removed_count") == 1, result
    assert not (primary / "Roms/PS/PartialTrack.bin").exists()
    assert (primary / "Roms/PS/PartialDisc.cue").exists()
    assert parent_path.read_bytes() == original_playlist
    with sqlite3.connect(fixture.db_path) as db:
        assert db.execute("SELECT id FROM games WHERE id=?", (parent,)).fetchone()
        assert db.execute("SELECT value FROM game_settings WHERE game_id=? AND key='display_name'", (parent,)).fetchone()[0] == "Keep parent metadata"
        assert not db.execute("SELECT 1 FROM hidden_roms WHERE rom_relpath='PS/PartialTrack.bin'").fetchone()
        assert db.execute("SELECT 1 FROM hidden_roms WHERE rom_relpath='PS/PartialDisc.cue'").fetchone()
        assert db.execute("SELECT value FROM settings WHERE key='five_game_ids'").fetchone()[0] == f"{parent},{keep}"
    fixture.failure_marker.unlink()
    with Client(fixture.socket) as client:
        preview = client.preview("PS/Partial.m3u")
        digest = ready(preview)
        assert any(item.get("missing") and item["rom_relpath"] == "PS/PartialTrack.bin" for item in preview["files"]), preview
        result = client.delete("PS/Partial.m3u", digest)
        assert result.get("phase") == DONE and result.get("removed_count") == 2, result
        assert result.get("absent_count") == 1, result
    assert not parent_path.exists() and not (primary / "Roms/PS/PartialDisc.cue").exists()
    with sqlite3.connect(fixture.db_path) as db:
        assert not db.execute("SELECT id FROM games WHERE id=?", (parent,)).fetchone()
        assert not db.execute("SELECT 1 FROM hidden_roms WHERE rom_relpath LIKE 'PS/Partial%'").fetchone()
        assert db.execute("SELECT value FROM settings WHERE key='five_game_ids'").fetchone()[0] == str(keep)


def library_generation(fixture):
    with Client(fixture.socket) as client:
        return client.request("library-status")["generation"]


def scan(fixture):
    before = library_generation(fixture)
    with Client(fixture.socket) as client:
        client.request("scan-library")
    wait_idle(fixture, before + 1)


def rows(fixture, pattern):
    with sqlite3.connect(fixture.db_path) as db:
        return {row[0] for row in db.execute("SELECT rom_relpath FROM games WHERE rom_relpath LIKE ?", (pattern,))}


def test_hash_filenames(fixture):
    """A '#' outside archive.zip#member is a filename, and the playlist owns its disc."""
    ps = fixture.primary / "Roms/PS"
    write(ps / "Hash #1 (Disc 1).cue", 'FILE "Hash #1 (Disc 1).bin" BINARY\n  TRACK 01 MODE2/2352\n')
    write(ps / "Hash #1 (Disc 1).bin", b"hash payload\n")
    write(ps / "Hash #1.m3u", "Hash #1 (Disc 1).cue\n")
    scan(fixture)
    # Scanner grouping is unchanged, so the CUE is listed beside its playlist.
    assert rows(fixture, "PS/Hash%") == {"PS/Hash #1.m3u", "PS/Hash #1 (Disc 1).cue"}
    with Client(fixture.socket) as client:
        preview = client.preview("PS/Hash #1.m3u")
        digest = ready(preview)
        assert files(preview) == {"PS/Hash #1.m3u", "PS/Hash #1 (Disc 1).cue", "PS/Hash #1 (Disc 1).bin"}, preview
        result = client.delete("PS/Hash #1.m3u", digest)
        assert result.get("phase") == DONE and result.get("removed_count") == 3, result
    # Deleting the playlist removed the separately listed disc's row too.
    assert not rows(fixture, "PS/Hash%")


def test_generated_playlist(fixture):
    """Deleting a "(Disc N)" game leaves nothing a rescan can turn back into it."""
    ps = fixture.primary / "Roms/PS"
    assert (ps / "Twin.m3u").exists() and rows(fixture, "PS/Twin%") == {"PS/Twin.m3u"}
    with Client(fixture.socket) as client:
        preview = client.preview("PS/Twin.m3u")
        digest = ready(preview)
        assert len(preview["files"]) == 5 and preview["files"][-1]["rom_relpath"] == "PS/Twin.m3u", preview
        result = client.delete("PS/Twin.m3u", digest)
        assert result.get("phase") == DONE and result.get("removed_count") == 5, result
    scan(fixture)
    assert not rows(fixture, "PS/Twin%") and not list(ps.glob("Twin*"))


def run(fixture):
    initial_generation = fixture.start()
    primary = fixture.primary
    test_serialization(fixture)
    test_partial_failure(fixture)

    # The preview keeps nothing: closing it is all Cancel needs.
    with Client(fixture.socket) as client:
        ready(client.preview("GBA/Cancel.gba"))
        rejected(client.request("rom-delete-status"))
    with Client(fixture.socket) as client:
        rejected(client.request("rom-delete", source_id="primary", rom_relpath="GBA/Cancel.gba", digest="short"))
        rejected(client.request("rom-delete", source_id="primary", rom_relpath="GBA/Cancel.gba"))
    assert (primary / "Roms/GBA/Cancel.gba").exists()

    # A different file set (here a different size) refuses the reviewed digest.
    with Client(fixture.socket) as client:
        digest = ready(client.preview("GBA/Stale.gba"))
        write(primary / "Roms/GBA/Stale.gba", "changed after review, must survive\n")
        result = client.delete("GBA/Stale.gba", digest)
        rejected(result)
        assert "changed" in result.get("error", ""), result
    with Client(fixture.socket) as client:
        rejected(client.delete("GBA/Stale.gba", digest))
    assert (primary / "Roms/GBA/Stale.gba").read_text() == "changed after review, must survive\n"

    # The test daemon consults this marker through its storage guard only.
    for source in ("primary", "secondary_sd"):
        write(fixture.readonly_marker, source)
        with Client(fixture.socket) as client:
            result = client.preview("GBA/ReadOnly.gba", source)
            rejected(result)
            assert result.get("readonly_source") == ("launcher_sd" if source == "primary" else source), result
            assert not result.get("digest"), result
        fixture.readonly_marker.unlink()
    with Client(fixture.socket) as client:
        digest = ready(client.preview("GBA/ReadOnly.gba"))
        write(fixture.readonly_marker, "primary")
        result = client.delete("GBA/ReadOnly.gba", digest)
        rejected(result)
        assert result.get("readonly_source") == "launcher_sd", result
        fixture.readonly_marker.unlink()
    assert (primary / "Roms/GBA/ReadOnly.gba").exists()
    assert (fixture.secondary / "Roms/GBA/ReadOnly.gba").exists()

    # A card that isn't inserted is not checked or mentioned.
    missing_card = fixture.secondary.with_name("secondary-away")
    fixture.secondary.rename(missing_card)
    try:
        with Client(fixture.socket) as client:
            preview = client.preview("GBA/Cancel.gba")
            ready(preview)
            assert files(preview) == {"GBA/Cancel.gba"}, preview
    finally:
        missing_card.rename(fixture.secondary)

    # Deleting a playlist also deletes a disc that is listed on its own.
    with sqlite3.connect(fixture.db_path) as db:
        db.execute("INSERT OR IGNORE INTO games(system,name,source_id,rom_relpath,rom_path) "
                   "VALUES('PS','Standalone disc','primary','PS/Disc.cue','Roms/PS/Disc.cue')")
    with Client(fixture.socket) as client:
        preview = client.preview("PS/Owner.m3u")
        digest = ready(preview)
        assert files(preview) == {"PS/Owner.m3u", "PS/Disc.cue", "PS/Track.bin"}, preview
        result = client.delete("PS/Owner.m3u", digest)
        assert result.get("phase") == DONE and result.get("removed_count") == 3, result
    assert not rows(fixture, "PS/Owner%") and not rows(fixture, "PS/Disc.cue")

    protected = {
        primary / "Roms/GBA/Delete.png": b"ROM-directory artwork",
        primary / "Images/GBA/Delete.png": b"image-root artwork",
        primary / "Saves/GBA/Delete.sav": b"save progress",
        primary / "States/GBA/Delete.state": b"save state",
        primary / "States/GBA/Delete.state.png": b"state thumbnail",
        fixture.secondary / "Roms/GBA/Delete.gba": b"Other card must survive\n",
    }
    for path, contents in protected.items():
        write(path, contents)
    with sqlite3.connect(fixture.db_path) as db:
        victim = db.execute("SELECT id FROM games WHERE source_id='primary' AND rom_relpath='GBA/Delete.gba'").fetchone()[0]
        survivor = db.execute("SELECT id FROM games WHERE source_id='primary' AND rom_relpath='GBA/Keep.gba'").fetchone()[0]
        db.execute("UPDATE games SET image_root_kind='roms',image_relpath='GBA/Delete.png',"
                   "image_path='Roms/GBA/Delete.png' WHERE id=?", (victim,))
        for game_id in (victim, survivor):
            db.execute("INSERT INTO favorites VALUES('game',?,1)", (game_id,))
            db.execute("INSERT INTO recents VALUES('game',?,1,20)", (game_id,))
            db.execute("INSERT INTO game_settings(game_id,key,value,updated_at) VALUES(?,'core','fixture',1)", (game_id,))
        for key, value in (("five_game_ids", f"{victim},{survivor}"), ("five_game_lock", "pin"),
                           ("five_game_pin_hash", "keep-this-hash")):
            db.execute("INSERT OR REPLACE INTO settings VALUES(?,?)", (key, value))
        for member in ("", "archive member"):
            db.execute("INSERT INTO hidden_roms VALUES('primary','GBA/Delete.gba',?)", (member,))
        db.execute("INSERT INTO hidden_roms VALUES('secondary_sd','GBA/Delete.gba','')")

    with Client(fixture.socket) as client:
        preview = client.preview("GBA/Delete.gba")
        digest = ready(preview)
        assert preview["bytes"] == (primary / "Roms/GBA/Delete.gba").stat().st_size, preview
        assert files(preview) == {"GBA/Delete.gba"}, preview
        result = client.delete("GBA/Delete.gba", digest)
        assert result.get("phase") == DONE and result.get("removed_count") == 1, result
        assert not (primary / "Roms/GBA/Delete.gba").exists()
    # The old digest names a file that is gone, so it can't match a replacement of another size.
    write(primary / "Roms/GBA/Delete.gba", b"replacement must survive")
    with Client(fixture.socket) as client:
        rejected(client.delete("GBA/Delete.gba", digest))
    assert (primary / "Roms/GBA/Delete.gba").read_bytes() == b"replacement must survive"
    for path, contents in protected.items():
        assert path.read_bytes() == contents, path
    with sqlite3.connect(fixture.db_path) as db:
        assert db.execute("SELECT id FROM games WHERE id=?", (victim,)).fetchone() is None
        assert db.execute("SELECT id FROM games WHERE id=?", (survivor,)).fetchone()
        for table, key in (("favorites", "target_id"), ("recents", "target_id"), ("game_settings", "game_id")):
            assert not db.execute(f"SELECT 1 FROM {table} WHERE {key}=?", (victim,)).fetchone()
            assert db.execute(f"SELECT 1 FROM {table} WHERE {key}=?", (survivor,)).fetchone()
        assert db.execute("SELECT value FROM settings WHERE key='five_game_ids'").fetchone()[0] == str(survivor)
        assert db.execute("SELECT value FROM settings WHERE key='five_game_pin_hash'").fetchone()[0] == "keep-this-hash"
        assert not db.execute("SELECT 1 FROM hidden_roms WHERE source_id='primary' AND rom_relpath='GBA/Delete.gba'").fetchone()
        assert db.execute("SELECT 1 FROM hidden_roms WHERE source_id='secondary_sd' AND rom_relpath='GBA/Delete.gba'").fetchone()
    with Client(fixture.socket) as client:
        status = client.request("library-status")
        assert status["generation"] > initial_generation, status

    # Lose the result after removal starts: the job still finishes and publishes.
    lost_file = primary / "Roms/GBA/LostResponse.gba"
    before_lost = library_generation(fixture)
    with Client(fixture.socket) as client:
        digest = ready(client.preview("GBA/LostResponse.gba"))
        accepted = client.request("rom-delete", source_id="primary", rom_relpath="GBA/LostResponse.gba",
                                  digest=digest)
        assert accepted.get("phase") == COMMITTING, accepted
    deadline = time.monotonic() + 10
    while lost_file.exists() and time.monotonic() < deadline:
        time.sleep(0.02)
    assert not lost_file.exists(), "accepted deletion never completed after disconnect"
    # The disconnected job is destroyed after it finishes; it must still publish.
    while library_generation(fixture) <= before_lost and time.monotonic() < deadline:
        time.sleep(0.02)
    assert library_generation(fixture) > before_lost, "disconnected deletion did not publish its generation"
    write(lost_file, b"replacement after lost response")
    with Client(fixture.socket) as client:
        rejected(client.delete("GBA/LostResponse.gba", digest))
    assert lost_file.read_bytes() == b"replacement after lost response"

    # jawakad keeps nothing between preview and delete, so a restart in between
    # changes nothing.
    with Client(fixture.socket) as client:
        digest = ready(client.preview("GBA/Restart.gba"))
    fixture.stop()
    fixture.start()
    with Client(fixture.socket) as client:
        result = client.delete("GBA/Restart.gba", digest)
        assert result.get("phase") == DONE and result.get("removed_count") == 1, result

    test_hash_filenames(fixture)
    test_generated_playlist(fixture)


def main():
    repo = Path(__file__).resolve().parents[1]
    daemon = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else repo / "build/bin/jawakad-delete-test"
    assert daemon.is_file(), f"build the daemon first: {daemon}"
    # Keep AF_UNIX socket names within the host's small path limit.
    with tempfile.TemporaryDirectory(prefix="jw-del-ipc-", dir="/tmp") as directory:
        fixture = Fixture(Path(directory), daemon)
        try:
            run(fixture)
        except Exception:
            if fixture.log.exists():
                print(fixture.log.read_text(errors="replace"), file=sys.stderr)
            raise
        finally:
            fixture.stop()
    print("PASS rom-delete-ipc-smoke")


if __name__ == "__main__":
    main()

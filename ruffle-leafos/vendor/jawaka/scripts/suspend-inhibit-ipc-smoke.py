#!/usr/bin/env python3
import json
import multiprocessing
import socket
import struct
import sys
import time


SOCKET = sys.argv[1]


def request(payload):
    body = json.dumps(payload, separators=(",", ":")).encode()
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(2)
        client.connect(SOCKET)
        client.sendall(struct.pack("!I", len(body)) + body)
        size = struct.unpack("!I", _read_exact(client, 4))[0]
        return json.loads(_read_exact(client, size))


def _read_exact(client, size):
    chunks = []
    remaining = size
    while remaining:
        chunk = client.recv(remaining)
        if not chunk:
            raise RuntimeError("short daemon frame")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def child_holder(pipe, release_token=None):
    if release_token:
        pipe.send(request({"type": "suspend-inhibit-release", "token": release_token}))
        pipe.close()
        return
    acquired = request({
        "type": "suspend-inhibit-acquire",
        "scope": "block-suspend",
        "reason": "native child holder",
    })
    pipe.send(acquired)
    command = pipe.recv()
    if command == "release":
        pipe.send(request({"type": "suspend-inhibit-release", "token": acquired["token"]}))
    pipe.close()


def spawn_holder(release_token=None):
    parent, child = multiprocessing.Pipe()
    process = multiprocessing.Process(target=child_holder, args=(child, release_token))
    process.start()
    return process, parent


def main():
    parent_lease = request({
        "type": "suspend-inhibit-acquire",
        "scope": "block-suspend",
        "reason": "native parent holder",
    })
    assert parent_lease["type"] == "suspend-inhibit-acquired"

    holder, pipe = spawn_holder()
    child_lease = pipe.recv()
    assert child_lease["type"] == "suspend-inhibit-acquired"
    status = request({"type": "suspend-inhibit-status"})
    assert status["active_count"] == 2, status
    assert sorted(h["reason"] for h in status["holders"]) == [
        "native child holder", "native parent holder"
    ]
    assert all("token" not in holder_status for holder_status in status["holders"])

    thief, thief_pipe = spawn_holder(parent_lease["token"])
    stolen = thief_pipe.recv()
    thief.join(3)
    assert stolen["type"] == "error" and "another process" in stolen["message"], stolen

    released = request({"type": "suspend-inhibit-release", "token": parent_lease["token"]})
    assert released["type"] == "ok"
    duplicate = request({"type": "suspend-inhibit-release", "token": parent_lease["token"]})
    assert duplicate["type"] == "ok"

    pipe.send("release")
    assert pipe.recv()["type"] == "ok"
    holder.join(3)
    assert request({"type": "suspend-inhibit-status"})["active_count"] == 0

    dead, dead_pipe = spawn_holder()
    assert dead_pipe.recv()["type"] == "suspend-inhibit-acquired"
    dead.terminate()
    dead.join(3)
    for _ in range(50):
        status = request({"type": "suspend-inhibit-status"})
        if status["active_count"] == 0:
            break
        time.sleep(0.02)
    else:
        raise AssertionError(f"dead holder was not reaped: {status}")

    malformed = request({"type": "suspend-inhibit-release", "token": "bad"})
    assert malformed["type"] == "error"
    print("suspend-inhibit-ipc-smoke: ok")


if __name__ == "__main__":
    main()

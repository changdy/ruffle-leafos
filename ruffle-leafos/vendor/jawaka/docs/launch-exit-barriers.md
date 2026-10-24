# Launch exit barriers

This is the A3b audit of every foreground path by which Leaf launches content
or an app. It is also the eligibility boundary for LIFE-1: only
`RETROARCH` and `EMULATOR` emit `game.start` / `game.finish`.

## The common barrier

Before either qualifying child can execute, the child calls `setpgid(0, 0)`
and the daemon calls `setpgid(pid, pid)`. Calling it on both sides closes the
fork/exec race. The daemon records that pgid with the active launch.

On leader exit, `jawakad` uses `waitid(..., WNOWAIT)` so the zombie leader
continues to reserve its pid/pgid. It does not reap until the group-absence
check proves that the group contains no non-zombie process. It then runs the
source/session finalizer and only afterwards clears `active-game.json` and
emits `game.finish`. Shutdown and forced-quit signals target the same group.

`internal/launcher/writer_group_test.c` and the LIFE-1 game fixture prove the
important wrapper case: the direct child exits while a writer descendant stays
alive, and neither the active record nor `game.finish` is released early.

## MLP1 launch inventory

| Path | Jawaka `child_kind` | LIFE-1 events | What is waited for | Saves/States writer | Result |
| --- | --- | --- | --- | --- | --- |
| RetroArch content, all libretro cores | `RETROARCH` | Yes | Reserved RetroArch process group; then shared-config/session finalizer | Yes | Proven |
| Mupen64Plus standalone | `EMULATOR` | Yes | Reserved group containing `launch.sh`, Mupen64Plus, and descendants; then standalone finalizer | Yes | Proven |
| PPSSPP standalone | `EMULATOR` | Yes | Reserved group; wrapper `exec`s `PPSSPPSDL`; then standalone finalizer | Yes | Proven |
| DraStic standalone | `EMULATOR` | Yes | Reserved group; wrapper `exec`s `drastic64`; then standalone finalizer | Its package currently writes internal emulator state, not the shared Saves/States preset | Proven |
| Flycast standalone | `EMULATOR` | Yes | Reserved group; wrapper `exec`s Flycast; then standalone finalizer | Yes | Proven |
| PortMaster port from the Ports system | `EMULATOR` | Yes | Jawaka proves only its outer launcher group. The launcher deliberately starts the selected port with `setsid` and waits for that session leader, but arbitrary port scripts may daemonize descendants. | No routine writer to Leaf's shared Saves/States roots; PortMaster uses its own userdata roots | Unproven for arbitrary ports |
| Foreground `.pak` app, including Thing-File and the RetroArch settings pak | `APP` | No | Direct child pid only; no general descendant barrier | Not an audited automatic writer. Manual file editing/loading is outside the guarantee | Unproven by design |
| PortMaster manager pak | `APP` | No | Direct child pid only | Writes PortMaster userdata, not the shared preset | Unproven by design |
| Jawaka launcher | `LAUNCHER` | No | Direct child pid | No | Not applicable |
| Normal or in-game menu | `MENU` / separately tracked menu child | No | Direct child pid | No; save requests are completed by the active emulator before a handoff | Not applicable |
| Stock `loong_pangu` launcher-switcher wrapper | Outside Jawaka | No | The stock wrapper `exec`s the Leaf session; it is the boot/session owner, not a content child | No | Not applicable |

The four standalone packages in the default MLP1 payload were inspected at
their packaged launch scripts. Mupen64Plus runs its binary synchronously in the
inherited group; PPSSPP, DraStic, and Flycast use `exec`; none calls `setsid`,
`setpgid`, or backgrounds the emulator. Thus the daemon's reserved group covers
their routine writers. PortMaster is explicitly different and is kept outside
the automatic Saves/States eligibility promise.

## MLP1 fixture qualification — 2026-08-08

`make mlp1-adb-life1-smoke` runs an isolated aarch64 fixture on the attached
MLP1. It pauses the live launcher daemon, bind-mounts a temporary executable
mock card under `/tmp`, starts a second `jawakad --daemon-only` with a complete
single-source PATH-2 environment, and removes the mount and every fixture
process before resuming the live daemon.

Both focused cases pass on device:

- `game-exchange`: the service subscribes with kernel peer identity, receives
  `game.start`/`game.cancel`, and replies ready; `active-game.json` remains
  present while the direct fixture child has exited but its writer descendant
  is live; `game.finish` arrives only after group absence and the finalizer.
- `game-malformed`: a malformed subscriber reply takes the verified service
  stop fallback and the writer launches only after that stop completes.

The run leaves no fixture process, temporary bind mount, or test tree. Its
ADB-inclusive launch-to-writer observations are diagnostic only; B0b owns the
in-process pause/launch timing gate and must not substitute host/ADB latency for
that measurement.

## Content switching

When a game-sensitive service participated in the current launch, a switch
settles the old savestate write, quits RetroArch, waits for its full group
barrier and finalizer, then commits a fresh active record for the next title.
This preserves the one-launch/one-`launch_id` invariant while that service is
running or waiting to restart.

Without a participating service, same-core and same-card switches retain the
existing in-process RetroArch path. The writer process group and source binding
do not change, so the durable active-game record remains authoritative without
adding a synthetic finish/start boundary.

## Product boundary

The Release P automatic preset may include shared Saves and States because all
routine writers of those trees in the default MLP1 emulator payload have a
proven barrier. ROM trees, app userdata, arbitrary PortMaster data, and manual
file-manager edits are not covered. Opening a `.pak` therefore neither pauses
nor stops sync; the UI and documentation must continue to warn that manual
editing of a managed tree while sync is active is outside the safety guarantee.

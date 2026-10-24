# LEAF-RA-1 - Pak-Supplied RetroArch Frontends: Normative Contract

Date: 2026-10-13

This is the **normative text** for `leaf-ra-1`: what a RetroArch build that a
content pak ships has to do so that Leaf can run it as its own RetroArch. A
pak opts in by declaring `frontend` on a `retroarch` core
([`content-paks.md`](content-paks.md#pak-supplied-retroarch-frontend)); this
document says what that declaration promises.

| Party | Who | Obligation in one line |
| --- | --- | --- |
| **Launcher** | Leaf's daemon (Jawaka's `jawakad`) | exec the frontend with Leaf's arguments and configuration file, then drive it over a local UDP command port |
| **Frontend** | the executable at `cores[].frontend.path` | run the core with the file it is given, answer the commands below, exit cleanly |

This public document is complete enough to implement against. It was written
from Leaf's command client and from the two patches that give Leaf's own
RetroArch the matching server side (`0001-command-menu-commands` and
`0002-jawaka-load-content-command`, applied to RetroArch v1.22.2). Those
patches are the reference implementation. If they and this text disagree, this
text wins and the patch is the bug, unless the disagreement shows the text is
wrong, in which case follow [Change procedure](#change-procedure).

The machine-checkable half of the manifest side (the `frontend` field and its
rejection reasons) is in
[`../contracts/leaf-content/`](../contracts/leaf-content/). This revision has
no wire fixtures for the command protocol: the reply formats below are exactly
the ones Leaf's client parses, and nothing else is accepted.

The words **must**, **must not** and **may** are used in their usual
specification sense.

---

## What a frontend is

A frontend is an executable Leaf can run in place of its own RetroArch binary.
It is usually a short wrapper script that sets up the runtime the pak's
RetroArch needs (for example a 32-bit loader and libraries) and then execs a
RetroArch build, but it may be the RetroArch binary itself. Either way, what
Leaf sees is one process that behaves like the RetroArch it ships. The
session is a **RetroArch session**: everything Leaf does for its own RetroArch
games it tries to do for this one, within the limits of the [probe](#capability-probe).

## Launch

Leaf runs the frontend with exactly this argument list:

```text
<frontend> -L <core> --config <runtime.cfg> [-e <slot>] <content>
```

| Argument | Meaning |
| --- | --- |
| `-L <core>` | Absolute path of the pak's core file (`file_name`). |
| `--config <runtime.cfg>` | Absolute path of the configuration file Leaf generated for this launch. See [Configuration](#configuration). |
| `-e <slot>` | Optional. RetroArch's `--entryslot`, a decimal state slot. Current Leaf releases load resume states over the command port after launch instead of sending it, but the form is part of the contract: pass it through. |
| `<content>` | Absolute path of the game file. |

The order is fixed and Leaf adds no other argument. The frontend **must** exec
RetroArch with these arguments unchanged (`exec ... "$@"`), and **must not**
add `--appendconfig`, change `--config`, or drop an argument.

**Environment.** Leaf passes its environment as it is, and the frontend passes
it on. Among other things it carries:

- `HOME`, set to RetroArch's state directory, where RetroArch keeps core
  options, remaps and overrides;
- `SDL_JOYSTICK_DEVICE` and the related SDL variables, which fix the
  controller order (an external pad is player 1);
- the `UMRK_*` and `JAWAKA_*` variables of the runtime contract.

A wrapper may add or remove only what its own binary needs in order to start:
the library search path, preloads, graphics driver selection. It **must not**
change `HOME` or the variables above.

**Process.** Leaf starts the frontend as the leader of a new process group and
tracks its pid. The frontend **must** `exec` RetroArch rather than start it as
a child, so the pid Leaf tracks and signals is the process that holds the game
and answers on the command port. It **must not** daemonize, detach, or leave
its process group. Standard input, output and error are inherited.

## Configuration

The file passed with `--config` is **authoritative**.

- The frontend **must** honor every key in it. Leaf writes protected keys
  there (paths, audio routing, the command port, menu and input settings) and
  later releases may add more, so the frontend **must not** filter the file
  through a list of keys it knows.
- The frontend **must not** generate a configuration of its own, rewrite or
  merge this file before RetroArch reads it, or point RetroArch at a different
  one.
- The file can hold the player's RetroAchievements credentials for this launch
  (`cheevos_username`, `cheevos_password`). The frontend **must not** log its
  contents, copy it, or leave a copy behind.
- RetroArch's own writes are expected. With `config_save_on_exit` set, RetroArch
  writes the running configuration back to the same `--config` path on exit,
  and that is how Leaf collects the player's changes. Writing back to that path
  is allowed; replacing it, or writing elsewhere and switching to the copy, is
  not. Leaf decides afterwards what, if anything, reaches the shared RetroArch
  configuration, and for that it reads the frontend's [`VERSION`](#commands-that-reply)
  reply: a frontend whose version differs from Leaf's own RetroArch never has
  settings promoted.
- RetroArch's per-core option, remap and override files, found through
  `rgui_config_directory` and `HOME`, work as they do in any RetroArch.

These keys carry behavior Leaf depends on. The list is not exhaustive.

| Key | Leaf relies on |
| --- | --- |
| `network_cmd_enable`, `network_cmd_port` | the [command port](#command-port) |
| `savestate_directory`, `sort_savestates_enable`, `savestate_thumbnail_enable` | state slots and their `.png` thumbnails being where Leaf and its menu read them, see [State files](#state-files) |
| `savestate_file_compression` | reported by `GET_STATE_SAVE_INFO`; the synchronous save refuses compressed states |
| `savefile_directory`, `sort_savefiles_enable`, `system_directory` | per-card saves and BIOS files |
| `screenshot_directory` | the target of `SCREENSHOT` |
| `libretro_directory`, `libretro_info_path` | where the core and its `.info` are found |
| `video_shader_enable`, `auto_shaders_enable`, `video_shader_dir`, `rgui_config_directory` | the shader picker and automatic preset scopes |
| `pause_nonactive` | `PAUSE` holding when the window loses focus |
| `input_player*` keys, `menu_swap_ok_cancel_buttons` | controller layout and the OK/Cancel swap |
| `config_save_on_exit` | whether the configuration is written back on exit |

Leaf may also write keys that describe what a pak frontend cannot do (for
example recording off, or the audio driver pinned). They are honored like any
other key.

## Command port

RetroArch's network command interface, on UDP.

- **Where.** The frontend **must** listen on `127.0.0.1` at the port in the
  configuration's `network_cmd_port`, with `network_cmd_enable` honored. Leaf
  currently writes `55355`, but the frontend **must** read the value rather
  than assume it.
- **Requests.** One command per datagram, ASCII, no terminator (the frontend
  **must** tolerate a trailing newline). A command is a name, then
  optionally one space and its argument. Arguments are single-space separated;
  a path argument (`JAWAKA_SET_SHADER`, `JAWAKA_LOAD_CONTENT`) is the rest of
  the line and may contain spaces. Unknown commands are ignored without a
  reply.
- **Replies.** At most one reply datagram per request, sent to the request's
  source address **from the command socket itself**, so the reply's source is
  `127.0.0.1:<network_cmd_port>`. This is not a courtesy: Leaf connects its UDP
  sockets for the shader, synchronous-save and synchronous-load exchanges, and
  the kernel discards a datagram from any other source. A reply is one
  datagram of at most 1023 bytes (4223 for `SAVE_STATE_SYNC`). Leaf strips
  trailing CR and LF.
- **Always listening.** The port **must** keep answering while the game is
  paused and while the frontend's own menu is open. Leaf polls `GET_STATUS` and
  sends `UNPAUSE` in exactly those states.
- **Fire-and-forget.** Some commands have no reply, and Leaf does not wait for
  one. An unpatched RetroArch replies to only a handful of commands (`VERSION`,
  `GET_STATUS`, `GET_CONFIG_PARAM`, `LOAD_STATE_SLOT`, the memory commands) and
  silently ignores `PAUSE`. That silence is the reason for the
  [probe](#capability-probe).
- **Timing.** Leaf waits 750 ms for an ordinary reply and 1000 ms for the
  shader commands. It probes with 100 to 300 ms budgets and treats silence
  inside that budget as "not supported". The command handler **must** answer
  from the main loop within a frame or two; a reply that takes seconds is a
  missing reply. `SAVE_STATE_SYNC` and `LOAD_STATE_SYNC` are the exceptions:
  their reply comes when the work is done, and Leaf polls for it without
  blocking (see their entries).

## Commands

### Commands without a reply

Leaf sends these and does not wait. `LOAD_STATE` and the toggles act on
RetroArch's current state, as they do in upstream RetroArch.

| Command | Effect |
| --- | --- |
| `PAUSE` | Pause the game. Idempotent, not a toggle. |
| `UNPAUSE` | Resume the game. Idempotent, not a toggle. |
| `OPEN_MENU` | Open the frontend's own menu (the **RetroArch Settings** row). The game stays paused behind it. |
| `OPEN_MENU SHADERS` | Open the frontend's menu directly on its shader options page. |
| `MENU_TOGGLE` | Toggle the frontend's own menu. Leaf uses it to close that menu. |
| `QUIT` | Exit cleanly, see [Exit](#exit). |
| `RESET` | Reset the core. |
| `SAVE_STATE` | Save to the current state slot (set by `SET_STATE_SLOT`; slot `-1` is the automatic slot). The write may finish after the command returns. |
| `LOAD_STATE` | Load the current state slot. |
| `SCREENSHOT` | Write a PNG of the current frame into `screenshot_directory`. |
| `AUDIO_REINIT` | Reinitialize audio output. Sent after Bluetooth audio changes; optional. |
| `SHOW_MSG <text>` | Show `<text>` as an on-screen message. Optional. |
| `RECORDING_TOGGLE` | Start or stop recording. Optional; a frontend that cannot record ignores it. |

### Commands that reply

Every reply below is exact. Leaf parses it strictly; a reply of any other shape
is a parse error and the feature that asked fails. (The one loose case is the
slot actions, noted in their table.) `<n>` is a decimal integer with an
optional leading `-`. All other tokens are literal.

**`VERSION`** replies with the bare version string and a newline, the one
reply without a command-name prefix: `1.22.2`. It is RetroArch's version, with
no leading `v`.

**`GET_INFO`** replies `GET_INFO <disks> <disk> <state>`:

- `<disks>`: number of disc images, `0` or more;
- `<disk>`: the current disc index, `0` or more (`0` when there are none);
- `<state>`: the current state slot, an integer from `-1` to `999999`, or
  `NO` when the running core cannot save states.

It **must** be answerable as soon as the command port is up, before content
has loaded (`GET_INFO 0 0 NO`). It is the capability probe.

**`GET_STATUS`** replies one of:

- `GET_STATUS CONTENTLESS`;
- `GET_STATUS <state> <system>,<content>[,<extra>...]`, where `<state>` is
  `PLAYING`, `PAUSED` or `MENU`.

`MENU` means the frontend's own menu is open and takes precedence over
`PAUSED` and `PLAYING`. `<system>` is the core's system id (from its `.info`);
`<content>` is the content's base name. Leaf reads `<system>` up to the first
comma and `<content>` up to the next, so neither may contain a comma. The
third field in upstream RetroArch is `crc32=<hex>`; Leaf ignores it.

**State slots.** `<n>` is the slot number. Slot `-1` is the automatic slot.

| Request | Reply | Notes |
| --- | --- | --- |
| `GET_STATE_SLOT` | `GET_STATE_SLOT <n>` or `GET_STATE_SLOT NO` | `NO` when states are unsupported. |
| `SET_STATE_SLOT <n>` | `SET_STATE_SLOT <n>` | Select slot `<n>` (`-1` or more) as the current slot. Echoes the number it parsed. |
| `SAVE_STATE_SLOT <n>` | `SAVE_STATE_SLOT <n>` | Save slot `<n>` (`-1` or more). Always replies, including on failure. The file may be finished after the reply; Leaf then watches it until its size stops changing, for up to 8 seconds. |
| `LOAD_STATE_SLOT <n>` | `LOAD_STATE_SLOT <n>` | Load slot `<n>` (0 or more). Always replies, including on failure. |

For `SET_STATE_SLOT` Leaf parses the echoed number. For `SAVE_STATE_SLOT` and
`LOAD_STATE_SLOT` it checks only that a reply arrives: the reply says the
command was understood, not that it succeeded, and Leaf reads success from the
files.

**Disc control.** `GET_DISK_COUNT` replies `GET_DISK_COUNT <n>`, `GET_DISK_SLOT`
replies `GET_DISK_SLOT <n>`, and `SET_DISK_SLOT <n>` replies
`SET_DISK_SLOT <n>` after swapping to that disc (eject, set index, insert). An
out-of-range `<n>` or a core with no disc control changes nothing and still
replies.

**`GET_PATH <kind>`** replies `GET_PATH <kind> <value>`; `<value>` is the rest
of the line, empty when the kind is unknown or has no value. Leaf uses
`content` (the absolute path of the loaded content, which Leaf compares with
the path it launched) and `savestate` (the path of the current state slot).
A frontend **should** also answer `core`, `config`, `config_append`,
`config_override`, `core_options`, `default_shader_preset`, `basename` and
`subsystem` as RetroArch does, which Leaf's tools use.

**`GET_STATE_SAVE_INFO`** replies `GET_STATE_SAVE_INFO 1 <bytes> <compressed>`
or `GET_STATE_SAVE_INFO 1 NO`. The leading `1` is the version of the
synchronous save and load protocol and **must** be `1`; any other value makes
Leaf treat the frontend as unable. `<bytes>` is the serialized size, above
zero. `<compressed>` is `1` when `savestate_file_compression` is on, else `0`.
`NO` means the core cannot serialize.

**`SAVE_STATE_SYNC <id> <slot> <max_bytes> <start_by_ms>`** writes a state for
the power-hold save. `<id>` is 1 to 32 characters of `[A-Za-z0-9_-]` (Leaf
sends at most 16); `<slot>` is 0 to 999; `<max_bytes>` is above zero;
`<start_by_ms>` is a deadline on `CLOCK_MONOTONIC` in milliseconds. The
frontend **must**:

1. refuse with `LATE` if the deadline has passed before it starts serializing
   and again before it opens the file, and with `TOO_LARGE` if the serialized
   state exceeds `<max_bytes>`;
2. write the **uncompressed** state, synchronously, to the slot's state path
   with `.tmp-<id>` appended, creating it exclusively;
3. `fsync` and close the file, and only then reply;
4. on any failure remove the partial file and reply with the error;
5. **never rename** the temporary file. Leaf owns publication, so a reply Leaf
   gave up on can never replace the existing slot.

It replies exactly once:

- `SAVE_STATE_SYNC <id> TMP_READY <bytes> <temp_path>`: `<bytes>` is above zero
  and `<temp_path>` is the absolute path of the file, the rest of the line;
- `SAVE_STATE_SYNC <id> ERROR <CODE>`: `<CODE>` is upper case letters and
  underscores. The codes are `UNSUPPORTED`, `COMPRESSED`, `BUSY`, `SERIALIZE`,
  `TOO_LARGE`, `LATE`, `OPEN`, `WRITE`, `FSYNC`, `CLOSE`, `BAD_ARGS` and
  `PATH`;
- `SAVE_STATE_SYNC - ERROR BAD_ARGS` when the line or its id cannot be parsed,
  so there is no id to echo.

**`LOAD_STATE_SYNC <id> <slot>`** applies a state for boot resume. `<id>` is as
above and `<slot>` is 0 to 999. The frontend reads the slot's state and applies
it to the running core **inside the handler**, with no task, no undo buffer
and no retry, so the reply answers whether that exact file is now the running
state. It replies exactly once:

- `LOAD_STATE_SYNC <id> OK <bytes>`: `<bytes>` above zero;
- `LOAD_STATE_SYNC <id> ERROR <CODE>`: the codes are `UNSUPPORTED`, `HARDCORE`
  (hardcore achievements are active), `BUSY`, `OPEN`, `READ`, `TOO_LARGE`,
  `UNSERIALIZE` (compressed, truncated or rejected by the core), `BAD_ARGS` and
  `PATH`;
- `LOAD_STATE_SYNC - ERROR BAD_ARGS` for an unparseable line.

A slot below zero is `BAD_ARGS`, replied at once with the id echoed and
nothing touched. Leaf uses exactly that (`LOAD_STATE_SYNC <id> -1`) to learn
the command exists without loading anything. A frontend that never answers it
is treated as unable to sync-load. Leaf allows a load up to 10 seconds to
answer.

**Shader commands.** Each carries a request id, generated per exchange by
Leaf: lowercase hexadecimal, at most 16 characters. The reply echoes it, and
Leaf discards any reply carrying another id. Replies are prefixed
`JAWAKA_SHADER <id> ` and then:

| Request | Reply tail |
| --- | --- |
| `JAWAKA_GET_SHADER <id>` | `GET NONE` (no runtime shader), or `GET OK <preset_path>` (the rest of the line) |
| `JAWAKA_SET_SHADER <id> <preset_path>` | `SET OK`, or `SET ERROR missing` (not on disk), `SET ERROR unsupported` (not a preset this video driver can load), `SET ERROR apply` (compile, link or apply failed) |
| `JAWAKA_CLEAR_SHADER <id>` | `CLEAR OK`, or `CLEAR ERROR apply`. Also clears the menu's shader pass list, so a later save cannot write a preset for a shader that is gone. |
| `JAWAKA_SAVE_SHADER_PRESET <id> <scope>` | `SAVE <scope> OK` or `SAVE <scope> ERROR` |
| `JAWAKA_REMOVE_SHADER_PRESET <id> <scope>` | `REMOVE <scope> OK`, `REMOVE <scope> ABSENT` (nothing to remove) or `REMOVE <scope> ERROR` |

`<scope>` is exactly one of `GAME`, `PARENT`, `CORE`, `GLOBAL`, and is echoed.
Saving and removing take **scopes only**, never a path: the frontend writes the
automatic preset for that scope under `video_shader_dir` and
`rgui_config_directory`. Saving with no shader applied is `ERROR`, not an empty
success. The reply to `SET` reports whether the shader actually compiled and
linked, which is the point of these commands over upstream `SET_SHADER`.

**`JAWAKA_LOAD_CONTENT <path>`** loads another game in the core that is
already running (Game Switcher). `<path>` is an absolute path, the rest of the
line. If it does not exist the reply is `JAWAKA_LOAD_CONTENT NO <path>`.
Otherwise the frontend replies `JAWAKA_LOAD_CONTENT OK <path>` **at once** and
performs the load on its main loop afterwards: it unpauses, then loads the
content with the current core without restarting the process. Leaf then polls
`GET_PATH content` for up to 2.5 seconds until it names that file. If it never
does, Leaf quits the frontend and launches the game cold.

### State files

Leaf finds and shows states by name, so the frontend **must** use RetroArch's
naming under the configured directories. With `sort_savestates_enable` true,
states live in `<savestate_directory>/<config_folder>/`, where `<config_folder>`
is the core's `config_folder` (the name the core reports as its library name).
For content `Game.chd`:

| Slot | State file | Thumbnail |
| --- | --- | --- |
| `0` | `Game.state` | `Game.state.png` |
| `N` above 0 | `Game.stateN` | `Game.stateN.png` |
| `-1` (automatic) | `Game.state.auto` | `Game.state.auto.png` |

The thumbnail is written beside the state when `savestate_thumbnail_enable` is
on. Slot `99` is reserved by Leaf: the Game Switcher snapshot, the power-hold
save and the boot-resume state all use it.

## Features and the commands they need

A frontend that declares `leaf-ra-1` implements every command above. This table
says which feature breaks without which command. Leaf checks each feature on
its own: a missing command costs that feature, not the session.

| Leaf feature | Commands it sends |
| --- | --- |
| Probe, and readiness after launch | `GET_INFO` |
| In-game menu: open, **Continue** | `PAUSE`, `UNPAUSE`, `GET_STATUS` |
| **Reset**, **Quit** | `RESET`, `QUIT` |
| **RetroArch Settings** row | `OPEN_MENU`, `GET_STATUS`, `MENU_TOGGLE` |
| Save and Load State, slot list | `GET_INFO`, `GET_STATE_SLOT`, `SET_STATE_SLOT`, `SAVE_STATE_SLOT`, `LOAD_STATE_SLOT`, `SAVE_STATE`, `LOAD_STATE`, plus the [state files](#state-files) |
| Save and quit | `GET_INFO`, `SAVE_STATE_SLOT 99`, `QUIT` |
| Disc control | `GET_INFO`, `GET_DISK_COUNT`, `GET_DISK_SLOT`, `SET_DISK_SLOT` |
| Shader picker | `JAWAKA_GET_SHADER`, `JAWAKA_SET_SHADER`, `JAWAKA_CLEAR_SHADER`, `JAWAKA_SAVE_SHADER_PRESET`, `JAWAKA_REMOVE_SHADER_PRESET`; advanced shader settings add `OPEN_MENU`, `OPEN_MENU SHADERS`, `GET_STATUS`, `MENU_TOGGLE` |
| Power-hold save | `GET_STATE_SAVE_INFO`, `SAVE_STATE_SYNC`, then `QUIT` |
| Boot resume | `GET_INFO`, `GET_STATE_SAVE_INFO`, `LOAD_STATE_SYNC` (probe with slot `-1`, then the load), `UNPAUSE`; `QUIT` if the load fails |
| Game Switcher, same core | `GET_INFO`, `SAVE_STATE_SLOT 99`, `JAWAKA_LOAD_CONTENT`, `GET_PATH content`, `LOAD_STATE_SLOT`, `UNPAUSE` |
| Game Switcher, other core, and resume after a cold launch | `GET_INFO`, `SAVE_STATE_SLOT 99`, `QUIT`; after the relaunch `GET_INFO`, `LOAD_STATE_SLOT`, `UNPAUSE` |
| Screenshot shortcut | `SCREENSHOT`, plus `screenshot_directory` |
| Settings promotion | `VERSION` |
| Notices on launch | `SHOW_MSG` (optional) |
| Bluetooth audio | `AUDIO_REINIT` (optional) |
| Recording shortcut | `RECORDING_TOGGLE` (optional) |

Without a working `GET_STATE_SAVE_INFO` or with `savestate_file_compression`
on, the power-hold save is off for the session; there is no fallback to an
asynchronous save. Without `LOAD_STATE_SYNC`, boot resume quits the frontend
and opens the launcher with a notice. Without `JAWAKA_LOAD_CONTENT`, the Game
Switcher falls back to quitting and relaunching.

## Capability probe

After it spawns the frontend, Leaf sends `GET_INFO` and retries for a bounded
window of a few seconds. Until a valid reply arrives the session is treated as
**plain**; the first valid reply promotes it. Answering `GET_INFO` is a claim
that the frontend implements this document.

If no valid reply arrives in the window, the session stays plain:

- Menu is forwarded to the frontend as a Guide tap (`BTN_MODE`), the same as
  for a path core with `supports_menu: true`, so the frontend's own menu opens
  as it would in stock RetroArch;
- Leaf's in-game menu and the features that need a command-capable frontend
  (Game Switcher, power-hold save, boot resume, shader picker) stay off for
  the session;
- Leaf logs the probe result once for the launch.

A reply that arrives after the window is ignored. The frontend therefore
**should** open its command port early and answer `GET_INFO` before content has
finished loading.

The probe exists because the first requests Leaf makes (`PAUSE`, `OPEN_MENU`)
have no reply. An unpatched RetroArch ignores them, and without the probe Leaf's
menu would open over a game that was never paused.

## Exit

- **`QUIT`.** The frontend **must** exit cleanly: it stops the core, flushes
  saves (SRAM, memory cards, the configuration file when `config_save_on_exit`
  is set), and exits with status `0`. Leaf sends it for the in-game menu's
  **Quit**, after a power-hold save, and when switching games, and it waits a
  short time before escalating. Exit promptly.
- **`SIGTERM`.** Leaf sends it to the frontend's process group when it must
  stop a game without the command port (shutdown, a stuck frontend). The
  frontend **must** treat it as `QUIT`: flush saves and exit, within the
  two-second grace that Leaf gives content-pak children between `SIGTERM` and
  `SIGKILL` (the path-core escape rule in
  [`content-paks.md`](content-paks.md#menu-on-a-content-pak-path-core)). A
  wrapper that stays in the way (instead of `exec`ing RetroArch) **must**
  forward the signal. During a power transition the grace can be shorter, so a
  frontend should not spend it on anything that is not a save.
- **`SIGKILL`.** The last resort. Nothing is flushed, so everything that has to
  survive must already be on the card.

## Change procedure

`leaf-ra-1` freezes when the first pak that declares it publishes. Until then:

1. Change this file first. It is the normative text.
2. Update the `frontend` rules, schema and fixtures in
   [`../contracts/leaf-content/`](../contracts/leaf-content/) to match, and
   re-run `scripts/validate_fixtures.py`.
3. A new manifest rejection rule gets exactly one invalid fixture naming its
   reason. A new command needs a row in the tables above and, if Leaf starts to
   depend on it, an entry in the feature table.
4. After the freeze, a change to a request, a reply format or a required
   command is a **new** protocol name (`leaf-ra-2`), never a mutation of
   `leaf-ra-1`. A manifest that names a protocol the reader does not know is
   refused with `unknown-frontend-protocol`, so a newer pak fails loudly on an
   older Leaf.
5. Adding an **optional** command that Leaf does not require is not a change to
   the protocol name. Leaf must tolerate a frontend that lacks it.

## Related

- [`content-paks.md`](content-paks.md#pak-supplied-retroarch-frontend): the `frontend` field, its rejection reasons, and the merge behavior
- [`../contracts/leaf-content/`](../contracts/leaf-content/): schema, fixtures and reference model for the manifest side

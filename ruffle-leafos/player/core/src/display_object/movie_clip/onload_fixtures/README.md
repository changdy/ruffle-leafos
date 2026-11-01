# AVM1 load-event fixtures

These SWFs and Flash-reference `output.txt` files are copied without changes from
[Ruffle PR #25054](https://github.com/ruffle-rs/ruffle/pull/25054), head commit
`d3c9b13d24c9303e31c7fba3150c4220f93fa6fe` (checked 2026-11-01; PR open).
Upstream paths below are relative to `tests/tests/swfs/` at that commit.

| Local directory | Upstream directory | Check |
| --- | --- | --- |
| `root_onload` | `avm1/root_onload` | First-frame definition of `_root.onLoad`; one call across 30 frames |
| `swfmill_root_onload` | `from_gnash/misc-swfmill.all/trace-as2/root_onload` | Root and child construction/load order |
| `action_execution_order_test4` | `from_gnash/misc-ming.all/action_order/action_execution_order_test4` | Frame, load, enterFrame and unload ordering |
| `issue_2870` | `avm1/issue_2870` | Preserve clip load before the first frame of `loadMovie` content |

`onload_tests.rs` embeds these files in the core unit-test executable and compares
all trace lines, without a renderer or audio device. `issue_2870/child.swf` is
written to an isolated temporary directory to exercise the actual asynchronous
loader. The upstream author measured the initial root movie in Flash Player 32;
loaded levels and replacement of `_level0` have no additional Flash-reference
coverage in this local port.

Run the core unit-test executable with the filter `avm1_onload_`. Building it with
`scripts/build_player.py test --package ruffle_core --offline --isolated-config`
only compiles the ARM64 executable; it does not run the tests.

| File | SHA-256 |
| --- | --- |
| `root_onload/test.swf` | `0972c9bcd6c854fc0043bba6ee8aced5894bc7165ce1aee36da72f16b055da1d` |
| `root_onload/output.txt` | `d2a1b3b2ca074278c21f6ba6002d391bb5cb07898c20939dde28d9035b495ee3` |
| `swfmill_root_onload/test.swf` | `6d010133820ab58d78e3952e0c94e28a0df25e1729b618504069acdabd515165` |
| `swfmill_root_onload/output.txt` | `c626f008ab1537ccc81924d2b098c9c7160a375cf5506c8cf4ee7fe542cdc7b3` |
| `action_execution_order_test4/test.swf` | `e4fec7a944f9fbd21e272a4aeab02f164591ac4cecef5ee06f5e8c86b3471494` |
| `action_execution_order_test4/output.txt` | `3f3ea78b5827e7b99d2c13b12ff49545148d27f4feb35a27eb78115fd89a577e` |
| `issue_2870/test.swf` | `6b584a03d746845dfb6cc6579fb8624f3bb31350e4c246910a67f1fb0accc16b` |
| `issue_2870/child.swf` | `e04fc0138f5740b4e86906f39506d26bd35dbd4ed94010eee49f16adfe617d2a` |
| `issue_2870/output.txt` | `4192dfb2c215f7189e1dd3023043f1f32f41c6a808a3de513fd2b0ef3edcd70c` |

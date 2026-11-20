# 构建与发布

以下 PowerShell 命令从仓库的 `ruffle-leafos/` 工程目录运行，目标为 Pocket1 的 `aarch64-unknown-linux-gnu`。

## 工具链与首次准备

本地构建使用 Git、Python、Node.js、JDK 17、Rust 1.95.0（Windows GNU 主机与 ARM64 GNU 标准库）、w64devkit 2.1.0、Zig 0.14.1 和 ARM GNU 14.2 SDK。目标链接使用 Zig 的 Clang/LLD 与 ARM GNU SDK 的头和库。CI 另使用 7-Zip 解包工具，运行 `python scripts/setup_release_tools.py` 准备锁定工具并生成本机配置。[Rust 锁](../adapter/rust-toolchain.lock.json) · [工具来源锁](../adapter/references.lock.json) · [ARM SDK 锁](../arm-gnu.lock.json)

本机工具路径可在工程根目录的 `workspace.local.json` 中指定，例如：

```json
{
  "host_tool_cache": "D:/tools/ruffle-host",
  "rust": "D:/tools/rust-1.95.0",
  "host_gcc": "D:/tools/w64devkit",
  "zig": "D:/tools/zig-0.14.1/zig.exe",
  "jdk": "D:/tools/jdk-17"
}
```

默认工具缓存为 `../work/toolchains/`，Cargo 依赖缓存位于其中的 `cargo-home/`。执行 `python scripts/setup_arm_gnu.py` 下载并校验 SDK，解包至 `.tools/`。首次下载 Cargo 依赖时省略 `--offline`。`--isolated-config` 隔离上级目录中的 Cargo 配置。全新机器的完整构建演练仍待验证。

## 构建播放器

```powershell
python scripts/build_player.py check --offline --isolated-config
python scripts/build_player.py build --offline --isolated-config
```

`check` 用于编译检查，`build` 会生成 `build/ruffle-leaf` 与 `build/player-build.json`。默认 `--tuning generic` 使用通用 ARM64、release opt-level 2、关闭 LTO、16 个 codegen units。实验参数 `--tuning pocket1` 使用 Cortex-A55、opt-level 3、ThinLTO 和相同 codegen units，供同设备对照。默认发布使用 generic。

构建记录含 ELF 的 SHA-256、实际工具链及配置，并通过 `source_inputs` 记录构建输入的逐文件 SHA-256 和总摘要。构建前后核对输入，打包还会复核当前输入、暂存 ELF 以及可选源码暂存树。缺失或失效的构建记录需要重新构建。Markdown 排除在 ELF 输入快照之外。发布仍会单独校验来源锁中列出的文件，包括播放器 README。[构建脚本](../scripts/build_player.py) · [绑定规则](../scripts/build_inputs.py) · [故障回归](validation/reliability-fixes-20261101.md)

## 按变更选择验证

| 命令 | 覆盖范围 |
| --- | --- |
| `python scripts/test_session.py` | 主机输入、窗口外会话状态、存储、截图路径及缓存策略 |
| `python -m unittest discover -s scripts -p test_language.py -v` | 语言、配置和翻译检查 |
| `python scripts/test_core.py` | 实际运行 Windows GNU 主机核心单元及集成测试，使用 Null 渲染后端，写入 `build/core-test-report.json` |
| `python scripts/test_swf.py` | 主机实际解析回归，每个测试进程树受 512 MiB 和 180 秒限制，写入 `build/swf-test-report.json` |
| `python scripts/test_renderer.py` | wgpu noop 闲置池和句柄所有权 |
| `python scripts/test_renderer.py --suite decode` | 无 GPU 的位图解码回归 |
| `python scripts/test_renderer.py --suite screenshot` | 实际 RGB PNG 编码、边界及写入失败 |
| `python scripts/test_zip_game.py` | ZIP 提取、缓存复用和异常输入 |
| `python -m unittest discover -s scripts -p test_build_inputs.py -v` | ELF 与实际构建输入绑定 |
| `python -m unittest discover -s scripts -p test_package_preview.py -v` | 试玩、三款 ZIP 游戏、双包布局及完整性 |
| `python -m unittest discover -s scripts -p test_release.py -v` | 版本检测、发布说明和附件选择 |

`python scripts/build_player.py test --package ruffle_core --offline --isolated-config` 只生成 ARM64 单元和集成测试 ELF（`--no-run`）。需要设备执行时确认测试框架的 GLIBC 要求，历史测试曾在隔离目录使用与 SDK 匹配的加载器及库。[设备测试记录](validation/stable-device-20261102.md#实际执行与画面检查)

原创 AS3 夹具用内置 `player/tools/asc/asc.jar`、JDK 和 ARM64 构建目录中配套的 `playerglobal_import.abc` 与 `playerglobal_avm2.swf`。例如 `python scripts/make_keyboard_fixtures.py --output ../work/keyboard-fixtures` 生成软键盘夹具，用 `--self-test-keyboard` 在隔离 Leaf 会话中执行。AVM1 存档夹具由原创动作流生成。[ASC 输入测试](../scripts/test_playerglobal_fixture_inputs.py) · [AVM1/AMF0](../player/core/tests/fixtures/shared-object-avm1-README.md) · [AVM2/AMF3](../player/core/src/player/tests/shared-object-README.md)

主机 Null/noop 回归覆盖逻辑、所有权和文件行为。设备侧单独记录真实 GPU 像素与同步、实体输入、人工听感及游戏性能。实机隔离运行须指定独立 `--storage-dir` 和 `--controls-dir`，临时入口、索引和数据核验遵循仓库约定。[实机调试约定](../../AGENTS.md#实机调试约定)

## 打包

### Pocket AIS / RGBox 安装包

推送到 `pocketais` 后，[Build and release Pocket AIS ZIP](https://github.com/changdy/ruffle-leafos/actions/workflows/pocketais.yml) 会重编 AArch64 播放器并运行主机测试。构建通过后，发布 job 会重新校验 ZIP 的 SHA-256，并以 `pocketais-v<版本>-build.<运行编号>` 为标签自动发布预览版。标签绑定构建提交。连续推送可以在 `pak_version` 相同的情况下各自发布。重跑同一次工作流时，已公开的 Release 会原样保留，未发布的草稿可以补传。写权限仅授予发布 job。

从 [GitHub Releases](https://github.com/changdy/ruffle-leafos/releases?q=pocketais) 的 Pocket AIS 预览版直接下载 `ruffle-pocketais-<版本>.zip` 和 `SHA256SUMS.txt`。同一 ZIP 仍保存在运行记录的 Artifacts 中。GitHub 的手动触发要求工作流存在于默认分支。Pocket1 的 `v<版本>` 发布工作流独立运行。

在 Windows 工程目录用同一套锁定工具链构建。已准备 Node.js、Python、JDK 17 和 7-Zip 后运行以下命令。

```powershell
python scripts/setup_release_tools.py
python scripts/build_player.py build --isolated-config --jobs 4
python scripts/test_session.py
python -m pip download --no-deps --only-binary=:all: PySDL2==0.9.17 -d ../work
$version = (Get-Content pak/pak.json -Raw | ConvertFrom-Json).pak_version
python scripts/package_aisos.py ../work/PySDL2-0.9.17-py3-none-any.whl "../outputs/ruffle-pocketais-$version.zip"
```

脚本校验播放器的源码及构建记录、AArch64 PIE、glibc 版本和 PySDL2 SHA-256。安装包使用固件自带的 SDL2、SDL2_ttf 和 ALSA。ZIP 顶层对应游戏卡根目录，含 `applyCenter/` 应用、`.ruffle-flash/manager.swf` 数据管理入口资源和 `flash/` 样例目录。ZIP 带有入口和播放器的 Unix 执行权限，ext4 卡解压时须保留。[打包脚本](../scripts/package_aisos.py) · [安装与操作](../README.md#aisos-应用菜单)

### AisOS / RGBox 内屏调色

从工程目录运行，复用已校验的 PySDL2 wheel 和仓库字体。独立 ZIP 包含内屏调色入口、服务安装脚本、PySDL2 和附 SIL OFL 的思源黑体。

```powershell
python scripts/package_aisos.py ../work/PySDL2-0.9.17-py3-none-any.whl ../outputs/rgbox-screen-color.zip --screen-color
```

将 ZIP 解压到外置游戏卡根目录并保留 `applyCenter/.screen-color/`。首次安装或升级后，在 AisOS 的 root 终端运行：

```sh
sh /storage/roms/applyCenter/.screen-color/install.sh
```

脚本在内部存储部署并启动 `rgbox-screen-color.service`，同时注册开机启动。用 `systemctl is-active rgbox-screen-color.service` 核对服务状态，然后从 RGBox 应用菜单启动 Screen Color。

### Pocket1 / Leaf 安装包

```powershell
python scripts/build_diagnostics.py
python scripts/package_preview.py --output ../outputs/pocket1-local --include-source
```

诊断脚本将 SDL 拦截和帧计时 C 模块编译成 WASM，在 Node.js 执行模拟 SDL 回归，再生成 AArch64 `build/libruffle_probe.so` 并核对导出和动态依赖。打包需要已经构建的播放器、诊断库及 Cargo 许可缓存。它校验源码与 ELF、AArch64 PIE、GLIBC ≤2.38、Leaf CONTENT-1 清单、Shell LF、ZIP 执行位和 CRC。[诊断构建](../scripts/build_diagnostics.py) · [打包脚本](../scripts/package_preview.py)

| 归档 | 内容 |
| --- | --- |
| `ruffle-leafos-<版本>.zip` | 顶层 `Apps/`、`Roms/`、`Images/`，含播放器、依赖、说明、Bloxorz 和探针 |
| `ruffle-leafos-games-<版本>.zip` | 顶层 `Roms/flash/`，包含三个独立游戏 ZIP，各自保留主 SWF 与原资源相对路径 |
| `ruffle-leafos-<版本>-source.zip` | 仅指定 `--include-source` 时生成，含源码、文档、游戏、锁文件与固定输入 |

打包按样例和游戏 manifest 核验文件集合与 SHA-256。主包的 `provenance/sample-games.json` 与 `provenance/bundled-games.json` 记录收录来源。`build-manifest.json` 中 `bundled_games`、`diagnostic_movie` 和 `optional_games` 分别记录试玩、探针和合集内容。`SHA256SUMS.txt` 只列本次生成的归档。两份安装归档各用独立暂存目录。[游戏清单与来源](../games/README.md)

## 自动发布

工作流在推送到 `main` 时比较整次推送前后的 `pak/pak.json` 中 `pak_version`，值变化触发完整构建。也可在 `main` 手动运行 **Build and release Pocket 1 ZIP**，无需预建标签。[发布工作流](../../.github/workflows/release.yml)

升版时同步 `pak/pak.json` 的 `version`、`pak_version`，前端 `Cargo.toml` 与 workspace `Cargo.lock` 中的包版本，以及 `leaf-source.lock.json` 中的 `frontend_version` 和受影响输入哈希。同步检查启动/诊断日志版本和工程进度。[版本与发布逻辑](../scripts/release.py)

发布 job 在 Windows 上从源码重建，运行构建绑定、输入、ZIP、打包回归和诊断库检查，校验两份安装归档。使用仓库 `GITHUB_TOKEN` 与 job 的 `contents: write` 创建开发预览，标签 `v<版本>` 绑定构建提交，只上传两个本版本附件。草稿重试可补传，已公开附件保留，同名 tag 指向其他提交时拒绝覆盖。Release 说明包含构建 SHA、变更列表及两个附件 SHA-256。自动流程覆盖构建和打包，Pocket1 验收见[工程进度](progress.md)。

## 仓库与本地工作目录

Git 收录源码、测试夹具、文档、锁文件和明确用途的固定输入。原始 leaf-12 ELF 用于对照与旧 adapter 校验，正式包从当前源码构建。SDL2、ALSA、字体及目标链接文件按对应来源锁保留。[第三方声明](THIRD_PARTY.md) · [忽略规则](../../.gitignore)

`outputs/`、`work/`、`adb-tools/`、`flash-transfer/`、工程 `.tools/`、`build/` 与 `workspace.local.json` 属于本地工作区。清理前核对绝对路径、Git 跟踪范围和进行中的构建。独立工作副本检查自己的 Git 状态。处理目录联接和符号链接时保留其指向的目录。

确认归档完成后，可清理 `build/preview-*`、`build/games-<版本>-*` 及相应暂存路径记录。保留当前 release 缓存、`build/ruffle-leaf`、`libruffle_probe.so` 和构建记录。若清除了配套 playerglobal，重新构建核心后再生成 AS3 夹具。按本机配置确认正在使用的 SDK、Rust、GCC、Zig 与离线依赖。清理输出目录时，保留设备数据备份、独有实验文件、验证证据和最新交付。

导入文件保留原始字节，Shell 脚本强制 LF，版本化哈希依赖这些字节。SWF、ZIP、SO、JAR 也可能是测试夹具或构建输入，应按文件用途处理。[属性规则](../../.gitattributes)

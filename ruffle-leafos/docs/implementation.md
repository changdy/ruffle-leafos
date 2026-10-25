# 工程实现细节

适用 **0.6.7 开发预览**，整理于 2026-10-25。本文维护当前实现和工程操作。安装使用见[README](../README.md)，完成状态、限制和验收见[工程进度](progress.md)。

## 目录与执行链路

| 路径 | 职责 |
| --- | --- |
| [player/sdl2test-rocknix/src/](../player/sdl2test-rocknix/src/) | 本项目掌机前端：输入、菜单、映射、光标、原生进度和音画 |
| [player/core/](../player/core/) | 固定 Ruffle core 及少量核心接口改动；其余 workspace 成员保留依赖关系 |
| [pak/](../pak/) | Leaf 清单、Flash/Apps 启动、路径、主题和诊断脚本 |
| [scripts/](../scripts/) | 构建、主机回归、诊断和打包 |
| [adapter/src/](../adapter/src/) | 复用的 SDL 诊断/物理输入采样 C 库；adapter 其他说明对应早期适配版本 |
| [runtime/leaf-12/](../runtime/leaf-12/)、[target-libs/](../target-libs/) | 固定的原始运行时对照、发布所需库/字体、目标链接输入 |
| [vendor/](../vendor/) | 固定 Leaf 契约与裁剪后的 Jawaka 菜单参考 |
| `build/`、`.tools/` | 编译包装器、Cargo 输出、诊断库、打包暂存和 SDK，均由 Git 忽略 |

游戏由 [pak.json](../pak/pak.json) 的 `ruffle_probe` 路径核心进入 [launch-game.sh](../pak/emulators/ruffle-probe/launch-game.sh)，校验所选卡内 SWF 路径、设置包外数据目录和音视频环境，再运行源码构建的播放器。Apps 经 [launch.sh](../pak/launch.sh) 进入同一播放器的数据管理模式；不推进演示 SWF。

启动器优先预加载 Leaf 系统 SDL2，以获得设备上的 Wayland 支持；随包 SDL2 作为后备。CPAL/ALSA 在可用时接 Leaf PulseAudio，已有用户 ALSA 配置优先。画面沿用 SDL/Wayland/GLES 路线；已经验证的设备版本见进度文档。

## 功能实现与源码入口

| 模块 | 实现约定 | 代码 |
| --- | --- | --- |
| 前端接入 | 事件循环、渲染、存储与平台环境衔接 | [leaf.rs](../player/sdl2test-rocknix/src/leaf.rs)、[gl_backend.rs](../player/sdl2test-rocknix/src/gl_backend.rs) |
| 暂停菜单 | 冻结 tick 与音频；恢复首帧重置时间；主子页面共享 Leaf 当前字体/主题；列表和宫格按实际操作显示提示 | [pause_menu.rs](../player/sdl2test-rocknix/src/pause_menu.rs)、[theme.sh](../pak/emulators/ruffle-probe/theme.sh) |
| 按键编辑 | 每游戏十方案，14 实体键及五个摇杆动作；五列动作宫格与九类 Tab；保存/返回独立；未保存切换提供继续、舍弃、保存；保存失败保留草稿 | [controls.rs](../player/sdl2test-rocknix/src/controls.rs)、[profiles.rs](../player/sdl2test-rocknix/src/profiles.rs) |
| 输入所有权 | 最多四键组合按序按下、反序释放，共享键引用计数；切方案、暂停、失焦或断连释放旧输入，恢复等待按键松开/摇杆归中 | [session.rs](../player/sdl2test-rocknix/src/session.rs)、[shortcuts.rs](../player/sdl2test-rocknix/src/shortcuts.rs) |
| 物理 Menu | 独立读取物理状态，组合键按下沿触发；短按松开唤起，组合键不附带打开菜单；系统长按逃生仍交由 Leaf | [probe_input.c](../adapter/src/probe_input.c)、[shortcuts.rs](../player/sdl2test-rocknix/src/shortcuts.rs) |
| 连发 | 每键 0/5/10/15 Hz，按住触发；单调时钟调度，卡顿不集中补发；仅键盘/组合键/鼠标左右键允许连发 | [turbo.rs](../player/sdl2test-rocknix/src/turbo.rs) |
| 全局光标 | 外观独立保存；空心圆中心点固定 1×1 屏幕像素；移动恢复显示，静止 3.5 秒隐藏；点击/按住/滚轮不延长显示 | [cursor.rs](../player/sdl2test-rocknix/src/cursor.rs) |
| 原生进度/画面/Apps/说明 | 内容身份、五槽、校验与写盘、确认复制/清理、本地归档；每游戏缩放/画质/FPS；独立关于单页与快捷键三组说明 | [experience.rs](../player/sdl2test-rocknix/src/experience.rs) |
| 系统音量 | 异步读取真实 Leaf 数值，仅显示系统硬件键反馈；旧每游戏音量/静音不再控制播放 | [system_volume.rs](../player/sdl2test-rocknix/src/system_volume.rs) |
| Flash FPS | 统计实际 Flash 帧执行量，按秒显示当前值 | [core/src/player.rs](../player/core/src/player.rs) |

M+A/B 切方案，M+十字下切左键保持，M+X 仅在本次运行互换十字/摇杆四方向，摇杆按下不变；交换状态不会写入映射文件。主菜单左右换方案和编辑页 L/R 沿用同一持久化/释放规则。光标页速度仅本次生效，方案内保存的速度仍随方案读取；外观由所有游戏共用。

暂停主菜单共九行，“快捷键说明”和“关于”分别路由到 `Page::Shortcuts` 与 `Page::About`。关于不处理翻页；快捷键的 `shortcut_tab` 在每次进入时复位，方向左右或肩键循环三组，A/B 仅返回主菜单。说明数据每项采用“目的、按法、补充说明”三字段，按使用场景分组；关于仅显示返回提示，快捷键增加翻页提示。两页继续处于暂停状态。

摇杆现有径向死区为 0.18，键盘/点击动作采用 0.45 按下、0.30 释放的滞回阈值。完整可调设置和坐标矩阵仍在待办，现有阈值不等于该项验收完成。

## 数据身份与持久化

用户可直接使用的数据路径列于[README](../README.md#apps-数据管理与数据路径)。正式启动器通过 Leaf 的 `USERDATA_PATH`、`SAVES_PATH`、`LOGS_PATH` 选择当前卡；用户配置与进度不写入可升级的 pak 目录。

| 数据 | 身份、格式与恢复规则 |
| --- | --- |
| 按键方案 | ROM 相对路径身份，十个 `slot-01.rmap`—`slot-10.rmap`；完整 identity 校验防止路径哈希碰撞；`selected.rmap` 记录 `Slot 1`—`Slot 10`，不替代槽位内容 |
| 映射版本 | 新写入 v5，共 46 个唯一字段；严格拒绝重复/缺失/未知字段，连发仅接受 0/5/10/15；兼容读取 v1—v4，不在加载时改写 |
| 全局光标 | `cursor.conf` 四字段：version/style/color/size；旧 arrow 只作为 ring-dot 读取别名，旧映射光标字段只校验、不再应用或自动迁移 |
| 游戏进度 | SWF 内容指纹身份，改名/移动仍匹配；RLS1、每游戏五槽、单槽最多 32 MiB；同一数据重复提交不更新时间，显示北京时间 |
| 音画 | 按内容身份保存 RAV1；旧音量/静音字段读入后不影响系统声音，当前保存使用固定中性值 |
| 清理 | Apps 确认默认取消；确认后将目标数据移入 `cleared-data/` 本地归档，不清 ROM、封面、旧 cfg、未迁移 sol 或独立音画配置 |

格式的全部字段、路径及旧版兼容关系见 [v5](formats/controls-format-v5.md)、[v4](formats/controls-format-v4.md)、[v3](formats/controls-format-v3.md)、[v2](formats/controls-format-v2.md)、[v1](formats/controls-format-v1.md)。旧文档里的模板、恢复默认及旧快捷键只描述当时版本；当前操作以 README 为准。

SharedObject 提交采用同目录临时文件写入、sync、校验及原子替换；保留有效 `.bak`，主文件损坏可从有效备份读取，主备都损坏时禁止当成空槽覆盖。显式 flush、周期请求（十秒）、打开菜单和正常退出会请求提交。切换存档先提交当前槽，成功后重启播放器读取目标槽；复制覆盖前再次确认。

新建游戏不批量写十份方案。缺少新方案 1 时可读旧此游戏 rmap，旧全局按键与 ROM 旁 cfg 不自动叠加。只有主动保存才升级旧映射并保留备份。按键按路径识别，进度按内容识别，两者迁移规则不同；具体限制与故障验收见[工程进度](progress.md)。

## 构建环境与命令

在本工程根目录执行：

```powershell
python scripts/build_player.py build --offline --isolated-config
python scripts/test_session.py
python scripts/build_diagnostics.py
python scripts/package_preview.py --output ../outputs/ruffle-0.6.7-rebuild --include-source
```

使用独立输出目录可保留已发布的安装包及其验证记录。主机测试覆盖前端状态逻辑；真机验证范围见 [工程进度](progress.md)。

依赖 Git、Python、Node.js、JDK 17、Rust 1.94.0（Windows GNU 主机及 `aarch64-unknown-linux-gnu` 标准库）、w64devkit 2.1.0、Zig 0.14.1、ARM GNU 14.2 SDK。版本与下载校验见 [Rust 工具锁](../adapter/rust-toolchain.lock.json)、[工具来源锁](../adapter/references.lock.json)和 [ARM SDK 锁](../arm-gnu.lock.json)。

本工作区默认复用 `../work/toolchains/` 的 Rust、w64devkit 和 Cargo 缓存。迁移到其他机器时，在工程根目录配置 `workspace.local.json`（不打入发布包）：

```json
{
  "host_tool_cache": "D:/tools/ruffle-host",
  "rust": "D:/tools/rust-1.94.0",
  "host_gcc": "D:/tools/w64devkit",
  "zig": "D:/tools/zig-0.14.1/zig.exe",
  "jdk": "D:/tools/jdk-17"
}
```

Cargo 缓存位于 `host_tool_cache/cargo-home/`。准备主机工具后运行 `python scripts/setup_arm_gnu.py`，下载并校验 SDK 到 `.tools/`。首次准备 Cargo 依赖时去掉 `--offline`；`--isolated-config` 使用工程配置隔离祖先目录的镜像设置。

本机受管 Windows 上 GNU 编译器启动曾报 OS623，当前构建使用 Zig 的 Clang/LLD 配合 SDK 头文件和库，包装器生成在 `build/`。全新机器构建演练保留为待办。


依赖版本是本工程固定构建输入，以上说明不代表已经完成全新机器构建验收。`package_preview.py` 需要现成的 `build/ruffle-leaf`、`build/libruffle_probe.so` 和 Cargo 许可缓存；仅运行打包命令不会重新编译播放器。`test_session.py` 使用构建步骤生成的本机链接包装器，覆盖无外部依赖的前端状态逻辑。

## 打包与验证

[package_preview.py](../scripts/package_preview.py) 校验清单版本与 Cargo 版本一致、锁定源码 SHA-256、AArch64 PIE、所需 GLIBC 不高于 2.38，并禁止用原始 leaf-12 播放器冒充源码构建结果。诊断库检查 AArch64 与动态依赖；清单使用固定 Leaf CONTENT-1 契约校验，Shell 文件需 LF，ZIP 内执行位和完整性需通过。

安装 ZIP 从 `pak/`、新编译的播放器与诊断库、固定 SDL2/ALSA/字体、生成的自有探针资源及许可组成。随包 README 使用当前项目 README，支持文档一并附带；历史交付继续保持发布时快照。`--include-source` 收录源码、文档、依赖锁、必要参考和原始 runtime，不收录内嵌 Git、SDK、构建目录或本机配置。

主机测试与静态校验后，还需按改动执行真机隔离验证、正式 Flash/Apps 通路、用户数据前后比对、安装文件指纹核对。开发时复用当前会话提供的 ADB 地址连接、截图和读取日志，不将临时地址视为固定配置。隔离验证使用单独数据目录，完整流程与具体发布结论查对应[进度及证据](progress.md)。

## 源码来源与仓库边界

| 保留内容 | 来源与保留理由 |
| --- | --- |
| Ruffle 与前端基线 | aweigit/ruffle-miyooflip 提交 `3bc39c5e0fdbc9fcde92681706395f6ef70f991b`，本项目变动及指纹见 [leaf-source.lock.json](../leaf-source.lock.json)。保留 Cargo workspace 成员、构建资源和测试 SWF/ZIP，不凭扩展名删除测试数据 |
| Jawaka 参考 | 提交 `915c48595fbd20ec56a9d23b2fc1f39a4f6f8803`，仅保留七个菜单/主题/设置/许可文件；[来源与逐文件指纹](../vendor/jawaka.lock.json) |
| Leaf 契约 | [references.lock.json](../adapter/references.lock.json) 固定的 leaf-contracts 提交；schema、校验器与测试夹具是版本化契约输入 |
| 原始 leaf-12 | 用户提供的原始播放器、库、字体及来源清单；[runtime.lock.json](../adapter/runtime.lock.json) 固定指纹。原始播放器用于对照与早期 adapter 校验，当前安装包使用新构建播放器 |
| 目标链接库 | [target-libs/provenance.json](../target-libs/provenance.json) 固定的 SDL2/ALSA/DBus 输入，构建所需，属于外部依赖 |
| adapter | C 诊断/输入库及其测试由当前工程复用；旧封装、来源审计与早期包保留用于回归和追溯，历史行为以其原文版本为准 |

原始 leaf-12 并非所有私有补丁均有公开源码；本工程基于固定公开基线重做 Leaf 前端，不声称能字节一致复现原包。第三方许可见[声明](THIRD_PARTY.md)。

Git 收录项目源码、必要资源与测试、配置、文档、依赖锁和明确用途的固定输入。`outputs/`、`work/`、`adb-tools/`、`flash-transfer/`、工程 `.tools/` 与 `build/`、所有 target/构建缓存、本机 `workspace.local.json`、环境文件和编辑器缓存均由根 [.gitignore](../../.gitignore) 排除。不要全局忽略 `.so`、`.jar`、`.zip` 或 `.swf`：其中有必要依赖与测试夹具。

[.gitattributes](../../.gitattributes) 保留导入源码的原始字节，Shell 脚本单独强制 LF；锁文件中的 SHA-256 依赖这些字节。整理时不批量格式化第三方源码。旧版本文档在 [archive/](archive/README.md)，验证摘要在 [validation/](validation/)，完整日志和截图在本地输出目录。裁剪的无关参考文件保存在本地 `work/repository-cleanup-20261025/unused-reference/`；Git 历史不做重写。

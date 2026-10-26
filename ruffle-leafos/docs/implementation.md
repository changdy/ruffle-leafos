# 工程实现细节

适用 **0.7.0 开发预览**，整理于 2026-10-25。本文维护当前实现和工程操作。安装使用见[README](../README.md)，完成状态、限制和验收见[工程进度](progress.md)。

## 命名与兼容标识

GitHub 仓库为 [changdy/ruffle-leafos](https://github.com/changdy/ruffle-leafos)，应用名称为 **Ruffle Flash**。Leaf 的游戏分类仍为 Flash，核心列表与播放器窗口显示 Ruffle Flash；Apps 入口和管理页显示“Flash 数据管理”；关于页标题为“Ruffle Flash · LeafOS 版”，版本号与开发预览状态单独显示。

安装目录 `Apps/mlp1/RuffleProbe.pak/`、核心 ID `ruffle_probe`、启动目录 `emulators/ruffle-probe/`、诊断库和环境变量沿用既有标识。控制目录 `$USERDATA_PATH/ruffle-probe/`、存档目录 `$SAVES_PATH/ruffle-leaf-preview/shared_objects/` 及内容身份算法均不因显示改名而变化，升级可继续读取原有按键与进度。早期 adapter、原始 runtime 和历史记录保留其发布时名称。

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

启动器优先预加载 Leaf 系统 SDL2，以获得设备上的 Wayland 支持；随包 SDL2 作为后备。CPAL/ALSA 在可用时接 Leaf PulseAudio，已有用户 ALSA 配置优先。窗口和菜单使用 SDL/Wayland/GLES，Flash 绘制可选择现有 GLES 或实验性 Vulkan；已经验证的设备版本见进度文档。

画面设置中的“渲染接口”进入 `experience::Page::Renderer`，以 `actual_renderer` 标识实际后端。选择不同接口进入默认取消的确认页；确认后先检查原生进度提交，退出循环再次提交成功才写入 RAV2 偏好并 exec 重启。保留当前进度槽，清除旧 `--renderer` 命令行覆盖，重启前销毁播放器、菜单、GL 上下文和窗口。保存失败不进入重启，不把菜单文字当成实际切换。

Vulkan 复用官方 Ruffle `nightly-2026-03-15` 的 `render/wgpu/`，提交 `71f18f0d0e3bd118945d5956f4ac4bb2e486e856`；该版本 `RenderBackend` 接口与本项目基线一致，54 个原始文件及 SHA-256 固定于 [ruffle-wgpu.lock.json](../vendor/ruffle-wgpu.lock.json)，不批量升级 Ruffle core。workspace 仅启用 wgpu 的本机 Vulkan/WGSL 功能。通过公开 HAL 导入接口创建逻辑设备，在上游同等 limits/features 基础上增加 DRM format modifier 扩展，并拒绝 CPU adapter。

Pocket1 原厂驱动缺少 `VK_KHR_wayland_surface`，但提供 GBM/EGL DMA-BUF 导入和 Vulkan 外部内存扩展。[vulkan_backend.rs](../player/sdl2test-rocknix/src/vulkan_backend.rs) 从 `/dev/dri/renderD128` 的 GBM 分配线性 ABGR8888 缓冲区，以明确 modifier/plane layout 导入 Vulkan，在官方 `WgpuRenderBackend` 的自定义 `RenderTarget` 中绘制，再以 EGLImage 作为 GLES 纹理完成全屏呈现；现有 `PauseMenu` 继续叠加菜单、光标和提示。显示链路没有逐帧 CPU 像素读回，不需要替换系统 GPU 驱动或 WSI layer。直接导出 Vulkan 分配再交 EGL 的方案在当前驱动上失败，因此使用共享 GBM 分配。

所有队列提交在 Flash 线程串行执行。Vulkan 绘制后通过原生 barrier 将图像释放到 `QUEUE_FAMILY_EXTERNAL`/GENERAL，有限超时 fence 等待完成后交 EGL/GLES 采样；`glFinish` 确保采样完成后才允许下一帧重新获取图像。当前同步较保守，不宣称速度或功耗优于 GLES。导入用重复 FD，成功的 `vkAllocateMemory` 接管该 FD，GBM/EGL/纹理与底层分配分别保留生命周期；resize/退出先等待 GPU，再释放同步、图像和内存资源。

`leaf-graphics actual=` 记录实际 Flash 绘制接口，`leaf-vulkan` 记录硬件 adapter、共享布局、首次呈现与累计帧数。`GL_*` 始终描述显示上下文，不能单独证明 Flash 使用 GLES。偏好启动的 Vulkan 初始化失败会显示提示、恢复 GLES 偏好并继续游戏；捕获的运行期 GPU 错误会终止循环、尝试提交原生进度，并将下次偏好恢复为 GLES。诊断用 `--renderer opengl-es|vulkan` 只覆盖本次选择，显式指定 Vulkan 时初始化错误直接返回，便于验证，Apps 始终使用 GLES。RAV1/RAV2 契约见[渲染设置格式](formats/graphics-settings.md)。

## 功能实现与源码入口

| 模块 | 实现约定 | 代码 |
| --- | --- | --- |
| 前端接入 | 事件循环、GLES/Vulkan 后端选择、存储与平台环境衔接 | [leaf.rs](../player/sdl2test-rocknix/src/leaf.rs)、[gl_backend.rs](../player/sdl2test-rocknix/src/gl_backend.rs)、[vulkan_backend.rs](../player/sdl2test-rocknix/src/vulkan_backend.rs) |
| 暂停菜单 | 冻结 tick 与音频；恢复首帧重置时间；主子页面共享 Leaf 当前字体/主题；列表和宫格按实际操作显示提示 | [pause_menu.rs](../player/sdl2test-rocknix/src/pause_menu.rs)、[theme.sh](../pak/emulators/ruffle-probe/theme.sh) |
| 按键编辑 | 每游戏十方案，14 实体键及五个摇杆动作；五列动作宫格与九类 Tab；保存/返回独立；未保存切换提供继续、舍弃、保存；保存失败保留草稿 | [controls.rs](../player/sdl2test-rocknix/src/controls.rs)、[profiles.rs](../player/sdl2test-rocknix/src/profiles.rs) |
| 输入所有权 | 最多四键组合按序按下、反序释放，共享键引用计数；切方案、暂停、失焦或断连释放旧输入，恢复等待按键松开/摇杆归中 | [session.rs](../player/sdl2test-rocknix/src/session.rs)、[shortcuts.rs](../player/sdl2test-rocknix/src/shortcuts.rs) |
| 物理 Menu | 独立读取物理状态，组合键按下沿触发；短按松开唤起，组合键不附带打开菜单；系统长按逃生仍交由 Leaf | [probe_input.c](../adapter/src/probe_input.c)、[shortcuts.rs](../player/sdl2test-rocknix/src/shortcuts.rs) |
| 连发 | 每键 0/5/10/15 Hz，按住触发；松开阶段调度不超过 10 ms，其余周期按下；单调时钟、截止时间唤醒，卡顿不集中补发；仅键盘/组合键/鼠标左右键允许连发 | [turbo.rs](../player/sdl2test-rocknix/src/turbo.rs) |
| 全局光标 | 外观独立保存；空心圆中心点固定 1×1 屏幕像素；移动恢复显示，静止 3.5 秒隐藏；点击/按住/滚轮不延长显示 | [cursor.rs](../player/sdl2test-rocknix/src/cursor.rs) |
| 原生进度/画面/Apps/说明 | 内容身份、五槽、校验与写盘、确认复制/清理、本地归档；每游戏缩放/画质/FPS；独立关于单页与快捷键三组说明 | [experience.rs](../player/sdl2test-rocknix/src/experience.rs) |
| 系统音量 | 异步读取真实 Leaf 数值，仅显示系统硬件键反馈；旧每游戏音量/静音不再控制播放 | [system_volume.rs](../player/sdl2test-rocknix/src/system_volume.rs) |
| Flash FPS | 统计实际 Flash 帧执行量，按秒显示当前值 | [core/src/player.rs](../player/core/src/player.rs) |

M+A/B 按编号向前/后循环查找非空方案，跳过全空方案，没有其他候选时保持当前方案；主菜单左右与编辑页 L/R 仍可选择全部十个方案。`Mapping::is_empty` 统一按 14 个按键和 5 个摇杆动作均为 `None` 且 `fine=false` 判断；鼠标精调是有效动作，基底、摇杆预设和速度等元数据不单独使方案非空。切换继续使用同一持久化/释放规则。M+十字下切左键保持，M+X 仅在本次运行互换十字/摇杆四方向，摇杆按下不变；交换状态不会写入映射文件。光标页速度仅本次生效，方案内保存的速度仍随方案读取；外观由所有游戏共用。

暂停主菜单共九行，“快捷键说明”和“关于”分别路由到 `Page::Shortcuts` 与 `Page::About`。关于不处理翻页，以三张卡片展示项目介绍、完整 GitHub 仓库地址和开源致谢，底部单独显示许可说明；文案维护在 `experience::BACKGROUND` 与 `pause_menu::draw_about`。快捷键的 `shortcut_tab` 在每次进入时复位，方向左右或肩键循环三组，A/B 仅返回主菜单。说明数据每项采用“目的、按法、补充说明”三字段，按使用场景分组；关于仅显示返回提示，快捷键增加翻页提示。两页继续处于暂停状态。

摇杆现有径向死区为 0.18，键盘/点击动作采用 0.45 按下、0.30 释放的滞回阈值。完整可调设置和坐标矩阵仍在待办，现有阈值不等于该项验收完成。

连发完整周期为 `1_000_000 / Hz` 微秒，按下阶段为周期减 8 ms，松开阶段为 8 ms，为用户要求的 10 ms 上限留出 2 ms 调度余量。小幅延迟保留原截止时间，可缩短接下来的松开阶段；错过完整阶段时从当前时间安排该阶段，单次更新最多翻转一次，不集中追赶。`Repeater::next_delay` 返回所有启用连发源的最近截止时间；主循环在 tick 前和渲染后检查，并以截止时间缩短原 16 ms 睡眠。输入事件仍在 Flash 主线程派发，脚本、GL 或系统调度阻塞可能延长实际松开间隔，不能保证任意负载下的硬实时时限。诊断输入日志附带单调时钟微秒时间，便于区分调度时序和设备实测。

## 数据身份与持久化

### 受限内存下的静态绘图资源

`core/src/display_object/graphic.rs` 在首次实际显示时创建静态 `ShapeHandle`，在所有实例间共享；屏外剔除、形状命中检测和预加载不触发 GPU 上传。`core/src/character.rs` 的位图库通过弱引用复用仍存活的纹理，压缩 SWF 素材继续保留，可重新解码。

Leaf 在每次完整游戏帧 `Player::render()` 提交后调用 `collect_static_render_resources()`，遍历已知电影的静态图形。保留本帧使用的句柄，释放本帧未显示的缓存；帧命令、其他图形或正在执行的绘制仍可持有资源。动态 BitmapData、缓存位图、滤镜与游戏主动保留的数据不在此静态缓存回收范围内。回收按已显示的完整帧执行，不能在局部/离屏绘制中途调用。重新显示可能产生解码和上传成本，不能据此保证所有游戏不再 OOM。

用户可直接使用的数据路径列于[README](../README.md#apps-数据管理与数据路径)。正式启动器通过 Leaf 的 `USERDATA_PATH`、`SAVES_PATH`、`LOGS_PATH` 选择当前卡；用户配置与进度不写入可升级的 pak 目录。

| 数据 | 身份、格式与恢复规则 |
| --- | --- |
| 按键方案 | ROM 相对路径身份，十个 `slot-01.rmap`—`slot-10.rmap`；完整 identity 校验防止路径哈希碰撞；`selected.rmap` 记录 `Slot 1`—`Slot 10`，不替代槽位内容 |
| 映射版本 | 新写入 v5，共 46 个唯一字段；严格拒绝重复/缺失/未知字段，连发仅接受 0/5/10/15；兼容读取 v1—v4，不在加载时改写 |
| 全局光标 | `cursor.conf` 四字段：version/style/color/size；旧 arrow 只作为 ring-dot 读取别名，旧映射光标字段只校验、不再应用或自动迁移 |
| 游戏进度 | SWF 内容指纹身份，改名/移动仍匹配；RLS1、每游戏五槽、单槽最多 32 MiB；同一数据重复提交不更新时间，显示北京时间 |
| 音画与渲染 | 按内容身份写 RAV2，兼容只读 RAV1 并默认 GLES；旧音量/静音字段不影响系统声音，新写固定中性值；字段见[契约](formats/graphics-settings.md) |
| 清理 | Apps 确认默认取消；确认后将目标数据移入 `cleared-data/` 本地归档，不清 ROM、封面、旧 cfg、未迁移 sol 或独立音画配置 |

格式的全部字段、路径及旧版兼容关系见 [v5](formats/controls-format-v5.md)、[v4](formats/controls-format-v4.md)、[v3](formats/controls-format-v3.md)、[v2](formats/controls-format-v2.md)、[v1](formats/controls-format-v1.md)。旧文档里的模板、恢复默认及旧快捷键只描述当时版本；当前操作以 README 为准。

SharedObject 提交采用同目录临时文件写入、sync、校验及原子替换；保留有效 `.bak`，主文件损坏可从有效备份读取，主备都损坏时禁止当成空槽覆盖。显式 flush、周期请求（十秒）、打开菜单和正常退出会请求提交。切换存档先提交当前槽，成功后重启播放器读取目标槽；复制覆盖前再次确认。

新建游戏不批量写十份方案。`Mapping::group` 仅为方案 1、2 提供默认键位；3—10 的 19 个绑定全为 `None`，摇杆为 `Stick::None`、精调与连发关闭。全空映射允许读取、保存、应用并写入 `selected.rmap`，便于清空已有方案或进入空方案编辑。已有槽位文件优先于默认值，不自动清空旧数据。缺少新方案 1 时可读旧此游戏 rmap，旧全局按键与 ROM 旁 cfg 不自动叠加。只有主动保存才升级旧映射并保留备份。按键按路径识别，进度按内容识别，两者迁移规则不同；具体限制与故障验收见[工程进度](progress.md)。

## 按需性能诊断

播放器接受 `--trace-performance`，默认关闭；需在已有 Leaf 启动环境中使用，可配合 `--diagnostic-seconds`、显式 `--renderer` 及独立 `--storage-dir`/`--controls-dir`。它只增加日志，不改变回收策略、帧调度或渲染接口。实现见 `sdl2test-rocknix/src/performance.rs` 和 `core/src/render_metrics.rs`。

每秒的 `leaf-perf` 日志包含窗口长度、Flash 帧推进量 `frames`、非暂停绘制次数 `draws`、异步加载任务/tick/render/静态资源回收/swap 的总墙钟耗时及最长 tick/render/循环；循环耗时不含主动 sleep。暂停绘制和菜单开销不计入 `draws`/`render`，因此基准应排除暂停和恢复边界窗口。`frames / 窗口秒数` 与画面提交速率 `draws / 窗口秒数` 分别统计，逻辑补帧时二者可以不同。

`shapes`、`bitmaps`、`bitmap_reloads`、`evictions` 记录静态图形创建、位图成功上传、弱缓存失效后的重载和缓存句柄释放次数；`bitmap_rgba_mib` 是宽×高×4 的等价累计尺寸，不是实际驻留内存。`shape_ms` 包含位图解码/上传，且包含于 `render_ms`；Vulkan 的 `vk_submit_ms` 包含队列提交和 fence 等待，`vk_poll_ms`、`vk_present_ms` 记录驱动轮询与最终 GLES 采样/`glFinish`，也包含于 `render_ms`。细分项不能与上层耗时重复相加，也不能将墙钟等待等同 GPU 纯执行时间。核心计数器按线程启用和取出，关闭时不读取高频计时器；不要跨线程汇总时误当全局统计。实测示例及限制见[拳皇性能报告](validation/kof-performance-20261026.md)。

## 构建环境与命令

在本工程根目录执行：

```powershell
python scripts/build_player.py build --offline --isolated-config
python scripts/test_session.py
python scripts/build_player.py test --package ruffle_core --offline --isolated-config
python scripts/build_diagnostics.py
python scripts/package_preview.py --output ../outputs/ruffle-leafos-0.7.0-rebuild --include-source
```

使用独立输出目录可保留已发布的安装包及其验证记录。主机测试覆盖前端状态逻辑；真机验证范围见 [工程进度](progress.md)。

`test --package ruffle_core` 只编译 ARM64 release 核心单元测试，不自动执行。两个静态资源测试可用生成的 `ruffle_core-<hash> static_ --nocapture` 运行。该 Rust 测试框架与应用 ELF 的系统要求可能不同；本轮测试框架需要 GLIBC 2.39，因此仅在隔离调试目录通过工具链自带的匹配加载器、libc/libm 执行，没有替换设备系统库。详情见[验证记录](validation/fighting-games-20261026.json)。

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

新生成的安装归档名为 `ruffle-leafos-<版本>.zip`，源码归档为 `ruffle-leafos-<版本>-source.zip`；旧交付目录及历史归档保留原名。

安装 ZIP 从 `pak/`、新编译的播放器与诊断库、固定 SDL2/ALSA/字体、生成的自有探针资源及许可组成。随包 README 使用当前项目 README，支持文档一并附带；历史交付继续保持发布时快照。`--include-source` 收录源码、文档、依赖锁、必要参考和原始 runtime，不收录内嵌 Git、SDK、构建目录或本机配置。

主机测试与静态校验后，还需按改动执行真机隔离验证、正式 Flash/Apps 通路、用户数据前后比对、安装文件指纹核对。开发时复用当前会话提供的 ADB 地址连接、截图和读取日志，不将临时地址视为固定配置。隔离验证使用单独数据目录，完整流程与具体发布结论查对应[进度及证据](progress.md)。

## GitHub Actions 自动发布

仓库根目录的 [release.yml](../../.github/workflows/release.yml) 在推送到 `main` 时比较推送前后 `pak/pak.json` 的 `pak_version`，仅版本值变化才运行完整构建；普通描述修改不会打包。比较使用整次推送前的提交，支持一次推送多个 commit 和合并提交。`main` 首次创建时的推送视为首次发布。Actions 中的 **Build and release Pocket 1 ZIP → Run workflow** 可在 `main` 手动发布当前版本或重试失败流程，不需要预先创建 tag。

发布版本使用 `主版本.次版本.修订号`（如 `0.6.9`）。升级时同步维护：

- `pak/pak.json` 的 `version` 与 `pak_version`。
- `player/sdl2test-rocknix/Cargo.toml` 的包版本，以及 `player/Cargo.lock` 中该包的版本。
- `leaf-source.lock.json` 的 `frontend_version`、`leaf_cargo_lock_sha256` 和 `leaf_files_sha256` 内受影响文件的 SHA-256；其中 `Cargo.lock` 同时有逐文件指纹。
- README、进度及其他受影响的版本说明。

[release.py](../scripts/release.py) 在编译前检查版本一致性、源码锁和已有 tag。版本未变化时跳过构建；版本变化但锁未更新时明确失败。`v<版本>` 已指向其他提交时拒绝覆盖，必须升新版本。相同提交重跑时允许继续；已发布的 Release 保留原附件，上传中断留下的草稿可补传并发布。发布使用 GitHub concurrency，每次只运行一个工作流，保留正在进行的构建；多次快速推送时 GitHub 只保留最新的待运行请求。

构建在 `windows-2022` 上执行，复用现有 Windows GNU → ARM64 路线。[setup_release_tools.py](../scripts/setup_release_tools.py) 按已有工具锁下载并逐个校验 Rust 1.94.0（含 GNU 主机和 ARM64 标准库）、w64devkit 2.1.0、Zig 0.14.1 和 ARM GNU 14.2 SDK，在工程 `.tools/` 中解包，并生成忽略的 `workspace.local.json`。Python 3.12、Node.js 22、JDK 17 由工作流准备，7-Zip 由 runner 提供。工具和 Cargo 下载缓存分别保存在 `.tools/` 和根目录 `work/toolchains/cargo-home/`；每次仍从源码编译播放器，不缓存成品播放器。

完整流水线依次执行：发布脚本回归、ARM64 release 编译、主机输入/状态回归、诊断库编译与测试、现有打包脚本的 ELF/GLIBC/源码/清单/ZIP 校验。自动打包不加 `--include-source`，只向 Release 附加一个 `ruffle-leafos-<版本>.zip`；GitHub 自带的 Source code 下载由平台生成。ZIP 顶层直接是 `Apps/`、`Roms/`、`Images/`，不含本机配置、SDK、构建缓存或外层版本目录。

流程使用仓库自带的 `GITHUB_TOKEN`，仅发布 job 申请 `contents: write`，不需要单独配置 PAT。自动创建 `v<版本>` tag，绑定实际构建的提交；当前均以开发预览（Prerelease）发布。说明中写入完整构建 SHA 及链接、上一可达 `v主.次.修订` tag 之后的 commit SHA/标题/作者、比较链接、安装方法和 ZIP SHA-256；首次发布列出全部历史。CI 说明明确标注未进行实机验证。

在仓库根目录可独立验证发布逻辑：

```powershell
python -m unittest discover -s ruffle-leafos/scripts -p test_release.py -v
```

线上 runner 执行和实际 Release 上传的验收状态见[工程进度](progress.md)。

## 源码来源与仓库边界

| 保留内容 | 来源与保留理由 |
| --- | --- |
| Ruffle 与前端基线 | aweigit/ruffle-miyooflip 提交 `3bc39c5e0fdbc9fcde92681706395f6ef70f991b`，本项目变动及指纹见 [leaf-source.lock.json](../leaf-source.lock.json)。保留 Cargo workspace 成员、构建资源和测试 SWF/ZIP，不凭扩展名删除测试数据 |
| 官方 wgpu 后端 | Ruffle 提交 `71f18f0d0e3bd118945d5956f4ac4bb2e486e856` 的 54 个原始文件；[来源锁](../vendor/ruffle-wgpu.lock.json) 固定字节与双许可，打包验证并随包附带来源锁 |
| Jawaka 参考 | 提交 `915c48595fbd20ec56a9d23b2fc1f39a4f6f8803`，仅保留七个菜单/主题/设置/许可文件；[来源与逐文件指纹](../vendor/jawaka.lock.json) |
| Leaf 契约 | [references.lock.json](../adapter/references.lock.json) 固定的 leaf-contracts 提交；schema、校验器与测试夹具是版本化契约输入 |
| 原始 leaf-12 | 用户提供的原始播放器、库、字体及来源清单；[runtime.lock.json](../adapter/runtime.lock.json) 固定指纹。原始播放器用于对照与早期 adapter 校验，当前安装包使用新构建播放器 |
| 目标链接库 | [target-libs/provenance.json](../target-libs/provenance.json) 固定的 SDL2/ALSA/DBus 输入，构建所需，属于外部依赖 |
| adapter | C 诊断/输入库及其测试由当前工程复用；旧封装、来源审计与早期包保留用于回归和追溯，历史行为以其原文版本为准 |

原始 leaf-12 并非所有私有补丁均有公开源码；本工程基于固定公开基线重做 Leaf 前端，不声称能字节一致复现原包。第三方许可见[声明](THIRD_PARTY.md)。

Git 收录项目源码、必要资源与测试、配置、文档、依赖锁和明确用途的固定输入。`outputs/`、`work/`、`adb-tools/`、`flash-transfer/`、工程 `.tools/` 与 `build/`、所有 target/构建缓存、本机 `workspace.local.json`、环境文件和编辑器缓存均由根 [.gitignore](../../.gitignore) 排除。不要全局忽略 `.so`、`.jar`、`.zip` 或 `.swf`：其中有必要依赖与测试夹具。

[.gitattributes](../../.gitattributes) 保留导入源码的原始字节，Shell 脚本单独强制 LF；锁文件中的 SHA-256 依赖这些字节。整理时不批量格式化第三方源码。旧版本文档在 [archive/](archive/README.md)，验证摘要在 [validation/](validation/)，完整日志和截图在本地输出目录。裁剪的无关参考文件保存在本地 `work/repository-cleanup-20261025/unused-reference/`；Git 历史不做重写。

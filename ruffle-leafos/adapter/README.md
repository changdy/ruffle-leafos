# Ruffle Leaf 0.1.4 开发预览

本版落实主需求中的环境采集、启动保护和解除验证输入拦截。它沿用已播放成功的 leaf-12 原播放器，**不是完整日常版**。

## 本次可以做什么

- 从 Leaf 游戏库的 Flash 分类选择 SWF，直接交给原播放器操作；游戏键不再被本次测试层截获并退出。
- 从 Apps 打开 Ruffle Probe，仍播放内置测试动画，松开启动键后再按任意游戏键退出。
- 启动时采集本地环境报告，记录 ABI、内核、可用内存、挂载、音频、输入设备和可找到的系统版本文件。
- 在实际播放器 GL context 上记录 vendor、renderer、GL/GLSL 版本及软件渲染线索；每 300 次交换记录呈现间隔和交换调用耗时。
- 检查本地数据和日志目录是否能实际写入；隔离 HOME/XDG/工作目录；保留日志上一轮。更新只替换 pak，不主动迁移或删除旧进度。

原播放器的按键和鼠标映射保持其已有实现。单摇杆鼠标、A/B 语义、启动按键是否误触仍需真机核对。**本版没有新增 12 组模板、内部暂停菜单、虚拟键盘、可靠存档／备份或休眠恢复。** 没有把这些能力放进菜单，也没有开启 supports_menu 或 supports_savestate。

0.1.4 热修复：将包内原有 CJK 字体接入 leaf-12 实际扫描的 HOME 字体目录，避免游戏备用字体数量为 0。缓存位于 HOME/.local/share/fonts/ruffle-leaf/MiSans-Normal.ttf；该文件名是 leaf-12 选择备用字体所需的兼容别名，字体内容与包内 menu-font.ttf 相同。启动时校验并通过临时文件替换缓存，不覆盖用户另放的字体。音频默认接入 Leaf 现有 PulseAudio，避免原厂 ALSA 选择 HDMI 声卡导致的 I/O 错误；仅在检测到 Leaf 的 Pulse socket 和 ALSA pulse 插件、且没有自定义 ALSA_CONFIG_PATH / HOME/.asoundrc 时启用。报告新增 Leaf 安装版本记录，并兼容缺少 getconf 的 Buildroot libc 版本采集。

## 安装和使用

1. 将 ruffle-leaf-probe-0.1.4.zip 解压到 Leaf **主卡根目录**，覆盖 Apps/mlp1/RuffleProbe.pak/，不要多套一层文件夹。
2. 自己的游戏放 Roms/flash/；多文件游戏保留原有资源子目录。封面目录为 Images/flash/。系统 ID FLASH 和核心 ID ruffle_probe 保持不变。
3. 重新扫描游戏库，从“Flash”选择游戏。**游戏入口现在不会任意键退出。Menu 仍由 Leaf 负责退出，没有游戏内菜单。** 原播放器若卡死，使用 Leaf 的长按 Menu 逃生手势。
4. Apps 的 Ruffle Probe 仍是画面测试，不能把 Apps 的任意键退出行为误认为游戏入口回退。

包内 min_leaf_version 仍沿用 0.12.0。SSH 已从目标机安装记录确认 Leaf 0.13.0、Buildroot 2021.11、glibc 2.38、内核 5.10.209；0.1.3 实际播放器日志为 Mali-G52 / OpenGL ES 3.2。帧时间、画面和音频的完整验收仍需分类测试。

升级和回滚均只覆盖 Apps/mlp1/RuffleProbe.pak/。保留游戏、USERDATA_PATH/ruffle-probe/ 及 SAVES_PATH 下原有内容。回滚到原 0.1.2 ZIP 会恢复任意键退出的测试行为。本次不覆盖或改写原 0.1.2 交付物，不自动重命名旧 Roms/FLASH，也不猜测旧存档的身份。

## 无 SSH 时如何回传诊断

先运行一次 Apps 测试，再从 Flash 分类运行一个游戏，停留几秒后通过 Menu 返回。随后将卡接回电脑，取 Leaf 的 LOGS_PATH（通常为 .userdata/mlp1/logs/）下这两份文件：

- ruffle-environment.txt：环境、版本来源、挂载及设备信息。
- ruffle-probe.log：当前启动、实际 GL context 和采样结果；上一轮为 .previous。

版本文件找不到时报告会保留 unknown，不使用 pak 的最低版本冒充真实版本。采集器仅写本地报告，不上传数据，不改变系统设置、不启动 SSH 服务。手动采集命令为包根目录的 collect-diagnostics.sh；自动采集无需先开 SSH。

有 SSH 时，在设备上执行（实际路径以卡挂载位置为准）：

~~~sh
sh <主卡>/Apps/mlp1/RuffleProbe.pak/collect-diagnostics.sh
~~~

## 参数与测量含义

普通使用不需要设置环境变量。开发调试可用：

| 环境变量 | 值与行为 |
| --- | --- |
| RUFFLE_SESSION_MODE | native（游戏默认，输入透传）或 probe（测试，任意新游戏键退出） |
| RUFFLE_START_IN_MOUSE_MODE | 1（原播放器鼠标模式，默认）或 0；probe 模式固定 0 |
| RUFFLE_PROBE_TIMING | 1 开启采样，0 关闭；不改变原渲染同步或绘制逻辑 |
| RUFFLE_PROBE_DRY_RUN | 1 只打印参数、目录和模式，不启动播放器或写报告 |

interval_* 是两次 SDL_GL_SwapWindow 返回之间的墙钟间隔，swap_* 是该调用本身的墙钟耗时。p50/p95/p99 是 2 的幂次直方图上界，字段带 upper；它们不是精确分位数、GPU 执行时间或实际游戏 FPS。原播放器可能限帧、停顿或只按需渲染。软件 renderer 名称检测仅是线索，不能据此宣称 GPU 效率已验收。

环境报告是启动快照；运行中的 CPU/RSS/GPU 内存与功耗、画面效果和长时稳定性仍待设备测量。报告限量读取已知文件（每个最多 64 KiB），不会输出整份环境变量或扫描整卡游戏。

## 数据与已知限制

HOME、XDG 配置、缓存和数据目录均位于 USERDATA_PATH/ruffle-probe/，播放器工作目录也是该目录；包更新不替换这里的内容。Leaf 已导出的所选卡路径优先于 env.sh 的缺省卡路径。

本版记录 SAVES_PATH，但**没有宣称已接好存档后端**，也不将旧原生存档搬到新位置。现有二进制如何写 SharedObject、正常退出是否完成提交仍需验证；Menu 的系统 SIGTERM 退出没有新增可靠保存保证。不要把“数据目录存在”理解为保存已完成。

启动错误会输出可理解的原因并返回；尚未新增图形错误页。目录只读、满卡或报告失败可从日志定位。测试层不会在 native 模式走两秒强制退出兜底；这不等于已经实现正式会话的保存退出流程。

## 源码与重建

本次适配源码在 ruffle-leaf-probe-source-0.1.4.zip。原 leaf-12 二进制、SDL2、ALSA 和字体 hash 由 runtime.lock.json 固定；它们没有被修改。

另外找到了公开基础源码候选 [aweigit/ruffle-miyooflip@3bc39c5e0fdbc9fcde92681706395f6ef70f991b](https://github.com/aweigit/ruffle-miyooflip/tree/3bc39c5e0fdbc9fcde92681706395f6ef70f991b)。该提交对应短提交及前端路径，但缺少 leaf-12 的 Leaf 专用修改，不能证明可逐字节重建原二进制。player-source.lock.json 固定其 Cargo.lock 和关系边界，单独提供基础源码快照以便后续接续。

重建本次适配包需要 Python 3、Node.js、Zig 0.14.1、固定 leaf-contracts 检出、BusyBox 或 /bin/sh，以及原 leaf-12 输入包。在适配源码根目录执行：

~~~text
python scripts/build.py --runtime-pak <原ruffle.pak> --zig <zig.exe> --output <交付目录> --contract <leaf-contracts固定检出> --shell <BusyBox或sh> --node <node.exe> --player-source <候选源码检出>
~~~

--player-source 可省略；提供时会核验完整提交、工作树和 Cargo.lock。它不会悄悄替换安装包里的原播放器。工具链和规范锁文件已包含在适配源码中。

## 验证结果

桌面已通过 44 项原 wrapper 回归、41 项新增会话/诊断检查、编译到 WASM 的 C 输入状态测试及实际 SDL 拦截实现测试、AArch64 ELF/导入检查、固定官方 CONTENT-1 校验、ZIP CRC/LF/脚本执行位和小写目录检查。

Windows 未具备创建符号链接的权限，因此符号链接逃逸动态用例明确记录为跳过；普通路径穿越已检查。已在 ARM64 真机采到 GL context，并通过临时隔离目录验证字体发现和 PulseAudio 流无 I/O 错误；听感、画面效果、单摇杆/ABXY、Menu、存档和休眠仍需分别验收。详见同目录构建报告及实施记录。

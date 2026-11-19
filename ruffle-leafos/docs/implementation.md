# 工程实现细节

Ruffle Flash 面向 **Miniloong Pocket 1 / leafOS**。共享核心、SWF 和官方渲染模块基于 Ruffle v0.7.1（`89a7049`），Leaf 前端及本地适配由本项目维护。应用版本见清单，源码来源与哈希见锁文件。[应用清单](../pak/pak.json) · [源码锁](../leaf-source.lock.json) · [第三方声明](THIRD_PARTY.md)

## 源码目录

Pocket AIS 的 `aisos/Ruffle Flash.app` 启动 `aisos/launcher.py`，后者用系统 SDL2、SDL2_ttf 和随包的 PySDL2 绘制 Ruffle 自己的游戏列表。已核对的 RGBox 与 EmulationStation 系统清单没有 Flash 分类。包通过 `applyCenter/Ruffle Flash.app` 注册应用，列表枚举外置卡 `flash/` 下的 SWF、ZIP 和子目录。选择游戏后，启动器释放窗口和手柄，将路径作为独立参数交给播放器，等待播放器退出后重开原目录。从列表按 Y 时，启动器用随包的 manager.swf 和 --app-settings 进入 Flash 数据管理，返回后恢复所在目录和选项。存档、配置与 ZIP 缓存在卡上 `gamedata/ruffle-flash/`，程序和字体在 `applyCenter/.ruffle-flash/`。[应用入口](../aisos/Ruffle%20Flash.app) · [游戏列表](../aisos/launcher.py)

启动脚本为内置 `play_joystick` 的 GUID 设置 SDL 映射并启用 Pocket AIS 输入。左右摇杆点击分别映射为 `leftstick:b11` 和 `rightstick:b12`，L2/R2 读取模拟轴 2 和 5。Home（`KEY_HOME=102`）经 evdev 进入 Menu 状态机，按键释放时区分短按和组合键。Home 加肩键调用 `/usr/bin/brightness`，其余组合沿用菜单快捷键。右摇杆方向和 R3 独立应用每游戏方案及连发配置。切换上下文后，两根摇杆各自回中后才接受对应输入。列表分别记录两根摇杆的纵向输入，L1/R1 每次移动八项。实体 Fn（KEY_FN=464）映射为 SDL Misc1（b13），用于目录和菜单返回。游戏进行中按 Fn 只打开暂停菜单并选中「保存并返回游戏列表」，仍需 A 确认。Select 保留为游戏键和软键盘大小写切换。Pocket AIS 画面设置省略渲染接口切换。[快捷键](../player/sdl2test-rocknix/src/shortcuts.rs) · [映射格式](formats/controls-format-v5.md)

Pocket AIS 的游戏列表在启动播放器时传入同一套配色。暂停、按键、画面、软键盘和数据管理使用深蓝背景、青色矩形焦点，以及同样的底栏规则。LeafOS 菜单继续从系统主题读取配色和焦点形状。[界面绘制](../player/sdl2test-rocknix/src/pause_menu.rs)

AisOS 的 Screen Color 以 SDL 显示预览，使用系统 libdrm 找到内置 DSI-1 当前的 CRTC，再通过驱动的 Gamma 接口写入 RGB 查找表。暖色档位为 0–20，逐档降低蓝色和少量绿色，0 使用三通道线性表。校色在 Rockchip 显示控制器的输出级完成。RGBox、Wayland 游戏及 UI 共用这段显示链路。Weston 和内核沿用系统版本。[调色程序](../aisos/screen_color.py) · [预览界面](../aisos/screen_color_ui.py)

外卡入口和内部存储的开机服务通过 `/run/rgbox-screen-color/lock` 串行化。按 A 后，内部 `/storage/.config/rgbox-screen-color/level` 经临时文件和原子替换保存。取消会恢复进入应用前的已保存档位。若预览进程异常退出，服务释放锁后按预览标记恢复。服务检测输出模式或系统休眠时间变化后重传已保存的表。[服务安装](../aisos/screen-color-install.sh)

| 路径 | 职责 |
| --- | --- |
| [player/sdl2test-rocknix/src/](../player/sdl2test-rocknix/src/) | Pocket1 前端，处理设备输入、菜单、进度、语言与音画 |
| [player/core/](../player/core/) · [player/swf/](../player/swf/) · [player/render/](../player/render/) | Flash VM、SWF 解析及共享渲染接口 |
| [pak/](../pak/) | Leaf 清单、Flash 与 Apps 启动、路径、主题和诊断 |
| [adapter/src/](../adapter/src/) | 正式包复用的 SDL 诊断与物理按键 C 模块 |
| [scripts/](../scripts/) | 构建、主机测试、诊断与打包 |
| [runtime/leaf-12/](../runtime/leaf-12/) · [target-libs/](../target-libs/) | 原始运行时对照、随包库与字体、ARM64 链接库 |
| [vendor/](../vendor/) | Leaf 契约与 Jawaka、Catastrophe 引用 |
| [games/](../games/README.md) | 试玩、可选合集、来源和校验清单 |

## 启动与平台接口

安装路径为 `Apps/mlp1/RuffleProbe.pak/`，Leaf 系统 ID 为 `FLASH`，核心 ID 为 `ruffle_probe`。清单要求 Leaf 0.13.0，声明 `supports_savestate=false`。Flash 核心经 `emulators/ruffle-probe/launch-game.sh` 接收所选卡内的绝对路径。脚本解析符号链接后再次检查路径属于对应的 Flash ROM 根目录。Leaf 以 pass-through 方式将 ZIP 交给播放器。[清单](../pak/pak.json) · [游戏启动脚本](../pak/emulators/ruffle-probe/launch-game.sh)

Apps 中的“Flash 数据管理”经 `pak/launch.sh` 启动同一个 ELF 的管理模式，使用 GLES 菜单界面。游戏和 Apps 共享语言、配置及数据目录。[Apps 启动脚本](../pak/launch.sh)

启动器优先预加载系统 SDL2，获取设备的 Wayland 支持，随包 SDL2 作为后备。CPAL 经 ALSA 输出，在 Leaf 的 PulseAudio 路由可用时使用该路由，显式 ALSA 配置优先。音频初始化失败时前端降级静音。Rust 前端工具使用 Rust TLS。Leaf 的窗口和菜单使用 SDL/Wayland/GLES。[渲染与诊断](rendering.md)

## ZIP 游戏与目录资源

`zip_game.rs` 流式读取归档计算 SHA-256，选择层级最浅的 SWF。同层有多个候选时，必须有唯一一个与 ZIP 主文件名匹配。归档最多 10,000 项、解压总量最多 4 GiB，拒绝越界路径、符号链接和大小写冲突。后台准备时由 `zip_loading.rs` 绘制进度条并显示失败提示。[提取与校验](../player/sdl2test-rocknix/src/zip_game.rs) · [加载界面](../player/sdl2test-rocknix/src/zip_loading.rs)

首次提取成功后，暂存目录通过重命名发布到 `$SAVES_PATH/ruffle-leaf-preview/cache/zip-games/<SHA-256>/`。启动脚本把 ZIP 缓存放在 Leaf 的 `.userdata` 树外，因为 SharedObject 会拒绝游戏 URL 中以点开头的路径组件。缓存复用检查完成标记、文件存在性和大小，以及主 SWF 的 CRC。其他资源只做大小检查，遇到同大小损坏可退出播放器后删除该缓存重建。旧目录游戏的 `assets/` 与 `_assets/` 布局继续可用。[缓存选择与启动](../player/sdl2test-rocknix/src/leaf.rs) · [正式路径验证](validation/all-zip-list-20261103.md)

## 功能与源码入口

| 模块 | 实现约定 |
| --- | --- |
| 暂停和切换 | 暂停冻结 VM tick 与音频，恢复首帧使用零 dt。输入事件释放后等待实体键松开及映射所用摇杆轴归中。[会话](../player/sdl2test-rocknix/src/session.rs) · [菜单](../player/sdl2test-rocknix/src/pause_menu.rs) |
| 物理菜单键 | C 库读取物理状态，短按松开打开菜单，组合键在按下沿触发。系统长按逃生由 Leaf 处理。[物理输入](../adapter/src/probe_input.c) · [快捷键](../player/sdl2test-rocknix/src/shortcuts.rs) |
| 按键和光标 | M+A/B 循环跳过全空方案，编辑器可选全部十套。最多四键按序按下、反序释放，重叠绑定用引用计数。M+X 临时交换十字与摇杆方向。摇杆径向死区 0.18，按下/释放阈值 0.45/0.30。光标移动后显示，静止 3.5 秒隐藏。[按键](../player/sdl2test-rocknix/src/controls.rs) · [光标](../player/sdl2test-rocknix/src/cursor.rs) |
| 连发 | 仅键盘、组合键和鼠标左右键支持 5/10/15 Hz。单调时钟调度，松开阶段目标 8 ms，主循环用最近截止时间缩短休眠。脚本、GPU 或系统调度阻塞时实际间隔可能延长。[连发](../player/sdl2test-rocknix/src/turbo.rs) |
| 音量和亮度 | 机身音量键、M+音量键由 Leaf 调整。播放器异步读取实际音量或亮度并显示百分比。[系统数值](../player/sdl2test-rocknix/src/system_volume.rs) |
| 画面设置 | RAV2 全局保存缩放、画质、FPS 和渲染接口。FPS 统计实际推进的 Flash 帧。更换后端须确认并提交当前原生进度，成功后保留槽位并重启。[设置与交互](../player/sdl2test-rocknix/src/experience.rs) · [RAV 契约](formats/graphics-settings.md) |
| 界面语言 | 首次跟随系统，之后由游戏和 Apps 共用配置。菜单使用集中译文，主题字体缺字时使用随包 CJK 字体，切换后按需重建文本纹理。[语言](../player/sdl2test-rocknix/src/i18n.rs) · [译文](../player/sdl2test-rocknix/src/translations.rs) |
| 截图和封面 | 暂停时从游戏重绘后的最终 framebuffer 读取舞台可见区域，早于菜单绘制。低内存时保持暂停并拒绝截图。封面按 ROM 所在卡选择路径，经确认后原子覆盖 PNG。普通照片优先使用独立挂载的副卡，同名写入失败时保留既有文件。[截图输出](../player/sdl2test-rocknix/src/screenshot_png.rs) · [截图回归](validation/screenshots-20261031.md) |

### Flash 软键盘

焦点回调只排队请求，由主循环读取当前可编辑 TextField 与 UTF-16 选区，释放游戏输入并暂停 VM 和音频。Catastrophe 布局在现有 GLES 菜单界面绘制，两种 Flash 渲染接口共用。[UI 后端](../player/sdl2test-rocknix/src/ui_backend.rs) · [键盘实现](../player/sdl2test-rocknix/src/virtual_keyboard.rs) · [布局来源](../vendor/catastrophe.lock.json)

草稿最大 16 KiB，密码预览遮挡，失焦或断连丢弃草稿。确认时核对输入框与原始文本，仅将变更范围换算为 UTF-16 选区，交给核心的 IME 提交或删除事件，保留游戏的长度、字符和事件取消规则。草稿不持久化，键盘日志只记录生命周期。夹具生成见[构建与发布](build-release.md#按变更选择验证)。

## 数据身份与持久化

Leaf 提供 `USERDATA_PATH`、`SAVES_PATH` 和 `LOGS_PATH`。升级前备份安装目录及数据目录，进度和配置写在 pak 之外。[使用指南](../README.md#安装与升级) · [路径脚本](../pak/emulators/ruffle-probe/paths.sh)

| 数据 | 位置 |
| --- | --- |
| 按键映射 | `$USERDATA_PATH/ruffle-probe/config/controls/`，含 `slots/<路径标识>/slot-01.rmap` 至 `slot-10.rmap` 与 `selected.rmap` |
| 全局光标 | `$USERDATA_PATH/ruffle-probe/config/controls/cursor.conf` |
| 界面语言 | `$USERDATA_PATH/ruffle-probe/config/language.conf` |
| 全局画面 | `$USERDATA_PATH/ruffle-probe/config/audiovisual/global.rav` |
| 原生进度 | `$SAVES_PATH/ruffle-leaf-preview/shared_objects/slots-v1/games/<内容指纹>/slot-1.rls` 至 `slot-5.rls` |
| 清理归档 | `$SAVES_PATH/ruffle-leaf-preview/shared_objects/cleared-data/` |
| 运行日志 | `$LOGS_PATH/ruffle-probe.log`，上次日志为 `.previous` |

按键以 ROM 相对路径为身份，`identity` 用于校验路径标识。新游戏仅方案 1、2 有默认键位，允许保存或选择空方案。原生进度以主 SWF 内容为身份，ZIP 路径用于按键、封面及目录。ZIP 没有独立方案时，只有目录记录能核对相同内容和旧目录路径身份才会复用旧方案。[映射契约](formats/controls-format-v5.md) · [目录契约](formats/data-manager-catalog.md)

RLS1 每游戏五槽，单槽上限 32 MiB。提交使用同目录临时文件、同步和原子替换。主文件损坏时可读取有效 `.bak`，主备均损坏时禁止按空槽覆盖。游戏显式 flush、十秒周期、打开菜单和正常退出均可请求提交。切换槽位需先成功提交当前槽，再重启载入目标槽。保存失败提示在成功重试后清除。[原生进度实现](../player/sdl2test-rocknix/src/experience.rs) · [拒写验证](validation/reliability-fixes-20261101.md)

### Apps 数据管理

数据管理按实际存档、控制方案、选择记录和既有目录信息关联条目，保留损坏或孤立数据的清理入口。旧目录记录继续可读，无法确认路径的数据保留清理入口，已知路径不存在的游戏显示“已失效”。清理二次确认且默认取消，确认后将对应数据移入归档并重新扫描，保留游戏、封面与全局设置。[管理实现](../player/sdl2test-rocknix/src/data_manager.rs) · [目录契约](formats/data-manager-catalog.md)

## 核心兼容约定

SWF 解析采用 v0.7.1 的长度与预分配保护。LZMA 声明长度先检查减 8 是否下溢。解压缓冲初始容量最多 128 MiB，场景和帧标签的初始容量最多 256 项，实际内容可继续增长。内嵌 DefineFont2/3 保留经校验的轮廓切片，首次使用时解码，ASCII 继续预热。不规则偏移或短轮廓回退到原读取路径。[SWF 读取](../player/swf/src/read.rs) · [字形](../player/core/src/font/glyph.rs) · [字体回归](validation/font-orphan-optimization-20261104.md)

AVM1 保留 goto 重建按钮的对象比对及菜单跟踪。AVM2 拖拽复用 roll 路径，根电影最终 progress 在首帧后、init 与 complete 之间派发，完成标志提前设置以防重入。AVM1 根时间轴在首帧脚本之后排入 onLoad，保留 loadMovie 的既有顺序。BitmapData 使用 Flash 的整数透明混合和重叠自复制方向，完整目标覆盖可跳过旧目标 GPU 读回。[核心回归](validation/upstream-compatibility-20261101.md)

孤立对象清理在共享快照无需删除条目时保留原 Rc，仍逐次检查弱引用、父对象和放置状态。需要删除时继续按原写时复制语义处理。[对象清理](../player/core/src/orphan_manager.rs)

Leaf 会启用 60 秒 VM 脚本预算，由 VM 检查点检测。原生函数、GPU 或其他同步调用中途无法检查，短按 M 的处理也可能延迟。运行日志使用 `RUST_LOG`，默认 `info`，由启动器收集 stderr。[Leaf 入口](../player/sdl2test-rocknix/src/leaf.rs) · [性能诊断](rendering.md#按需性能诊断)

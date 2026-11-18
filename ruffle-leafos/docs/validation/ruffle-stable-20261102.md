# Ruffle 官方稳定版升级（2026-11-02）

本次将共享 Ruffle 核心和官方渲染模块一起迁移到 **v0.7.1**，保留 Pocket1 / leafOS 前端及现有功能。应用仍为 **0.8.3 开发预览**；Ruffle 的稳定版标识不表示 Leaf 移植已经完成实机验收。

本文保留初版 `85b868eb…` 的构建、主机回归与打包身份，后续结果另行标识。2026-11-02 的 `422e3fd9…` 已正式安装，1,597 文件核验、双游戏 GLES 实际对战、Apps 主菜单及用户数据复原已完成；保护/拒写等限定 QA 通过，Vulkan 六分钟压力未通过，测试归档、索引缓存清理、最终文件及租约状态核验均已完成。[本轮实机报告](stable-device-20261102.md)记录当前身份与失败边界，不把后续结果覆盖为初版结果。

## 来源与范围

2026-11-02 核对官方 GitHub Release：最新非预发布为 [v0.7.1](https://github.com/ruffle-rs/ruffle/releases/tag/v0.7.1)，发布于北京时间 2026-10-08（UTC 2026-10-07），固定提交 `89a7049965f3090e706265670df309a5128233ac`。本次包含 `core/`、`swf/`、`flv/`、`wstr/`、`frontend-utils/`、`video/`、共享 `render/` 与官方 `render/wgpu/` 等播放器依赖；wgpu、naga 同步为 30 系列，不追踪 nightly/master。

旧共享基线是 `aweigit/ruffle-miyooflip@3bc39c5e0fdbc9fcde92681706395f6ef70f991b`。fork 删除了官方祖先历史，无法通过 Git 证明精确 merge-base；本地树对象对比显示其共享代码高度接近官方 `nightly-2026-03-15@71f18f0d0e3bd118945d5956f4ac4bb2e486e856`。原来单独导入的 wgpu 后端也来自这个官方快照。本次同时核对原 fork 差异和后续 Leaf 修改，避免只保留 Leaf 后加补丁而遗漏前者。

`leaf-source.lock.json` 与 `vendor/ruffle-wgpu.lock.json` 记录当前来源和构建指纹，Cargo.lock 固定实际依赖。`adapter/player-source.lock.json` 仍描述历史原始来源；原 fork 的致谢和 MIT / Apache-2.0 许可保留。没有恢复已移除的 `sdl2test-flip`，没有新增其他硬件支持。

## 功能与补丁保留矩阵

本表说明实现的迁移位置和需要保护的契约，不单独构成实机通过证据。

| 范围 | 保留的行为与适配 |
| --- | --- |
| Leaf 启动与音画 | Pocket1 Flash/Apps 入口、Wayland/GLES、系统字体/主题/音量；CPAL 适配官方 0.16，保留 F32/F64/I16/U16、暂停/恢复及初始化失败静音回退；HTTP 使用纯 Rust TLS |
| 暂停与输入 | 暂停停止 VM/音频；恢复清理时基；失焦/断连释放输入；实体按键松开及仅有效摇杆轴归中的中立门 |
| 按键与鼠标 | 十套方案、空方案跳过、组合键所有权、5/10/15 Hz 连发、方向互换、鼠标保持及全局光标 |
| 原生进度与身份 | 五槽 SharedObject、原内容/路径身份、RLS1/RAV2/rmap/语言目录与格式不变；原子写、有效备份、失败提示与重试；checked flush 继续阻止失败后的退出/换槽/后端重启 |
| 菜单与输入文字 | 中英文、主题、系统字体、Unicode 键盘草稿与差分提交、标准可编辑 TextField 请求和取消；新增上游多文件对话框接口委托 Null，不改变原本未提供文件选择 UI 的范围 |
| 截图与数据管理 | 干净游戏画面、舞台裁剪、封面确认、副卡选择、流式 RGB PNG、低内存拒绝；Apps 数据列表、按身份清理及本地归档 |
| 画面与诊断 | 全局缩放/画质/FPS/渲染偏好、Flash 帧计数和按需性能指标；默认 generic，A55/ThinLTO 仍为显式实验选项 |
| 静态绘图缓存 | 延迟上传、位图弱句柄、默认 8 MiB CPU 像素 LRU、可选预解码、GLES 默认 16 MiB/1,024 项闲置形状及 128/160 MiB 压力迟滞；官方缩放细分的每个静态变体接入同一回收预算 |
| GLES 与 Vulkan | GLES scratch/PBO 保留；新版 PixelRegion 区域及 UV 适配；wgpu 30 的 GBM/DMA-BUF 显示桥与生命周期保留。后续新增 wgpu 闲置池压力退让、旧代/退休池归还保护、Leaf 持续低内存退出；MemoryUsage 保留，Manual 实验撤回 |
| SWF 解析防护 | LZMA 长度减法、128 MiB 初始容量封顶、场景/帧标签 256 项初始容量；继续允许合法内容增长，保留原损坏样本和真实游戏回归 |
| 鼠标、加载与 VM | AS3 误点击/重复悬停、AVM1 菜单按钮与根 onLoad、根 Loader progress/完成重入、BitmapData 混合与重叠复制/完整覆盖免回读、参数化 Vector 和 ScopeChain 命名空间修复及测试继续保留 |

静态形状继续沿用官方缩放感知细分：比例阈值为 2，每个原形状最多四项缩放 LRU。静态变体仅在可回收的 `StaticRenderResource` 槽中保存强 GPU 句柄，缩放索引保存资源容器；上传和命中都登记到全局活动队列。回收后删除空变体索引，再显示时按当前缩放重建。当前帧、在途命令、动态 BitmapData、驱动和游戏数据继续在闲置预算之外，不能把这些预算解释为总 RSS/GPU 上限。

兼容修复的来源与原夹具见 [2026-11-01 报告](upstream-compatibility-20261101.md)；旧结果不替代本次新核心的测试执行。

SWF 回归保留 v0.7.1 对有效 end-marked LZMA 流的兼容：旧 `lzma_huge_length_returns_error` 夹具只有声明长度为 `0xffffffff`，实际压缩体与正常 51 字节 SWF 相同，并非损坏数据；上游新版会成功解码。本地改为核对解码/标签与正常样本一致且初始容量仍有界，另用真正非法的 LZMA 属性验证拒绝。不为了旧错误断言回退新版 decoder；这是声明长度容错的边界变化，不是删除长度减法或预分配保护。

## 初版构建与主机验证

工具链固定升级为 **Rust 1.95.0 Windows GNU → AArch64 Linux**。wgpu v30.0.1 自身的声明不要求更高版本（wgpu/core/hal/naga 为 1.87、仓库级默认 1.93），但实际 gc-arena 0.7 调用的 [`ptr::as_ref_unchecked`](https://doc.rust-lang.org/stable/std/primitive.pointer.html#method.as_ref_unchecked) 在 Rust 1.95 才稳定，原 1.94 编译报 E0658，故升级到最小足够稳定版本。官方 manifest 和四个组件 SHA-256 已核验，GNU 主机/ARM64 API 编译探针通过；来源与下载由 `adapter/rust-toolchain.lock.json` 固定，隔离安装不修改全局配置。完整播放器构建另行记录，不能把探针通过当作完整构建通过，也不能复用旧 ELF 记录。

初版完整 ARM64 check/release、目标测试编译和主机回归均使用固定 Rust 1.95.0。下表 ARM64 与初版打包结果对应 85b；新增 148 项前端、9 项 noop 另记，后续 422e 构建和设备范围见末节。原始报告和日志在忽略目录 build/，不随源码提交。

| 验证 | 当前结果 |
| --- | --- |
| ARM64 `build_player.py check --offline --isolated-config` | 完整播放器检查通过，exit code 0；含 GLES/Vulkan 前端与新依赖，保留现有编译告警 |
| ARM64 release `build_player.py build --offline --isolated-config`、ELF/GLIBC 与源码输入绑定 | 初版构建及绑定核验通过，exit code 0；AArch64 PIE、最高 GLIBC 2.38，实际 1,512 项输入与 ELF 绑定 |
| ARM64 核心测试编译 `build_player.py test --package ruffle_core --offline --isolated-config` | `--lib --tests --no-run` 编译通过，exit code 0；产生核心单元及 AVM1 独立集成两个 AArch64 测试 ELF；本节构建阶段未执行，后续设备执行见下节 |
| `test_core.py` 主机实际核心/VM/事件/缓存单元及集成回归 | 172 项单元 + 2 项独立 AVM1 集成全部通过；单元内含两项旧 AMF3 存档、缩放变体回收、11 项 copyPixels、Loader/鼠标/Vector/ScopeChain 等；匹配播放器特性、Null 后端、串行执行 |
| `test_session.py` 前端状态 | 初版 143 项；新增持续低内存状态回归后 148 项全部通过 |
| `test_renderer.py` noop 句柄/资源池 | 初版 3 项；新增池退让/延迟读回后 5 项单元+4 项集成共 9 项通过，不验收真实 GPU 像素或驱动占用 |
| `test_renderer.py --suite decode` / `--suite screenshot` | CPU 解码 65 项、PNG 编码 3 项全部通过 |
| `test_swf.py` 单元/集成/随包 SWF 受限内存解析 | 86 项单元、8 项集成全部通过；包含两款随包目录实际 93 个 SWF 的解析。各测试进程树限制 512 MiB 提交内存、每二进制 180 秒 |
| `test_language.py` 语言契约 | 3 项通过 |
| `test_build_inputs.py` 源码/ELF 绑定 | 25 项运行，24 项通过、1 项 Windows 符号链接权限不足跳过；设置 TEMP/TMP 到工程 build/tmp 后执行 |
| `test_package_preview.py` 游戏与打包 | 4 项通过 |
| `test_release.py` 发布脚本 | 15 项通过 |
| `test_playerglobal_fixture_inputs.py` 当前 ASC 导入选择 | 3 项通过；实际用新 debug `playerglobal_import.abc` 重建 SharedObject SWF/SOL 成功，字节与既有合成夹具一致 |
| `build_diagnostics.py` 诊断库 | 真实 C SDL 拦截与 metrics 编译为 WASM 后执行通过，AArch64 `.so` 重建/ELF 检查通过 |
| 安装/源码 ZIP、来源锁与构建绑定 | 本地生成成功；独立 CRC、1,512 项实际输入、ELF/来源锁、402 个随包游戏文件哈希及 Shell LF/执行位检查通过 |

五组 Python 脚本回归合计执行 50 项：49 项通过、1 项符号链接权限不足跳过。前端报告为 `session-test-report.json`，核心为 `core-test-report.json`，noop/解码/PNG/SWF 分别为 `renderer-test-report.json`、`bitmap-decode-test-report.json`、`screenshot-test-report.json`、`swf-test-report.json`；完整 ARM64 check 日志为 `upgrade-check.log`。

`test_core.py` 通过 Cargo `--lib --tests` 实际执行主机单元及集成测试；核心 ARM64 `test --no-run` 本身只证明目标测试可编译。目标产物为 `ruffle_core-24676c8b4fc89d46` 和 `shared_object_avm1-747480172158b36a`；本节原始构建报告完成时没有执行，后续已在 Pocket1 实际运行同一核心的 172 项单元和 2 项 AVM1 集成，均通过，证据见[实机报告](stable-device-20261102.md#实际执行与画面检查)。这些 Null 后端核心测试不替代设备 GPU 图像、实体输入手感、音频或游戏长时验收。

初版播放器采用 `generic` / release、opt-level 2、LTO=false、16 个 codegen units，Rust 为 `1.95.0 (59807616e 2026-04-14)`。初版 `player-build.json` 记录的播放器 SHA-256 为 `85b868eb093ce5bbd15994bc88ffdf4a2e178cdb9e246202f8af5013596b65dd`，1,512 项实际输入总摘要为 `0f42570b70ca54c9fa28db846bb41af6321b01efc29754bbfde1746a13f20b04`；源码、锁文件和打包 ELF 按该绑定核验，不复用升级前构建记录。

本地交付目录为仓库根的 `outputs/ruffle-stable-v0.7.1-20261102/`，含安装/源码 ZIP、构建报告和校验文件。ZIP 自身摘要以目录内 `SHA256SUMS.txt` 为准，避免随包文档自引用。初版和 719a 的阶段清单保留，当前 422e 已正式重装核验 1,597 文件并完成双游戏 GLES/Apps 主菜单，用户数据已复原；测试归档、临时入口/索引缓存清理、第二次 GBA/安装/用户数据终验和租约状态核验均已完成，仍为本地交付，未上传 Release。

本次旧存档覆盖四项实际 VM 回归，全部通过：AVM2/AMF3 两项在 172 项单元内，AVM1/AMF0 两项作为独立公开 Player API 集成测试执行。两组都固定各自旧存储 key，以原 fork 锁定 `flash-lso@998f47c` 实际 writer 生成的合成字节为种子，涵盖 Unicode、数字、Bool、Array 和嵌套 Object；AVM1 使用旧 `Amf0Writer/ObjWriter`，未用新 writer 或 AMF3 改头伪装旧格式。实际脚本读取并修改后由 checked flush 写回，新 Player 重开读到更新；拒写 backend 返回 false 并保留旧字节，恢复写入后重试成功。[AVM2/AMF3 来源](../../player/core/src/player/tests/shared-object-README.md)和 [AVM1/AMF0 来源](../../player/core/tests/fixtures/shared-object-avm1-README.md)记录生成方式及范围；这些合成格式回归不代表完整旧播放器、真实游戏或 SD 卡升级验收。

夹具和游戏工具中的 ASC 路径已随上游迁到 `tools/asc/asc.jar`，共享 helper 改为明确选择最新已完成的 debug/release `playerglobal_import.abc`，拒绝静默采用旧 `playerglobal.swf`。本次实际选择的新版 ABC SHA-256 为 `bacacb58df8390161c34e68f9e69406b9f1c9f6103bdacd3162034f42cce1eb3`，原始记录在忽略的 `work/ruffle-upgrade-071/shared-object-fixtures-new-globals/`。

## 实机与未验范围

核心 172 项单元及 2 项 AVM1 集成已在 Pocket1 实际执行。当前 422e 正式安装的 1,194 应用+403ROM 共 1,597 文件全哈希匹配；正式 GLES 死神单人 CPU 战、拳皇 Terry 对 K′实际对战、槽 1 读取、方案 1→2→1、M/B 恢复 dt=0 及保存退出通过。本次 Apps 只复核主菜单和 B/B 退出，719a 阶段另验过子列表；FTE/CFF 等限定诊断不扩大为全字体/全游戏兼容通过。

日志桥和仅 Leaf 的有限 60 秒脚本预算保留，核心默认不改。正式 launch-game 与隔离 launch-app 可能采用不同 Leaf 性能 profile，约 19.4/31 秒初始化差异不归因于抗锯齿或提速。CPAL 44.1 kHz 双声道 F32 初始化已记录，未进行人工听感验收。

Vulkan 跨轮发生过真实 OOM/exit 137，单次申请约 128 MiB。MemoryUsage 的 High 起始测试在 6.6 秒采样后由外部 64 MiB 阈值中止；Manual4–16 MiB 实验又在 5.5 秒触发同类阈值，均 SIGTERM 正常退出，不算六分钟通过。Manual 预留仍约 339.7 MB、活动 126.3 MB，无足够收益，已撤回；起始 RAV 不证明全场实际 MSAA。

新池退让和持续低内存保护源码通过 148 项前端、9 项 noop。422e ARM64 构建及 1,512 输入绑定通过：ELF `422e3fd9c148bf330db8c14593a433dcbced38cda2160ca01992762f95a7e3eb`，输入摘要 `806039013a5e670b82b9b764957862f8d5878abb23418d5e013d3940899132a2`。GLES/Vulkan 各一次 18 项像素和 7 项 FTE 夹具共四组通过；真实死神 Vulkan 在初始化后以有效可用内存 128,860,160 字节触发内置保护，释放输入/暂停音频/checked flush 成功后 exit 1，无新增 OOM。没有进入战斗，也未完成六分钟；重型游戏仍建议 GLES。422e 隔离三组 QA 亦已通过：96 MiB 进程级计数下，Vulkan 拒写 exit 1/flush false/原 RLS 不变且保留真实错误；无 CLI 覆盖时成功写回并仅隔离偏好退回 GLES；GLES 继续到 12 秒定时 exit 0。未真实填 RAM 或改系统计数，用户全局偏好保持。GBA 351 文件两次全哈希与原清单相等。422e 已正式安装并完成上述 GLES/Apps 主菜单复核；试玩五处变化先备份再复原，原 39 配置+42 存档、home1/data0/images1 全哈希相等。测试归档及临时入口/索引缓存清理通过；1,597 安装文件、GBA 351 文件和原用户数据最终全哈希相等，租约已自然到期、无残留 holder，WiFi power_save 恢复 on。

完整身份、备份、索引恢复与失败见[本轮实机报告](stable-device-20261102.md)。ADB 断线原因未定，基线自动休眠关闭，不归因为 TOOL 睡眠。实际听感、休眠、广泛角色/地图和长时内存尚未全面验收；游戏仍有脚本/资源/音频告警，不断言全部音效正常或属于新回归。

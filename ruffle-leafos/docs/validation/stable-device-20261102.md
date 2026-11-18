# Ruffle v0.7.1：Pocket1 实机验证与重新安装

日期：2026-11-02（Asia/Shanghai）。设备为 Miniloong Pocket1 / leafOS、RK3566、AArch64 Linux 5.10.209，物理内存 978,824 KiB，无 swap。本文区分初版安装、719a 正式流程和后续保护构建；来源与主机回归见[稳定版升级报告](ruffle-stable-20261102.md)。

**当前 422e 已正式安装并完成限定 GLES 验证。** 1,597 文件全哈希核验，两款游戏实际对战、M/B 恢复/保存退出和 Apps 主菜单复核通过。719a 的练习场及数据子列表作为前阶段结果保留。422e 的四组图形、真实 Vulkan 低内存退出和三组隔离 QA 已完成，但重型游戏 Vulkan 未通过六分钟战斗压力。试玩五处变化先备份再复原，原配置/存档等哈希已全等；测试归档、临时入口/索引缓存清理及最终完整性核验均通过；GBA 351 文件第二次全哈希保持，临时租约已自然到期。

## 授权范围、备份与安装

用户授权清理旧 Flash 游戏与两份旧 Ruffle 安装，再用项目随包游戏重新安装；另行确认保留 GBA。实际删除范围为主卡 `Roms/flash/`、`Apps/mlp1/ruffle.pak` 和 `Apps/mlp1/RuffleProbe.pak`。GBA 的 351 个文件以及现有配置、SharedObject、HOME 和数据目录不在这次安装清理范围内。

先完成旧 Flash、应用及相关用户数据的本地备份，再执行删除。备份传输曾中断；从可恢复部分取得 1,256 个文件，补取余下 143 个后，归档内共 1,399 个文件均按原设备清单核对哈希。完整归档 `original-flash-apps-userdata.tar.gz` 为 410,220,864 字节，SHA-256 为 `8ec5c49508a5faad85657aedc31a5ed9520dc7146b28c6f6762bbed4f1438a70`；索引数据库另有备份及摘要。中断归档不被当作完整备份。

初次重装删除 499 个旧 Flash 文件，写入并逐文件验证 1,596 个文件：1,193 个应用文件和 403 个 ROM 目录文件；后者为 402 个随包游戏文件加一个 LeafProbe 诊断文件。两款随包游戏仍只应选择各自主 SWF，资源 SWF 的索引记录不表示独立可玩游戏。安装核验当时，保留的用户数据未改变，历史调试目录尚未删除。证据为本地 `backup.json`、`cleanup-plan.json`、`installed.json` 和 `desired.json`。

719a 阶段清单另存 `final-installed-719a.json`。当日最终正式安装为 `422e3fd9c148bf330db8c14593a433dcbced38cda2160ca01992762f95a7e3eb`，输入摘要 `806039013a5e670b82b9b764957862f8d5878abb23418d5e013d3940899132a2`；`final-installed.json`核验 **1,194 应用+403ROM 共 1,597 文件** 全部 SHA 匹配，配置/存档在这次安装前后相同。后续试玩变化和复原另有独立核验，不将安装前后一致扩大为试玩从未写盘。

用户数据复原已完成：5 处试玩变化先本地备份并核验哈希，再原子恢复 3 个旧存档文件、移除 2 个测试新增选择文件；`finalize-userdata-restore.json` 确认原 39 配置、42 存档及 home 1/data 0/images 1 全部 SHA 与清理前相同。

随后用户单独授权删除四个历史调试、测试和部署备份目录：`/mnt/sdcard/ruffle-tests`，以及 `.userdata/mlp1/ruffle-probe/` 下的 `debug-tests`、`debug-backups`、`deploy-backups`。此前已明确这些可丢弃历史不在上述完整内容归档内；本次没有将其伪称为已完整备份。四目录合计约 7.5 GiB，删除后均确认不存在，主卡可用空间由约 1.4 GiB 增至 8.9 GiB。依据为 `history-cleanup-plan.json` 和 `history-cleanup.json`，这些是磁盘空间，不是运行内存。

## 索引误判、恢复与最终清理范围

索引清理初次错误地把 94 条相对 ROM 路径按设备绝对路径检查，从而误判为文件不存在。该操作及误判清单保留于 `index-cleanup-initial-error.json`，不按成功清理记账。发现后从数据库备份精确恢复相关游戏行、收藏、最近游玩及游戏设置；`index-recovery.json` 确认 94 条恢复记录与备份相等。

修正后的定向清理仅针对两条已确认旧目录不存在的 Ruffle Apps 索引：主卡旧 `Apps/mlp1/ruffle.pak` 和离线副卡旧 `/media/sdcard1/Apps/mlp1/ruffle.pak`。新 `RuffleProbe.pak` 入口与其他应用、其他存储源的记录保留；副卡未挂载，不能因离线而批量清理其记录。刷新同时核对 Apps 和搜索索引；本轮状态记录为 `scan_running=false`、`pending_rescan=false`、`scan_error=""`，扫描代次已推进。最终重装后的清理和缓存刷新结果见下段。

最终收尾先完整下载测试归档：`final-validation-artifacts.tar.gz` 为 558,373,145 字节，368 个普通文件逐项哈希相等，SHA-256 为 `14a3ede12a0e401d74d16be850c67f722bdd23960977cdeaec15e8f48ec49255`。随后删除临时 `RuffleStableDebug.pak`、隔离测试目录及三个 `/tmp` helper，`finalize-cleanup.json` 通过。备份数据库后仅删除已不存在的 DebugApp id 19，保留游戏、设置、其他 Apps、收藏和最近记录，没有覆盖此前恢复的数据库。SQL 清理后额外触发扫描刷新 Leaf 内存中的 Apps/搜索缓存；`final-cache-refresh.json` 确认 generation 77、`scan_running=false`、`pending_rescan=false`、`scan_error=""`，无旧版或调试 Apps，Flash 94 条且 FTS 完整性通过；最终界面留图 `final-leaf-library.png`。

`finalize-verify.json` 完成末次独立核验：1,194 应用 + 403 ROM 共 1,597 文件与安装包逐项哈希匹配；GBA 第二次全量 351 文件、原 39 配置和 42 存档及 home/data/images 均与原清单相等。四个旧历史目录、旧安装、本轮临时 App/隔离目录和三个 helper 均不存在；Flash 94 条，FTS/SQLite 完整性通过。同一 boot_id 下无播放器进程，主卡可用空间约 9.1 GiB。

## 构建身份与失败修复

以下六份均为 Rust 1.95.0、generic AArch64 release，opt-level 2、LTO=false、16 个 codegen units。构建成功与运行结果分别记述；旧失败不由新版本的通过覆盖。

| ELF SHA-256 | 用途与本轮结果 |
| --- | --- |
| `85b868eb093ce5bbd15994bc88ffdf4a2e178cdb9e246202f8af5013596b65dd` | 初版正式安装；拳皇 GLES 练习场通过，死神首次初始化停在图片资源 4/4，未到 `games_inited` |
| `fe9bf87d8f8ffee82b2ad1bc4df754276e093276bb094b547b3b21a4b52dea75` | 仅日志适配；恢复上游核心错误可见性，明确复现死神初始化脚本超时，未解决该失败 |
| `c1bf201d4d51bb2e6d622cfbada0784a64b658c35538c0d3d54ffabecb178621` | Leaf 有限 60 秒脚本预算；死神 GLES 初始化、选人和对战通过，Vulkan 实际对战及跨轮后发生 OOM，不能作为内存验收通过版本 |
| `c4b32396b3f0d7bca8e33bc5de7348b7a93191f3e7e081069d22678eb075f235` | Vulkan 内存优先策略候选；十项诊断通过，高画质起始配置进入战斗后触发低内存测试保护，正常退出；未完成六分钟压力验证 |
| `719acd7296fe73c230bdace3b0e6ae86dfb84adddb5e29c7183068b4b917f383` | 低频 allocator 诊断；十项诊断、正式安装及双游戏 GLES/Apps 限定流程已完成，尚有 Vulkan 压力缺口 |
| `422e3fd9c148bf330db8c14593a433dcbced38cda2160ca01992762f95a7e3eb` | 当日最终正式安装；1597 文件全核验、双游戏 GLES 实际对战、Apps 主菜单及用户数据复原通过；保护/拒写 QA 通过，Vulkan 六分钟压力未通过 |

初版只安装 `env_logger`，而 v0.7.1 核心、资源加载和音频后端已经使用 `tracing`。改为工作区锁定的 `tracing-subscriber` 后，保留 `RUST_LOG`、默认 info、stderr 和旧 `log::` 输出桥接，日志版才显示 `complete` 回调中的脚本超时。该适配没有改变游戏行为，不能把此前看不到错误解释为此前没有错误。

死神首次特效缓存是一个约 31 秒的合法同步初始化回调。旧核心的稀疏超时检查曾碰巧未截断它；新核心检查触及原默认约 15 秒预算后终止回调，后续一直等不到 `games_inited`。修复只在 Leaf 的 PlayerBuilder 配置有限 60 秒预算，保留核心默认及随包游戏字节，输出 `leaf-script max_execution_seconds=60` 以核对实际配置。`bvn-logging-result.log` 保留明确超时错误，`bvn-budget60-initialized.log` 记录初始化完成。初始化期间短按 Menu 和播放器菜单可能延迟；使用指南提示等待完成，Leaf 系统长按 Menu 返回方式沿用既有约定，本轮未专门验收该系统逃生操作。

`c1bf201d…` 的 Vulkan 跨轮测试确实失败：进程退出码 137，内核 OOM 日志指向同一 `ruffle-candidat` 进程，不能归为普通退出或仅因截图误判。调查涉及 wgpu 30 性能优先策略的 128 MiB 内存块申请；Pocket1 的 1 GiB 为 CPU/GPU 共享，闲置缓存预算不约束驱动块与所有活动分配。`c4b32396…` 将 Leaf Vulkan 设备策略改为 `MemoryHints::MemoryUsage`，并传给 HAL，未靠删角色或素材规避压力。相关证据为 `bvn-vulkan-final.json`、`kernel-vulkan-performance-policy-oom.txt` 和候选构建记录。

内存优先候选以 RAV 的 High 起始配置进入战斗后，原定六分钟的资源采样只进行 **6.6 秒**，即得到 HWM 790,236 KiB、RSS 775,796 KiB、`MemAvailable` 64,860 KiB，低于外部验证脚本的 64 MiB 阈值。脚本发送 SIGTERM，进程退出码 0 且 `progress_flushed=true`，无新增 OOM；这是保护性中止，不是六分钟通过。证据为 `bvn-vulkan-memory-memory-samples.json` 和同 case 日志。起始 RAV 不能推断整个对战时段的实际 MSAA。

719a 保留 MemoryUsage，并在 `--trace-performance` 启用时约每秒查询一次 allocator 报告。生成/排序会临时占 CPU 堆；它仅描述 wgpu 管理的分配，排除导入 GBM、其他驱动分配及 CPU 内存，不是 GPU 或进程内存限额。关闭追踪时不采样，也不能据诊断存在就宣称 OOM 已解决。

Manual 4–16 MiB 小块实验仍在 **5.5 秒**资源采样时达到 HWM 779,676 KiB、RSS 771,668 KiB、可用 56,096 KiB，由同类外部阈值中止，exit 0 并成功 flush。末次 allocator 记录 `reserved_bytes=339738624`、`allocated_bytes=126324284`、`blocks=41`，预留与活动分配仍有明显差距；小块调整没有足够收益，实验已撤回，恢复 MemoryUsage。证据为 `bvn-vulkan-manual-memory-samples.json`、`bvn-vulkan-manual.json/.log`。上述两次是验证脚本 SIGTERM 的记录，不是下述内置保护的实测，也都不算持续六分钟。

## 新增池退让与内置低内存退出

wgpu 普通/离屏纹理、Globals 缓存和离屏读回缓冲池现接入同一压力状态。只释放闲置项，借用中的资源与在途命令继续有效；压力前借出的项不能在恢复后重新填池。旧离屏读回池退休后，旧 Arc 仍可完成延迟读回，但不再留存归还资源。它不会强制销毁活动纹理，逻辑释放不等于驱动物理分配立即下降。

Leaf 仅在实际 Vulkan 且有效可用内存低于 128 MiB 时累计连续状态：首次先让缓存回收，后续有效低读数跨至少一秒才结束会话；无效读数、恢复至至少 128 MiB 或 GLES 清状态。触发后不覆盖已有 GPU 错误，跳过后续渲染，释放输入、暂停 VM/音频、checked flush 并沿既有偏好恢复通路退出；显式 `--renderer vulkan` 不改变全局偏好，保存失败的真实原因附入返回错误。这是保守结束策略，仍不能防住采样间的突发 OOM。

当前主机为 **148 项前端状态 + 9 项 wgpu noop** 全过；后者为 5 项池单元和 4 项所有权/延迟读回集成，不验收真实 GPU 像素。422e ARM64 构建与 1,512 项输入绑定通过，输入摘要 `806039013a5e670b82b9b764957862f8d5878abb23418d5e013d3940899132a2`；GLES/Vulkan 各一次 18 项像素和 7 项 FTE，共四组实际夹具通过。

422e 的真实死神测试复制原 Medium RAV、仅显式覆盖 Vulkan。初始化到 `games_inited`/更新提示后，实际 `MemAvailable=128860160` 字节低于 128 MiB，内置保护触发，输入释放、音频暂停、`progress_flushed=true`，按保护路径退出，退出码 1，内核未新增 OOM。没有进入战斗，未完成六分钟；对该重型游戏推荐 GLES。证据为 `bvn-vulkan-pressure.json/.log` 和 `kernel-pressure-pools.txt`。随后已独立执行下文三组隔离 QA，分别验证拒写、成功保存/偏好回退及 GLES 不退出。

## 422e 隔离低内存与拒写 QA

三组实际设备 QA 均核对进程 ELF 为 422e 并通过，记录分别为 `guard-422e-vk-reject-guard-qa.json`、`guard-422e-vk-preference-guard-qa.json` 和 `guard-422e-gles-low-guard-qa.json`。使用既有进程级 LD_PRELOAD shim，仅将播放器对 `/proc/meminfo` 的精确打开路径重定向到该 case 的普通文件；注入 96 MiB 计数，不真实分配 RAM、不修改系统文件或真实可用内存。夹具由当前 playerglobal 导入生成，存档/设置完全隔离。

| QA | 实际结果 |
| --- | --- |
| Vulkan + 拒写 | 在隔离 `slot-1.rls.bak` 位置放置空目录，阻止原子备份替换；注入低读数后 exit 1、`progress_flushed=false`，真实 `Is a directory (os error 21)` 同时出现在保存失败及最终返回错误中。原有效 RLS 的 SHA 不变，不把失败写成保存成功 |
| Vulkan + 隔离偏好 | 不传 CLI renderer，读取隔离的 Vulkan 偏好；低读数后将 dirty 原生数据成功写回，exit 1、`progress_flushed=true`，仅隔离偏好回到 GLES。显式 renderer 覆盖的 case 不改其偏好，三组测试中用户全局偏好原 SHA 始终不变 |
| GLES + 低读数 | 同为 96 MiB 计数，继续运行至少三个性能窗口，到 12 秒诊断定时正常 exit 0、`progress_flushed=true`；没有误触发 Vulkan 退出保护 |

这证明保护、保存失败传播和偏好作用域的限定路径，不是真实低 RAM 负载或六分钟 Vulkan 稳定性通过。另 `gba-final.json` 已对全部 351 个 GBA 文件重新计算哈希，均与清理前原始清单相等；试玩产生的 Flash 配置/存档变化已按下文完成备份和复原。

## 实际执行与画面检查

| 项目 | 已确认结果与范围 |
| --- | --- |
| ARM64 核心与旧档格式 | 设备实际执行 172 项核心单元和 2 项独立 AVM1 集成，均通过，退出码 0；分别耗时 7.04 / 0.76 秒。本次已超出原先只交叉编译的范围 |
| 719a 阶段 GLES / Vulkan 诊断 | 两后端各五项共 10/10 通过，逐项有 PASS、实际 renderer、exit 0 及成功 flush；不转记为 422e 全部结果 |
| 前端主机状态回归 | 新保护逻辑加入后 148/148 通过；这是主机状态测试，不替代实体输入验收 |
| wgpu 池/所有权主机回归 | 5 项池单元+4 项集成共 9 项通过；noop 验证活动引用、旧代/退休池和延迟读回，不验收真实 GPU 像素 |
| 初版正式拳皇 / GLES | 正式 Flash 入口、鼠标确认更新页、主菜单和练习场、暂停/恢复及保存退出通过；实际 ELF 核对为 `85b868eb…` |
| 60 秒预算版死神 / GLES | 已确认特效初始化、主菜单、单人对 CPU、角色/地图加载及实际对战画面；正常结束日志含 `progress_flushed=true` |
| 60 秒预算版死神 / Vulkan | 实际 Vulkan 后端初始化、角色/地图加载、战斗与换轮可见；随后 OOM，作为失败记录保留 |
| 内存优先候选死神 / Vulkan | 已进入战斗，6.6 秒资源采样触发低内存保护后正常退出；未完成六分钟压力验证 |
| 719a 正式死神 / GLES | 完整 HUD、计时 48 的实际单人 CPU 战，M/B 恢复首帧 dt=0、保存退出 true |
| 719a 正式拳皇 / GLES | 草薙京/八神庵、bridge 练习 HUD，方案 1→2→1、既有第一槽在首 tick 前读取，M/B 恢复与保存退出 true |
| 719a 正式 Apps | 只读查看数据列表，B/B 返回并退出；没有执行清理 |
| 422e 新保护版 / Vulkan | 初始化后实际低内存触发内置退出，exit 1、flush 成功，无新增 OOM；未进入战斗 |
| 422e 正式拳皇 / GLES | Terry 对 K′实际对战，日志 roofFW/K_2；既有槽 1 读取、方案 1→2→1、M 暂停/B 恢复 dt=0 及菜单保存退出 true，退出后无播放器进程 |
| 422e 正式死神 / GLES | 约 20 秒初始化后实际单人 CPU 战，完整 HUD 和计时 60 Fight，继续约 30 秒方向/攻击；M/B 恢复 dt=0、保存退出 true，退出后无播放器进程 |
| 422e 正式 Apps | 本次只读复核数据管理主菜单，B/B 退出 true；`installed-apps-list.png`实际为主菜单，未把它写成数据子列表，也未执行清理 |

422e 正式日志及截图为 `installed-kof-exit.log` / `installed-kof-actual-battle.png`、`installed-bvn-exit.log` / `installed-bvn-fight.png`、`installed-apps-exit.log` / `installed-apps-list.png`。拳皇本次为 Terry 对 K′实际对战，不沿用 719a 的草薙京/八神庵 bridge 练习结论。测试导航最初单次 ADC 事件未将鼠标移到确定按钮，有界 10ms 重发的测试 helper 完成导航，无产品修改。

719a 正式死神约 19.4 秒、422e 约 20.0 秒、隔离 launch-app 曾约 31 秒初始化；不同 Leaf 入口可能采用不同性能 profile，不将差异归因为抗锯齿或提速。CPAL 只确认 44,100 Hz 双声道 F32 初始化，未人工听感验收；422e 拳皇有`ko_snd_Terry`未注册告警，死神保留#1006、缺 frame 和 MP3 告警，不断言所有音效正常或这些告警属于新回归。

核心测试使用实际 ARM64 Player/VM 及 Null 后端；旧档用固定旧 key 和原 fork 旧 writer 生成的原创合成 AMF0/AMF3 数据，实际读取、修改、checked flush、拒写保留、重试和重开均验证。它们不等于真实游戏用户旧档已逐一续用。原始执行输出为 `arm64-core.json/.log` 和 `arm64-shared-object.json/.log`。

719a 的十项诊断按同一表逐后端执行；422e 当前仅完成上文四组图形夹具，不转记 719a 的全部结果：

| 每个后端的诊断 | 有区分力的检查与限制 |
| --- | --- |
| BitmapData copyPixels | 18 个 Flash 参考结果的复制/读回检查，`COPY_PIXELS_PROBE PASS cases=18` |
| FTE 嵌入 CFF 字体 | 七个有效字符、三组不同形状、重复光栅化 67,200 像素精确比较；不验收完整 Flash 字体参考图，也不覆盖位图 atlas 的 UV 路径 |
| AVM1 虚拟键盘 | 确认/取消、Unicode、密码、restrict、maxChars、多行、只读、空值、帮助、中立门、失焦及断连诊断 |
| AVM2 虚拟键盘 | 上述诊断及 AVM2 输入事件行为；不等于系统原生输入法或人工输入手感验收 |
| 方案/编辑器/光标 | 方案切换、九类动作、网格、编辑保存、光标与空闲显示等内置状态/VM 联动检查 |

719a 证据为 `fixture-results.json` 中 `alloc-*` 十条及同名日志；422e 图形结果为 `pressure-*` 四条；此前 `c4b32396…` 的 `final-*` 十条仍作为对应版本的结果保留，不转记给新 ELF。夹具使用当前 `playerglobal_import.abc` 重新生成。FTE 首轮错误采用了不符合新版格式应用路径的设备字体假设，第二轮又请求了 CFF 子集中缺失的字符；两次失败保留在 `fixtures.json` 和早期日志，均不计入通过数。最终使用官方测试字体已有的 H/I/A/O/4/E/G，保留 CFF 载荷，字体来源及标签摘要记录于 `fte_font_provenance`。当前实现经过 FontFace/Drawing，该夹具虽沿用 atlas 名称，不能被用来宣称新增 PixelRegion 的 atlas 分支已经像素验收。

死神最初截图处于局内开场，尚未显示 HUD；此前血条缺失判断已撤销。旧 Vulkan 后续 `bvn-vulkan-round2-play.png` 有完整血条、计时 47 和 10 HITS，但该会话随后 OOM。719a 正式 `final-formal-bvn-play.png` 则为完整 HUD、计时 48 的实际 CPU 战；拳皇初版为 ice，719a 正式 `final-formal-kof-battle.png` 为 bridge 练习场。旧诊断日志含多次游戏 `TypeError #1006`，仍保留异常边界，不将战斗截图扩大为所有场景无异常；短时 FPS 也不证明配对性能提升。

## 连接与临时待机抑制

本轮 ADB 地址曾临时变化，并发生 `closed` 和重连超时；断线原因尚未确定，不按“TOOL 测试导致自动睡眠”记结论。安装前备份数据库中的 `auto_sleep_seconds=0` 表示当时自动休眠关闭；已有深睡/无线挂起日志也不能直接解释后来每次断线。临时地址不作为长期连接约定写入本文。

重连后，设备自带 `jawaka-inhibitctl` 已实际取得 900 秒的 `block-suspend` 租约，`suspend-inhibit.json` 记录 `active_count=1`、验证用途、对应 holder PID 和 `pending_sleep=none`。该租约延迟深睡，不禁止第一阶段熄屏，不改永久设置。末次 `finalize-release.json` 核验前后均为 `active_count=0`、`holders=[]`、`pending_sleep=none`；已知租约自然到期，没有手工 TERM，临时 WiFi power_save 已恢复为 on。

## 验收边界

- Vulkan 六分钟/跨轮稳定性没有通过。当前保护在危险余量时结束会话，不以停止游戏冒充压力存活；Pocket1 的死神 3.3 建议使用 GLES。进程级计数注入只验证控制和保存路径，不能证明真实低内存下绝不 OOM。
- 广泛游戏、角色、技能和地图，真实旧档逐项续用，人工听感、休眠恢复、系统长按返回专项及性能/功耗对照仍未完成。已有游戏脚本、资源和音频告警保留，不扩大为全部兼容或音效正常。

详细归档、设备数据库、构建记录、JSON、日志和截图位于忽略目录 `outputs/stable-device-20261102/`，不提交用户数据或打包产物。本轮安装、正式入口、用户数据复原及设备清理收尾均已核验。

# Pocket1 上游修复与优化实施顺序

官方上游状态核对：**2026-11-02**；当前执行队列及 ruffle-miyooflip 源码核对：**2026-11-04**。适用：**Ruffle Flash 0.8.6 / Miniloong Pocket 1 / leafOS**。

当前升级基线为官方稳定 **`v0.7.1` / `89a7049965f3090e706265670df309a5128233ac`**，上游提交于 2026-10-08（北京时间）；core、配套库和 wgpu 使用同一快照，保留 Pocket1/LeafOS 适配与本地补丁。本文维护后续实施顺序及各项源码处置；本次升级的构建、主机执行与实机结论统一见[工程进度](progress.md)，构建方法见[工程实现细节](implementation.md)。旧验证报告只证明报告内的旧候选，不作为本次升级后的验收。

## 排序依据与使用方法

**01、05、06、07 已由稳定版等效覆盖；02—04 保留本地修复。**编号作为稳定任务入口保留，当前实施次序以下面的执行队列为准。已经存在的实现只复核回归，不再次 cherry-pick；未进入该稳定快照的候选仍须单独评估。

排序综合考虑：能否避免崩溃或误操作、是否影响默认 GLES 和随包游戏、移植及验证成本、是否持续占用 CPU/内存，以及收益证据是否适用于本设备。难度包含验证和适配成本，不按 PR 行数直接判断；下表收益为工程判断，上游微基准不等于 Pocket1 实测。

本设备的具体约束：

- 默认游戏绘制为 **GLES**；实验 Vulkan 使用同版官方 wgpu，经本地 GBM/DMA-BUF 与 GLES 共享呈现。wgpu 优化不能直接记作默认 GLES 收益。
- Flash 逻辑、显示树和同步解码仍主要串行。增加高频调用检查、全树遍历或强制 GC，可能抵消优化收益。
- 既有拳皇 GLES 限定测试 RSS 峰约 683 MiB、最低可用内存约 133 MiB；死神 3.6 限定测试进程内存峰约 713 MiB。它们是[既有场景记录](progress.md)，不是通用内存预算。
- 本地已有按需上传、静态资源回收、CPU 像素缓存、GLES 临时池上限和完整 BitmapData 覆盖免回读。移植不能抹掉这些修复，也不应重复实现。
- 原前端来源 `aweigit/ruffle-miyooflip@3bc39c5e0fdbc9fcde92681706395f6ef70f991b` 保存在来源锁的 `frontend_origin`；当前共享核心和 wgpu 均以官方 v0.7.1 为准。近期 master 已合并的 PR 不一定进入 2026-10-08 的稳定版。

**允许提前的情况：**确认保存失败或数据丢失时提前 08；确认实际子 SWF 加载/卸载持续累积并逼近 OOM 时提前 14；确认目标游戏被缺字、滤镜或音频缺失阻断时，提前对应兼容项。单个浏览器 issue 或相似报错不足以证明本地存在同一根因。

## 当前执行队列（2026-11-04）

先处理可独立验证、默认 GLES 也能受益的分配与复制，再处理需要游戏复现或大范围事件验证的优化。08 的失档/拒写、14 的持续增长接近 OOM 仍可按前述条件插队；13 的分阶段计时贯穿后续评估。第一批完成代码及主机验证，Pocket1 对照等待本轮可用 ADB，不沿用旧二进制的实机结论。

2026-11-07 本地排查已实施像素缓存降零时的线性批量清理，以及卸载维护免临时分配；这属于 13/14 相关维护路径的小范围优化。它不等于帧时间稳定性或 MovieLibrary 生命周期专项完成，后续仍需实机定位与长局增长曲线。[本轮证据](validation/cache-maintenance-20261107.md)。

| 次序 | 原任务编号 / 工作 | 当前处置与进入下一步的条件 |
| --- | --- | --- |
| 1 | **21 内嵌字体按需解码** | 已适配 v0.7.1；225 个随包 SWF 扫描、178 份字体的全部轮廓/指标对照和主机回归通过。字体解析累计分配请求 9.67 → 0.31 MiB；实际启动、首次文字和整机内存待实测 |
| 2 | **22a 未改变的孤立对象列表免复制** | 已实现共享快照下的无删除快速路径；每次仍检查弱引用和放置状态，不增加生命周期缓存。列表身份、迭代快照及 GC 回归通过；实际嵌套 goto 收益待测 |
| 3 | **10 屏外静态文字剔除** | 下一个独立候选；先证明默认 GLES 的长列表无效绘制，再审查 BitmapData.draw、缓存、滤镜、遮罩和 scrollRect，未实施 |
| 4 | **09 缓存抖动及动画遮罩** | 用本地死神 3.3/3.6 复现，取得纹理重建/画面证据后拆开适配；上游 2.6 样本不能直接当本地结论 |
| 5 | **22b 孤立对象工作集合 / Loader 空闲分支缓存** | 先取得 Construct、FrameScripts、Cleanup 热点；与 22a 分开。需完整动态增删、addFrameScript、嵌套 goto、Loader 和 GC 验证，未实施 |
| 6 | **12 鼠标命中边界计算** | 仅在移动/悬停采样确认热点后做；保护文字尺寸时序，并核对新增遍历成本 |
| 7 | **11 AS3 DSE** | 后于上述局部优化；需要真实脚本瓶颈及异常边/副作用语义回归，计入预优化时间和内存 |

08 的备份恢复/导入导出保留为数据功能队列；AIR 自动识别、GameInput 空设备兼容、摇杆中心校准按具体不兼容/漂移样本推进。计时器重写、电影库所有权、动态音频和 Vulkan 批处理继续遵循各专项的进入条件。

本轮参考 [ruffle-miyooflip 最近提交](https://github.com/aweigit/ruffle-miyooflip/commits/main)：`a8231e0` 的混合键鼠主要由现有独立映射覆盖；`b1b9cbf` 的 GL 加载问题在正式 SDL 路径已有对应机制；`98964d9` 的直接 DRM/KMS 呈现不直接适用于 Wayland/SDL。`c1e74c4` 虽启用 Cargo release LTO，Leaf 的实际 generic 构建仍由脚本覆盖为 opt-level 2、LTO=false、16 codegen units；既有 A55/ThinLTO 实验未证明稳定收益，暂不更改默认参数。

## 任务状态总表（保留原编号）

“稳定版已含”描述本次目标源码；“保留本地修复”表示稳定版之外的既有补丁；“待评估”表示尚未新增实施。“已合并/开放”为 2026-11-02 查询的上游状态，不能代替稳定版覆盖判断。每个编号仍可作为后续独立任务入口。

| 顺序 | 任务与上游来源 | 综合难度 | Pocket1 的主要收益与代价 | 当前行动 |
| --- | --- | --- | --- | --- |
| 01 | [异常 SWF 分配与长度防护](https://github.com/ruffle-rs/ruffle/pull/23962)、[场景/帧标签预分配](https://github.com/ruffle-rs/ruffle/pull/23960)；均已合并 | 低 | 避免损坏文件触发巨额预分配/下溢；几乎不增加稳定运行开销 | v0.7.1 已含；采用新版读取路径并保留损坏/大内容回归。[旧验证范围](validation/swf-length-guards-20261101.md) |
| 02 | [AS3 拖出误点击](https://github.com/ruffle-rs/ruffle/pull/23803)、[重复悬停事件](https://github.com/ruffle-rs/ruffle/pull/23807)、[AVM1 按钮高亮残留](https://github.com/ruffle-rs/ruffle/pull/25045)；均开放 | 低—中 | 改善摇杆鼠标、左键保持和拖拽的正确性；运行代价小 | 稳定版未含；保留本地 02a—02c 与回归，摇杆/快捷释放完整矩阵待验。[旧验证范围](validation/upstream-compatibility-20261101.md) |
| 03 | [AS3 根电影最终 progress 事件](https://github.com/ruffle-rs/ruffle/pull/25051)；开放 | 低—中 | 解除一类永久停在加载页的问题；主要发生在加载阶段 | 稳定版未含；保留根 progress/完成重入修复及回归。[旧验证范围](validation/upstream-compatibility-20261101.md) |
| 04 | [AVM1 根时间轴 onLoad 顺序](https://github.com/ruffle-rs/ruffle/pull/25054)；开放 | 低—中 | 修复初始化/声音代码漏执行；主要改变首次加载时序 | 稳定版未含；保留 level 首帧 onLoad 修复与参考 trace。[旧验证范围](validation/upstream-compatibility-20261101.md) |
| 05 | [BitmapData 自复制与混合精度](https://github.com/ruffle-rs/ruffle/pull/24676)；已合并 | 中 | 修复重叠复制画面错误，并优化部分 CPU 复制；需保护本地免回读路径 | v0.7.1 已含；同时保留完整覆盖免回读及本地像素回归。[旧验证范围](validation/upstream-compatibility-20261101.md) |
| 06 | [Vector 原生调用优化](https://github.com/ruffle-rs/ruffle/pull/24660)；已合并 | 低 | 改动集中、部分操作更便宜；实际游戏收益可能很小 | v0.7.1 已含参数化 Vector 判断；保留优化开/关语义回归，性能待测 |
| 07 | [删除 ScopeChain 缓存](https://github.com/ruffle-rs/ruffle/pull/24193)；已合并 | 中 | 减少冗余缓存与内存，改善作用域语义；不承诺明显提速 | v0.7.1 已移除错误缓存；保留命名空间回归，性能待测 |
| 08 | 原生进度恢复与导入导出；参考 [#8470](https://github.com/ruffle-rs/ruffle/issues/8470)、[#22409](https://github.com/ruffle-rs/ruffle/issues/22409) | 中—高 | 直接保护长期游戏进度；大部分成本在低频文件操作和界面 | 08a 已补持久失败状态与现有重试入口；完整退出选择及 08b—08d 暂缓 |
| 09 | [位图缓存抖动](https://github.com/ruffle-rs/ruffle/pull/24992)、[矢量缓存抖动](https://github.com/ruffle-rs/ruffle/pull/25003)、[动画遮罩](https://github.com/ruffle-rs/ruffle/pull/24949)；均开放 | 中—高 | 上游明确涉及死神 2.6 菜单；本地 3.3/3.6 是否受影响需复现 | 目标未含这三项；继续复现本地 3.3/3.6，核对依赖及纹理重建代价 |
| 10 | [屏外静态文字剔除](https://github.com/ruffle-rs/ruffle/pull/24944)；已合并 | 低—中 | 有长列表/大量屏外文字时可省绘制；要防止错误隐藏文字 | 10-19 才合并，未进入 v0.7.1；仍需默认 GLES 热点及裁剪/离屏验证 |
| 11 | [无效赋值消除 DSE](https://github.com/ruffle-rs/ruffle/pull/25035)；开放 | 中—高 | 有机会降低 AS3 解释执行量；优化阶段更贵、语义验证要求高 | 目标无该 DSE pass；仍需真实 AS3 瓶颈及优化阶段成本对照 |
| 12 | [减少鼠标命中边界计算](https://github.com/ruffle-rs/ruffle/pull/24860)；开放 | 中—高 | 可能降低复杂显示树 CPU 成本；伴随文字尺寸时序和遍历成本 | 目标未含该遍历调整；先采样 bounds 热点，再评估额外遍历成本 |
| 13 | 帧时间稳定性；参考 [#22758](https://github.com/ruffle-rs/ruffle/issues/22758) | 中—高 | 可能改善平均 FPS 正常但体感不顺；不能解决脚本本身的长帧 | 升级不等于调度问题已解决；继续分离脚本/解码/呈现与调度 |
| 14 | [电影库 GC](https://github.com/ruffle-rs/ruffle/pull/23071)、[unloadAndStop](https://github.com/ruffle-rs/ruffle/pull/24918)、[弱引用与 GC](https://github.com/ruffle-rs/ruffle/pull/23897)；均开放 | 高 | 若持续泄漏则收益很大；当前方案有生命周期语义和停顿风险 | 目标仍无 GC 管理电影库，unloadAndStop 仍为 stub；先取得增长曲线 |
| 15 | [计时器机制重写](https://github.com/ruffle-rs/ruffle/pull/24915)；开放 | 高 | 对齐到期顺序与错过周期的处理；会改变游戏逻辑时序 | 目标仍为原 BinaryHeap/累计时间机制；缺不兼容样本时不另行重写 |
| 16 | [设备字体回退](https://github.com/ruffle-rs/ruffle/pull/24728)、[非 TextField 文字事件](https://github.com/ruffle-rs/ruffle/pull/24151)；均开放 | 中—高 | 改善缺字及自定义文字控件；软键盘仍需单独适配 | 目标未含完整回退链与非 TextField textInput；继续复现，中文组词另立设计 |
| 17 | [BitmapData.draw 支持滤镜](https://github.com/ruffle-rs/ruffle/pull/21672)；开放 | 高 | 修复部分游戏缺失特效；可能增加离屏纹理、绘制和同步成本 | 目标 draw 路径仍禁用位图缓存；先确认缺失阻断及离屏/同步代价 |
| 18 | [sampleData 动态音频](https://github.com/ruffle-rs/ruffle/pull/25055)、[Sound.extract](https://github.com/ruffle-rs/ruffle/pull/25056)；均开放 | 高 | 让依赖自混音的内容发声；引入解码、队列和延迟成本 | 目标无 sampleData 生成，Sound.extract 仍为 stub；继续按需求评估 |
| 19 | [深递归 StackOverflowError](https://github.com/ruffle-rs/ruffle/pull/24973)；开放 | 中—高 | 将部分进程崩溃变成脚本异常；审查指出方法调用开销增加 | 目标未含该保护；缺递归崩溃样本时先跟踪低开销方案 |
| 20 | [wgpu 透明像素颜色矩阵](https://github.com/ruffle-rs/ruffle/pull/25047)；开放；[wgpu 批处理](https://github.com/ruffle-rs/ruffle/pull/24947)；已合并 | 前者低—中，后者高 | 在本项目主要影响实验 Vulkan；不改善默认 GLES 路径 | 20a 目标仍有零 alpha 除法；20b 于 10-22 合并，未进稳定版；均保留为专项 |
| 21 | [内嵌字体按需解码，fork 849f3aa](https://github.com/aweigit/ruffle-miyooflip/commit/849f3aab9edd32aa0370add3d9aee4ee8c0836e8) | 中 | 降低 DefineFont2/3 初始轮廓分配；部分成本转移到首次使用，需核对文字正确性 | 第一批代码及主机验证通过；[证据和边界](validation/font-orphan-optimization-20261104.md)，实机待验 |
| 22 | [孤立对象清理与遍历，fork e696060](https://github.com/aweigit/ruffle-miyooflip/commit/e696060424055a0d67498c3f3d60cde34a37df4e)、[849f3aa](https://github.com/aweigit/ruffle-miyooflip/commit/849f3aab9edd32aa0370add3d9aee4ee8c0836e8)、[Loader 55ed89a](https://github.com/aweigit/ruffle-miyooflip/commit/55ed89ab64154e85769260f49c8ab2fa302e18f3) | 22a 低；22b 高 | 减少嵌套帧清理复制及空闲分支遍历；工作缓存涉及生命周期和失效正确性 | 仅 22a 已实现并通过主机回归；22b 未实施，不能把小修结论扩大为 Loader/全树缓存通过 |

## 01—05：维护已修范围与稳定版兼容

### 01 异常 SWF 分配与长度防护

- 当前处置：[swf/src/read.rs](../player/swf/src/read.rs) 采用 v0.7.1 的统一长度减法检查、128 MiB 初始解压容量与 256 项场景/帧标签初始容量；旧回移由等效上游实现覆盖，损坏样本与大有效内容回归继续保留。新版同时按 SWF 声明的 body length 截断读入，LZMA 则改用未知解包长度的结束标记模式，不能把旧版的宽松解码说明直接沿用到新版。
- [长度防护报告](validation/swf-length-guards-20261101.md) 记录旧基线的主机、ARM64、两后端夹具及限定游戏验证；本次升级结论以工程进度为准。128 MiB 仍是初始容量，允许有效内容在声明长度内继续增长，256 项也不是条目数量上限；这些检查不构成通用 OOM 保护。

### 02 鼠标释放、拖拽与按钮状态

- **02c 处置：** 稳定版未含，保留已审查的本地按钮/`trackAsMenu` 修复；5 项 native 状态回归继续保留。[旧来源与验证](validation/upstream-compatibility-20261101.md)。

- **02b 处置：** 稳定版未含，保留本地拖动与释放时的 AVM2 悬停事件去重；3 项完整事件顺序回归继续保留。[旧来源与验证](validation/upstream-compatibility-20261101.md)。

- **02a 处置：** 稳定版未含，保留 AVM1 专用对象等价回退的适用范围修正；2 项 VM 回归继续保留，AVM1 goto 专项待验。[旧来源与验证](validation/upstream-compatibility-20261101.md)。

- 本地入口：[core/src/player.rs](../player/core/src/player.rs) 及 AVM1 按钮实现。按 **02a #23803 → 02b #23807 → 02c #25045** 分开验证；这是建议拆分顺序，不宣称三者存在上游依赖。
- 实施：限制 AVM1 专用对象等价/拖拽事件逻辑的适用范围。保留 AVM1 在 goto 后重建按钮的兼容处理，以及 `trackAsMenu` 的菜单跟踪行为。
- 验收：按下后拖出松开不误点击；移出再移入恢复正确状态，AS3 rollOver/rollOut 不重复；AVM1 普通按钮与菜单按钮分别测试。实机覆盖摇杆鼠标、M+下左键保持、暂停/切方案释放；不要只测桌面鼠标。

### 03 AS3 加载页卡死与完成事件重入

- **03 处置：** 稳定版未含，迁移本地最终 progress 与完成防重入补丁，并采用新版 `is_from_bytes()` 判断区分 loadBytes；2 项根电影顺序/goto 回归保留，子 Loader/分块待验。[旧来源与验证](validation/upstream-compatibility-20261101.md)。

- 本地入口：[player.rs](../player/core/src/player.rs)、[loaderinfo_object.rs](../player/core/src/avm2/object/loaderinfo_object.rs)。复现“首帧 stop，监听 progress 到 100% 才 play”的 SWF。
- 实施：对齐根电影最终 progress 与 init/complete 的顺序，完成标志在可重入事件前处理。保护分块加载及子 Loader 行为。
- 验收：监听器能收到最终进度，事件顺序和次数匹配 Flash；回调内 goto 不递归重复完成事件；本地文件和子 SWF 资源加载都能继续。

### 04 AVM1 根时间轴 onLoad 顺序

- **04 处置：** 稳定版未含，保留 Stage 下 level 首帧脚本后派发 onLoad 的本地补丁，继续保留 4 项 Flash 参考 trace。[旧来源与验证](validation/upstream-compatibility-20261101.md)。

- 本地入口：[movie_clip.rs](../player/core/src/display_object/movie_clip.rs) 的 `run_frame_avm1`。
- 实施：区分 Stage 下的 level 首帧与 `loadMovie` 替换内容；不能把所有 load 事件统一延后。
- 验收：根首帧定义的 onLoad 恰好执行一次，初始化与声音正常；对照上游 root_onload、action execution order 和 issue_2870 测试，覆盖 level 加载、loadMovie 与 onEnterFrame 顺序。

### 05 BitmapData 复制与透明混合

- **05b 处置：** v0.7.1 已含方向自复制和行复制，采用上游实现并重新整合本地完整目标覆盖免回读；旧有 10 项核心回归及 68 组 Flash 网格保留，新增透明源复制到不透明目标的颜色还原与免回读组合回归。[旧来源与验证](validation/upstream-compatibility-20261101.md)。

- **05a 处置：** v0.7.1 已含 256 基整数混合，采用等效上游实现并保留 256 组 Flash 参考色值。[旧来源与验证](validation/upstream-compatibility-20261101.md)。

- 本地入口：[operations.rs](../player/core/src/bitmap/operations.rs)、[copy_on_cpu.rs](../player/core/src/bitmap/operations/copy_on_cpu.rs)、[bitmap_data.rs](../player/core/src/bitmap/bitmap_data.rs)。新版透明源写入不透明目标时先还原非预乘颜色；保留该修正与本地免回读路径的组合。
- 实施：移植重叠区域的 Flash 覆盖语义、可用的快速复制和混合精度修正；把本地完整覆盖免回读重新整合到最终路径，不能被上游旧的 `dest.sync()` 覆盖。
- 验收：上下左右重叠、自复制、透明/不透明、mergeAlpha、边界裁剪、局部/整图覆盖和 GPU-dirty 来源/目标都检查真实像素；跑既有 [BitmapCopyProbe](../scripts/resources/BitmapCopyProbe.as) 并扩展缺失场景。GLES/Vulkan 分别验证；复测[死神换回合](validation/bvn33-pve-stutter-20261031.md)不能退回旧目标同步瓶颈。

## 06—12：核对稳定版覆盖与真实收益

### 06 Vector 原生调用优化

- **06 处置：** v0.7.1 已含，旧回移由等效上游实现覆盖；优化开/关及 29 项语义断言继续保留，未据此宣称整机提速。[旧来源与验证](validation/upstream-compatibility-20261101.md)。

- 本地入口：[type_aware.rs](../player/core/src/avm2/optimizer/type_aware.rs)。目标源码已有 `is_final()` 或参数化 Vector 判断，以及原参数数量、类型和 fast_call 校验，无需重复应用该 PR。
- 实施与验收：确认本地 Vector 类型约束和 CallNative 接口匹配，测不同元素类型、长度/索引访问、异常和继承限制；再测实际战斗场景。作者未观察到真实 SWF 的明显整体收益，因此把它当低成本增量，不宣传游戏数倍提速；若依赖适配扩大，后移。

### 07 删除 ScopeChain 缓存

- **07 处置：** v0.7.1 已含，旧回移由等效上游实现覆盖；2 项命名空间查找回归继续保留，性能收益未测。[旧来源与验证](validation/upstream-compatibility-20261101.md)。

- 本地入口：[scope.rs](../player/core/src/avm2/scope.rs)、相关 vtable。目标源码已使用 `Gc<Vec<Scope>>`，不再含 `ScopeContainer`、额外 PropertyMap 查找缓存及 `get_trait_with_ns`。
- 实施与验收：带入 `scopes_dont_cache` 等语义测试，检查动态属性、with-scope、命名空间和作用域修改后的查找结果；记录启动分配、RSS 和脚本耗时。它首先是正确性与冗余缓存清理，不是无条件性能优化。

### 08 原生进度可恢复性与导入导出

- 保存提示小步已完成：失败状态跨页保留，当前槽 Y 重试，成功后清除；完整重试/取消/明确继续退出及备份/导入导出仍待做。[验证](validation/reliability-fixes-20261101.md)。

- 类型：本地功能完善，issue 提供需求参考，没有可直接移植的完整方案。入口：[experience.rs](../player/sdl2test-rocknix/src/experience.rs)、[data_manager.rs](../player/sdl2test-rocknix/src/data_manager.rs)、[leaf.rs](../player/sdl2test-rocknix/src/leaf.rs)。
- 顺序：**08a 失败重试/取消及明确状态 → 08b 可恢复的完整备份集合 → 08c 导出 → 08d 导入**。本地已在部分退出/重启路径阻止保存失败后继续，并有有效 `.bak` 回退；先查缺口，避免重做已有逻辑。
- 实施：沿用内容身份和五槽语义，恢复/导入前停止并销毁旧 Player，防止旧实例写回；备份记录身份、版本、清单和校验。多 `.sol` 使用一致集合，先验证并准备完整候选，再提交，失败保留旧有效档。导入还需检查路径和大小边界。
- 验收：缺文件、损坏、满卡/只读、重复操作、版本身份冲突及失败中断均不覆盖旧有效进度；导出再导入能在游戏内继续。原生数据恢复与完整运行现场快照分别表述。详细要求沿用[进度中的 F5.4/F5.6](progress.md)。

### 09 死神菜单、滤镜与动画遮罩抖动

- 复现入口：[#12788](https://github.com/ruffle-rs/ruffle/issues/12788)、[#24957](https://github.com/ruffle-rs/ruffle/issues/24957)。先跑最小夹具，再确认本地死神 3.3/3.6；上游在 2.6 的结果不能自动外推。
- 顺序：**09a #24992 → 09b #25003**，后者明确依赖前者且当前分支包含其改动，不能重复应用。**09c #24949** 处理动画遮罩，单独判断依赖和必要性。
- 验收：固定同帧截图与 Flash 对照，LOW/HIGH、缩放、亚像素移动、遮罩、cacheAsBitmap 开关/恢复都覆盖；同时测临时纹理峰值、重建频率和长帧。画面改善但引发明显内存或重建回退时继续修正，不直接默认启用。

### 10 屏外静态文字剔除

- 当前处置：#24944 于 2026-10-19 合并，晚于 v0.7.1；目标 [text.rs](../player/core/src/display_object/text.rs) 的 `render_self` 仍直接绘制字形，未含该屏外剔除。继续作为低成本候选，不因 master 已合并而标为本地已完成。
- 实施与验收：先用长列表证明默认 GLES 存在无效绘制；保持 BitmapData.draw、滤镜及缓存的离屏内容正确。上游描述已列出遮罩、scrollRect/3D 等边界差异，要专门验证。保留文字正确性优先；只有合成列表加速时，将收益限定为该场景。

### 11 AS3 无效赋值消除 DSE

- 当前处置：v0.7.1 未含 #25035 的 DSE pass；现有 `dce.rs` 的不可达代码清理不代表本项已实现。进入条件仍为代表游戏受 AS3 执行限制，且能构造有意义的回归样本。
- 实施：先检查与本地优化器的依赖差异，覆盖分支、循环、异常边、局部变量类型和可观察副作用，避免删除语义上仍需执行的写入；对照开启/关闭优化的输出和画面。
- 验收：相同游戏、存档、输入和构建配置下，分别记录启动/首次调用与稳定战斗帧时间、内存；至少重复三轮。上游报告多数样本执行指令减少约 2–5%，不等于整机帧时间减少 2–5%；新增优化时间和临时内存也必须计入。

### 12 鼠标命中检测与文字尺寸时序

- 当前处置：v0.7.1 未含 #24860 的命中边界/文字尺寸遍历调整。本地入口：[movie_clip.rs](../player/core/src/display_object/movie_clip.rs)、[player.rs](../player/core/src/player.rs)；先采样鼠标移动/悬停时的边界遍历成本。
- 实施：分析 `world_bounds` 的副作用与文字延迟 autosize 更新时点。上游同时增加渲染阶段遍历，不能只交换条件，也不能假定全树遍历总会更便宜。
- 验收：隐藏/裁剪文字、按钮不同状态、文字宽度/位置、Event.RENDER 与 setTimeout(0) 的观察结果对齐；有/无鼠标活动的真实游戏都比较 CPU 和帧时间。无收益或引入更多遍历则暂缓。

## 13—20：有证据或特定内容需求再做

### 13 帧时间稳定性与调度

- 先用现有 `--trace-performance` 分开看执行、解码、渲染、GPU 等待与休眠；参考 [#24884](https://github.com/ruffle-rs/ruffle/issues/24884) 的方法，但不把其私有样本结论直接当成本地根因。
- 若抖动来自调度，再检查截止时间计算、等待精度及呈现节奏；若来自长脚本/同步解码，应修对应热点。保留 SWF 帧率和事件语义，不用强行改成 60 FPS 掩盖问题。
- 验收：30/60 FPS 样本的帧时间分布、输入延迟、音画同步、暂停恢复和功耗/CPU 变化一起检查；平均 FPS 不变也可能有收益，忙等提高 CPU 换来的平滑需明确权衡。

### 14 MovieLibrary、卸载与弱引用生命周期

- 当前处置：v0.7.1 仍以 `PtrWeakKeyHashMap<Weak<SwfMovie>, MovieLibrary>` 管理电影库，尚非候选的 GC 管理 MovieLibrary；`Loader.unloadAndStop` 仍调用 stub。升级 gc-arena 本身不等于这些生命周期方案已实现。调研入口：[#24864](https://github.com/ruffle-rs/ruffle/issues/24864)，继续区分提前 GPU 上传、已卸载电影持有、正常缓存、弱引用与游戏自身对象。
- 先做可重复加载/卸载夹具和内存增长曲线，再决定修改。已有按需上传/闲置回收覆盖部分 GPU 问题，不能再用上游未修基线估算本地收益；反复进入同一场景与不断加载不同场景分别测。
- [#24918](https://github.com/ruffle-rs/ruffle/pull/24918) 自述释放库后，仍存活对象不能继续实例化其符号；停止声音、流、计时器和 stage 监听器也尚未完成。不能直接把该实现当作完整 unloadAndStop。
- [#23071](https://github.com/ruffle-rs/ruffle/pull/23071) 改动涉及电影库所有权；[#23897](https://github.com/ruffle-rs/ruffle/pull/23897) 涉及弱字典/监听器与 GC，缺新增回归测试。三者有重叠且不应无审查叠加；先核对上游最新所有权演进，再按实际泄漏根因拆开做。
- 验收：多 Loader 共用资源、跨 ApplicationDomain、保留旧实例后继续创建符号、卸载后重载、弱键枚举/失效、声音和事件生命周期都正确；重复几十轮后内存进入合理平台期且无强制 GC 长停顿。达不到时不以“内存下降”单项判通过。

### 15 计时器机制

- 当前处置：[timer.rs](../player/core/src/timer.rs) 仍为 `BinaryHeap` 与 `cur_time` 累计机制；#24915 晚于稳定版且仍开放，未随本次升级加入。这是 Flash VM 计时器，和掌机按键连发调度是两套机制。
- 实施与验收：核对同批到期顺序、过期 interval 不集中补发、回调中新增/删除/重启、更改 delay 和 ID 行为；真实游戏覆盖卡顿后恢复。上游 PR 尚未解决全部帧内执行时点，不能声称完整对齐 Flash，也不能当作通用提速方案。

### 16 字体回退与自定义输入控件

- 当前处置：目标仍未含 #24728 的完整默认设备字体回退链；文本输入事件仍只进入聚焦 EditText，未含 #24151 的非 TextField 分支。现有 Leaf 设备字体扫描、CJK 回退及标准 TextField 软键盘继续保留。
- **16a 缺字：**先复现 [死神中文问题 #17134](https://github.com/ruffle-rs/ruffle/issues/17134)，区分嵌入字体缺字、设备字体回退、字体样式和资源缺失，再评估 #24728；本地 UI 字体正常不代表游戏字体正常。
- **16b 自定义控件：**#24151 只补向聚焦非 TextField 对象发 textInput；本地软键盘还需识别目标和提交机制。防止标准 TextField 重复输入，保留取消、密码遮挡、preventDefault 和暂停语义。
- 中文拼音组词是独立功能，不能因合入上述 PR 就标为完成；[#1778](https://github.com/ruffle-rs/ruffle/issues/1778) 也不能代替本地输入法设计与验收。

### 17 BitmapData.draw 的滤镜语义

- 当前处置：v0.7.1 的 BitmapData.draw 仍设置 `use_bitmap_cache: false`，未包含 #21672 的绘制滤镜支持。先确认目标游戏确实使用该路径，再评估缺失画面；不要把普通 applyFilter 的已有支持当成本项完成。
- 实施与验收：核对核心缓存及本地 GLES/wgpu 离屏接口。低画质、滤镜组合、变换、裁剪与混合的像素正确；现有 scratch/PBO 上限继续生效。记录额外纹理、渲染次数和同步，不能只修桌面 Vulkan。

### 18 动态音频与 Sound.extract

- 当前处置：v0.7.1 虽已有 SampleDataEvent 类型，但尚无 #25055 的动态声音生成路径，`Sound.extract` 仍为 stub。顺序继续为 **18a #25055 → 18b #25056**；先确认无声内容实际依赖这些 API，而非资源路径或解码失败。
- 适配本地音频后端，验证采样格式、队列、位置、完成事件、循环、暂停/恢复及退出；实机听感、延迟、欠载和 CPU/内存必须记录。已有普通音效和背景音乐不可回退；不依赖该 API 的游戏没有直接收益。

### 19 深递归崩溃保护

- 当前处置：#24973 创建于稳定版发布之后，目标未含该深递归保护。其[审查意见](https://github.com/ruffle-rs/ruffle/pull/24973#pullrequestreview-5659171189) 指出约 4% 方法调用开销增加；这不是整机帧率下降 4%，后续仍需评估 Pocket1 成本。
- 先核对 ARM64/目标 libc 上剩余栈检测能力和后备深度策略，再决定实现。覆盖不同调用路径、value stack、异常可捕获及正常深调用；在普通游戏测调用成本。优先跟踪减少检测开销的后续方案。

### 20 Vulkan 专项

- **20a #25047：**可独立先做。入口：[color_matrix.wgsl](../player/render/wgpu/shaders/filter/color_matrix.wgsl)，本地仍有 alpha 为零时除法。用透明像素和颜色/alpha 偏移夹具验证真实 Vulkan 像素；上游也声明还有显示列表颜色偏移差异，不能扩大结论为滤镜完全兼容。
- **20b #24947：**于 2026-10-22（北京时间）合并，晚于 v0.7.1；本次升级尚未取得该批处理改造。后续单独评估绘制顺序、遮罩、混合、滤镜和位图句柄所有权，保留本地 BitmapHandle 修复、DMA-BUF 和队列同步，再测真实设备 draw call、CPU、等待和内存。
- 只有用户主要使用 Vulkan 或确认 Vulkan 问题阻断使用时才提前该专项；20a 的小修不需要等待 20b，也不需要整体升级 core/wgpu。

## 21—22：fork 中可拆分的分配与遍历优化

### 21 内嵌字体按需解码

- 适配 `849f3aa` 的思路到新版 `font/glyph.rs`：读取 DefineFont2/3 时保留经过校验的轮廓字节范围；首次命中测试或注册字形时解码并缓存，ASCII 保持既有预热。
- 字体指标、字符索引、重复码点覆盖顺序、kerning、字体版本缩放保持。偏移不规则或单字形不足两字节时回退稳定版 eager reader；不带入旧 fork 对极短字形的宽松行为。损坏的延迟轮廓只在自己的范围内读取，失败缓存为空，不污染邻字形。
- 第一批包括 3 项核心字体回归、3 项解析器回归，以及随包 225 个 SWF / 178 份字体的全部轮廓对照。累计分配请求不是常驻/峰值内存；首次文字帧、GPU、实际加载时间和 FPS 留待 Pocket1 对照。[本轮验证](validation/font-orphan-optimization-20261104.md)。

### 22 孤立对象清理与工作集合

- **22a 已实现：**`cleanup_dead_orphans` 在列表由外层回调共享且全部条目仍应保留时，直接保留同一个 Rc 列表，避免 `Rc::make_mut` 复制全部弱引用。需要移除条目时仍使用原 retain 和写时复制，外层快照顺序不变；无共享者时沿用原单次扫描。
- 每次清理仍重新检查弱引用、parent 和 `placed_by_avm2_script`，不引入跨 GC/帧的有效性缓存，也不强制 GC。2 项核心回归核对列表身份、移除后的旧快照及弱引用失效。
- **22b 未实施：**fork `849f3aa` 的分阶段工作集合和 `55ed89a` 的 Loader 子树汇总依赖时间轴摘要及完整失效链；本地没有这组前置机制，不能独立摘取 Loader 小补丁。先采样，再拆构造阶段、脚本阶段、清理有效期和 Loader 唤醒，保护 SimpleButton 等特殊容器与嵌套 goto。

## 本轮处置边界

本次统一稳定基线后，01/05/06/07 采用稳定版等效实现，02—04 保留既有本地补丁；原回移的设备证据仍限于[旧合并候选](validation/upstream-compatibility-20261101.md)。08a 的失败提示、重试及现有可靠性保护保留，其余备份/导入导出仍待做。09—20 尚未因为本次升级完成专项验收；其中 10 和 20b 虽在 master 合并，却晚于稳定版，仍是后续候选。20a 的透明像素 2D Vulkan 问题也继续保留。升级结果及本次实际执行检查集中记录到工程进度，不沿用旧二进制的实机通过结论。出现目标游戏复现、真实失档或逼近 OOM 的证据时，按本文条件提前。

## 待复现问题与暂缓方向

这些条目先作为夹具或调查任务，不因 issue 仍开放就认定本地有 bug。

| 来源 | 后续动作与排序调整条件 |
| --- | --- |
| [#24812 三键技能](https://github.com/ruffle-rs/ruffle/issues/24812) | 在 02 后核对物理输入、组合键事件顺序、Key.isDown 状态和游戏响应；现有绑定本来就按序按下，不能未经 Flash 对照改成“原子同时按下”。复现后另立输入任务。 |
| [#12556 读取存档卡死](https://github.com/ruffle-rs/ruffle/issues/12556) | 在 08 中用存档副本隔离复现，区分 AMF/VM 解析、脚本预算和文件 IO；写盘可靠不等于内容能正确载入。不可用原始用户档直接试验。 |
| [#23832 copyPixels 频繁回读](https://github.com/ruffle-rs/ruffle/issues/23832) | 在 05 后采样残留局部覆盖/混合路径；只有定位真实同步热点后才研究 GPU 复制。不能误以为本地整图覆盖优化已消除全部读回。 |
| [#24032 长对局逐渐变慢](https://github.com/ruffle-rs/ruffle/issues/24032) | 做长时帧时间、对象/资源和内存曲线；维护者明确指出其 BitmapData 根因仍是假设，不能据此直接改缓存。 |
| [#24651 关卡切换失焦](https://github.com/ruffle-rs/ruffle/issues/24651) | 结合 02/16 验证键盘焦点；宿主窗口恢复焦点与 Flash 内部焦点分别记录，避免反复要求用户点击。 |
| [#24541 Playerglobal 启动优化](https://github.com/ruffle-rs/ruffle/pull/24541)（开放） | 排在稳定运行问题之后。先量出 playerglobal 占启动耗时/内存的比例；跨实例共享对当前单 Player/进程重开路径的收益有限，仍可单独评估静态数据免复制等子改动。 |
| [#9119 速度调节](https://github.com/ruffle-rs/ruffle/issues/9119) | 作为额外玩法功能暂缓；需定义逻辑、计时器、输入和音频同步语义，不能作为游戏性能不足的修复。 |
| [#1446 完整即时存档](https://github.com/ruffle-rs/ruffle/issues/1446) | 涉及 VM、资源和宿主状态序列化，成本远高于原生进度管理。先做 08；现有 SharedObject 五槽不等同完整现场快照。 |

## 每项实施的共同流程

1. **记录基线。**检查工作区和暂存区；记录本地 commit、上游 PR 最新 head/合并 commit、引用的依赖、游戏/SWF 与存档指纹。确认本地尚未以等价方式修复。
2. **先有能判定结果的样本。**正确性修复采用 Flash 参考输出、像素或明确事件序列；性能项采用实际热点和可重复场景。不要只写与实现镜像的测试。
3. **控制范围。**一个修复或紧密依赖组单独提交；跨代接口以适配本地语义为准，不为小补丁顺带升级全部依赖、改帧率、增加缓存或更换后端。
4. **分层验证。**先主机针对性回归，再 ARM64 构建，最后隔离 Pocket1 测试。涉及实机时尽量提供当前可用 ADB 地址和连接说明；已有连接直接复用，没有连接继续主机工作，并标明实机待验证。临时入口及 Apps 索引清理遵循仓库约定。
5. **核对发布输入。**代码修改后更新受影响的 `leaf-source.lock.json`；导入 wgpu 文件同时按现有约定维护 `vendor/ruffle-wgpu.lock.json` 的来源与本地补丁记录。先重建再打包，核对 ELF 与 build manifest，不用更新哈希替代重建或来源说明。
6. **落结论。**分别写编译、主机执行、夹具实机、真实游戏和人工听感结果；更新[工程进度](progress.md)及受影响文档，检查差异后只暂存相关文件并提交。无收益/不适用的候选记录理由后暂缓，不强行改成“已修复”。

### 工程命令与验证边界

在 `ruffle-leafos/` 运行，按改动选择命令，不要求每项机械跑全套：

```powershell
# ARM64 编译检查与实际构建
python scripts/build_player.py check --offline --isolated-config
python scripts/build_player.py build --offline --isolated-config

# 修改前端输入、进度、菜单等时运行对应主机回归
python scripts/test_session.py

# 修改渲染资源所有权时运行；默认仅为 wgpu noop 所有权回归
python scripts/test_renderer.py

# 实际执行主机核心与受限 SWF 回归
python scripts/test_core.py
python scripts/test_swf.py

# 编译 ARM64 核心单元测试；不会自动运行
python scripts/build_player.py test --package ruffle_core --offline --isolated-config
```

新增核心/SWF 测试必须实际执行，并记录使用的宿主或隔离设备运行方法；不能把上面的测试编译命令当成测试通过。构建参数、SDK/运行库限制及其他专项命令以[实现细节](implementation.md)为准。

### 性能项的保留条件

- 基线与候选使用相同 Leaf/驱动、后端、分辨率、画质、构建 tuning、游戏和输入脚本；用存档副本，区分启动/首次资源构建与稳定阶段。同场景交替重复至少三轮，记录测试时长与可见温度条件。
- 优先复用现有诊断，记录 Flash 帧率、主循环/帧时间的 p50/p95/p99、超过 33.333/50 ms 的次数、CPU、RSS 峰与最低 MemAvailable；有相应计数时记录 GPU 资源和同步等待。主循环耗时、呈现间隔、GPU 纯耗时不能互相替代。
- 若现有日志只提供窗口汇总，先以受控、有限样本采集补足分位数，测完关闭；不为验收永久增加高频日志。保留原始样本在忽略的本地工作目录。
- 至少覆盖拳皇、死神 3.3 目录版和一个轻量 AVM1 样本；仅目标场景受益就如实限定，不扩展为通用提速。只影响 Vulkan 的改动按 Vulkan 验收，公共核心/渲染接口改动需检查两后端正确性。
- 变化落在重复测量波动内时不宣称提速；若补丁还增加复杂度或内存，优先暂缓。不能用平均 FPS 上升掩盖长帧、输入、声音或存档回退。

### 单项结果记录模板

实施时把摘要放入工程进度，详细独立证据放入适当的验证报告；本计划仅更新对应编号的处置和链接，避免复制多份结论。

```text
编号与任务：
处置：待评估 / 实施中 / 已验证合入 / 主机通过但实机待验 / 不适用 / 暂缓
本地基线与修复 commit：
上游 PR、实际采用 commit 与依赖：
修改范围及保留的本地差异：
复现样本、游戏/存档/候选 ELF 指纹：
主机实际执行结果：
ARM64 编译结果：
实机环境、场景、时长与结果：
性能/内存对照与可比性限制：
剩余风险、文档与验证报告链接：
```

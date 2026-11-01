# Pocket1 上游修复与优化实施顺序

整理与上游状态核对：**2026-11-01**。适用：**Ruffle Flash 0.8.3 / Miniloong Pocket 1 / leafOS**。

本地评估基点：`d393ceb1fd351d9ecf130fece931f85d287afa4f`。本文维护实施顺序及各项处置；已移植范围以总表及逐项处置为准。完成状态和正式验证结论统一维护在[工程进度](progress.md)，构建方法见[工程实现细节](implementation.md)。

## 排序依据与使用方法

建议先完成 **01—05**，再逐项评估 **06—12**。13 之后按复现结果和实际游戏需求推进，不要求把所有候选都合入。每项先核对最新上游状态、实际依赖和本地实现，再决定移植、改写或暂缓。

排序综合考虑：能否避免崩溃或误操作、是否影响默认 GLES 和随包游戏、移植及验证成本、是否持续占用 CPU/内存，以及收益证据是否适用于本设备。难度包含验证和适配成本，不按 PR 行数直接判断；下表收益为工程判断，上游微基准不等于 Pocket1 实测。

本设备的具体约束：

- 默认游戏绘制为 **GLES**；Vulkan 是独立导入的实验后端，经 GBM/DMA-BUF 与 GLES 共享呈现。wgpu 优化不能直接记作默认 GLES 收益。
- Flash 逻辑、显示树和同步解码仍主要串行。增加高频调用检查、全树遍历或强制 GC，可能抵消优化收益。
- 既有拳皇 GLES 限定测试 RSS 峰约 683 MiB、最低可用内存约 133 MiB；死神 3.6 限定测试进程内存峰约 713 MiB。它们是[既有场景记录](progress.md)，不是通用内存预算。
- 本地已有按需上传、静态资源回收、CPU 像素缓存、GLES 临时池上限和完整 BitmapData 覆盖免回读。移植不能抹掉这些修复，也不应重复实现。
- Ruffle core 来自 `aweigit/ruffle-miyooflip@3bc39c5e0fdbc9fcde92681706395f6ef70f991b`；wgpu 来自官方 `71f18f0d0e3bd118945d5956f4ac4bb2e486e856`。不能假设近期 PR 可以无依赖地 cherry-pick。

**允许提前的情况：**确认保存失败或数据丢失时提前 08；确认实际子 SWF 加载/卸载持续累积并逼近 OOM 时提前 14；确认目标游戏被缺字、滤镜或音频缺失阻断时，提前对应兼容项。单个浏览器 issue 或相似报错不足以证明本地存在同一根因。

## 推荐顺序总表

“待评估”意味着尚未实施；“已合并/开放”只描述上游 PR 状态。每个编号可作为后续独立任务入口。

| 顺序 | 任务与上游来源 | 综合难度 | Pocket1 的主要收益与代价 | 当前行动 |
| --- | --- | --- | --- | --- |
| 01 | [异常 SWF 分配与长度防护](https://github.com/ruffle-rs/ruffle/pull/23962)、[场景/帧标签预分配](https://github.com/ruffle-rs/ruffle/pull/23960)；均已合并 | 低 | 避免损坏文件触发巨额预分配/下溢；几乎不增加稳定运行开销 | 已验证合入；主机、ARM64、两后端夹具及限定 GLES 游戏启动通过；[范围](validation/swf-length-guards-20261101.md) |
| 02 | [AS3 拖出误点击](https://github.com/ruffle-rs/ruffle/pull/23803)、[重复悬停事件](https://github.com/ruffle-rs/ruffle/pull/23807)、[AVM1 按钮高亮残留](https://github.com/ruffle-rs/ruffle/pull/25045)；均开放 | 低—中 | 改善摇杆鼠标、左键保持和拖拽的正确性；运行代价小 | 02a/02b 已合入，02c 待处理；[范围](validation/upstream-compatibility-20261101.md) |
| 03 | [AS3 根电影最终 progress 事件](https://github.com/ruffle-rs/ruffle/pull/25051)；开放 | 低—中 | 解除一类永久停在加载页的问题；主要发生在加载阶段 | 优先移植 |
| 04 | [AVM1 根时间轴 onLoad 顺序](https://github.com/ruffle-rs/ruffle/pull/25054)；开放 | 低—中 | 修复初始化/声音代码漏执行；主要改变首次加载时序 | 优先移植 |
| 05 | [BitmapData 自复制与混合精度](https://github.com/ruffle-rs/ruffle/pull/24676)；已合并 | 中 | 修复重叠复制画面错误，并优化部分 CPU 复制；需保护本地免回读路径 | 移植并做像素回归 |
| 06 | [Vector 原生调用优化](https://github.com/ruffle-rs/ruffle/pull/24660)；已合并 | 低 | 改动集中、部分操作更便宜；实际游戏收益可能很小 | 低成本验证后决定 |
| 07 | [删除 ScopeChain 缓存](https://github.com/ruffle-rs/ruffle/pull/24193)；已合并 | 中 | 减少冗余缓存与内存，改善作用域语义；不承诺明显提速 | 移植语义测试后评估 |
| 08 | 原生进度恢复与导入导出；参考 [#8470](https://github.com/ruffle-rs/ruffle/issues/8470)、[#22409](https://github.com/ruffle-rs/ruffle/issues/22409) | 中—高 | 直接保护长期游戏进度；大部分成本在低频文件操作和界面 | 按本地功能分小步实施 |
| 09 | [位图缓存抖动](https://github.com/ruffle-rs/ruffle/pull/24992)、[矢量缓存抖动](https://github.com/ruffle-rs/ruffle/pull/25003)、[动画遮罩](https://github.com/ruffle-rs/ruffle/pull/24949)；均开放 | 中—高 | 上游明确涉及死神 2.6 菜单；本地 3.3/3.6 是否受影响需复现 | 先复现，再按依赖移植 |
| 10 | [屏外静态文字剔除](https://github.com/ruffle-rs/ruffle/pull/24944)；已合并 | 低—中 | 有长列表/大量屏外文字时可省绘制；要防止错误隐藏文字 | 有场景收益再保留 |
| 11 | [无效赋值消除 DSE](https://github.com/ruffle-rs/ruffle/pull/25035)；开放 | 中—高 | 有机会降低 AS3 解释执行量；优化阶段更贵、语义验证要求高 | 先补回归，再测真实游戏 |
| 12 | [减少鼠标命中边界计算](https://github.com/ruffle-rs/ruffle/pull/24860)；开放 | 中—高 | 可能降低复杂显示树 CPU 成本；伴随文字尺寸时序和遍历成本 | 采样确认热点后处理 |
| 13 | 帧时间稳定性；参考 [#22758](https://github.com/ruffle-rs/ruffle/issues/22758) | 中—高 | 可能改善平均 FPS 正常但体感不顺；不能解决脚本本身的长帧 | 先分离调度与执行耗时 |
| 14 | [电影库 GC](https://github.com/ruffle-rs/ruffle/pull/23071)、[unloadAndStop](https://github.com/ruffle-rs/ruffle/pull/24918)、[弱引用与 GC](https://github.com/ruffle-rs/ruffle/pull/23897)；均开放 | 高 | 若持续泄漏则收益很大；当前方案有生命周期语义和停顿风险 | 定位后拆分设计，避免整包照搬 |
| 15 | [计时器机制重写](https://github.com/ruffle-rs/ruffle/pull/24915)；开放 | 高 | 对齐到期顺序与错过周期的处理；会改变游戏逻辑时序 | 有不兼容样本时推进 |
| 16 | [设备字体回退](https://github.com/ruffle-rs/ruffle/pull/24728)、[非 TextField 文字事件](https://github.com/ruffle-rs/ruffle/pull/24151)；均开放 | 中—高 | 改善缺字及自定义文字控件；软键盘仍需单独适配 | 按缺字/输入样本分开做 |
| 17 | [BitmapData.draw 支持滤镜](https://github.com/ruffle-rs/ruffle/pull/21672)；开放 | 高 | 修复部分游戏缺失特效；可能增加离屏纹理、绘制和同步成本 | 明确目标游戏后推进 |
| 18 | [sampleData 动态音频](https://github.com/ruffle-rs/ruffle/pull/25055)、[Sound.extract](https://github.com/ruffle-rs/ruffle/pull/25056)；均开放 | 高 | 让依赖自混音的内容发声；引入解码、队列和延迟成本 | 确认无声根因后推进 |
| 19 | [深递归 StackOverflowError](https://github.com/ruffle-rs/ruffle/pull/24973)；开放 | 中—高 | 将部分进程崩溃变成脚本异常；审查指出方法调用开销增加 | 跟踪低开销方案后再评估 |
| 20 | [wgpu 透明像素颜色矩阵](https://github.com/ruffle-rs/ruffle/pull/25047)；开放；[wgpu 批处理](https://github.com/ruffle-rs/ruffle/pull/24947)；已合并 | 前者低—中，后者高 | 在本项目主要影响实验 Vulkan；不改善默认 GLES 路径 | Vulkan 专项，两个改动独立做 |

## 01—05：先修安全与常见兼容问题

### 01 异常 SWF 分配与长度防护

- 处置：已在 [swf/src/read.rs](../player/swf/src/read.rs) 移植 checked subtraction、128 MiB 初始解压容量与 256 项场景/帧标签初始容量，保留正常解码与容错行为；上游四个损坏样本和超过初始容量的有效内容已加入回归。
- 验证与来源统一见[长度防护报告](validation/swf-length-guards-20261101.md)；主机、ARM64、Pocket1 两后端各 9 组解析夹具及两款游戏的限定 GLES 启动/暂停/退出通过。实战、游戏 Vulkan、长时及真实低内存待验；128 MiB 不是解压总量限制，256 项不是条目数量上限，也不构成通用 OOM 保护。

### 02 鼠标释放、拖拽与按钮状态

- **02b 处置：** 已移植；3 项完整鼠标事件顺序回归通过。[来源与验证](validation/upstream-compatibility-20261101.md)。

- **02a 处置：** 已移植；2 项 VM 回归通过；AVM1 goto 专项待验。[来源与验证](validation/upstream-compatibility-20261101.md)。

- 本地入口：[core/src/player.rs](../player/core/src/player.rs) 及 AVM1 按钮实现。按 **02a #23803 → 02b #23807 → 02c #25045** 分开验证；这是建议拆分顺序，不宣称三者存在上游依赖。
- 实施：限制 AVM1 专用对象等价/拖拽事件逻辑的适用范围。保留 AVM1 在 goto 后重建按钮的兼容处理，以及 `trackAsMenu` 的菜单跟踪行为。
- 验收：按下后拖出松开不误点击；移出再移入恢复正确状态，AS3 rollOver/rollOut 不重复；AVM1 普通按钮与菜单按钮分别测试。实机覆盖摇杆鼠标、M+下左键保持、暂停/切方案释放；不要只测桌面鼠标。

### 03 AS3 加载页卡死与完成事件重入

- 本地入口：[player.rs](../player/core/src/player.rs)、[loaderinfo_object.rs](../player/core/src/avm2/object/loaderinfo_object.rs)。复现“首帧 stop，监听 progress 到 100% 才 play”的 SWF。
- 实施：对齐根电影最终 progress 与 init/complete 的顺序，完成标志在可重入事件前处理。保护分块加载及子 Loader 行为。
- 验收：监听器能收到最终进度，事件顺序和次数匹配 Flash；回调内 goto 不递归重复完成事件；本地文件和子 SWF 资源加载都能继续。

### 04 AVM1 根时间轴 onLoad 顺序

- 本地入口：[movie_clip.rs](../player/core/src/display_object/movie_clip.rs) 的 `run_frame_avm1`。
- 实施：区分 Stage 下的 level 首帧与 `loadMovie` 替换内容；不能把所有 load 事件统一延后。
- 验收：根首帧定义的 onLoad 恰好执行一次，初始化与声音正常；对照上游 root_onload、action execution order 和 issue_2870 测试，覆盖 level 加载、loadMovie 与 onEnterFrame 顺序。

### 05 BitmapData 复制与透明混合

- 本地入口：[operations.rs](../player/core/src/bitmap/operations.rs)、[bitmap_data.rs](../player/core/src/bitmap/bitmap_data.rs)。本地仍是逐像素自复制路径，已有不同位图完整覆盖时跳过旧目标回读的修改。
- 实施：移植重叠区域的 Flash 覆盖语义、可用的快速复制和混合精度修正；把本地完整覆盖免回读重新整合到最终路径，不能被上游旧的 `dest.sync()` 覆盖。
- 验收：上下左右重叠、自复制、透明/不透明、mergeAlpha、边界裁剪、局部/整图覆盖和 GPU-dirty 来源/目标都检查真实像素；跑既有 [BitmapCopyProbe](../scripts/resources/BitmapCopyProbe.as) 并扩展缺失场景。GLES/Vulkan 分别验证；复测[死神换回合](validation/bvn33-pve-stutter-20261031.md)不能退回旧目标同步瓶颈。

## 06—12：以真实收益决定是否保留

### 06 Vector 原生调用优化

- 本地入口：[type_aware.rs](../player/core/src/avm2/optimizer/type_aware.rs)。现有优化仅检查 `is_final()`，尚未包含参数化 Vector 的判断。
- 实施与验收：确认本地 Vector 类型约束和 CallNative 接口匹配，测不同元素类型、长度/索引访问、异常和继承限制；再测实际战斗场景。作者未观察到真实 SWF 的明显整体收益，因此把它当低成本增量，不宣传游戏数倍提速；若依赖适配扩大，后移。

### 07 删除 ScopeChain 缓存

- 本地入口：[scope.rs](../player/core/src/avm2/scope.rs)、相关 vtable。当前仍有 `ScopeContainer` 缓存。
- 实施与验收：带入 `scopes_dont_cache` 等语义测试，检查动态属性、with-scope、命名空间和作用域修改后的查找结果；记录启动分配、RSS 和脚本耗时。它首先是正确性与冗余缓存清理，不是无条件性能优化。

### 08 原生进度可恢复性与导入导出

- 类型：本地功能完善，issue 提供需求参考，没有可直接移植的完整方案。入口：[experience.rs](../player/sdl2test-rocknix/src/experience.rs)、[data_manager.rs](../player/sdl2test-rocknix/src/data_manager.rs)、[leaf.rs](../player/sdl2test-rocknix/src/leaf.rs)。
- 顺序：**08a 失败重试/取消及明确状态 → 08b 可恢复的完整备份集合 → 08c 导出 → 08d 导入**。本地已在部分退出/重启路径阻止保存失败后继续，并有有效 `.bak` 回退；先查缺口，避免重做已有逻辑。
- 实施：沿用内容身份和五槽语义，恢复/导入前停止并销毁旧 Player，防止旧实例写回；备份记录身份、版本、清单和校验。多 `.sol` 使用一致集合，先验证并准备完整候选，再提交，失败保留旧有效档。导入还需检查路径和大小边界。
- 验收：缺文件、损坏、满卡/只读、重复操作、版本身份冲突及失败中断均不覆盖旧有效进度；导出再导入能在游戏内继续。原生数据恢复与完整运行现场快照分别表述。详细要求沿用[进度中的 F5.4/F5.6](progress.md)。

### 09 死神菜单、滤镜与动画遮罩抖动

- 复现入口：[#12788](https://github.com/ruffle-rs/ruffle/issues/12788)、[#24957](https://github.com/ruffle-rs/ruffle/issues/24957)。先跑最小夹具，再确认本地死神 3.3/3.6；上游在 2.6 的结果不能自动外推。
- 顺序：**09a #24992 → 09b #25003**，后者明确依赖前者且当前分支包含其改动，不能重复应用。**09c #24949** 处理动画遮罩，单独判断依赖和必要性。
- 验收：固定同帧截图与 Flash 对照，LOW/HIGH、缩放、亚像素移动、遮罩、cacheAsBitmap 开关/恢复都覆盖；同时测临时纹理峰值、重建频率和长帧。画面改善但引发明显内存或重建回退时继续修正，不直接默认启用。

### 10 屏外静态文字剔除

- 本轮补充的低成本候选。入口：[text.rs](../player/core/src/display_object/text.rs)，当前静态文字绘制未做相同的屏外判断。
- 实施与验收：先用长列表证明默认 GLES 存在无效绘制；保持 BitmapData.draw、滤镜及缓存的离屏内容正确。上游描述已列出遮罩、scrollRect/3D 等边界差异，要专门验证。保留文字正确性优先；只有合成列表加速时，将收益限定为该场景。

### 11 AS3 无效赋值消除 DSE

- 进入条件：代表游戏确实受 AS3 执行限制，并且能为优化器构造有意义的回归样本。
- 实施：先检查与本地优化器的依赖差异，覆盖分支、循环、异常边、局部变量类型和可观察副作用，避免删除语义上仍需执行的写入；对照开启/关闭优化的输出和画面。
- 验收：相同游戏、存档、输入和构建配置下，分别记录启动/首次调用与稳定战斗帧时间、内存；至少重复三轮。上游报告多数样本执行指令减少约 2–5%，不等于整机帧时间减少 2–5%；新增优化时间和临时内存也必须计入。

### 12 鼠标命中检测与文字尺寸时序

- 本地入口：[movie_clip.rs](../player/core/src/display_object/movie_clip.rs)、[player.rs](../player/core/src/player.rs)。先采样鼠标移动/悬停时的边界遍历成本。
- 实施：分析 `world_bounds` 的副作用与文字延迟 autosize 更新时点。上游同时增加渲染阶段遍历，不能只交换条件，也不能假定全树遍历总会更便宜。
- 验收：隐藏/裁剪文字、按钮不同状态、文字宽度/位置、Event.RENDER 与 setTimeout(0) 的观察结果对齐；有/无鼠标活动的真实游戏都比较 CPU 和帧时间。无收益或引入更多遍历则暂缓。

## 13—20：有证据或特定内容需求再做

### 13 帧时间稳定性与调度

- 先用现有 `--trace-performance` 分开看执行、解码、渲染、GPU 等待与休眠；参考 [#24884](https://github.com/ruffle-rs/ruffle/issues/24884) 的方法，但不把其私有样本结论直接当成本地根因。
- 若抖动来自调度，再检查截止时间计算、等待精度及呈现节奏；若来自长脚本/同步解码，应修对应热点。保留 SWF 帧率和事件语义，不用强行改成 60 FPS 掩盖问题。
- 验收：30/60 FPS 样本的帧时间分布、输入延迟、音画同步、暂停恢复和功耗/CPU 变化一起检查；平均 FPS 不变也可能有收益，忙等提高 CPU 换来的平滑需明确权衡。

### 14 MovieLibrary、卸载与弱引用生命周期

- 调研入口：[#24864](https://github.com/ruffle-rs/ruffle/issues/24864)。先区分提前 GPU 上传、已卸载电影仍被持有、活跃电影正常缓存、AS3 弱引用未生效，以及游戏自身持有对象。
- 先做可重复加载/卸载夹具和内存增长曲线，再决定修改。已有按需上传/闲置回收覆盖部分 GPU 问题，不能再用上游未修基线估算本地收益；反复进入同一场景与不断加载不同场景分别测。
- [#24918](https://github.com/ruffle-rs/ruffle/pull/24918) 自述释放库后，仍存活对象不能继续实例化其符号；停止声音、流、计时器和 stage 监听器也尚未完成。不能直接把该实现当作完整 unloadAndStop。
- [#23071](https://github.com/ruffle-rs/ruffle/pull/23071) 改动涉及电影库所有权；[#23897](https://github.com/ruffle-rs/ruffle/pull/23897) 涉及弱字典/监听器与 GC，缺新增回归测试。三者有重叠且不应无审查叠加；先核对上游最新所有权演进，再按实际泄漏根因拆开做。
- 验收：多 Loader 共用资源、跨 ApplicationDomain、保留旧实例后继续创建符号、卸载后重载、弱键枚举/失效、声音和事件生命周期都正确；重复几十轮后内存进入合理平台期且无强制 GC 长停顿。达不到时不以“内存下降”单项判通过。

### 15 计时器机制

- 本地入口：[timer.rs](../player/core/src/timer.rs)。这是 Flash VM 计时器，和掌机按键连发调度是两套机制。
- 实施与验收：核对同批到期顺序、过期 interval 不集中补发、回调中新增/删除/重启、更改 delay 和 ID 行为；真实游戏覆盖卡顿后恢复。上游 PR 尚未解决全部帧内执行时点，不能声称完整对齐 Flash，也不能当作通用提速方案。

### 16 字体回退与自定义输入控件

- **16a 缺字：**先复现 [死神中文问题 #17134](https://github.com/ruffle-rs/ruffle/issues/17134)，区分嵌入字体缺字、设备字体回退、字体样式和资源缺失，再评估 #24728；本地 UI 字体正常不代表游戏字体正常。
- **16b 自定义控件：**#24151 只补向聚焦非 TextField 对象发 textInput；本地软键盘还需识别目标和提交机制。防止标准 TextField 重复输入，保留取消、密码遮挡、preventDefault 和暂停语义。
- 中文拼音组词是独立功能，不能因合入上述 PR 就标为完成；[#1778](https://github.com/ruffle-rs/ruffle/issues/1778) 也不能代替本地输入法设计与验收。

### 17 BitmapData.draw 的滤镜语义

- 先确认目标游戏确实把带滤镜对象绘制到 BitmapData，缺失画面由该路径导致；不要将所有滤镜差异归给 #21672。
- 实施与验收：核对核心缓存及本地 GLES/wgpu 离屏接口。低画质、滤镜组合、变换、裁剪与混合的像素正确；现有 scratch/PBO 上限继续生效。记录额外纹理、渲染次数和同步，不能只修桌面 Vulkan。

### 18 动态音频与 Sound.extract

- 顺序：**18a #25055 → 18b #25056**，后者明确基于前者。先确认无声内容实际依赖 sampleData/extract，而非资源路径或解码失败。
- 适配本地音频后端，验证采样格式、队列、位置、完成事件、循环、暂停/恢复及退出；实机听感、延迟、欠载和 CPU/内存必须记录。已有普通音效和背景音乐不可回退；不依赖该 API 的游戏没有直接收益。

### 19 深递归崩溃保护

- [审查意见](https://github.com/ruffle-rs/ruffle/pull/24973#pullrequestreview-5659171189) 指出约 4% 的方法调用开销增加；这不是整机帧率下降 4%，但在 Pocket1 也不能忽略。
- 先核对 ARM64/目标 libc 上剩余栈检测能力和后备深度策略，再决定实现。覆盖不同调用路径、value stack、异常可捕获及正常深调用；在普通游戏测调用成本。优先跟踪减少检测开销的后续方案。

### 20 Vulkan 专项

- **20a #25047：**可独立先做。入口：[color_matrix.wgsl](../player/render/wgpu/shaders/filter/color_matrix.wgsl)，本地仍有 alpha 为零时除法。用透明像素和颜色/alpha 偏移夹具验证真实 Vulkan 像素；上游也声明还有显示列表颜色偏移差异，不能扩大结论为滤镜完全兼容。
- **20b #24947：**批处理是大改造，单独分支评估 API/依赖差异、绘制顺序、遮罩、混合、滤镜和位图句柄所有权；保留本地 BitmapHandle 生命周期修复、DMA-BUF 和队列同步。测真实设备 draw call、CPU、等待和内存后再决定保留。
- 只有用户主要使用 Vulkan 或确认 Vulkan 问题阻断使用时才提前该专项；20a 的小修不需要等待 20b，也不需要整体升级 core/wgpu。

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

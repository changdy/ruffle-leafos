# Pocket1 上游兼容修复验证（2026-11-01）

基线：`815d454`。本轮每个问题独立提交，相关测试使用合并候选执行；结果不代表每个中间提交分别完成完整构建或实机矩阵。版本保持 0.8.3 开发预览，正式应用未替换。

## 核心验证条件

在 Windows 通过锁定离线工具链编译 ARM64 release 核心测试，在 Pocket1 / Leaf 0.13.0 执行，Null 渲染/音频后端运行真实 Player、VM、事件和像素算法。测试 ELF 使用仅测试目录中的 SDK GLIBC 2.39 加载器、libc/libm，未替换系统库。该执行不等同实际摇杆、GPU、听感或长时游戏验收。

核心 ELF SHA-256：`844f8288422e276a2073aa16a56d842b2210a5ff4c5d2f4b308244ca54cfe6a0`。原始记录位于本地忽略目录 `outputs/upstream-fixes-20261101/`，不随仓库提交。

构建：`python scripts/build_player.py test --package ruffle_core --offline --isolated-config`；此命令仅编译。运行测试 ELF 时可分别筛选 `player::`、`avm1_onload_`、`copy_pixels_tests`、`blend_matches_`、`scopes_dont_cache`。测试夹具、参考输出和来源说明随对应源码提交，生成中间 ABC 留在忽略的 build 目录。

## 02a AS3 拖出释放误点击

来源：[上游 #23803](https://github.com/ruffle-rs/ruffle/pull/23803)，实际参考 head `86ff3a3a12c98c349d736e050565080567929cbd`，2026-11-01 核对。上游仍开放，本地适配不表示上游已合并。

结构等价回退只用于 AVM1，动态 AS3 实例即使 depth/id 相同也保持独立。保留 AVM1 goto 重建按钮的路径。

2 项实际 VM 鼠标回归：另一 Sprite 上释放不 click，原处释放仍 click。AVM1 goto 重建按钮专项待验。

## 02b AS3 拖动和释放的重复悬停事件

来源：[上游 #23807](https://github.com/ruffle-rs/ruffle/pull/23807)，实际参考 head `9ce50747410a0983871508e012f47017a301682c`，2026-11-01 核对。上游仍开放，本地适配不表示上游已合并。

按住时的 DragOver/DragOut 仅用于 AVM1；AVM2 复用 roll 路径。本地另将释放后的补发 RollOver 限于 AVM1，避免 roll 层被 LCA 抑制后仍重复 mouseOver；光标更新保留。

3 项实际 VM 回归检查完整 mouseOver/mouseOut/rollOver/rollOut 顺序、拖出移回、普通悬停和移到另一实例后松开。

## 02c AVM1 未按按钮高亮残留

来源：[上游 #25045](https://github.com/ruffle-rs/ruffle/pull/25045)，实际参考 head `e900266e092bd8e0a83b463dc1dd3031e2cefda3`，2026-11-01 核对。上游仍开放，本地适配不表示上游已合并。

未按普通按钮忽略拖入/拖出，trackAsMenu 动态属性覆盖 SWF 跟踪标志；新增 common string 沿用本地旧版 intern 接口。

5 项真实 native 按钮状态回归，覆盖普通按钮、原按钮移出移回、SWF menu 标志及动态 true/false 覆盖。

## 03 AS3 根电影最终 progress 与完成重入

来源：[上游 #25051](https://github.com/ruffle-rs/ruffle/pull/25051)，实际参考 head `0c16e8d4e5e33d4c9003859faa223713f420e198`，2026-11-01 核对。上游仍开放，本地适配不表示上游已合并。

根电影最后一次 progress 延到首帧后、init 与 complete 之间；完成标志在 progress/httpStatus 前设置。先取出长度并释放本地 RefLock 借用，再派发可重入事件。子 Loader 的原进度路径保持。

2 项 Flash 参考顺序回归，覆盖首帧 stop 后监听最终进度及 progress 回调 gotoAndStop(1)，完成事件均仅一次。子 Loader/loadBytes 与分块流矩阵未新增验收。

## 04 AVM1 根时间轴 onLoad 顺序

来源：[上游 #25054](https://github.com/ruffle-rs/ruffle/pull/25054)，实际参考 head `d3c9b13d24c9303e31c7fba3150c4220f93fa6fe`，2026-11-01 核对。上游仍开放，本地适配不表示上游已合并。

仅 Stage 下 level 的首帧在脚本之后排入 Load；loadMovie 替换 clip 保持先 Load 的顺序。

4 项上游 Flash 参考完整 trace 通过：root_onload、swfmill root_onload、action_execution_order_test4 和带外部 child.swf 的 issue_2870。前三项运行 30 帧；加载 level/_level0 替换没有新增 Flash 对照。

## 05a BitmapData 透明混合整数精度

来源：[上游 #24676](https://github.com/ruffle-rs/ruffle/pull/24676)，实际参考 head `a211d2b00fc8bd21d50221d648c65f084edd8784`，2026-11-01 核对。上游已合并。

Color::blend_over 改用 Flash 的 256 基整数混合，作为后续透明自复制的独立修复提交。

1 项核心测试核对 256 组固定 Flash 参考色值（多组颜色及 alpha）；未声称覆盖所有色值组合。

## 05b BitmapData 重叠自复制及行复制

来源：[上游 #24676](https://github.com/ruffle-rs/ruffle/pull/24676)，实际参考 head `a211d2b00fc8bd21d50221d648c65f084edd8784`，2026-11-01 核对。上游已合并。

按 Flash 的方向规则遍历自复制；异向重叠保留 Flash 读取已覆盖像素的行为，非混合路径按行复制。完整目标覆盖仍丢弃旧目标的 GPU 回读，局部/混合/GPU-dirty 来源保持必要同步。

10 项核心回归含 68 组 Flash 重叠/裁剪网格、不同 stride 邻居保留及既有免回读保护。公开 GPU 夹具扩至 18 项；两后端实际结果见本报告后续合并候选记录。

## 06 参数化 Vector 原生调用优化

来源：[上游 #24660](https://github.com/ruffle-rs/ruffle/pull/24660)，实际参考 head `c0efacf6edf604281f33a25b21d6ff63d1186fab`，2026-11-01 核对。上游已合并。

静态调用判断加入参数化 Vector（本地只有 Vector 使用 class.param），保留原参数类型/数量和 fast_call 校验。

1 项优化开/关实际 VM 对照，八类 Vector、29 项 length/fixed/index/增长/截断/pop/异常断言；ASC 拒绝 Vector 子类（1016）。没有 Pocket1 性能基准，不宣称整机提速。

## 07 删除错误的 ScopeChain 查找缓存

来源：[上游 #24193](https://github.com/ruffle-rs/ruffle/pull/24193)，实际参考 head `084e2796b53e87662d1b46feff92b6443b917218`，2026-11-01 核对。上游已合并。

删除 ScopeContainer 和多余 PropertyMap 缓存，改用 Gc<Vec<Scope>> 按词法顺序查找；保留 get_trait 的属性命名空间语义，删除无其他调用者的 get_trait_with_ns。

2 项实际 AVM2 命名空间查找回归。设备基线 outer→inner 错取 outer-value（1 通过/1 失败），修复后均为 inner-value；不宣传未经测量的 RSS 或帧时间改善。


## 合并候选与实机结果

Windows 离线 ARM64 release 构建通过，目标 `generic`、Rust 1.94.0、opt-level 2、无跨 crate LTO。版本/源码指纹实际校验及 15 项发布脚本主机回归通过。
播放器 ELF SHA-256：`bed543d7066dc4dea8dbe81eb100802ad20b5438c75234448ee088708a915132`；实际依赖 GLIBC 最高 2.35。版本未提升、未发布安装包，正式应用未替换。

最终核心测试 ELF 共执行 **30 项针对性回归，全部通过**：鼠标/根进度/Vector 13 项、AVM1 onLoad 4 项、copyPixels 10 项、混合精度 1 项、ScopeChain 2 项。
其中像素算法包含 68 组 Flash 网格及 256 组固定混合色值；若干 GPU-dirty 同步保护用例预期触发 Null renderer 的同步 panic，测试框架将其判为成功，不能把这些日志误报为应用崩溃。
Vector 初始夹具误把固定 Vector 的同长赋值当成成功；对照 [Adobe avmplus 的 Vector 实现](https://github.com/adobe/avmplus/blob/master/core/VectorClass-impl.h)的先 checkFixed 行为改为断言 #1126 后，全部 29 项执行完成，优化开/关输出相同。此修正不改变生产 Vector API。

| Pocket1 / Leaf 0.13.0 场景 | 结果与边界 |
| --- | --- |
| 默认 GLES、实际 Vulkan 的 BitmapCopyProbe | 各 18 项公开 API 像素检查通过；先 draw 形成 GPU-dirty，再核对完整/局部覆盖、混合及八方向重叠复制。实际后端均核验，Vulkan 首帧呈现通过 |
| Vector / GLES | 29 项 PASS 和 DONE、正常定时退出及 progress flush；不证明游戏整体提速 |
| 拳皇 Wing 1.91 / GLES | 28 秒限定启动、标题页短 M 暂停、B 恢复、定时正常退出及 progress flush；未测战斗 |
| 死神 3.3 目录版 / GLES | 首次 28 秒检查在同步初始化期间注入 M，未观察到 pause，保留为失败记录；独立 60 秒重试等待 games_inited 后，更新说明页暂停/恢复、定时正常退出及 progress flush 通过；未测战斗 |

GPU SWF SHA-256：`3162ae88f8d94b4185bd545dea9d7ac6217f36d8d4b9ca5af8ff69c65fc8a787`。
两次游戏暂停/恢复截图已目视核对。死神首轮 `tasks_ms=31145.880`，同日旧候选已有 `tasks_ms=30591.760` 的启动长任务；这仅说明已有同类阻塞，不能视为规范性能对照或通用改善。初始化长任务中的菜单响应仍未解决。

正式应用、全部 Flash ROM、配置与 SharedObject 前后逐文件哈希一致。临时 Apps 入口已移到 Apps 外，备份数据库后刷新并核验 apps/apps_fts 无残留、其他 Apps 记录完全一致；没有手动删除记录。
同一 boot 的内核日志未新增 OOM，无测试播放器残留。详细证据保留于本地 `gpu-summary.json`、`gpu-vector-summary.json`、`gpu-game-summary.json`、`gpu-cleanup.json` 及对应原始日志；数据库备份和用户数据副本不提交。

## 未扩大验收的范围

没有新增长时战斗、全角色、音频听感、真实低内存、完整 M+下保持/暂停/切方案释放、混合 VM、子 Loader/loadBytes/分块流矩阵及 AVM1 goto 重建按钮验收。
没有给出 ScopeChain/Vector 的 RSS 或帧时间提升结论，也没有证明死神换回合性能优于既有版本；原免回读路径由核心计数和两后端像素回归保护，完整性能复测仍待进行。

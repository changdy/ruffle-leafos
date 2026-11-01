# Pocket1 上游兼容修复验证（2026-11-01）

基线：`815d454`。本轮每个问题独立提交，相关测试使用合并候选执行；结果不代表每个中间提交分别完成完整构建或实机矩阵。版本保持 0.8.3 开发预览，正式应用未替换。

## 核心验证条件

在 Windows 通过锁定离线工具链编译 ARM64 release 核心测试，在 Pocket1 / Leaf 0.13.0 执行，Null 渲染/音频后端运行真实 Player、VM、事件和像素算法。测试 ELF 使用仅测试目录中的 SDK GLIBC 2.39 加载器、libc/libm，未替换系统库。该执行不等同实际摇杆、GPU、听感或长时游戏验收。

核心 ELF SHA-256：`102c0e55456edbd73948ed0d2b402cfe6a3ccb8c41e90d37f472cda82b9473b3`。原始记录位于本地忽略目录 `outputs/upstream-fixes-20261101/`，不随仓库提交。

构建：`python scripts/build_player.py test --package ruffle_core --offline --isolated-config`；此命令仅编译。运行测试 ELF 时可分别筛选 `player::`、`avm1_onload_`、`copy_pixels_tests`、`blend_matches_`、`scopes_dont_cache`。测试夹具、参考输出和来源说明随对应源码提交，生成中间 ABC 留在忽略的 build 目录。

## 02a AS3 拖出释放误点击

来源：[上游 #23803](https://github.com/ruffle-rs/ruffle/pull/23803)，实际参考 head `86ff3a3a12c98c349d736e050565080567929cbd`，2026-11-01 核对。上游仍开放，本地适配不表示上游已合并。

结构等价回退只用于 AVM1，动态 AS3 实例即使 depth/id 相同也保持独立。保留 AVM1 goto 重建按钮的路径。

2 项实际 VM 鼠标回归：另一 Sprite 上释放不 click，原处释放仍 click。AVM1 goto 重建按钮专项待验。
## 02b AS3 拖动和释放的重复悬停事件

来源：[上游 #23807](https://github.com/ruffle-rs/ruffle/pull/23807)，实际参考 head `9ce50747410a0983871508e012f47017a301682c`，2026-11-01 核对。上游仍开放，本地适配不表示上游已合并。

按住时的 DragOver/DragOut 仅用于 AVM1；AVM2 复用 roll 路径。本地另将释放后的补发 RollOver 限于 AVM1，避免 roll 层被 LCA 抑制后仍重复 mouseOver；光标更新保留。

3 项实际 VM 回归检查完整 mouseOver/mouseOut/rollOver/rollOut 顺序、拖出移回、普通悬停和移到另一实例后松开。

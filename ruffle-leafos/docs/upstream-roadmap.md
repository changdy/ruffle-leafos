# Pocket1 上游修复与优化计划

以下编号沿用已有任务记录。候选先核对当前源码、前置依赖及目标场景，再决定是否移植。分配、停顿和画质候选优先覆盖默认 GLES。用户失档、接近 OOM 或能稳定复现的兼容阻断可优先处理。已实现功能与最新验证见[工程进度](progress.md)。

## 优先评估

| 次序 | 候选 | 进入条件与验收 |
| --- | --- | --- |
| 1 | 10 屏外静态文字剔除 | 先用默认 GLES 长列表证明屏外绘制造成开销，再核对遮罩、scrollRect、滤镜与 BitmapData.draw 的离屏结果。[#24944](https://github.com/ruffle-rs/ruffle/pull/24944) |
| 2 | 09 缓存抖动与动画遮罩 | 在本地死神 3.3/3.6 复现。位图缓存调整先于矢量调整，动画遮罩单独审查。对照固定帧、画质及缩放，测纹理重建、内存和长帧。[#24992](https://github.com/ruffle-rs/ruffle/pull/24992) · [#25003](https://github.com/ruffle-rs/ruffle/pull/25003) · [#24949](https://github.com/ruffle-rs/ruffle/pull/24949) |
| 3 | 22b 孤立对象工作集合 / Loader 空闲分支 | 先采样构建、帧脚本和清理。工作集合需要完整失效链，验收动态增删、addFrameScript、嵌套 goto、SimpleButton、Loader 和 GC。[fork 849f3aa](https://github.com/aweigit/ruffle-miyooflip/commit/849f3aab9edd32aa0370add3d9aee4ee8c0836e8) · [fork 55ed89a](https://github.com/aweigit/ruffle-miyooflip/commit/55ed89ab64154e85769260f49c8ab2fa302e18f3) |
| 4 | 12 鼠标命中边界 | 取得移动和悬停的 bounds 热点后，评估额外遍历，验证延迟 autosize、文字尺寸、按钮状态及事件时序。[#24860](https://github.com/ruffle-rs/ruffle/pull/24860) |
| 5 | 11 AS3 无效赋值消除 | 取得实际脚本瓶颈后评估。覆盖分支、异常与可见副作用，比较首次优化和稳定阶段的耗时及内存。[#25035](https://github.com/ruffle-rs/ruffle/pull/25035) |

08 原生进度按保存失败处理、可恢复备份、导出、导入推进。恢复前停止旧 Player，校验身份、版本、清单及文件完整性，完整候选准备成功后再替换。重点覆盖多对象一致性、故障中断和重复操作。[现有拒写验证](validation/reliability-fixes-20261101.md) · [待验范围](progress.md#待完成与验收)

## 按复现推进

| 编号 | 方向与条件 |
| --- | --- |
| 13 帧时间 | 分开采样脚本、解码、回收、保存、GPU 等待与调度。确认长帧来源后修改对应路径。[采样说明](rendering.md#按需性能诊断) |
| 14 电影库与卸载 | 用重复加载/卸载及内存增长曲线区分未释放电影、仍存活对象和正常缓存。验证多 Loader、跨域、存活引用继续实例化、重载及声音/事件生命周期。[电影库 GC #23071](https://github.com/ruffle-rs/ruffle/pull/23071) · [unloadAndStop #24918](https://github.com/ruffle-rs/ruffle/pull/24918) · [弱引用 #23897](https://github.com/ruffle-rs/ruffle/pull/23897) |
| 15 VM 计时器 | 复现计时不兼容后，核对同批到期顺序、长帧后 interval、回调中修改计时器及实际游戏时序。[#24915](https://github.com/ruffle-rs/ruffle/pull/24915) |
| 16 字体与非 TextField 输入 | 区分游戏字体缺字、字体回退和资源缺失。自绘控件需补焦点与提交识别，覆盖标准 TextField、取消及密码。中文组词单独设计。[字体 #24728](https://github.com/ruffle-rs/ruffle/pull/24728) · [文本事件 #24151](https://github.com/ruffle-rs/ruffle/pull/24151) |
| 17 BitmapData.draw 滤镜 | 先复现目标游戏实际缺失的效果，再对照两后端的离屏像素、额外纹理与同步。[#21672](https://github.com/ruffle-rs/ruffle/pull/21672) |
| 18 动态音频与 Sound.extract | 确认问题源自 API 缺失后，先验证动态音频，再验证提取。记录采样格式、队列、延迟、欠载与普通 BGM 回归。[#25055](https://github.com/ruffle-rs/ruffle/pull/25055) · [#25056](https://github.com/ruffle-rs/ruffle/pull/25056) |
| 19 深递归保护 | 有进程崩溃样本后评估 ARM64 栈检测、异常可捕获性，以及正常方法调用成本。[#24973](https://github.com/ruffle-rs/ruffle/pull/24973) |
| 20 Vulkan | 透明像素颜色矩阵可独立评估。批处理需覆盖绘制顺序、遮罩、滤镜、BitmapHandle 所有权、DMA-BUF 同步及实机性能。[颜色矩阵 #25047](https://github.com/ruffle-rs/ruffle/pull/25047) · [批处理 #24947](https://github.com/ruffle-rs/ruffle/pull/24947) |

## 复测方法

- 记录本地构建、候选来源、游戏与存档指纹。正确性改动用 Flash 参考事件序列或像素判断，性能改动用可重复的实机场景与明确热点。
- 对照保持 Leaf、驱动、后端、分辨率、画质、构建参数、存档副本与输入一致。区分启动、首次资源使用及稳定游玩，同一场景交替重复至少三轮。
- 比较帧时间 p50/p95/p99、超过 33.333/50 ms 的次数、CPU、RSS 峰值和最低 MemAvailable。有条件时记录 GPU 资源和同步等待。通用结论应覆盖拳皇、死神和轻量 AVM1 样本，单场景收益按场景报告。
- 分别记录主机执行、ARM64 编译、设备夹具、实际对战和人工听感。源码变更后按构建规则更新受影响的锁与输入指纹。[构建与验证命令](build-release.md#按变更选择验证)

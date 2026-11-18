# 字体分配与孤立对象清理优化

日期：2026-11-04。适用：Ruffle Flash 0.8.6 开发预览、官方 Ruffle v0.7.1 核心、Pocket1 / leafOS。修改前代码为 `b37d861`；本轮只完成代码、主机验证和 ARM64 构建，未连接设备、安装或发布。

## 范围与来源

1. **计划 21，内嵌字体按需解码。**参考 [ruffle-miyooflip 849f3aa](https://github.com/aweigit/ruffle-miyooflip/commit/849f3aab9edd32aa0370add3d9aee4ee8c0836e8)，适配官方稳定版拆分后的 `font.rs` / `font/glyph.rs`，保留原字符映射、指标、版本缩放、kerning 和 ASCII 预热。DefineFont2/3 先读取元数据及有界轮廓切片，首次绘制/命中测试时解码、缓存；不规则偏移和不足两字节的轮廓回退原 reader。后者与旧 fork 的宽松短轮廓处理不同，避免改变当前稳定版的既有兼容路径。
2. **计划 22a，未改变的共享孤立对象列表免复制。**参考 [e696060](https://github.com/aweigit/ruffle-miyooflip/commit/e696060424055a0d67498c3f3d60cde34a37df4e) 中独立的 cleanup 小改动。外层迭代仍持有 Rc 快照且所有条目都应保留时，省去整表写时复制；有删除工作继续原 retain，无共享者保持原单次扫描。每次重新检查弱引用、parent 和放置标记，没有引入跨帧/GC 缓存或强制 GC。

`849f3aa` 的完整分阶段工作集合、时间轴摘要及 [55ed89a](https://github.com/aweigit/ruffle-miyooflip/commit/55ed89ab64154e85769260f49c8ab2fa302e18f3) 的 Loader 空闲分支缓存均未移植。执行顺序和进入条件统一维护在[优化计划](../upstream-roadmap.md)，不将 22a 当作完整生命周期缓存完成。

## 主机回归

| 检查 | 结果与范围 |
| --- | --- |
| `python scripts/test_core.py` | **177 项单元 + 2 项 AVM1 集成通过**；实际 Player/VM、Null 后端。新增 3 项字体和 2 项清理回归，其余兼容/存储/缓存测试继续通过 |
| `python scripts/test_swf.py` | **89 项单元 + 8 项长度/分配集成通过**，沿用 Windows Job Object 的 512 MiB/180 秒进程树限制；新增 3 项字体读取回归 |
| 随包字体完整对照 | 扫描 **225 个游戏/资源 SWF**；36 个文件包含 **178 份 DefineFont2/3、3,273 个字形、136,191 个轮廓记录**，延迟路径展开全部轮廓后与原 reader 的完整 Font 结构逐项相等；本语料没有触发 eager 回退。另核对 3 个现有 DefineFont2/3 夹具 |
| 来源与构建输入 | 更新 8 个改动/新增源码文件指纹；上游原始 SHA 从已核验的官方 `89a7049` Git blob 读取；版本校验通过，最终 ELF 的源码输入快照与工作区一致 |
| `python -m unittest discover -s scripts -p test_release.py -v` | 17 项发布/工具下载主机回归通过；仅测试模拟，不执行真实发布。受影响文档的本地文件链接检查通过 |

新增回归包括：字体 2/3 的缩放和指标、重复 Unicode 码点、非法 Unicode 替代字符、ASCII 预热、非 ASCII 首次命中/绘制缓存、原 movie 释放后的字节所有权、损坏轮廓失败缓存且不读入邻字形；实际 Player preload 注册及不规则偏移回退；窄/宽偏移与字符码、空字体、kerning 和越界拒绝；共享列表不复制、需要删除时保留外层快照、GC 后弱条目清理。

## 字体解析阶段测量

`player/swf/examples/font_decode.rs` 是独立主机诊断例程，可接收 SWF 文件或目录；先做全部轮廓/元数据对照，再比较 eager 与 deferred 元数据读取。使用与主机回归一致的 Windows GNU 离线工具环境编译 release；每文件每轮重复 5 次，共运行 3 轮。累计分配请求由该诊断程序自己的 allocator 包装记录，不进入播放器。

| 225 个随包 SWF 内字体的合计 | 原 eager | deferred 元数据 |
| --- | --- | --- |
| 累计申请字节，每轮相同 | 10,138,640 B（9.67 MiB） | 329,544 B（0.31 MiB），约减少 96.7% |
| 字体解析耗时，三轮 | 8.286 / 9.052 / 9.030 ms | 0.138 / 0.145 / 0.150 ms |

这些计时是各文件重复解析均值的合计，包含解析和销毁临时字体结构；不包含 SWF I/O/解压、Player 初始化、ASCII 预热、GPU 注册或稍后的首次轮廓使用。分配数据是累计请求量，包含 realloc 请求，**不是存活堆、峰值内存、RSS 或设备可用内存**；不能将解析阶段降幅用于整机启动、FPS 或内存承诺。首次文字使用会补做解码，全部字形都用到时最终仍会展开全部轮廓。

在相同主机隔离环境中，可从工程根以 `cargo run --locked --offline --release --target x86_64-pc-windows-gnu --manifest-path player/Cargo.toml -p swf --example font_decode -- games` 重做游戏语料对照；工具缓存和 linker 配置按[构建环境](../build-release.md#工具链与首次准备)准备。本轮实际驱动和三轮原始 TSV/汇总位于本地 `work/font-optimization-20261104/`、`outputs/font-optimization-20261104/`，不进入 Git。

22a 的主机证据是原 Rc 身份保持，证明在共享且无删除时没有复制列表；本轮未测真实游戏的嵌套 goto 频率或帧时间收益。

## ARM64 构建与实机待验

`python scripts/build_player.py build --offline --isolated-config` 的 **ARM64 release 构建通过**。保持 generic、opt-level 2、LTO=false、16 codegen units；所需最高 GLIBC 仍为 2.38。构建工具没有把 Cargo.toml 中的 LTO=true 当作实际最终参数。

- 当前 `build/ruffle-leaf` SHA-256：`6ba0db5e74516db062a8668e8bbd130eb243d10104ec0107d61bbad1d7a5c4e2`。
- 修改前本地播放器 SHA-256：`8e9e67855f3ea621f060e1378a79e85f4780d33f7624271502d7e7e131404b25`；其 ELF 和构建记录已保留到本地输出目录，供后续相同场景对照。
- 没有执行 ARM64 测试 ELF、GPU 像素或实际游戏验收；没有生成新安装包或改变正式设备入口。

本轮用户要求先优化代码，ADB 留到后续提供。设备可用后优先验证：GLES 下拳皇、死神 3.3/3.6 和 Bloxorz 的启动/首次文字、菜单和存档；固定存档/角色/地图重复三轮的启动、首文字帧、帧时间和 RSS；动态加载字体/Loader 与嵌套 goto；再做实验 Vulkan 的字体像素及正常退出回归。临时入口、用户数据保护和清理遵循仓库约定。

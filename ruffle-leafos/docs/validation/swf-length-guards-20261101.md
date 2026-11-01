# 异常 SWF 长度与预分配防护

日期：2026-11-01。对应[上游实施计划 01](../upstream-roadmap.md#01-异常-swf-分配与长度防护)，本地基线 `7b7c9ed`，修复提交 `df512d1`，版本保持 0.8.3 开发预览。处置：**已移植，主机、ARM64 构建及限定 Pocket1 实机验证通过**。

## 来源与修改范围

- [Ruffle #23962](https://github.com/ruffle-rs/ruffle/pull/23962)：采用 `d7f684ac927f28a405250b348ff3d88d05ea2b79` 的解析修复；上游合并提交 `ca6737c6e1ed11e304a324227e11bd0f8bb253e5`。
- [Ruffle #23960](https://github.com/ruffle-rs/ruffle/pull/23960)：采用场景/帧标签初始容量修复及损坏样本；上游最终 head `cb8180b123ea2b594fb99a248c3878b8169a63b3`，合并提交 `91b61d405b13320842167171b8030b34a286db1e`。
- 两项于当日通过 GitHub API 核对为已合并；仅改动 `player/swf/src/read.rs` 的相关解析路径，无需升级依赖。保留本地渲染、缓存、BitmapData 免回读、事件顺序与存档行为。

LZMA 声明文件长度减 8 改为 `checked_sub`，不足 8 返回 `InvalidData`。通用解压输出和 LZMA 中间缓冲最多初始预分配 128 MiB；场景/帧标签最多初始预分配 256 项，随后仍可随有效内容增长。长度不匹配流沿用原来的容错；上游巨大 LZMA 样本返回解码错误，巨大场景/标签样本在截断内容处返回 tag 86 解析错误。

这是初始容量保护，不是解压总量、LZMA 字典或进程内存上限。真实大型内容、压缩炸弹、其他分配或低内存设备仍可能 OOM；本次没有增加常驻轮询或整树扫描，也未测量或宣称游戏提速。

## 固定损坏样本

样本保存在 `player/swf/tests/swfs/`，沿用 Ruffle 的 MIT/Apache-2.0 双许可。两份 LZMA 样本来自 #23962 上述 head 的同名路径；场景与帧标签样本分别来自 #23960 合并提交中的 `tests/tests/swfs/timeline/scene_count_oom/test.swf` 和 `frame_label_count_oom/test.swf`，只调整本地文件名，字节保持。

| 本地文件 | 字节数 | SHA-256 |
| --- | ---: | --- |
| `lzma-malformed-length.swf` | 66 | `08bab711b6172583ff9303f080c96076d2f439679e98c400ff4828e8fbaf575c` |
| `lzma-length-too-large.swf` | 66 | `db24b0db5185594b0b9da3ed301d3640ae8efa963f7cdd1e3f52066481059b14` |
| `scene-count-too-large.swf` | 117 | `ae1d3e016f2f0c3b8ad20a17705b475a3e5f1fc92d8299acb04128241d01a419` |
| `frame-label-count-too-large.swf` | 109 | `4f2ed13f29240dcfec064d16069ca6cdeed7d93046d490abba82cece2f0ff928` |

解析器、新增集成测试及四个样本的 SHA-256 纳入 `leaf-source.lock.json`。

## 主机实际执行

工程目录中运行：

```powershell
python scripts/test_swf.py
```

使用锁定 Rust 1.94.0、Windows GNU、离线依赖与隔离 Cargo 配置，先编译，再实际执行 86 项既有单元测试和 7 项新增集成测试，全部通过。每个测试进程树使用 Windows Job Object 限制 512 MiB 提交内存；分配成功后才通过输入门启动测试，测试串行，每个二进制超时 180 秒。该限额仅用于主机测试，不作为设备内存预算。

新增回归覆盖：

- LZMA 声明长度 0—7 和上游短长度样本返回 `InvalidData`，巨大长度样本可控报错。
- 把正常 FWS/CWS 的声明长度改为 `u32::MAX` 后，实际解码字节与解析标签保持，输出容量不超过初始上限。
- 构造实际正文超过 128 MiB 的 FWS 流，验证可以继续增长、读完正文并保留原标签；不提交大体积样本。
- 上游场景/帧标签巨大声明数量在内容不足处报错；有效 257 个场景及 257 个标签完整读取，不被 256 的初始容量截断。
- 两款随包主入口均存在，递归解压并解析随包游戏及资源的全部 **93 个 SWF**。使用当前 `games/manifest.json` 固定内容，主文件指纹仍为拳皇 `bb829289b03fff84aa981bcfed436a9e9ccc4100d018526a2fdeed3fd115ca19`、死神 `294e5e3c60e24fe01475c1fbb4dfc1f9aad1e23e4fbd62dd9849308f2fb020c0`。

正常 ZWS 解压仍由既有 `read_swfs` 与写回测试覆盖。游戏主机解析不执行 AVM 脚本，也不验证画面、声音、真实加载流程或游戏手感。

源码指纹核验及 15 项发布脚本回归也通过。原始 SWF 测试结果：本地忽略目录 `build/swf-test-report.json`。

## ARM64 与实机边界

```powershell
python scripts/build_player.py check --offline --isolated-config
python scripts/build_player.py build --offline --isolated-config
```

ARM64 check 与 generic release 构建通过，沿用既有告警；未修改版本或打包发布。候选 ELF 为本地 `build/ruffle-leaf`，SHA-256 `7a6b60259912ca070fb214551bbec83b2a897c329cff87f0b09d90dc5db41ce6`，AArch64 PIE，GLIBC 最高需求 2.35。Rust 1.94.0，release opt-level 2、无跨 crate LTO、16 个 codegen units；原始参数见 `player-build.json`，构建日志为 `build/swf-arm64-build.log`。

## Pocket1 隔离实机验证

Pocket1 / RK3566 / Mali-G52，Linux 5.10.209、AArch64；设备 `.umrk/mlp1/release.json` 确认 Leaf **0.13.0**。在独立调试目录运行上述 `7a6b6025…` ELF，传输后核对 SHA-256；使用正式包的库、字体及 Leaf Wayland/输入链路。每个会话独立 HOME、XDG 目录、进度和按键，通过最小清单的临时工具入口启动，不带正式包 `provides` 声明。

### 两后端解析夹具

GLES 与 Vulkan **各 9 组**，合计 18 组，全部通过。每组显式指定后端，日志确认 `leaf-graphics actual=opengl-es/vulkan`；Vulkan 不是失败后回退的 GLES。夹具关闭音频，正常/容错场景由 5 秒诊断计时正常退出；外层 35 秒超时仅作后备，本轮没有靠超时终止才算通过的场景。

| 每个后端的样本 | 数量 | 实际结果 |
| --- | ---: | --- |
| 正常 FWS、CWS、ZWS | 3 | 帧循环运行，退出码 0，`progress_flushed=true` |
| LZMA 短长度、巨大长度 | 2 | 分别返回 `Malformed LZMA length` 和 LZMA 解码错误；退出码 1，无分配崩溃或进程挂起 |
| 巨大场景、帧标签数量 | 2 | 按核心既有容错继续，脚本输出 `Start`，正常退出并提交进度；不把主机整电影解析的 tag 86 错误等同于播放器必须拒载 |
| 正常 FWS/CWS 改声明长度为 `u32::MAX` | 2 | 长度不匹配告警后继续原有内容，正常退出并提交进度 |

实机还原了同一组固定损坏样本与正常样本；超过 128 MiB 的实际正文和 257 项有效场景/标签只在主机验证，未为此向设备传入大文件。夹具未覆盖所有损坏格式、LZMA 字典或真实低内存故障矩阵。

### 随包游戏的限定 GLES 检查

两款设备主入口的 SHA-256 与前述主机/manifest 指纹一致。使用显式 GLES、独立新进度与配置、正常音频初始化；通过物理 evdev → Leaf 虚拟手柄 → SDL 注入输入并采集实机画面：

- **死神 VS 火影 3.3 目录版**：外部资源初始化、更新说明、主模式菜单与操作教学画面可见；暂停、B 恢复、再次暂停后选择“保存并退出”通过。
- **拳皇 Wing 1.91 单文件**：首屏更新说明可见，用摇杆鼠标及左键确认后进入开场动画；暂停、B 恢复、菜单“保存并退出”通过。初次停留在更新说明的会话也保留原始记录，不将键盘 J 输入当成鼠标确认成功。

两款游戏的测试退出码均为 0，日志均含 `resume_first_dt_us=0` 和 `progress_flushed=true`。这些会话验证启动、限定画面、输入通路及暂停/退出，不包含实战、真实听感、全角色、长时、原档续玩或两款游戏的 Vulkan 验收；本次不计算 FPS、内存或加载时间改善比例。

### 数据与入口收尾

验证前完整备份现有配置与 SharedObject，并拉回主机核验归档 SHA-256。测试后逐文件复核：**803 个正式应用文件、499 个 Flash 游戏文件、39 个原配置文件、42 个原 SharedObject 文件**的集合与 SHA-256 均保持；正式应用未替换。测试新增数据保留在独立调试目录。

临时 `RuffleSwfLengthDebug.pak` 已归档到 `Apps/` 之外；清理前备份 Leaf SQLite 数据库并拉回核对，刷新列表后核验 `apps` 与 `apps_fts` 无测试入口，其他 Apps 记录与测试前相同。结束时无测试播放器进程；同次开机前后内核日志核对未新增 OOM，不能扩大为任意低内存内容都安全。

原始日志、逐文件哈希、数据库/用户数据备份、夹具结果和截图在本地忽略目录 `outputs/swf-length-guards-20261101/`；操作脚本与传输材料在 `work/swf-length-guards-20261101/`。这是该 ELF 的隔离验证，没有新增正式安装或 GitHub Release 附件验收。

# Apps 同名 Flash 管理入口排查与清理

日期：2026-10-28。设备：Miniloong Pocket1 / LeafOS 0.13.0；正式安装清单版本 0.7.0。本轮只处理调试入口及其索引，没有更新播放器或系统程序。

## 原因

应用数据库中有三个名称均为“Flash 数据管理”的记录：

| `pak_dir` | 排查时状态 |
| --- | --- |
| `Apps/mlp1/RuffleProbe.pak` | 正式数据管理入口，保留 |
| `Apps/mlp1/RuffleFightingTest.pak` | 既往格斗游戏测试入口，目录已归档，数据库仍有记录 |
| `Apps/mlp1/RuffleCpuBenchmark.pak` | CPU 对照测试入口，仍在 Apps 中，沿用正式应用名称 |

另有 `RuffleArcuzTest.pak` 留在 Apps 中，同样复制了正式清单，但尚未被索引。这些测试包的启动脚本指向隔离的游戏测试目录，并不是正式数据管理器。复制的清单还包含不适用于测试包的 `provides`；清理前内容目录诊断记录 CPU 测试包的 `missing-file`。

重复名称来自调试入口清理遗漏。当前 Leaf 配置的第二存储源 `/media/sdcard1` 离线，正常刷新还会保留旧应用行。实机扫描日志明确记录：

```text
ROM discovery: retaining unavailable source secondary_sd (/media/sdcard1)
App discovery: retaining rows because source secondary_sd is unavailable
```

首次刷新完成、generation 从 22 增至 23 后，三条 Flash 记录仍在。它不是同一个正式包被播放器反复注册；残留测试包和副卡离线时的索引保留行为共同造成了本次现象。本轮没有修改 Leaf 的存储扫描策略。

## 处理与核验

- 在设备调试目录 `debug-tests/20261028-apps-cleanup/library-before.db` 保存 SQLite 一致性备份。
- 将 CPU 和 Arcuz 两个测试入口分别移入各自调试目录的 `completed-test.pak`，归档前后四个文件的 SHA-256 一致。已有的格斗游戏测试归档保留。
- 确认两个已入库测试目录均不存在后，在单个 SQLite 事务中按完整 `pak_dir` 只删除这两条失效记录；保留原有 FTS 删除触发器，其他应用行保持不变。
- 再次通过 Leaf 的 `scan-library` 刷新，generation 增至 24，`scan_error` 为空，扫描及排队状态均为空闲。Apps 表及全文搜索各只返回一个“Flash 数据管理”，均指向正式 `RuffleProbe.pak`，原 id 15 保持。
- 正式 `RuffleProbe.pak`、原始 `ruffle.pak` 的全部普通文件、用户配置和 SharedObject 进度前后逐文件 SHA-256 一致；正在运行的正式管理器 PID 和命令行保持，没有关闭或重启会话。
- 内容目录不再保留先前的测试包诊断文件。其余存储源的应用记录未清理。

完整清单、扫描应答、备份位置及前后对照保存在忽略的本地 `outputs/apps-cleanup-20261028/`。本轮没有操纵当前管理器去拍摄 Apps 列表；验证依据是实际应用数据库、全文搜索、设备扫描状态与文件校验。没有播放器代码变更，因此没有重新构建或扩大游戏功能验收范围。

长期约定已补入仓库根 `AGENTS.md`：测试入口使用明确的调试名称、最小清单；结束后归档到 Apps 外，并实际核验列表及搜索索引，不能只凭目录移走判断清理完成。

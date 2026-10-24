# 工作区索引

当前代码 **0.6.2**，文档更新于 2026-10-24。相对路径基于工作区根目录。

| 位置 | 用途 |
| --- | --- |
| [README.md](../../README.md) | 工作区入口 |
| [ruffle-leafos/README.md](../README.md) | 项目结构和构建 |
| [ruffle-leafos/docs/README.md](README.md) | 当前文档、需求、TODO 与历史导航 |
| [player/sdl2test-rocknix/src/](../player/sdl2test-rocknix/src/) | 本项目 Rust 前端，主要修改入口 |
| [player/](../player/) | 完整 Ruffle 源码；本机保留原上游 Git 数据，工作区仓库跟踪当前源码文件 |
| [pak/](../pak/)、[scripts/](../scripts/) | 安装清单、启动、构建、测试及打包工具 |
| [outputs/ruffle-0.6.2/](../../outputs/ruffle-0.6.2/) | 当前发布的安装包、源码快照、校验和与真机证据 |
| [outputs/](../../outputs/) | 历史版本交付和原始需求副本 |
| [work/](../../work/) | 各批次基线、构建/设备验证过程与工作区审计 |

工作区文档持续维护；版本化 ZIP 和发布验证 JSON 是发布时快照。本次文档整理未重新打包或改动已验证安装包，源码 ZIP 中的旧文档不代表工作区最新文档。

Git 管理范围和新克隆所需的本地依赖见[工作区入口](../../README.md#git-管理范围)。`outputs/`、`work/` 和工具缓存保留在本机，不纳入源码仓库。

离线开发继续使用 `work/toolchains/`、工程 `.tools/` 与 `build/cargo/` 的当前缓存；原始 runtime、历史源码基线和各版交付保留。2026-10-24 的缓存清理范围及核对记录见 [清理记录](workspace-cleanup-20261024.md)。

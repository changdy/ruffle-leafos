# Ruffle LeafOS

面向 Miniloong Pocket 1 的 Flash 播放器，当前版本 **0.6.3 开发预览**，已验证环境为 Leaf 0.13.0 / ARM64 / Mali-G52 / GLES 3.2。

已实现每游戏 10 个按键方案、5 份 Flash 原生进度、音画设置、Apps 数据管理，以及所有游戏共用的空心圆加单像素中心点光标。按键编辑直接进入宫格，M+X 可在本次运行互换十字/摇杆方向；模板流程已移除。真实游戏体验、断电与休眠等验收仍在 [TODO](docs/todo.md)。

## 文档

| 阅读目的 | 文档 |
| --- | --- |
| 安装和使用 | [0.6.3 使用说明](docs/preview-0.6.3.md) |
| 查看最终效果、代码位置和验证证据 | [实现结果](docs/implementation-report.md) |
| 继续开发 | [当前需求](docs/requirements.md)、[TODO](docs/todo.md) |
| 查数据格式和历史版本 | [文档目录](docs/README.md)、[版本摘要](docs/progress.md) |
| 查源码、交付和缓存位置 | [工作区索引](docs/workspace-index.md) |

## 代码结构

| 目录 | 用途 |
| --- | --- |
| `player/sdl2test-rocknix/src/` | 掌机前端：输入、按键编辑、光标、菜单、原生进度与音画设置 |
| `player/core/` | 固定 Ruffle 基线及少量核心接口改动 |
| `pak/` | Leaf 清单、游戏/Apps 启动脚本、主题与诊断 |
| `scripts/` | 构建、主机测试、打包及开发工具 |
| `adapter/` | 早期适配工程与复用的诊断库；其 README 对应历史版本 |
| `runtime/leaf-12/`、`target-libs/` | 原始运行时及构建所需目标库、字体 |
| `vendor/` | 固定 Leaf 契约和 Jawaka 菜单参考源码 |
| `.tools/`、`build/` | 工程 SDK 缓存、生成的编译包装器与构建产物 |

源码来源及指纹见 [leaf-source.lock.json](leaf-source.lock.json)。本工程基于公开提交 `3bc39c5e0fdbc9fcde92681706395f6ef70f991b`，原始 `leaf-12` 作为对照保留。

## 构建与验证

在本工程根目录执行：

```powershell
python scripts/build_player.py build --offline --isolated-config
python scripts/test_session.py
python scripts/build_diagnostics.py
python scripts/package_preview.py --output ../outputs/ruffle-0.6.3-rebuild --include-source
```

使用独立输出目录可保留已发布的安装包及其验证记录。主机测试覆盖前端状态逻辑；真机验证范围见 [实现结果](docs/implementation-report.md)。

依赖 Git、Python、Node.js、JDK 17、Rust 1.94.0（Windows GNU 主机及 `aarch64-unknown-linux-gnu` 标准库）、w64devkit 2.1.0、Zig 0.14.1、ARM GNU 14.2 SDK。版本与下载校验见 [Rust 工具锁](adapter/rust-toolchain.lock.json)、[工具来源锁](adapter/references.lock.json)和 [ARM SDK 锁](arm-gnu.lock.json)。

本工作区默认复用 `../work/toolchains/` 的 Rust、w64devkit 和 Cargo 缓存。迁移到其他机器时，在工程根目录配置 `workspace.local.json`（不打入发布包）：

```json
{
  "host_tool_cache": "D:/tools/ruffle-host",
  "rust": "D:/tools/rust-1.94.0",
  "host_gcc": "D:/tools/w64devkit",
  "zig": "D:/tools/zig-0.14.1/zig.exe",
  "jdk": "D:/tools/jdk-17"
}
```

Cargo 缓存位于 `host_tool_cache/cargo-home/`。准备主机工具后运行 `python scripts/setup_arm_gnu.py`，下载并校验 SDK 到 `.tools/`。首次准备 Cargo 依赖时去掉 `--offline`；`--isolated-config` 使用工程配置隔离祖先目录的镜像设置。

本机受管 Windows 上 GNU 编译器启动曾报 OS623，当前构建使用 Zig 的 Clang/LLD 配合 SDK 头文件和库，包装器生成在 `build/`。全新机器构建演练保留为待办。

安装目录为 `/mnt/sdcard/Apps/mlp1/RuffleProbe.pak/`。原始 `ruffle.pak` 及各版交付保留；安装、数据路径和回滚注意事项见 [使用说明](docs/preview-0.6.3.md)。

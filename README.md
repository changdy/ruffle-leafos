# Ruffle Flash

面向迷你龙 Miniloong Pocket 1 / LeafOS 的本地 Flash 播放器，当前为 **0.8.0 开发预览**。

GitHub 仓库：[changdy/ruffle-leafos](https://github.com/changdy/ruffle-leafos)。掌机应用名为 **Ruffle Flash**，Apps 入口显示为 **Flash 数据管理**。

| 阅读目的 | 主文档 |
| --- | --- |
| 了解项目、安装和使用 | [项目 README](ruffle-leafos/README.md) |
| 查看完成情况、待办、版本和验证范围 | [工程进度](ruffle-leafos/docs/progress.md) |
| 了解架构、数据、源码、构建和发布 | [工程实现细节](ruffle-leafos/docs/implementation.md) |

工程位于 `ruffle-leafos/`。[格式契约与验证摘要](ruffle-leafos/docs/README.md)作为补充资料保留。

安装包从 [GitHub Releases](https://github.com/changdy/ruffle-leafos/releases) 下载 `ruffle-leafos-<版本>.zip`，解压后将 `Apps/`、`Roms/`、`Images/` 按原层级复制到迷你龙系统 SD 卡根目录即可安装。`main` 分支安装清单的版本号变更会自动编译并发布，Release 说明包含构建提交、commit 列表和 ZIP 的 SHA-256；维护方法见[自动发布](ruffle-leafos/docs/implementation.md#github-actions-自动发布)。

当前打包流程会将仓库收录的《拳皇 Wing 1.91》和《死神 VS 火影 3.3》两款 **-定制版** 一起放入 `Roms/flash/`，死神包含完整 `assets/`。游戏入口、来源及验证范围见[随包游戏说明](ruffle-leafos/games/README.md)；此前已发布的 ZIP 保持原内容。

`outputs/` 为本地交付与真机证据，`work/` 为工具缓存及工作记录，`adb-tools/`、`flash-transfer/` 为本地设备工具与传输文件；这些目录均不进入 Git。全新克隆的构建准备见[构建说明](ruffle-leafos/docs/implementation.md#构建环境与命令)。

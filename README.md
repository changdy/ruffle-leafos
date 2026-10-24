# Ruffle LeafOS 工作区

面向 Miniloong Pocket 1 的 Flash 播放器，当前为 **0.6.3 开发预览**。文档已按当前代码整理，更新于 2026-10-24。

工作区已有 **0.6.4 待真机验收候选**：逐键连发、系统音量反馈及 Menu 输入修正；71 项主机测试和 ARM64 构建通过，设备连接恢复后继续验证。见 [本轮实施记录](ruffle-leafos/docs/input-refresh-20261024.md)。

- [项目说明与构建](ruffle-leafos/README.md)
- [使用说明](ruffle-leafos/docs/preview-0.6.3.md)：安装、操作、按键映射、原生进度和数据管理。
- [实现效果](ruffle-leafos/docs/implementation-report.md)：功能结果、代码位置、真机截图和验证范围。
- [TODO](ruffle-leafos/docs/todo.md)：已完成事项及剩余工作。
- [文档目录](ruffle-leafos/docs/README.md)：当前需求、格式说明及历史记录。

当前交付位于 [outputs/ruffle-0.6.3/](outputs/ruffle-0.6.3/)，含安装包、源码包、校验和与真机证据。版本化交付保留发布时的文档快照；本轮五项实现、逐项提交和设备验证见项目实施记录。

## Git 管理范围

仓库位于工作区根目录，纳入 `ruffle-leafos/` 中的代码、工程配置、测试、文档、依赖锁文件、vendor 参考源码，以及构建和打包所需的 `runtime/leaf-12/`、`target-libs/`。`player/` 和 `vendor/` 的源码以普通文件快照保存；本机已有的内嵌 Git 数据保留，不作为 submodule 提交。源码来源见 [leaf-source.lock.json](ruffle-leafos/leaf-source.lock.json) 和适配工程内的来源锁文件。

工具链、构建缓存、历史工作副本、交付包和设备传输清单位于 `.gitignore` 排除的目录中，继续保留在本机。新克隆需按[构建说明](ruffle-leafos/README.md#构建与验证)准备工具和 Cargo 依赖；`outputs/` 中的历史发布及验证证据需另外保存或获取。

`.gitattributes` 保留源码原始字节，避免自动换行转换使锁定的 SHA-256 失效，并为 Shell 脚本指定 LF。首次提交使用 `main` 分支，暂不配置 remote。

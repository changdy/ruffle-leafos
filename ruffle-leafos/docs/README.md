# Ruffle Flash 文档索引

| 阅读目的 | 文档 |
| --- | --- |
| 安装、添加游戏与掌机操作 | [中文使用指南](../README.md) · [English user guide](../README.en.md) |
| 当前版本、已验证范围和待办 | [工程进度](progress.md) |
| 启动链路、源码入口和数据路径 | [工程实现细节](implementation.md) |
| 工具链、主机回归、打包和发布 | [构建与发布](build-release.md) |
| GLES/Vulkan、内存预算和性能日志 | [渲染与诊断](rendering.md) |
| 兼容性修复与性能优化候选 | [上游修复与优化计划](upstream-roadmap.md) |
| 按键、画面、语言和数据目录字段 | [格式契约](formats/README.md) |
| 编译、主机测试与 Pocket1 实机证据 | [验证报告](validation/README.md) |
| 游戏内容与来源 | [试玩与可选合集](../games/README.md) |
| 第三方许可与实机配图 | [第三方声明](THIRD_PARTY.md) · [截图来源](images/README.md) |

维护时将使用行为写进中英文指南，将状态和待办写进工程进度，将字段与兼容规则写进对应格式文档。验证记录应标明构建身份、场景和未覆盖范围。旧发布说明、需求快照和重复报告可用 `git log -- <路径>` 与 `git show <提交>:<路径>` 查阅，原始日志与构建产物保存在本地工作目录。安装包附带使用指南和验证摘要，源码链接以仓库检出目录为准。

# 文档索引

当前版本：**0.7.1 开发预览**，文档整理于 2026-10-31。

| 主文档 | 职责 |
| --- | --- |
| [项目 README](../README.md) | 介绍、安装和完整使用说明 |
| [工程进度](progress.md) | 已完成、待办、版本、验收及证据范围 |
| [工程实现细节](implementation.md) | 架构、源码入口、数据、构建、发布与仓库边界 |

补充资料：[当前映射格式 v5](formats/controls-format-v5.md)（包含旧版兼容链接）、[音画与渲染设置 RAV1/RAV2](formats/graphics-settings.md)、[界面语言](formats/language-settings.md)、[验证摘要](validation/)、[第三方声明](THIRD_PARTY.md)、[历史归档](archive/README.md)。完整发布包、截图和原始日志在工作区本地 `outputs/`，不进入 Git。

资源缓存的当前实现、性能对照与验收边界见[资源缓存实机报告](validation/resource-cache-20261030.md)。

维护时，使用行为只更新项目 README，进度/待办与证据范围只更新 progress，实现与工程命令只更新 implementation。字段级契约维护在 formats；发布后实施记录和需求快照进入 archive。历史文档不作为当前需求或待办入口。

拳皇 GLES 内存分解、滤镜临时池修复与限定验证见[本轮报告](validation/kof-gles-memory-20261031.md)。

# 文档索引

当前版本：**0.8.3 开发预览**，整理于 2026-10-31。

| 主文档 | 职责 |
| --- | --- |
| [项目首页](https://github.com/changdy/ruffle-leafos)、[English](https://github.com/changdy/ruffle-leafos/blob/main/README.en.md) | 游戏效果、主要功能、快速安装与下载入口 |
| [中文使用指南](../README.md)、[English user guide](../README.en.md) | 安装、添加游戏、具体操作与常见问题；随安装包附带 |
| [工程进度](progress.md) | 当前结果、待办、版本与验证证据入口 |
| [工程实现细节](implementation.md) | 架构、源码入口、数据、构建、发布与仓库边界 |

| 补充资料 | 内容 |
| --- | --- |
| [按键与光标](formats/controls-format-v5.md) | 当前 v5 全字段、v1—v4 兼容表及全局光标 |
| [画面 RAV1/RAV2](formats/graphics-settings.md)、[语言](formats/language-settings.md)、[数据管理目录](formats/data-manager-catalog.md) | 字段级契约 |
| [验证报告](validation/) | 各轮场景、二进制身份、原始数据位置和验收边界；当前结论由工程进度汇总 |
| [稳定功能验证摘要](validation/stable-features.md) | 输入、进度、正式入口、语言、图标及旧版基础验收 |
| [游戏截图验证](validation/screenshots-20261031.md) | 纯游戏画面、封面/照片、流式编码内存、低内存拒绝/恢复和两后端实机检查 |
| [死神人机卡顿验证](validation/bvn33-pve-stutter-20261031.md) | 换回合 BitmapData 完整覆盖、旧目标读回消除及实际像素检查 |
| [0.6.9 后逐提交审查](validation/review-after-0.6.9-20261031.md) | 39 个提交的代码合理性与历史问题闭环 |
| [第三方声明](THIRD_PARTY.md) | 来源与许可 |
| [截图来源](images/README.md) | 首页与指南使用的实机图片、拍摄日期及版本范围 |

使用行为维护在 README，状态/待办维护在 progress，实现/工程命令维护在 implementation，字段契约维护在 formats。验证报告只保留当前仍有独立价值的内容，稳定功能的旧验收合并为一份摘要，不再逐版保存使用说明或需求副本。完整发布包、截图和原始日志在本地 `outputs/`，不进入 Git。

删除的旧文档可从 Git 历史读取，例如在仓库根运行 `git show 95dbdf3:ruffle-leafos/docs/archive/requirements-v2.7.md`。历史设计不覆盖当前 README、进度与格式契约。

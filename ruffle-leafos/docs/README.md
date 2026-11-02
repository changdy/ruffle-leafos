# 文档索引

当前版本：**0.8.4 开发预览**，索引整理于 2026-11-02。共享核心和官方渲染基线为 Ruffle v0.7.1；上游版本与应用版本分开记录。

0.8.4 修复随包目录资源误列为游戏、补齐亮度提示并收录死神 3.6；已完成主机验证与重建，尚未实机安装。[本轮证据与升级说明](validation/game-list-brightness-20261102.md)。此前稳定版迁移保留 Leaf 功能与数据身份。当前 422e 已正式安装并逐项核验 1,597 文件，通过两款游戏的 GLES 实际对战、暂停恢复/保存退出及 Apps 主菜单复核；719a 阶段另有数据子列表验证。148 项前端、9 项 noop、核心 172+2 主机/设备执行、四组图形及三组保护/拒写 QA 的范围分别记账。用户数据五处试玩变化已备份后复原，原 39 配置和 42 存档等哈希全等，GBA 351 文件两次全哈希均与原清单相等。Vulkan 重型游戏仍未完成六分钟压力，Manual 实验已撤回；测试归档、临时入口及 Apps/搜索缓存清理、安装/用户数据终验和临时租约到期核对均完成。

| 主文档 | 职责 |
| --- | --- |
| [项目首页](https://github.com/changdy/ruffle-leafos)、[English](https://github.com/changdy/ruffle-leafos/blob/main/README.en.md) | 游戏效果、主要功能、快速安装与下载入口 |
| [中文使用指南](../README.md)、[English user guide](../README.en.md) | 安装、添加游戏、具体操作与常见问题；随安装包附带 |
| [工程进度](progress.md) | 当前结果、待办、版本与验证证据入口 |
| [工程实现细节](implementation.md) | 架构、源码入口、数据、构建、发布与仓库边界 |

| 补充资料 | 内容 |
| --- | --- |
| [官方稳定版升级](validation/ruffle-stable-20261102.md) | v0.7.1 来源、保留矩阵和初版身份；后续 148 项前端、9 项 noop、422e 构建与池/低内存保护边界 |
| [本轮 Pocket1 实机与重新安装](validation/stable-device-20261102.md) | 当前 422e 正式安装、双游戏 GLES 实际对战/Apps 主菜单、数据复原；真实 OOM、两次阈值中止、内置保护/拒写 QA 及最终完整性核验 |
| [上游修复与优化实施顺序](upstream-roadmap.md) | 面向 Pocket1，按移植难度、运行代价和实际收益排序的候选、前置条件与逐项验收方法；不代表已经完成 |
| [异常 SWF 长度与预分配防护](validation/swf-length-guards-20261101.md) | 上游计划 01 的来源与损坏样本、受限内存主机回归、ARM64 构建、两后端各 9 组实机解析夹具及限定 GLES 游戏启动/暂停/退出 |
| [上游 02—07 兼容修复](validation/upstream-compatibility-20261101.md) | 独立问题提交、30 项核心设备回归、两后端各 18 项像素检查、限定游戏启动/暂停恢复及未验范围 |
| [输入、保存提示与构建绑定](validation/reliability-fixes-20261101.md) | 有效轴归中、保存错误保留/重试、构建输入与 ELF 绑定，以及主机/两后端隔离故障验证 |
| [2026-11-01 修复版正式安装](validation/formal-install-20261101.md) | 当次 Pocket1 安装身份、旧应用/数据备份、配置与存档保留、Flash 和 Apps 基本验证；当前升级安装状态见本轮实机报告 |
| [按键与光标](formats/controls-format-v5.md) | 当前 v5 全字段、v1—v4 兼容表及全局光标 |
| [画面 RAV1/RAV2](formats/graphics-settings.md)、[语言](formats/language-settings.md)、[数据管理目录](formats/data-manager-catalog.md) | 字段级契约 |
| [验证报告](validation/) | 各轮场景、二进制身份、原始数据位置和验收边界；当前结论由工程进度汇总 |
| [稳定功能验证摘要](validation/stable-features.md) | 输入、进度、正式入口、语言、图标及旧版基础验收 |
| [游戏截图验证](validation/screenshots-20261031.md) | 纯游戏画面、封面/照片、流式编码内存、低内存拒绝/恢复和两后端实机检查 |
| [死神人机卡顿验证](validation/bvn33-pve-stutter-20261031.md) | 换回合 BitmapData 完整覆盖、旧目标读回消除、实际像素检查与正式安装/版本清理 |
| [死神 3.6 定制目录版](validation/bvn36-custom-20261031.md) | 3.3/3.6 文件对比、AIR/TLF 离线适配、素材/特效校验、限定实机战斗与设置重开 |
| [0.8.4 游戏列表与亮度修复](validation/game-list-brightness-20261102.md) | 私有资源目录、亮度百分比、3.6 正式收录及主机/打包验证边界 |
| [ZIP 与目录实机对照](validation/zip-vs-directory-20261102.md) | ZIP 首次解压、缓存复用、首帧/初始化及同场景 FPS 的限定样本 |
| [0.6.9 后逐提交审查](validation/review-after-0.6.9-20261031.md) | 39 个提交的代码合理性与历史问题闭环 |
| [第三方声明](THIRD_PARTY.md) | 来源与许可 |
| [截图来源](images/README.md) | 首页与指南使用的实机图片、拍摄日期及版本范围 |

使用行为维护在 README，状态/待办维护在 progress，实现/工程命令维护在 implementation，字段契约维护在 formats。验证报告只保留当前仍有独立价值的内容，稳定功能的旧验收合并为一份摘要，不再逐版保存使用说明或需求副本。完整发布包、截图和原始日志在本地 `outputs/`，不进入 Git。

删除的旧文档可从 Git 历史读取，例如在仓库根运行 `git show 95dbdf3:ruffle-leafos/docs/archive/requirements-v2.7.md`。历史设计不覆盖当前 README、进度与格式契约。

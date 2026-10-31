# 工程进度

当前版本：**0.7.2 开发预览**。状态整理：2026-10-31。操作见[项目 README](../README.md)，源码与工程命令见[实现细节](implementation.md)。本页保留当前结果、缺口和证据入口；各轮完整测试条件、安装指纹及数据保留记录由验证报告维护。

## 已完成

| 功能 | 当前结果 | 验证边界与证据 |
| --- | --- | --- |
| Leaf 接入与菜单 | Flash 分类启动、Apps 数据管理、暂停/恢复、正常退出、系统主题与音量 | Pocket1 / Leaf 0.13.0；[0.6.8 正式 Release 验证](validation/release-device-0.6.8-20261025.json)及下列后续安装报告 |
| 按键与鼠标 | 每游戏十方案、空方案跳过、逐键 5/10/15 Hz 连发、方向互换、左键保持、全局光标 | [空方案](validation/controls-empty-schemes-20261025.json)、[连发时序](validation/device-validation-0.6.8.json)；不保证阻塞负载下硬实时响应 |
| 原生进度 | SharedObject 五槽、复制、重开选档、有效备份回退；按内容识别 | 完整 VM 快照未实现；[0.6.0 摘要](archive/iteration-0.6.0.md) |
| 中英文界面 | 首次读取系统语言，保存后全局共用；英语环境可切回中文 | [语言实机报告](validation/language-ui-20261028.md)；不翻译游戏和用户命名 |
| 全局画面设置 | 缩放、Flash 画质、FPS、渲染偏好共用；旧每游戏文件不读取/迁移 | [RAV 契约](formats/graphics-settings.md)、[FPS 验证](validation/fps-default-20261026.json) |
| 软键盘 | 标准可编辑 TextField、Unicode 草稿、密码遮挡、差分提交及暂停中立门 | AVM1/AVM2、GLES/Vulkan 限定夹具通过；[实机报告](validation/soft-keyboard-20261028.md) |
| 图标与说明 | Apps/Flash 分类自定义图标、独立快捷键说明、关于和社区致谢 | [图标](validation/app-icons-20261028.md)、[关于](validation/about-refresh-20261025.json) |
| 构建与打包 | 锁定依赖、ARM64 构建、版本触发发布、两款定制游戏校验与安装路径 | [发布验证](validation/release-0.6.9-20261025.json)、[游戏收录](validation/bundled-games-20261028.md)；全新机器演练未完成 |

模板管理、全局按键自动继承、旧 M+左右、游戏音量/静音、编辑页仅本次应用及恢复默认已移除。旧格式兼容范围见[按键契约](formats/controls-format-v5.md)，历史里程碑与需求见[归档](archive/README.md)。

## 当前渲染、性能与数据管理

- **Vulkan 仍为实验选项**：通过 GBM/DMA-BUF 共享图像，由 GLES 最终显示和绘制菜单；初始化失败回退，捕获的运行期错误尝试提交进度后退出。固定场景和后端切换已验证，未证明普遍快于 GLES；广泛滤镜/混合/BitmapData/Stage3D 与长时矩阵未完成。[实现与实机](validation/vulkan-backend-0.7.0-20261025.json)、[位图所有权修复](validation/renderer-cache-fix-20261027.md)。
- **静态资源和解码**：按需创建形状，弱引用活动队列回收，GLES 闲置缓存默认 16 MiB / 1,024 项，CPU 像素缓存默认 8 MiB；128/160 MiB 可用内存迟滞控制退让，后台预解码默认关闭。这些预算不限制总 RSS/GPU 内存。[缓存报告](validation/resource-cache-20261030.md)。
- **拳皇 GLES 内存**：滤镜临时纹理闲置池限制为 16 MiB / 32 项，读回 PBO 为 8 MiB / 16 项；固定 Glow 夹具画面对照一致。修复后同进程两局到胜利页并正常退出，RSS 峰约 683.24 MiB、最低可用约 132.52 MiB，未新增 OOM；全角色与长时仍待验证。[内存报告](validation/kof-gles-memory-20261031.md)。
- **Flash 数据管理**：实际存档/按键及备份、损坏数据可管理；七行翻页、层级返回、失效标记、确认默认取消、归档后重扫。130 项主机回归、通用及 A55/ThinLTO release 构建和 13 组隔离实机检查通过；42 个归档文件哈希多重集合一致。正式安装 863 文件核验及 Apps 通路通过，原配置/存档哈希保持，临时入口及索引已清理。[数据管理报告](validation/data-manager-20261031.md)。
- **性能结论**：显示树、解压容量提示和 A55/ThinLTO 均保留原事件时序，但短样本不证明通用提速或消除长帧；Flash 逻辑与同步解码仍主要串行。默认构建保持 generic，Pocket1 tuning 为显式选项。[显示树](validation/bvn33-traversal-20261027.md)、[编译评估及安装](validation/pocket1-build-20261028.md)、[线程与解压](validation/bvn33-threads-20261029.md)。

上述结果来自各报告的既有验证。本次代码审查不新增实机验收，也不将某一候选二进制的验证扩大为所有同版本构建或 GitHub 附件均已验收。

## 定制游戏与历史实验

当前打包收录《拳皇 Wing 1.91》和《死神 VS 火影 3.3》两款 `-定制版`，来源与逐文件指纹见 [games/](../games/README.md)。早期“离线版/脚本版/素材版”是当轮实验名称，其未分发说明不代表当前安装包内容。游戏的内容身份、旧目录与进度迁移方式以当前 README 为准。

**已提交死神主 SWF 的 LOW 默认仍被入口覆盖为 MEDIUM**；玩家可手动选择 LOW。`f9f210e` 的初始预期已由 `56e9c4c` 的实测更正，入口修复与保存重开不计为已完成。[默认值历史](validation/bvn33-default-low-20261028.md)、[更正与 CPU 评估](validation/bvn33-feature-profile-20261028.md)。

| 历史分析/实验 | 保留证据 |
| --- | --- |
| 格斗游戏资源依赖、早期 OOM 与拳皇性能 | [资源排查](validation/fighting-games-20261026.json)、[性能](validation/kof-performance-20261026.md)、[SWF 素材优化](validation/kof-swf-optimization-20261026.md) |
| 死神 3.3 离线资源、核心回收与脚本优化 | [离线适配](validation/bvn33-offline-20261026.md)、[核心采样](validation/bvn33-performance-20261027.md)、[脚本分析](validation/bvn33-swf-logic-analysis-20261027.md)、[补丁](validation/bvn33-swf-patch-20261027.md)、[实机对照](validation/bvn33-swf-device-20261027.md) |
| 死神版本对比与 AVM2 素材优化 | [2.6/3.3 对比](validation/bvn26-vs33-20261028.md)、[素材报告](validation/bvn33-assets-20261028.md) |
| Apps 临时入口及索引残留 | [清理记录](validation/apps-duplicates-20261028.md)；长期约定见根 AGENTS.md |

## 版本与发布边界

| 版本 | 变化与证据 |
| --- | --- |
| 0.6.9 | 关于文案与发布同步；线上 ZIP 下载/指纹核对通过，独立实机安装未验证。[记录](validation/release-0.6.9-20261025.json) |
| 0.7.0 | 实验性 Vulkan 与后端切换；Pocket1 正式入口/安装验证见[报告](validation/vulkan-backend-0.7.0-20261025.json) |
| 0.7.1 | 中英文界面与版本同步；语言验证见[报告](validation/language-ui-20261028.md) |
| 0.7.2 | 清单、Cargo、来源锁及启动日志同步；后续改动继续处于开发预览，未记录该 Release 附件的独立实机安装验收 |

## 待完成与验收

- [ ] 实际游戏手感：十方案切换、摇杆阈值、长名称和真实拖拽；真实音频听感、画质/缩放差异和代表游戏性能矩阵。
- [ ] F3.2：死区、响应曲线、滞回的可调设置；四角点击、黑边、缩放/旋转坐标矩阵。现有固定阈值不替代可调功能。
- [ ] F5：设备重启/断电、旧 `.sol` 迁移与广泛真实游戏进度；五槽及有效 `.bak` 已实现，完整 VM 快照未实现。
- [ ] F5.4：手动/自动历史备份、轮换及恢复管理；单份 `.bak` 和清理归档不等同完整历史备份。
- [ ] F5.6：进度本地导入/导出、身份及校验检查。
- [ ] 临时鼠标层及其他快捷层。
- [ ] 软键盘的广泛游戏兼容性（含标准 TextField 与自绘输入）、中文组词及完整外设/休眠矩阵。
- [ ] F2/F5：多 SWF/多对象共享与隔离、相对资源/FlashVars、缺资源/网络超时、矢量/遮罩/滤镜/混合/BitmapData/Stage3D、长时音视频同步。
- [ ] 失焦/断连、长按 M 逃生、休眠恢复、长时运行与故障注入的完整实机矩阵。
- [ ] 双卡、迁移、升级/回滚/卸载保留、多 Leaf 版本兼容及全新机器构建演练；本地打包的构建时源码摘要与 ELF 绑定加固，见本次审查报告。
- [ ] F6.4：满卡、只读、拔卡、短写/IO 失败、OOM、驱动/显示/音频流丢失、脚本超时的提示与恢复，并证明已提交有效数据保持。
- [ ] F2/F6：记录启动/菜单响应、帧时间、CPU、RSS/GPU 内存及可观测温度/功耗；代表游戏连续 2 小时、100 次菜单/方案切换、20 次可用休眠恢复。
- [ ] F7.4（P1）：USB/蓝牙手柄及键鼠、热插拔、多人、HDMI、ZIP 与游戏身份；Apps 默认设置/日志/版本/显示检测扩展。
- [ ] F7.5（P2）：完整运行现场快照、倒带、视频硬解，以及有必要时的替代图形后端预研。

原始目标还包括菜单响应 ≤200 ms、导航 ≤100 ms、小型基准启动 ≤5 s，尚未全部验收，也不承诺所有 SWF 达到 60 FPS。需求细节见[需求 v2.7](archive/requirements-v2.7.md)及[后续快照](archive/README.md#需求与设计快照)。`supports_savestate=false`；编译、主机测试、自动实机事件与人工游戏/听感分别记录。

## 本次逐提交审查

审查 `v0.6.9..6b57580` 的 39 个提交，逐项结论、历史缺陷的后续修复、当前风险及本轮主机检查见[审查报告](validation/review-after-0.6.9-20261031.md)。本次文档整理压缩已稳定功能的重复实施过程，保留上述未完成项与验证边界。

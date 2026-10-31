# README 截图来源 / Screenshot sources

首页与中英文使用指南使用真实的 Miniloong Pocket 1 / LeafOS 抓屏。新增图片从本地既有实机记录逐字节复制，未重绘、拼接或替换界面文字；本次文档改版未连接设备拍摄，也不新增实机验证结论。

The project pages and user guides use real Pocket1 device captures. Newly included images are byte-for-byte copies of existing captures, with no compositing or relabeling. This documentation update did not capture new device images or extend hardware validation.

## 日期与版本范围 / Dates and scope

| 图片 / Images | 日期与范围 / Date and scope |
| --- | --- |
| `game-kof.png` | 2026-10-26，拳皇素材优化实机样本；[限定验证](../validation/kof-swf-optimization-20261026.md) |
| `game-bvn.png` | 2026-10-28，死神 LOW 入口修复隔离样本；[限定验证](../validation/bvn33-low-fix-20261028.md) |
| `controls.png` | 2026-10-25，沿用原 README 的 0.6.8 按键截图；[基础记录](../validation/stable-features.md) |
| `pause-menu-en.png`、`controls-en.png`、`progress-slots-en.png`、`mouse-settings-en.png`、`display-settings-en.png` | 2026-10-28，英语界面候选构建；[语言验证摘要](../validation/stable-features.md#语言字体与图标) |
| `keyboard-zh.png`、`keyboard-en.png` | 2026-10-28，0.7.1 软键盘本地构建，使用标准文本输入测试内容；[验证报告](../validation/soft-keyboard-20261028.md) |
| `data-manager-zh.png`、`data-manager-en.png` | 2026-10-31，数据管理重整构建；[验证报告](../validation/data-manager-20261031.md) |
| `pause-menu-zh.png`、`screenshot-zh.png`、`screenshot-en.png` | 2026-10-31，0.8.2 截图功能隔离构建，背景为截图测试内容；[验证报告](../validation/screenshots-20261031.md) |

截图包含不同开发预览、语言和 Leaf 主题。旧截图可能使用“鼠标光标 / Mouse & cursor”“Renderer”“Choose game save”等名称；当前操作和菜单名称以[中文指南](../../README.md)与[English guide](../../README.en.md)为准。英文暂停菜单拍摄于截图功能加入之前。游戏画面展示不代表全角色、全流程、所有后端或长时间游玩已验证。

Captures span several development previews and themes. Some labels have since changed, and the English pause-menu image predates the Screenshot entry. Follow the current guides for menu names. Gameplay images do not establish full-game or long-session compatibility.

## 原始文件 / Original files

本地 `outputs/` 不进入 Git；下面记录原始相对路径，便于在有完整工作区时复核。README 使用的副本保存在本目录并随仓库和安装包分发。

Original paths below are local evidence locations, excluded from Git. The copies in this directory are included with the repository and installation package.

| 仓库图片 / Repository image | 原始本地文件 / Local source | 像素 / Pixels |
| --- | --- | --- |
| `game-kof.png` | `outputs/kof-swf-optimization-20261026/optimized-c2-after-sample.png` | 960 × 720 |
| `game-bvn.png` | `outputs/bvn33-low-fix-20261028/fresh2-battle.png` | 960 × 720 |
| `pause-menu-zh.png` | `outputs/screenshot-20261031/home-zh.png` | 960 × 720 |
| `pause-menu-en.png` | `outputs/language-device-20261028/english-home.png` | 960 × 720 |
| `controls-en.png` | `outputs/language-device-20261028/english-controls.png` | 960 × 720 |
| `progress-slots-en.png` | `outputs/language-device-20261028/english-saves.png` | 960 × 720 |
| `mouse-settings-en.png` | `outputs/language-device-20261028/english-cursor.png` | 960 × 720 |
| `display-settings-en.png` | `outputs/language-device-20261028/english-display.png` | 960 × 720 |
| `keyboard-zh.png` | `outputs/soft-keyboard-20261028/installed-game-keyboard.png` | 960 × 720 |
| `keyboard-en.png` | `outputs/soft-keyboard-20261028/avm2-gles-en/keyboard-01.png` | 960 × 720 |
| `data-manager-zh.png` | `outputs/data-manager-20261031/formal-home.png` | 960 × 720 |
| `data-manager-en.png` | `outputs/data-manager-20261031/en-home.png` | 960 × 720 |
| `screenshot-zh.png` | `outputs/screenshot-20261031/menu-zh.png` | 960 × 720 |
| `screenshot-en.png` | `outputs/screenshot-20261031/menu-en-vulkan.png` | 960 × 720 |
| `controls.png` | 沿用原 README 已收录图片；本地原始抓屏路径未单独记录 | 960 × 720 |

旧版 `gameplay.png`、`pause-menu.png`、`about.png` 本次未改动，也未在新首页与指南中展示。

# 当前实现结果

版本：**0.6.5 开发预览**。文档核对日期：2026-10-24。按当前源码、清单及已有发布验证记录整理；原 0.2.0 审计保存在 [历史交付](../../outputs/ruffle-0.2.0/implementation-report.md)。

## 已实现效果与代码

源码路径均相对工程；点击进入对应文件。

| 能力 | 当前效果 | 代码入口 |
| --- | --- | --- |
| Leaf 接入 | Flash 分类选 SWF 启动；Apps 进入数据管理；最低 Leaf 0.13.0 | [pak.json](../pak/pak.json)、[游戏启动](../pak/emulators/ruffle-probe/launch-game.sh)、[Apps 启动](../pak/launch.sh) |
| 暂停菜单 | 跟随 Leaf 生效主题及字体，叶片形选中条；暂停冻结 tick 与音频；恢复首帧时间重置 | [pause_menu.rs](../player/sdl2test-rocknix/src/pause_menu.rs)、[theme.sh](../pak/emulators/ruffle-probe/theme.sh)、[leaf.rs](../player/sdl2test-rocknix/src/leaf.rs) |
| 按键编辑 | “按键设置”直接打开五列宫格，L/R 切换十组方案；实体键及摇杆动作、九类动作 Tab、逐键 Y 连发；独立保存/返回操作区，未保存切换或退出可选择继续编辑、舍弃、保存 | [controls.rs](../player/sdl2test-rocknix/src/controls.rs) |
| 游戏快捷键 | M+A/B 循环切换映射，M+十字下切换左键保持，M+X 本次方向互换；切换释放旧输入并等待中立 | [shortcuts.rs](../player/sdl2test-rocknix/src/shortcuts.rs)、[session.rs](../player/sdl2test-rocknix/src/session.rs) |
| 光标 | 所有游戏共用一份独立全局外观，映射 v5 不含光标字段；默认空心圆＋固定 1×1 中心点；四样式/七色/四尺寸，3.5 秒静止隐藏、移动恢复；旧 arrow 兼容读取 | [cursor.rs](../player/sdl2test-rocknix/src/cursor.rs)、[controls.rs](../player/sdl2test-rocknix/src/controls.rs) |
| 原生进度 | 每游戏 5 份 SharedObject 进度与北京时间；启动先选档、复制、保存后重启读另槽；内容指纹身份、校验、原子替换及有效备份回退 | [experience.rs](../player/sdl2test-rocknix/src/experience.rs)、[leaf.rs](../player/sdl2test-rocknix/src/leaf.rs) |
| 画面与音量 | 保持比例/铺满/原始大小、低/中/高画质及实际 Flash FPS；音量统一由系统按键调节，显示真实百分比 | [experience.rs](../player/sdl2test-rocknix/src/experience.rs)、[pause_menu.rs](../player/sdl2test-rocknix/src/pause_menu.rs)、[帧计数接口](../player/core/src/player.rs) |
| 数据管理 | 所选游戏/全部原生进度清理、按键重置；默认取消、确认后移入本地归档；Apps 不运行演示游戏 | [experience.rs](../player/sdl2test-rocknix/src/experience.rs)、[leaf.rs](../player/sdl2test-rocknix/src/leaf.rs) |
| 构建与发布 | 固定公开源码、依赖与工具；ARM64 播放器、主机状态测试、清单/ELF/ZIP/指纹检查及源码打包 | [来源锁](../leaf-source.lock.json)、[构建脚本](../scripts/build_player.py)、[打包脚本](../scripts/package_preview.py) |

操作和数据位置见 [使用说明](preview-0.6.5.md)，映射契约见 [格式 v5](controls-format-v5.md)。

## 真机效果

本轮截图覆盖 0.6.5 真机隔离交互与正式 Leaf 入口；具体范围见[本轮验证](menu-refresh-20261024.md)。

![逐键连发宫格](../../outputs/ruffle-0.6.5/screenshots/turbo-editor.png)

![未保存时切换方案](../../outputs/ruffle-0.6.5/screenshots/unsaved-switch.png)

## 已有验证证据

本轮完成 75 项主机测试、ARM64 构建、方案切换与未保存处理、保存重开、十方案循环和物理音量验证；正式游戏/Apps、正常退出、用户配置保留及 593 文件安装校验通过。历史记录只代表对应版本。

| 基线 | 已记录结果 | 证据 |
| --- | --- | --- |
| 0.6.5 | 75 项主机测试；菜单风格统一、合并按键设置、未保存保护、保存重开及十方案循环 | [验证摘要](device-validation-0.6.5.json)、[实施记录](menu-refresh-20261024.md) |
| 0.6.4 | 71 项主机测试；A 连发 10 次/秒、B 普通按键、重开保留；32 次 Menu 恢复；真实音量与暂停提示；593 文件校验 | [验证摘要](device-validation-0.6.4.json)、[实施记录](input-refresh-20261024.md) |
| 0.6.3 | 61 项主机测试；直接按键宫格、十方案、全局光标重开/跨 SWF、物理 M+X 互换与恢复、593 文件安装校验 | [验证摘要](device-validation-0.6.3.json)、[逐项实施](controls-refresh-20261024.md)、[正式通路](../../outputs/ruffle-0.6.3/formal-validation.json) |
| 0.6.0 | 57 项主机测试；AVM1 五槽/重开、AVM2 重开、SWF 中文改名、首帧前选档、第五槽复制、音画持久化和四类隔离清理 | [device-validation.json](../../outputs/ruffle-0.6.0/device-validation.json) |
| 0.6.1 | 58 项主机测试；菜单与紧凑提示截图，M+A/B、十槽循环、旧 M+左右停用及正式游戏/Apps 通路 | [device-validation.json](../../outputs/ruffle-0.6.1/device-validation.json)、[formal-validation.json](../../outputs/ruffle-0.6.1/formal-validation.json) |
| 0.6.2 | 61 项主机测试；旧 arrow 读取原文件不变，按键/模板/隐藏回归，正式 Leaf 四档中心点均为 1×1，593 个安装文件校验 | [主机日志](../../outputs/ruffle-0.6.2/device-evidence/host-tests.log)、[设备记录](../../outputs/ruffle-0.6.2/device-validation.json)、[正式通路](../../outputs/ruffle-0.6.2/formal-validation.json)、[安装记录](../../outputs/ruffle-0.6.2/installation.json) |

目标环境为 Miniloong Pocket 1、Leaf 0.13.0、Mali-G52/GLES 3.2、glibc 2.38。当前播放器和安装包校验值见 [SHA256SUMS.txt](../../outputs/ruffle-0.6.5/SHA256SUMS.txt)。本轮结果见 [实施记录](menu-refresh-20261024.md)，前轮输入结果见 [0.6.4 记录](input-refresh-20261024.md)。

## 保留的限制

五份原生进度只保存游戏 SharedObject，清单仍为 `supports_savestate=false`。有效 `.bak` 回退和本地清理归档已实现；完整 VM 快照、旧 `.sol` 迁移、历史备份管理及游戏进度导入导出继续待做。

自动设备事件通路和截图已验证上述范围；真实游戏的人工操作/听感、广泛资源与图形兼容、断电/重启/休眠、长时与故障矩阵尚未全部验收。完整事项及后置软键盘见 [TODO](todo.md)。

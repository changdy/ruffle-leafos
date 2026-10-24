# 当前实现结果

版本：**0.6.2 开发预览**。文档核对日期：2026-10-24。按当前源码、清单及已有发布验证记录整理；原 0.2.0 审计保存在 [历史交付](../../outputs/ruffle-0.2.0/implementation-report.md)。

## 已实现效果与代码

源码路径均相对工程；点击进入对应文件。

| 能力 | 当前效果 | 代码入口 |
| --- | --- | --- |
| Leaf 接入 | Flash 分类选 SWF 启动；Apps 进入数据管理；最低 Leaf 0.13.0 | [pak.json](../pak/pak.json)、[游戏启动](../pak/emulators/ruffle-probe/launch-game.sh)、[Apps 启动](../pak/launch.sh) |
| 暂停菜单 | 跟随 Leaf 生效主题及字体，叶片形选中条；暂停冻结 tick 与音频；恢复首帧时间重置 | [pause_menu.rs](../player/sdl2test-rocknix/src/pause_menu.rs)、[theme.sh](../pak/emulators/ruffle-probe/theme.sh)、[leaf.rs](../player/sdl2test-rocknix/src/leaf.rs) |
| 按键编辑 | 每游戏 10 个独立按键方案，直接打开的五列实体键宫格及摇杆四方向/按下动作，动作宫格/九类 Tab；单项与整套恢复、本次应用或保存 | [controls.rs](../player/sdl2test-rocknix/src/controls.rs) |
| 游戏快捷键 | M+A/B 循环切换映射，M+十字下切换左键保持，M+X 本次方向互换；切换释放旧输入并等待中立 | [shortcuts.rs](../player/sdl2test-rocknix/src/shortcuts.rs)、[session.rs](../player/sdl2test-rocknix/src/session.rs) |
| 光标 | 默认空心圆＋固定 1×1 中心点；四样式/七色/四尺寸，3.5 秒静止隐藏、移动恢复；旧 arrow 兼容读取 | [cursor.rs](../player/sdl2test-rocknix/src/cursor.rs)、[controls.rs](../player/sdl2test-rocknix/src/controls.rs) |
| 原生进度 | 每游戏 5 份 SharedObject 进度与北京时间；启动先选档、复制、保存后重启读另槽；内容指纹身份、校验、原子替换及有效备份回退 | [experience.rs](../player/sdl2test-rocknix/src/experience.rs)、[leaf.rs](../player/sdl2test-rocknix/src/leaf.rs) |
| 音画与提示 | 每游戏音量/静音、保持比例/铺满/原始大小、低/中/高画质；实际 Flash FPS，提示底色随文字尺寸收缩 | [experience.rs](../player/sdl2test-rocknix/src/experience.rs)、[pause_menu.rs](../player/sdl2test-rocknix/src/pause_menu.rs)、[帧计数接口](../player/core/src/player.rs) |
| 数据管理 | 所选游戏/全部原生进度清理、按键重置；默认取消、确认后移入本地归档；Apps 不运行演示游戏 | [experience.rs](../player/sdl2test-rocknix/src/experience.rs)、[leaf.rs](../player/sdl2test-rocknix/src/leaf.rs) |
| 构建与发布 | 固定公开源码、依赖与工具；ARM64 播放器、主机状态测试、清单/ELF/ZIP/指纹检查及源码打包 | [来源锁](../leaf-source.lock.json)、[构建脚本](../scripts/build_player.py)、[打包脚本](../scripts/package_preview.py) |

操作和数据位置见 [使用说明](preview-0.6.2.md)，映射契约见 [格式 v3](controls-format-v3.md)。

## 真机效果

菜单、按键与五槽截图取自 0.6.1 的验证基线，0.6.2 沿用；新光标截图取自 0.6.2。完整音画、管理及四档光标截图分别保留在 [0.6.1](../../outputs/ruffle-0.6.1/screenshots/)和 [0.6.2](../../outputs/ruffle-0.6.2/screenshots/)交付目录。

暂停主菜单：

![Leaf 风格暂停主菜单](../../outputs/ruffle-0.6.1/screenshots/formal-menu.png)

五列动作宫格与九类 Tab：

![按键动作选择](../../outputs/ruffle-0.6.1/screenshots/mapping-actions.png)

五份原生进度：

![五槽原生进度](../../outputs/ruffle-0.6.1/screenshots/saves.png)

空心圆与单像素中心点：

![0.6.2 光标设置](../../outputs/ruffle-0.6.2/screenshots/ring-medium.png)

## 已有验证证据

以下为各版已有记录，本次文档整理未重新执行真机测试。

| 基线 | 已记录结果 | 证据 |
| --- | --- | --- |
| 0.6.0 | 57 项主机测试；AVM1 五槽/重开、AVM2 重开、SWF 中文改名、首帧前选档、第五槽复制、音画持久化和四类隔离清理 | [device-validation.json](../../outputs/ruffle-0.6.0/device-validation.json) |
| 0.6.1 | 58 项主机测试；菜单与紧凑提示截图，M+A/B、十槽循环、旧 M+左右停用及正式游戏/Apps 通路 | [device-validation.json](../../outputs/ruffle-0.6.1/device-validation.json)、[formal-validation.json](../../outputs/ruffle-0.6.1/formal-validation.json) |
| 0.6.2 | 61 项主机测试；旧 arrow 读取原文件不变，按键/模板/隐藏回归，正式 Leaf 四档中心点均为 1×1，593 个安装文件校验 | [主机日志](../../outputs/ruffle-0.6.2/device-evidence/host-tests.log)、[设备记录](../../outputs/ruffle-0.6.2/device-validation.json)、[正式通路](../../outputs/ruffle-0.6.2/formal-validation.json)、[安装记录](../../outputs/ruffle-0.6.2/installation.json) |

目标环境为 Miniloong Pocket 1、Leaf 0.13.0、Mali-G52/GLES 3.2、glibc 2.38；0.6.2 ARM64 二进制最高符号需求 GLIBC 2.34。播放器 SHA256：`dbeb749ecbd0401091ba62c341b0f44dc4f3facb09c29ec252170cc3b5a04ced`。安装包和源码包校验值见 [SHA256SUMS.txt](../../outputs/ruffle-0.6.2/SHA256SUMS.txt)。

## 保留的限制

五份原生进度只保存游戏 SharedObject，清单仍为 `supports_savestate=false`。有效 `.bak` 回退和本地清理归档已实现；完整 VM 快照、旧 `.sol` 迁移、历史备份管理及游戏进度导入导出继续待做。

自动设备事件通路和截图已验证上述范围；真实游戏的人工操作/听感、广泛资源与图形兼容、断电/重启/休眠、长时与故障矩阵尚未全部验收。完整事项及后置软键盘见 [TODO](todo.md)。

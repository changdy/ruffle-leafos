# 2026-10-28 Flash 软键盘验证

对象：Miniloong Pocket 1 / Leaf 0.13.0 / ARM64 / Mali-G52。本轮为 0.7.1 本地开发构建，不创建新版本或发布 GitHub Release。键盘布局、导航和编辑逻辑参考固定 Catastrophe 源码，适配为播放器现有窗口中的 Rust/GL 界面。

## 构建与主机检查

- ARM64 release 编译通过，AArch64 PIE，所需 GLIBC 最高 2.35。
- 实测播放器 SHA-256：`9d24c9c33cb50866d2736ce5cfad2a48cf09fffeeef5930a608014243b382dab`。
- 109 项前端主机回归通过；其中新增 UTF-8/UTF-16 选区、删除、差分提交、密码预览、容量限制和输入中立门检查。
- 15 项发布脚本回归通过。测试临时目录显式使用工作区 `build/tmp`；首次使用受管系统临时目录遇到权限限制，调整测试目录后通过。
- 源码及 Catastrophe 参考指纹一致。保留导入文件原始 CRLF；自有来源锁使用 LF，以 `git -c core.whitespace=cr-at-eol diff --check` 检查差异。

## 隔离设备回归

`scripts/make_keyboard_fixtures.py` 生成原创 AVM1 / AVM2 SWF，包含普通输入、密码、数字限制、多行、只读文本及 AVM2 取消输入监听器。所有夹具、日志、截图和数据都位于本地忽略目录及掌机独立调试目录。

| 组合 | 结果 | 本地证据 |
| --- | --- | --- |
| AVM1 / GLES / 中文 | 12 阶段通过并正常退出 | `avm1-gles-final.log`、`avm1-gles-final/` |
| AVM2 / GLES / 英语及 Inter 字体 | 12 阶段通过并正常退出 | `avm2-gles-en.log`、`avm2-gles-en/` |
| AVM2 / Vulkan / 英语 | 12 阶段通过并正常退出，日志确认 Mali-G52 Vulkan 和首次呈现 | `avm2-vulkan-final.log`、`avm2-vulkan-final/` |

断言覆盖：脚本主动聚焦与鼠标点击打开、已有文本与选区、大小写、删除、空格、光标移动、确认回填、取消保留原文、同框重开、密码遮挡和 Unicode、多行换行、`restrict` / `maxChars`、只读不打开、清空全部、帮助、确认键松开前禁止游戏输入。输入期间 VM tick 保持不变。AVM2 `textInput.preventDefault()` 保持有效。

失焦和断连检查调用对应前端处理流程，验证取消草稿并进入暂停菜单；没有将其记为实际拔插或系统休眠验收。最初主动聚焦夹具的诊断把光标误当作位于末尾，导致两次测试断言失败；仅修正诊断起始光标预期后完成上述最终回归，未改变游戏原始选区语义。

## 实际手柄事件

重新连接后，通过身份校验的 Loong Gamepad 物理 evdev 事件进入 Leaf 虚拟手柄及 SDL。独立 AVM2 会话完成：R1 移动文字光标、A 输入、十字键选字、B 删除、X 空格、Select 大小写、M 帮助开关和 Start 确认。游戏自己的 `change` 监听器记录预期的原创测试文字 `seedq W`。

随后通过游戏 Tab 映射聚焦密码框，核对遮挡界面，输入后用 Y 取消；通过实际暂停菜单“保存并退出”正常结束。截图为 `physical-draft.png`、`physical-help.png`、`physical-password.png`，日志为 `manual-avm2-resume.log` 和同名 trace 文件。该阶段前后正式配置、存档逐文件哈希一致。

## 安装与清理

正式应用已完整备份到 `/mnt/sdcard/.userdata/mlp1/ruffle-probe/debug-backups/20261028-keyboard/pak-before`，备份与原目录逐文件哈希一致。

完整 0.7.1 本地安装包已更新到正式 `RuffleProbe.pak`，850 个安装文件与本地暂存逐项 SHA-256 一致。正式 Apps 数据管理页启动、显示与正常退出通过，截图为 `installed-complete-manager.png`。直接使用原始 IPC 发起正式 Apps 检查时，最初只排队了启动请求；完成 Leaf 前台交接后正常启动，没有将排队期间记为播放器启动成功。

进一步使用已安装的正式 `launch-game.sh` 启动原创 AVM2 输入夹具，配置、存档及日志显式指向独立调试目录。实际手柄完成输入、确认回填和暂停菜单退出，预期 `seedq` 由游戏自己的变化监听器记录；证据为 `installed-formal-wrapper.log`、同名 trace 和 `installed-game-keyboard.png`。夹具随即移出 `Roms/flash/`。这验证正式脚本/安装字节与键盘输入，不作为用户游戏兼容性验收。

本轮重连时保存了新的用户数据基线，不恢复覆盖用户在前次测试之后产生的数据。隔离阶段正式配置和存档完全一致；正式升级前已有 32 个配置文件及 25 个存档文件的内容继续保持。升级首次使用新增 `config/language.conf`，完整启动器按系统语言保存 `zh-CN`；此前单独替换运行文件时测试新建的默认语言文件已归档后重新检测，未修改用户原有偏好。原始独立 `ruffle.pak` 保持不变。

临时 `RuffleKeyboardTest.pak` 已归档到 `Apps/` 外。索引清理前备份数据库，只处理已确认目录不存在的本次测试入口，刷新后核验 Apps、FTS 搜索及临时 ROM 记录均无残留，正式应用和其他记录保留。摘要为 `installation.json`、`cleanup.json`、数据前后清单及安装文件清单。

完整证据保存在本地 `outputs/soft-keyboard-20261028/`，不随 Git 或安装包分发。

## 适用范围

标准可编辑 Flash TextField 已覆盖上述夹具。屏幕键盘提供字母、数字、符号及多行换行，支持已有 Unicode 内容编辑，尚无中文拼音组词输入。游戏自绘输入界面、广泛游戏兼容性、真实外设拔插、休眠恢复和长时运行仍待独立验收。

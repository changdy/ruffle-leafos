# 2026-10-28 英语界面实机检查

对象为 Miniloong Pocket1 / LeafOS / ARM64 / GLES。范围是播放器界面语言、通知和字体，不扩大游戏兼容性或性能验收。

## 修复与结论

初版主菜单、Apps 管理页及语言页标题固定使用“语言 / Language”，英语模式仍含中文。现改用翻译入口：英语显示 `Language`，中文显示“语言”。语言选项仍以 `简体中文` / `English` 自称；游戏标题和用户内容保持原文。

修复后，下列已检查的页面与提示未发现意外中文。未触发的运行期 GPU 故障、真实满卡/介质损坏、所有用户游戏及所有错误路径不在本轮实机结论内。

| 检查 | 结果与证据文件名 |
| --- | --- |
| 暂停主菜单及语言入口 | 英语；`fixed-home.png` |
| 按键编辑、连发和动作分类 | 英语，包含常用/鼠标/符号分类、未保存确认；`english-controls.png`、`english-turbo-status.png`、`english-picker-common.png`、`english-picker-mouse.png`、`english-picker-symbols.png`、`english-unsaved-confirm.png` |
| 游戏存档和复制确认 | 英语，成功保存及复制到槽 2；`english-native-copy-confirm.png`、`english-toast-native-copied.png` |
| 画面设置、渲染页与切换确认 | 英语，本轮取消切换；`english-display.png`、`english-renderer.png`、`english-renderer-confirm.png` |
| 快捷键说明与关于 | 三组及关于页均为英语；`english-shortcuts-game.png`、`english-shortcuts-menu.png`、`english-shortcuts-editor.png`、`english-about.png` |
| 鼠标光标、速度及外观保存 | 英语；`english-cursor.png`、`english-cursor-speed-status.png`、`english-cursor-saved-status.png` |
| Apps 管理与语言页 | 英语；`english-apps-manager.png`、`english-apps-language.png` |
| 中英即时切换 | 标题、返回/确认、保存提示立即更新；`fixed-language-chinese.png`、`fixed-language-english.png` |
| 英语主题字体与中文回退 | 使用系统 Inter 字体，中文选项及切回中文可读；`english-latin-language.png`、`chinese-latin-fallback.png` |
| 已保存英语优先 | 将已保存的英语偏好复制到隔离配置，使用 `UMRK_LANGUAGE=zh_CN` 重开，仍显示英语；`english-latin-home.png` |

## 通知与错误场景

`english-fixed.log` 的 10 条 `leaf-toast` 全部不含汉字，实际覆盖：

- 切到方案 2 / 切回方案 1。
- 十字键与摇杆互换 / 恢复。
- 鼠标左键保持开启 / 关闭。
- 原生进度保存到槽 1、复制到槽 2。
- 方案选择写入失败，显示本次生效及保存失败。
- 语言保存失败，显示英文系统错误并保持 English 选中标记。

先在可正常写盘的隔离目录完成保存/复制，再将该测试会话的配置目录暂移、用普通文件阻断目录创建，触发写入失败；完成后恢复测试配置目录。按键保存失败的页面状态也为英语，见 `english-control-save-failed.png`；语言失败的页面及 toast 见 `english-language-save-failed.png`。未修改正式用户配置目录。

`english-vulkan-failure.log` 通过隔离的 Vulkan 偏好及无效 `VK_ICD_FILENAMES` 触发初始化失败，实际回退 GLES，toast 为 `Vulkan failed to start; restored OpenGL ES`，截图 `english-toast-vulkan-fallback.png`。没有替换系统驱动或修改正式渲染偏好。

首次测试使用隐藏目录内的原创 AVM1 SWF，受既有 SharedObject 路径限制而不产生进度，实际检查了“尚无原生进度”和无法复制提示，均为英语。随后使用 `/tmp` 下的同一原创文件，检查成功保存/复制。该无进度样本不计作成功保存验收。

## 构建、数据与恢复

- 本轮当前工作区 100 项前端主机回归通过，新增英文成功/恢复/写入失败通知覆盖，并将语言页加入无中文残留检查；只允许语言选项的中文自称。ARM64 release 构建通过，所需 GLIBC 最高 2.35。
- 修复版实测二进制 SHA-256：`f03dd695cf7108ab1b1e3bdaee0b888825287b06326796505f527dec60dabc54`。本轮使用当前工作区构建，包含原有尚未提交的全局画面配置，修复按独立范围提交。此指纹描述实测构建，不代表独立提交的成品指纹。
- 所有截图、完整日志、正式配置/存档前后清单保存在本地 `outputs/language-device-20261028/`，不进入安装包或 Git。
- 正式 `config/`、新格式原生进度目录及已安装播放器、`theme.sh`、`paths.sh` 前后 SHA-256 一致。本轮没有覆盖正式安装。隔离播放器均正常退出并提交各自测试进度，临时 Apps 入口已归档到调试目录，启动器恢复。
- 网络切换后 ADB 重连，启动器一度盖住直启测试窗口；部分测试按键落入启动器并打开《死神 VS 火影 2.6》。已正常结束这次误启动并复核上述正式数据不变。相关截图不作为播放器界面证据；之后改用 Leaf 的 Apps 生命周期启动，并在注入按键前拒绝任何正式游戏或启动器并存的状态，重新完成通知截图。没有把这段过程当作游戏验收。

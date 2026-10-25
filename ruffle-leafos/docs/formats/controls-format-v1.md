# 控制配置与模板格式 v1

> 旧版格式契约：保留用于兼容读取。当前写入格式见 [v5](controls-format-v5.md)，当前操作见[项目 README](../../README.md)。

UTF-8 纯数据文本，扩展名 .rmap，文件至多 16 KiB。每行严格为 key=value；字段完整且唯一，不允许额外字段。模板名称最多 32 个 Unicode 字符，不含控制字符或等号。内置包不携带用户模板。

```text
version=1
name=My Template
identity=
base=1
mode=keyboard
speed=normal
stick=directions
fine=false
bind.0=32
bind.1=1073742048
bind.2=1073742049
bind.3=122
bind.4=120
bind.5=99
bind.6=none
bind.7=none
bind.8=13
bind.9=27
bind.10=1073741906
bind.11=1073741905
bind.12=1073741904
bind.13=1073741903
```

base 为 0（通用）或 1～12（内置方案顺序）。mode 为 hybrid/keyboard/mouse；speed 为 slow/normal/fast；stick 为 directions/steering/mouse/none；fine 开启时，未绑定的十字键可以作鼠标精调。Menu 不在配置字段中。

bind.0～13 顺序固定为 A、B、X、Y、L1、R1、L2、R2、Start、Select、十字上、下、左、右。动作可以是受支持的 SDL 稳定键码十进制整数，或用逗号排列的 2～4 个不同键码；顺序即按下顺序，释放反向。1073742049,9 为 Shift+Tab。支持字母、数字、常用控制键、方向键、F1–F12 和编辑器列出的标点；未知键码拒绝。

其他动作：none、left-click、right-click、slow、fast、wheel-up、wheel-down、drag-lock、center。字段不会传给 shell、eval 或进程启动器。损坏、未知版本、重复/多余字段、过长名称、无效键码和完全无可用动作的配置会被拒绝。导入仅读取 imports 目录的普通 .rmap 文件，并存入下一个空闲模板槽，最多 16 槽，不覆盖已有模板。

模板及全局文件的 identity 为空。游戏文件包含 UTF-8 游戏逻辑路径的十六进制编码。路径去除 Roms/flash 前的挂载位置，将 Flash 分类前缀规范为 flash，并保留游戏子目录和文件名；同名不同目录分离。文件名使用固定 FNV-1a 64 位索引，读取和覆盖时还要核对完整 identity，发生不匹配会拒绝覆盖，避免把索引当成无条件身份。此策略不处理游戏改名、跨目录迁移或 SharedObject 身份；这些仍属于 F5 后续工作。

写入前验证整个记录。已有有效文件先同步到 .bak；已有坏文件另存为 .invalid-*，不覆盖有效备份。新文件在同目录以 create_new 创建、写满、sync_all 后原子替换；Linux 再同步父目录。Windows 的主机测试使用 MoveFileExW 的替换/写穿标志。删除会归档主文件和备份，防止下次自动读取时复活已删除的模板。启动损坏回退保留原文件。

格式只包含当前控制配置。未来扩展必须使用明确的版本迁移，不静默接受额外字段。存档、网络、画面和音量配置不在此格式中。

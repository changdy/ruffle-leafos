# 按键格式 v4 与全局光标

> 旧版格式契约：保留用于兼容读取。当前写入格式见 [v5](controls-format-v5.md)，当前操作见[项目 README](../../README.md)。

2026-10-24 起新保存使用 v4，兼容读取旧 v1/v2/v3。v4 共 27 个唯一字段：`version=4`、`name`、`identity`、`base`、`mode`、`speed`、`stick`、`fine`、`bind.0`—`bind.13`、`axis.0`—`axis.4`。重复、缺失、未知字段拒绝。字段值、按键白名单、组合键顺序和游戏身份规则沿用 [v3](controls-format-v3.md)。base 0—12 仅为旧映射兼容；13/14 为当前两组默认键位。

每游戏十组文件路径仍为 `slots/<原有游戏FNV标识>/slot-01.rmap`—`slot-10.rmap`，记忆选择仍为 `selected.rmap` 且 `name=Slot 1`—`Slot 10`，不因中文命名改变旧文件路径。每次映射主动保存才升级格式并备份原文件；仅打开不会重写。

全局光标独立为 `config/controls/cursor.conf`，四字段：

```ini
version=1
style=ring-dot
color=white
size=24
```

样式 ring-dot/crosshair/dot/square（读取兼容 arrow），七色和 16/24/32/48 四档大小不变。保存原子替换，保留 `cursor.conf.bak`，损坏原文另存 invalid 文件；回退备份时不把损坏主文件变成有效备份。取消预览不应用、不写盘。切换游戏、方案、恢复按键或重置全部按键均不改变全局光标。

旧映射 v2/v3 的三项 cursor 字段仍严格校验，但不再应用，也不自动迁移为全局外观。无全局文件时统一采用默认空心圆；旧全局按键及模板文件保留，不再提供模板管理功能。

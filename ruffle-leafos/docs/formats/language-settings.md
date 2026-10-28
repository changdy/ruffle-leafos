# 全局界面语言

语言文件为 `$USERDATA_PATH/ruffle-probe/config/language.conf`；源码入口 `session::i18n`。路径从 `controls_root` 的父目录解析，所有游戏与 Apps 共用，独立于按键方案、画面设置和存档。Apps 清理按键/存档不删除语言文件。

```ini
version=1
language=en
```

UTF-8 文本，最多 128 字节，每行 `key=value`，字段顺序不限，接受 LF/CRLF；字段恰为 `version` 与 `language`，不接受重复、缺失或未知字段。`version` 必须是 `1`；`language` 仅接受 `zh-CN`（简体中文）或 `en`（英语）。写入采用同目录临时文件、sync 和原子替换，不写入 pak，不改写 Leaf 系统设置。

首次缺少文件或文件无效时，依次取首个非空环境值：`UMRK_LANGUAGE`、`JAWAKA_LANGUAGE`、`RUFFLE_SYSTEM_LANGUAGE`、`LC_ALL`、`LC_MESSAGES`、`LANG`。启动脚本仅在前两个均为空时，从当前卡的 `.umrk/mlp1/library.db` 只读查询 `settings.language` 并导出 `RUFFLE_SYSTEM_LANGUAGE`。环境值取去除 `-`、`_`、`.`、`@` 后缀的基础代码，不区分大小写；`zh` 选简体中文（含其他中文地区），其他值或全部缺失均选英语。检测结果立即持久化，不继续跟随系统变化。

有效文件优先于系统语言；读取发生 I/O 错误时保留原文件，本次使用系统识别值并提示无法保存。首次写入失败也会提示，之后启动可重试；菜单切换只有保存成功后才改变当前语言和选中标记，失败时保留原语言。保存后不重启游戏。语言仅控制播放器界面，不翻译 SWF 内容、文件路径或用户游戏名称。

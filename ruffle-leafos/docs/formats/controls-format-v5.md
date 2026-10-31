# 按键映射与全局光标格式

当前写入 **v5**，兼容读取 v1—v4。本文统一维护字段、身份、存储及旧版兼容；操作方法见[项目 README](../../README.md#按键映射)。实现见 [controls.rs](../../player/sdl2test-rocknix/src/controls.rs)、[cursor.rs](../../player/sdl2test-rocknix/src/cursor.rs)与 [turbo.rs](../../player/sdl2test-rocknix/src/turbo.rs)。

## 映射记录

`.rmap` 是 UTF-8 纯数据文本，最多 **16,384 字节**，每行 `key=value`。写入使用 LF，读取兼容 CRLF；不支持注释或空行。字段完整且唯一，缺失、重复、未知字段和未知版本均拒绝。内容不会交给 shell、eval 或进程启动器。

v5 恰好 **46 个字段**：

| 字段 | 数量 | 取值与含义 |
| --- | ---: | --- |
| `version` | 1 | 当前写入 `5` |
| `name` | 1 | 最多32个 Unicode 字符，不可全为空白，不含控制字符或 `=` |
| `identity` | 1 | 逻辑游戏路径的 UTF-8 字节十六进制编码；字段最多4096个字符，只接受 ASCII 十六进制，非游戏记录可为空 |
| `base` | 1 | `0`—`14`；0—12保留旧基底，13/14为当前两组默认键位；不据此判断方案是否为空 |
| `mode` | 1 | `hybrid`、`keyboard`、`mouse` |
| `speed` | 1 | `slow`、`normal`、`fast` |
| `stick` | 1 | `directions`、`steering`、`mouse`、`none`、`custom`；v3起运行时以显式 `axis.*` 为准 |
| `fine` | 1 | 严格为 `true` 或 `false`，表示十字键鼠标精调 |
| `bind.0`—`bind.13` | 14 | A、B、X、Y、L1、R1、L2、R2、Start、Select、十字上、下、左、右 |
| `axis.0`—`axis.4` | 5 | 摇杆上、下、左、右、按下 |
| `turbo.0`—`turbo.13` | 14 | 对应实体键的连发频率 |
| `axis-turbo.0`—`axis-turbo.4` | 5 | 对应摇杆动作的连发频率 |

`bind.*` 与 `axis.*` 接受相同动作：`none`、`left-click`、`right-click`、`slow`、`fast`、`wheel-up`、`wheel-down`、`drag-lock`、`center`、`pointer-0`—`pointer-3`（鼠标上、下、左、右），或1—4个不同的 SDL 稳定键码十进制整数，组合以逗号分隔。按记录顺序按下、反序释放，共享动作使用引用计数；例如 `1073742049,9` 为 Shift+Tab。Menu 不在可绑定字段中。

键码仅接受 `a`—`z`、`0`—`9`、标点 `` `-=[]\;',./ ``，以及 Backspace/Tab/Enter/Esc/Space/Delete、F1—F12、Insert/Home/PageUp/End/PageDown、四方向、左 Ctrl/Shift/Alt；数值白名单见 `controls.rs::allowed_key`，未知键码拒绝。

连发只接受 `0`、`5`、`10`、`15`，分别为关闭及每秒5/10/15次的目标频率。只有键盘单键、组合键和鼠标左右键允许非零值，其他动作带非零连发拒绝。实际时序受主线程负载影响，文件中的频率不是硬实时保证。

## 游戏身份、槽位与默认值

控制目录为 `config/controls/`，每游戏十个方案位于 `slots/<16位FNV索引>/slot-01.rmap`—`slot-10.rmap`，选择记录为同目录 `selected.rmap`。选择记录同样采用完整 Record，保存时 `name=Slot 1`—`Slot 10`；读取名称中1—10的编号和匹配身份，失败选择方案1。其中的映射副本不替代槽位文件。

身份生成先将路径反斜杠转为 `/`；找到 `/roms/flash/` 前缀时忽略该前缀大小写和其前挂载位置，规范为 `flash/`，保留后面的子目录及文件名；未找到时使用原路径。将结果的 UTF-8 字节编码为十六进制，再对该编码字符串的 ASCII 字节计算 FNV-1a 64位索引。装载时仍比较完整 `identity`，有效主文件身份冲突时禁止覆盖；索引相同不能当作身份相同。游戏改名或移动不会自动迁移按键，SharedObject 内容身份另行维护。

缺少文件时，方案1/2使用内置两组默认键位，方案3—10的19个绑定均为 `none`，`stick=none`、`fine=false`、全部连发 `0`，其余为 `base=13`、`mode=hybrid`、`speed=normal`。启动不批量生成十份文件。19个绑定全为 `none` 且 `fine=false` 才是全空，速度和基底等元数据不影响判定；空方案允许读取、保存、应用与记忆选择。

已有槽位主文件或备份优先。仅在方案1主文件和备份都不存在、旧 `games/<索引>.rmap` 存在时兼容读取旧此游戏映射；旧全局按键、模板及 ROM 旁 `.cfg` 不自动叠加。只读取不改写旧文件，主动保存才写v5。

## 旧版兼容

| 版本 | 字段数 | 字段差异及当前读取行为 |
| --- | ---: | --- |
| v1 | 22 | 8个头字段＋14个 `bind.*`；`base` 最大12，不接受 `stick=custom`；没有显式轴向、光标或连发字段 |
| v2 | 25 | v1增加 `cursor-style`、`cursor-color`、`cursor-size`；光标值仍严格校验，但不应用或迁移到全局外观 |
| v3 | 30 | 8个头字段＋14个绑定＋5个轴向＋3个旧光标字段；`base` 最大14，接受 `custom`，读取显式轴向；旧光标仅校验 |
| v4 | 27 | 8个头字段＋14个绑定＋5个轴向；移除旧光标字段 |
| v5 | 46 | v4增加19个连发字段，当前写入版本 |

v1/v2按 `stick` 派生摇杆四方向：`mouse` 对应 `pointer-0`—`pointer-3`，`directions` 复制 `bind.10`—`bind.13`，`steering` 仅复制左右，`none` 均未绑定；摇杆按下始终未绑定。v1—v4的19个连发值统一补0。当前读端对所有受支持版本均允许全空映射。

v2/v3旧光标字段接受下节相同样式、颜色和尺寸，`arrow` 为 `ring-dot` 读取别名。主动保存不再写旧光标字段，原文按备份规则保留。旧播放器可能不识别v5、ring-dot或空方案，回滚需保留旧安装和升级前配置；不自动反向转换。

## 全局光标

独立文件 `config/controls/cursor.conf`，UTF-8，最多 **512字节**，恰好四个唯一字段：

```ini
version=1
style=ring-dot
color=white
size=24
```

`style` 为 `ring-dot`、`crosshair`、`dot`、`square`；读取兼容 `arrow` 并转为 ring-dot，写入不再生成 arrow。`color` 为 `white`、`black`、`red`、`yellow`、`green`、`cyan`、`magenta`；`size` 为 `16`、`24`、`32`、`48`。默认值如上，缺失、重复、未知字段或无效值拒绝。

全局外观与游戏/方案分离，切换游戏、方案或重置全部按键不改变它；旧映射光标字段不会自动迁移。取消预览不写盘，保存失败不应用草稿。

## 写入、备份与恢复

读取和写入目标须为普通文件，拒绝符号链接及目录。映射保存前验证整个记录；已有有效主文件先备份为 `.rmap.bak`，坏主文件另存 `.invalid-<唯一标识>`，不覆盖有效备份；原主文件超过上限则拒绝覆盖。全局光标同理使用 `cursor.conf.bak` 及损坏文件归档。

新数据在同目录以 `create_new` 创建临时文件，写满并 `sync_all` 后原子替换；Unix再同步父目录，Windows使用 MoveFileExW替换/写穿标志。主文件读取失败可回退有效备份，原损坏文件保留。加载槽位仍检查完整游戏身份，不将损坏文件或其他游戏的数据视为有效配置。读取不触发格式升级。

Apps清理将选定按键数据移入本地 `cleared-data/` 归档，操作范围与入口见[数据管理说明](../../README.md#apps-数据管理)，路径见[数据身份与持久化](../implementation.md#数据身份与持久化)。历史模板导入/导出及全局按键自动继承已移除，不属于当前格式的使用入口。

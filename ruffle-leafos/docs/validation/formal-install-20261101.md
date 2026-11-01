# 正式安装与数据保留验证（2026-11-01）

在 Pocket1 / Leaf 0.13.0 将本地修复构建安装到正式 `Apps/mlp1/RuffleProbe.pak/`。候选源码为 `c4b6347`，版本仍为 **0.8.3 开发预览**，没有发布新的 GitHub Release。

- 播放器 ELF SHA-256：`bbb8243a6d7871f2e3a1dbfdbc5427e27f41affe3a3d3162281133c9c2d608b4`。
- 安装 ZIP SHA-256：`a702f5ef6926fe90a530d5d7a936af065001c0d24b9ecc74f8dac9f0a03ba7b4`。
- 构建记录、包内播放器与当前 1,463 项编译输入绑定核验通过。修复和既有专项验证见[上游兼容修复](upstream-compatibility-20261101.md)及[输入、保存提示与构建绑定](reliability-fixes-20261101.md)；这些专项结果不扩大为本次正式安装的全部游戏验收。

## 备份与安装

安装前确认无播放器运行，备份原 App、配置、原生进度、home 和日志，并备份 Leaf 数据库。归档中 885 份 App/用户数据文件逐一匹配安装前哈希，另含两份运行日志；本机与设备备份压缩包 SHA-256 均为 `2b155c00a9723cdd057c24499c1b98bc5e8cec0dd4d4b6785c665966cf1ee880`。

只将 ZIP 的 App 子树覆盖到原 App 的同卡暂存副本，核对后替换正式目录；没有复制 ZIP 的 `Roms/` 或 `Images/`。原 App 为 803 文件，新 App 为 808 文件，10 文件更新、5 文件新增；原 App 没有需要额外迁移的包内自定义文件，安装计划没有需覆盖的配置差异。安装后及最终收尾均再次核验全部 808 个正式文件与 803 个回滚文件。

备份和回滚入口保留在 Apps 之外：

- 本地证据及备份：仓库根目录的 `outputs/formal-install-20261101/`，含 `original-application-and-data.tar.gz`、`library-before.db`、逐文件清单、日志和截图，均不提交。
- 设备备份目录：`/mnt/sdcard/.userdata/mlp1/ruffle-probe/deploy-backups/20261101-c4b6347/`。
- 原 App 回滚副本：上述设备目录下的 `previous-RuffleProbe.pak/`。

## 正式入口检查

| 检查 | 结果与范围 |
| --- | --- |
| Flash 分类启动原版《拳皇 Wing 1.91》 | 正式 ROM 路径 `Roms/flash/拳皇Wing1.91.swf`；运行进程的 `/proc/<pid>/exe` 路径及 SHA 均匹配正式播放器和上述 ELF |
| 既有原生进度 | 槽 1 在首个 tick 前选择并加载；没有转换或重建存档 |
| 暂停、恢复、保存退出 | GLES 进入更新说明首画面，M 暂停、B 恢复、暂停菜单保存并退出通过；日志确认 `progress_flushed=true`，未进入实战 |
| Apps 数据管理 | 正式入口进入只读游戏数据列表，B 返回、B 退出通过；未执行清理或修改配置 |
| 返回 Leaf | 等待退出过渡结束后，`leaf-settled-after-validation.png` 清晰显示 Leaf Flash 分类；即时退出的两张黑图仅为过渡记录，不作为界面验收证据 |
| 索引与收尾 | 刷新无错误，15 条 Apps 记录完全不变，搜索仍指向正式 Flash 数据管理；同一 boot 无新增内核 OOM，无播放器残留 |

## 用户数据核对

安装后首次启动前，配置、存档目录、ROM、图片、home、data 六组清单与安装前逐文件完全一致。正式入口检查结束后，无文件新增或删除：

| 数据 | 最终核对 |
| --- | --- |
| 配置 39 文件 | 全部字节一致，含已有按键、语言和画面设置 |
| 原生进度与备份 16 文件 | 全部字节一致 |
| 存档目录共 42 文件 | 数量不变，唯一变化是拳皇既有 catalog 元数据追加 ROM 路径，详见下文 |
| Flash ROM 499 文件、图片 1 文件 | 全部字节一致，原版及定制游戏均保留 |
| home 1 文件、data 0 文件 | 清单及字节保持 |

唯一 catalog 变化保留原有两行游戏名称和按键路径标识，追加 `path=<UTF-8 路径的十六进制编码>`，解码为原路径 `/mnt/sdcard/Roms/flash/拳皇Wing1.91.swf`；没有改变游戏内容身份或原生进度。不能将这次正常元数据补全表述为存档目录全部字节未变。

关键证据为本地 `installed.json`、`game-running.json`、`game-resume.json`、`game-exit.json`、`apps-exit.json`、`data-diff.json` 和 `final-integrity.json`，以及对应日志和截图。

## 验证边界

本次正式安装仅完成上述拳皇首画面和 Apps 通路检查，没有新增实战、死神正式入口、Vulkan 游戏、全角色、音频听感、长时、断电或多卡验收。包内文档沿用该 ZIP 打包时的快照；本报告及后续文字整理不改变已安装 ELF 的验证范围。

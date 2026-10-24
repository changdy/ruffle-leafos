# LeafOS SSH 真机调试记录 · Ruffle 0.1.4

2026-10-23，已通过 `ssh -p 12222 sshadmin@192.168.5.4` 成功登录并完成首轮调试。此账号在设备上的实际身份为 uid 0。Ruffle 0.1.4 热修复已直接安装，重新打开游戏即可生效。密码未写入调试脚本、报告或交付包。

| 项目 | 真机证据 |
| --- | --- |
| Leaf | 0.13.0，来自 `/mnt/sdcard/.umrk/mlp1/release.json` 的 managed-install 记录 |
| 基础系统 | Buildroot 2021.11；内核 5.10.209；aarch64；glibc 2.38 |
| 实际图形上下文 | ARM / Mali-G52；OpenGL ES 3.2；GLSL ES 3.20；960×720 |
| 当前安装 | `Apps/mlp1/RuffleProbe.pak`，0.1.3 已更新为 0.1.4 |
| 远程服务 | SSH 12222、ADB TCP 5555 正在监听 |
| USB | gadget 为 ADB 模式，UDC 状态 `not attached`；本次没有证明 USB 主机枚举成功 |

平台 manifest 中的 `0.0.1-poc` 属于平台组件元数据；pak 的 `min_leaf_version=0.12.0` 属于最低版本声明。两者均未当作实测 Leaf 版本。LoongOS 自身的产品版本仍未取得独立版本字段，上表保留实际系统构建信息。

本次落实了三项修复。

- **备用中文字体加载。** 原日志显示 0 个系统字体，并反复报告缺少 CJK fallback。0.1.4 将包内原有字体放入播放器实际扫描的 HOME 字体目录，使用兼容文件名；校验内容后通过临时文件更新缓存。真机日志已变为 1 个字体，缺少 CJK fallback 的警告消失。字形完整性和具体游戏文字外观仍需查看屏幕验收。
- **默认音频输出。** 原厂 ALSA 默认声卡为 HDMI。隔离测试先出现 314 次音频流 I/O 错误；接入 Leaf 已运行的 PulseAudio 后降为 0。0.1.4 仅在发现 Leaf 音频 socket、ALSA pulse 插件且不存在自定义 ALSA 配置时启用该路径。最终测试观察到 48 kHz 双声道 PulseAudio 流及内置 RK817 声卡播放状态；听感、音量和同步仍需听音验收。
- **《魔塔》方向配置。** 原有 `魔塔.cfg` 的 `Right=Left` 已修正为 `Right=Right`。只修改这一行，原文件已单独备份；未改动其他按键方案。

环境采集器还补充了 Leaf 安装版本记录，并兼容此 Buildroot 缺少 `getconf` 时的 libc 版本采集。没有修改 Leaf 的 `.system` 目录或刷写固件。

验证使用全新的临时 HOME、日志和 SAVES_PATH。《魔塔》运行 16 秒，Apps 内置动画运行 8 秒，均进入播放并创建真实 GL context。最终两个入口都成功加载备用字体、选择 Leaf PulseAudio，且音频流错误为 0。测试由预设 timeout 结束，返回码 124 属于预期；**这不代表 Menu 退出、存档提交或休眠已通过验收**。结束后没有残留 Ruffle 进程，Leaf 启动器、Weston 和远程服务仍在运行。

《魔塔》的一个 299 区间样本窗口记录：呈现间隔均值 39.336 ms，最大 166.605 ms；swap 调用均值 0.788 ms。这些是墙钟呈现间隔与调用耗时，不能当作 GPU 执行时间或稳定游戏帧率。采样时系统存在下述 `udevd` 高占用，不能据此宣布性能优化完成。

桌面检查通过：44 项 wrapper 检查、44 项会话与诊断检查、编译后的 C 输入及实际 SDL 适配器检查、官方 CONTENT-1 校验、ARM64 ELF 与安装包检查。Windows 文件符号链接动态用例因权限不可用而跳过，未记为通过。安装后的 7 个变更文件逐一核对 SHA-256，与交付包一致。

仍有一个独立的系统问题：`udevd` 持续运行，约占用一个 CPU 核心。USB 服务日志同时存在重复 update 请求。临时停止 USB gadget 后该高占用依然存在，因此不能断定由 ADB 或 Ruffle 引起。测试后已恢复 USB gadget，ADB 开关和原配置保持开启。此问题尚未修复；本次未替换 udev、修改启动脚本或重启系统。

日志还保留了 en_US.UTF-8 Compose 文件缺失提示；组合文本输入尚未验收，本次未修改系统 locale。

按原需求，本批补齐了 F1.4 的这台设备环境记录、F2.3 的真实 GL context 证据，并修复 F2.4 中的字体发现及音频默认路径。完整游戏内菜单、12 组新映射、虚拟键盘、可靠原生进度/备份和休眠恢复仍未实现。播放器源码重建问题也仍未解决：leaf-12 原二进制和 SDL 适配库的 hash 均保持原样。

设备上的回滚备份位于：

```text
/mnt/sdcard/.userdata/mlp1/ruffle-probe/debug-backups/20261023-ssh-hotfix/
  pak-files-0.1.3.tar
  mota.cfg.before
```

电脑上的相同备份、两入口日志和环境报告位于本目录 `device-evidence/`。需要回滚时先退出游戏，再在 SSH 内执行下列命令；字体缓存属于本地数据，保留它不影响旧版运行。

```sh
tar -xf /mnt/sdcard/.userdata/mlp1/ruffle-probe/debug-backups/20261023-ssh-hotfix/pak-files-0.1.3.tar -C /mnt/sdcard/Apps/mlp1/RuffleProbe.pak
cp /mnt/sdcard/.userdata/mlp1/ruffle-probe/debug-backups/20261023-ssh-hotfix/mota.cfg.before /mnt/sdcard/Roms/flash/魔塔.cfg
```

原安装包 0.1.3 及其交付记录未覆盖。本目录提供 0.1.4 安装包、适配源码包、构建报告和 `device-validation.json`；构建报告中的 `on_device_tests` 是构建时快照，之后的真机验证以本记录和随附证据为准。

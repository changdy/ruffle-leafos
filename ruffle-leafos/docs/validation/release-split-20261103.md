# 0.8.6 双包发布与 Bloxorz 验证

日期：2026-11-03。目标：Miniloong Pocket 1 / Leaf 0.13.0。此版本拆分分发内容，未改变 Ruffle 核心或游戏执行逻辑。

## 分发结构

`ruffle-leafos-0.8.6.zip` 是主安装包，顶层直接为 `Apps/`、`Roms/`、`Images/`。`Roms/flash/` 只附英文原版 `Bloxorz-翻滚方块.swf` 和原创测试 `Leaf-GPU-Probe.swf`；探针的内部 `res/` 副本继续保留。

`ruffle-leafos-games-0.8.6.zip` 是可选合集，顶层为 `Roms/flash/`，只含拳皇 Wing 1.91、死神 VS 火影 3.3 / 3.6 三个 ZIP。合集外层解压到卡根目录，内层游戏 ZIP 保持原样；其主 SWF、398 / 554 个外部资源、原始 `assets/` 相对路径及主文件内容身份不变。更新主包不会删除已安装游戏。

两个归档使用独立暂存树。打包检查样例和合集清单、源码输入与 ELF 绑定、AArch64 PIE / GLIBC、Leaf 清单、归档 CRC 和 Shell 执行位。发布脚本要求两个本版本归档均存在，说明各自安装方式并列出两份 SHA-256；创建 Release 与草稿补传都明确上传这两个附件，不上传残留旧包或可选源码包。已公开 Release 的重跑仍保留原附件。Actions 已接入该流程；本次没有推送代码或执行线上 Actions / GitHub 发布。

## Bloxorz 来源

收录 Damien Clarke（Roboman / DX Interactive）的英文 Miniclip 分发版，来自[历史 SWF 镜像](https://locker.phinugamma.org/swf/miniclip/bloxorz_miniclip.swf)，原发布入口为 Miniclip 的 Bloxorz 页面。2,286,600 字节，SHA-256：

```text
296c5c17b7a7b6529f6b8faa19aae78ebf4382c161c07e5070ab338fd8264803
```

下载字节原样保留，没有修改游戏、汉化或修改播放器来绕过来源检查。`games/samples/manifest.json` 固定来源及字节；主包内另附 `provenance/sample-games.json`。候选网站专用版本含站点限制，未采用；最终 Miniclip 版已实际离线进入第一关。FFDec 仅用于检查来源脚本，导出结果在忽略的 `outputs/` 中。

## 主机与构建

- ARM64 release 离线构建通过，播放器 SHA-256 为 `8e9e67855f3ea621f060e1378a79e85f4780d33f7624271502d7e7e131404b25`，AArch64 PIE，最高 GLIBC 2.38；输入清单与最终 ELF 绑定检查通过。
- 发布脚本 17 项回归通过：版本与来源锁、双附件与双校验值、缺合集拒绝发布、精确附件选择、草稿重试及已发布附件保留。
- 打包 8 项回归通过：959 个合集源文件与资源字节、三个游戏 ZIP 的内部路径/哈希、主包与合集的外层目录隔离、Bloxorz 原始字节与探针、缺失/额外/变更试玩文件拒绝打包。
- 实际执行打包脚本生成两个完整归档，主包约 24.5 MiB、合集约 218.4 MiB；最终归档指纹记录在本地输出目录的 `SHA256SUMS.txt`，不将产物、缓存或日志提交 Git。

本机受管环境的系统临时目录拒绝访问，测试改用工程 `build/tmp/` 后通过；未因此跳过用例。此轮没有更改核心代码，也未重复执行与本次打包改动无关的完整核心测试矩阵。

## Pocket1 隔离验证

使用上述 0.8.6 ELF 与同字节 Bloxorz，在 SD 卡 `ruffle-tests/release-0.8.6-20261103/` 下隔离启动，使用独立 HOME、配置、缓存、controls 与 saves。复用正式安装的 SDL/字体/音频依赖，没有覆盖正式应用，也没有创建 Apps 或正式 ROM 测试入口。

实际 OpenGL ES 启动通过：英文主菜单、鼠标点击 Start New Game、说明页、进入第一关；打开 M 菜单切换到方案 2，B 返回游戏后，十字右使方块滚动且 Moves 从 0 变为 1。再次打开暂停菜单后选择“保存并退出”，日志为 `progress_flushed=true`，退出码 0。截图与运行日志保存在本地 `outputs/release-0.8.6-device/`。

Bloxorz 此场景没有产生 SharedObject 数据（`has_data=false`），不据此声称已保存关卡或完整运行现场；可记下游戏显示的 Passcode 并用 Load Stage 继续。尚未验证全部关卡、分裂方块的 Space 操作、通关、高分联网、人工听感、Vulkan 或长时稳定性；也未进行 0.8.6 正式安装/Leaf 扫描验收。

测试前后比对正式应用、现有 Flash 游戏、配置及 SharedObject 存档文件的 SHA-256。原数据保持，测试目录位于 Apps / Roms 之外，无新增游戏索引；设备 Wi-Fi 省电恢复为测试前的 on。设备留存隔离测试文件，未删除已有安装或历史备份。

# 试玩与可选游戏合集

主安装包附英文 Bloxorz 和原创测试动画 `Leaf-GPU-Probe.swf`。可选的 `ruffle-leafos-games-<版本>.zip` 另含三款定制游戏。安装、旧版升级与按键操作见[使用指南](../README.md#安装与升级)。

## 主包试玩

`samples/Bloxorz-翻滚方块.swf` 为 Damien Clarke（Roboman / DX Interactive）的英文 Miniclip 分发版，收录于 2026-11-03，保留下载原字节。文件为 2,286,600 字节，SHA-256 为 `296c5c17b7a7b6529f6b8faa19aae78ebf4382c161c07e5070ab338fd8264803`。`samples/manifest.json` 固定来源与字节并由打包校验。[收录清单](samples/manifest.json) · [历史 SWF 镜像](https://locker.phinugamma.org/swf/miniclip/bloxorz_miniclip.swf) · [Bloxorz 第一关实机记录](../docs/validation/release-split-20261103.md)

## 可选合集

| 游戏 | 合集内启动文件 | 源文件 |
| --- | --- | --- |
| 拳皇 Wing 1.91 | `Roms/flash/拳皇Wing1.91-定制版.zip` | 掌机素材优化 SWF |
| 死神 VS 火影 3.3 | `Roms/flash/死神VS火影3.3-定制版.zip` | 掌机离线、脚本及素材优化版，含 LOW 启动入口修复 |
| 死神 VS 火影 3.6 | `Roms/flash/死神VS火影3.6-定制版.zip` | LOW / 30 FPS 独立定制目录版 |

拳皇 ZIP 内为单个 SWF，两款死神 ZIP 内为主 SWF 与分别 398 / 554 个资源。打包将仓库中的 `_assets/` 还原为归档内的 `assets/`，逐项校验源文件及归档内容。主 SWF 字节保持，原生进度仍按内容识别。[959 个源文件的清单与 SHA-256](manifest.json) · [正式 ZIP 启动及 Leaf 索引验证](../docs/validation/all-zip-list-20261103.md)

## 来源与处理范围

- **拳皇 Wing 1.91** 裁剪 6,620 张私有位图的透明外边，合并 109 张重复定义并补偿填充矩阵，保留脚本、时间轴与 34 FPS 帧头。当前主文件 SHA-256 为 `bb829289b03fff84aa981bcfed436a9e9ccc4100d018526a2fdeed3fd115ca19`。[拳皇 GLES 专项验证](../docs/validation/kof-gles-memory-20261031.md)
- **死神 3.3** 来自包含本体和资源 ZIP 的 4399 封装。提取的本体曾与独立镜像核对。定制版外置 398 个资源、停用原内嵌资源加载入口并重绑根类。四处脚本优化涉及计时数字、气量条、AI 范围及碰撞候选的临时对象，保留战斗判定。素材阶段改写 76 个资源 SWF。新进度默认 LOW，目标 30 FPS，闪光效果 15 FPS。已保存画质优先于默认值。当前主文件 SHA-256 为 `294e5e3c60e24fe01475c1fbb4dfc1f9aad1e23e4fbd62dd9849308f2fb020c0`。[独立镜像](https://github.com/naptestdev/bleach-vs-naruto-3.3) · [限定实机对战](../docs/validation/bvn33-pve-stutter-20261031.md)
- **死神 3.6** 基于原 AIR 分发，适配离线资源、存档、地图与启动布局，使用原版 Adobe TLF 供界面离线加载。原独立版有完整主 SWF、素材指纹、渲染像素对照，以及 Pocket1 / GLES 限定 CPU 对战、设置重开与退出记录。[原独立版验证](../docs/validation/bvn36-custom-20261031.md)

| 死神 3.3 来源阶段 | SHA-256 |
| --- | --- |
| 4399 封装包 | `2c85b1f5e963b1327c66b03374ed9162ecf81a70e3133ce5e284e4616e6e5a22` |
| 提取的本体 | `c10533a4491110816093c19861ee496e173eaf402818f4c58aaf5a94992b435d` |
| 完成离线入口适配 | `148592ba6fbffc79938660997ff8ae6d38c4f90d962208c46c41626199a9a4f5` |

死神仍可能出现脚本、动作帧或 MP3 告警。全部角色、地图、Vulkan、长时稳定性及人工听感待进一步验证。游戏、角色、图像、声音、脚本和 Adobe TLF 的权利属于原作者或相应权利方，沿用游戏内作者说明。[第三方声明](../docs/THIRD_PARTY.md)

## 开发工具

在 `ruffle-leafos/` 运行，输出应使用尚不存在的路径。素材优化器处理可安全重写的 FWS/CWS 私有无损位图和形状填充，默认限于 AVM1。AVM2 需评估主电影与外部调用者后显式启用 `--avm2-private-bitmaps`，并验证实际像素和填充变换。输出改动了 SWF 字节时会使用独立原生进度身份。[素材优化器](../scripts/optimize_swf_bitmaps.py)

```powershell
python scripts/optimize_swf_bitmaps.py <输入.swf> --output <新输出.swf> --report <新报告.json>
python -m unittest discover -s scripts -p test_optimize_swf_bitmaps.py -v
```

死神 3.3 的单文件实验恢复内嵌资源加载，并分次缓存启动特效。它有独立进度和按键身份，Pocket1 实验内存高于目录版。复现需要完成 ARM64 核心构建并配置 JDK 和配套 playerglobal。[单文件验证](../docs/validation/bvn33-single-swf-20261031.md) · [构建环境](../docs/build-release.md#工具链与首次准备)

```powershell
python scripts/package_bvn_single_swf.py --output "../outputs/死神VS火影3.3-定制单文件版.swf" --report ../outputs/bvn-single-report.json
python -m unittest discover -s scripts -p test_package_bvn_single_swf.py -v
```

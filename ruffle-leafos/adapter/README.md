# SDL 诊断与早期 Leaf 适配

正式播放器复用 `src/` 的 SDL 拦截、帧计时与物理按键采样。主包构建通过 `ruffle-leafos/scripts/build_diagnostics.py` 运行模拟 SDL 回归并生成 `libruffle_probe.so`。[正式包构建](../docs/build-release.md) · [渲染与诊断](../docs/rendering.md)

## 重建早期探针包

本目录的 `scripts/build.py` 与 `pak/` 用于原始 leaf-12 的对照包。它复用用户提供的原始播放器、库和字体，逐项指纹见 [runtime.lock.json](runtime.lock.json)。公开的 `aweigit/ruffle-miyooflip` 基线与原包的短提交和前端路径匹配。Leaf 私有修改只出现在原始二进制，来源关系及候选 Cargo.lock 指纹见 [player-source.lock.json](player-source.lock.json) 和 [第三方声明](THIRD_PARTY.md)。

从本目录运行，需要 Python、Node.js、Zig 0.14.1、BusyBox 或 `/bin/sh`、固定 Leaf 契约与原始运行时包：

```text
python scripts/build.py --runtime-pak <原ruffle.pak> --zig <zig.exe> --output <交付目录> --contract <固定leaf-contracts检出> --shell <BusyBox或sh> --node <node.exe>
```

可选 `--player-source` 验证候选源码的完整提交、工作树与 Cargo.lock。旧版使用说明和逐轮验收可按原路径从 Git 历史查阅，现行安装与操作见[使用指南](../README.md)。

# Ruffle Flash 播放器源码

本目录是 Miniloong Pocket 1 / leafOS 播放器的 Rust workspace。应用版本为 **0.8.3 开发预览**，共享核心、SWF/FLV、字符串、前端工具、视频及官方渲染模块的来源统一为官方 [Ruffle v0.7.1](https://github.com/ruffle-rs/ruffle/releases/tag/v0.7.1)，提交 `89a7049965f3090e706265670df309a5128233ac`；Vulkan 使用 wgpu 30。

Pocket1 前端保留在 `sdl2test-rocknix/`，包含 Leaf 启动、暂停与输入、十套按键方案、五份原生进度、中英文菜单、屏幕键盘、截图、数据管理及 GLES/Vulkan 选择。源码中的目录名是兼容标识，不能据此推断本项目已支持 ROCKNIX 或其他硬件。当前使用方法见[中文指南](../README.md)和 [English guide](../README.en.md)。

前端最初源自 `aweigit/ruffle-miyooflip@3bc39c5e0fdbc9fcde92681706395f6ef70f991b`，必要的 fork 和 Leaf 定制继续保留；该 fork 不再是共享核心基线。现存 `sdl2test-u`、`sdl2test-tb` 是保留的来源代码，未纳入当前设备支持和验收；旧的 Miyoo Flip 专属前端不再收录。来源和许可见 [leaf-source.lock.json](../leaf-source.lock.json)、[第三方声明](../docs/THIRD_PARTY.md)及 [LICENSE.md](LICENSE.md)。

从 `ruffle-leafos/` 工程目录使用锁定工具链构建，具体准备、缓存和发布步骤见[工程实现细节](../docs/implementation.md#构建环境与命令)：

```powershell
python scripts/build_player.py check --offline --isolated-config
python scripts/build_player.py build --offline --isolated-config
python scripts/test_session.py
python scripts/test_core.py
python scripts/test_renderer.py
python scripts/test_renderer.py --suite decode
python scripts/test_renderer.py --suite screenshot
python scripts/test_swf.py
```

`--offline` 要求先准备依赖。构建脚本生成并核验 ARM64 ELF 与实际源码输入绑定；不要直接用旧 ELF 和新来源锁打包。`build_player.py test --package ruffle_core --offline --isolated-config` 仅编译 ARM64 核心测试，不能当作测试执行成功。此次升级的构建、主机回归和实机待验边界集中在[稳定版升级记录](../docs/validation/ruffle-stable-20261102.md)。

## English

This workspace builds Ruffle Flash for **Miniloong Pocket 1 / leafOS**. The application remains 0.8.3 development preview. Shared Ruffle modules and the official renderer are pinned together to **Ruffle v0.7.1**, commit `89a7049965f3090e706265670df309a5128233ac`, with wgpu 30.

The Leaf frontend remains in `sdl2test-rocknix/`, preserving its controls, pause behavior, five native progress slots, bilingual menus, on-screen keyboard, screenshots, data management and GLES/Vulkan presentation. Its original `ruffle-miyooflip` ancestry remains credited; it no longer supplies the current shared-core baseline. Other retained SDL frontends are source references, not additional supported targets.

Run the commands above from `ruffle-leafos/` after preparing the pinned dependencies. See the [build and implementation guide](../docs/implementation.md), [user guide](../README.en.md) and [upgrade validation record](../docs/validation/ruffle-stable-20261102.md). Cross-compilation and host tests do not establish on-device acceptance.

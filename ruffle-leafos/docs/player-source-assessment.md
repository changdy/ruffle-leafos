# leaf-12 源码追踪

2026-10-23 已检出公开 [ruffle-miyooflip 完整提交](https://github.com/aweigit/ruffle-miyooflip/tree/3bc39c5e0fdbc9fcde92681706395f6ef70f991b)。这是本次新获得的资料，旧需求里“短提交未绑定仓库”已推进到可追溯的基础源码候选。

完整 SHA 为 3bc39c5e0fdbc9fcde92681706395f6ef70f991b；包含 sdl2test-rocknix/src/main.rs、gl_backend.rs、keycodes.rs、navigator.rs、ui_backend.rs。Cargo.lock 的 SHA-256 为 c0df2d92702e187a07dd469f7ea58cde731c18ee1941fa8b35d56a4439736404。

与原样本匹配的证据是：build-manifest 的短提交、二进制内的前端路径、Glow/GLES2 初始化路线、软件视频和按游戏 cfg 的结构。仍有明确差异：公开源码没有原包的 RUFFLE_* 环境开关，也没有 Leaf 文件浏览器、中文控制菜单和 glFinish/首帧读回修补。因此不能宣称获得了原 leaf-12 的全部构建输入或能字节一致重建。

公开候选 main.rs 中可确认：请求 GLES 2.0 context；以 DiskStorageBackend::new(PathBuf::from("./")) 接存储；按所选 SWF 路径生成 file URL。keycodes.rs 先查游戏旁 cfg，再查工作目录 keymap/<stem>.cfg。这些结论只属于检出的基础源码，不外推到已被二次修改的二进制。

后续应从该固定版本建立 Leaf 前端，保留原包为对照，逐项重做显示修补、Menu、路径和保存；以相同 SWF 真机画面验证后才能换掉原播放器。本次安装包仍使用 hash 固定的原 binary。

工具链检查使用独立工作目录中的 Rust 1.94.0（Windows GNU host、AArch64 GNU target）、Zig 0.14.1、JDK 17；依赖由 Cargo.lock 固定。交叉检查与 ARM64 链接/上机是不同关口，不以 cargo check 代替完整构建。

本次完整候选 cargo check 未通过：Windows GNU 主机依赖已推进，当前停止于 ring 的 AArch64 C 编译；cc-rs 传入的四段目标 triple 与 Zig 的三段目标表示冲突。单独尝试 Zig cc 时还遇到沙箱临时对象 rename Permission denied。本次可安装库采用已验证的 raw -cc1/LLD 路线构建，不受该候选播放器构建失败影响。失败日志单独交付，未将编译状态记成通过。下一步应修正 cc-rs 工具适配，或使用 mlp1 SDK 的 Linux 构建环境。

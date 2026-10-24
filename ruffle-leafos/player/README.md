# Ruffle - Flash Player 模拟器 (掌机版本)

[English](#english)

基于 [Ruffle](https://github.com/ruffle-rs/ruffle) 的 Flash Player 模拟器，专门为掌机设备（Trimui Brick、Miyoo Flip）和桌面 Linux 优化的 SDL2 前端版本。

## 项目特性

- 基于 SDL2 + OpenGLESv2 渲染
- 支持手柄控制和自定义按键映射
- 针对 ARM64 掌机设备优化

## 构建指南

### 前置依赖

- Java
- SDL2
- OpenGLESv2
- 交叉编译工具链 (用于 ARM64 目标)


### 构建目标

#### 1. x64 Ubuntu 20 (桌面版本)

```bash
cargo build --release -p sdl2test-u
```


#### 2. Trimui Brick (ARM64)

```bash
export PKG_CONFIG_SYSROOT_DIR="/lib-trimui"
export CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER=aarch64-none-linux-gnu-gcc
export RUSTFLAGS="-C link-arg=-L/lib-trimui -C link-arg=-Wl,-rpath-link=/lib-trimui"
cargo build --release --target aarch64-unknown-linux-gnu -p sdl2test-tb
```

#### 3. Miyoo Flip (ARM64)

```bash
export PKG_CONFIG_SYSROOT_DIR="/lib-flip"
export CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER=aarch64-none-linux-gnu-gcc
export RUSTFLAGS="-C link-arg=-L/lib-flip -C link-arg=-Wl,-rpath-link=/lib-flip"
cargo build --release --target aarch64-unknown-linux-gnu -p sdl2test-flip
```

#### 4. ROCKNIX / Wayland (ARM64)

适用于运行 sway 等 Wayland 合成器的 ROCKNIX 系统，使用 SDL2 + OpenGL ES 显示，不占用 DRM Master。

```bash
export TOOLCHAIN=/path/to/build.ROCKNIX-*/toolchain
export SYSROOT=$TOOLCHAIN/aarch64-rocknix-linux-gnu/sysroot
export PATH=$TOOLCHAIN/bin:$PATH
export PKG_CONFIG_SYSROOT_DIR=$SYSROOT
export PKG_CONFIG_PATH=$SYSROOT/usr/lib/pkgconfig
export CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER=aarch64-rocknix-linux-gnu-gcc
export CC_aarch64_unknown_linux_gnu=aarch64-rocknix-linux-gnu-gcc
export RUSTFLAGS="-C link-arg=-Wl,-rpath-link,$SYSROOT/usr/lib"
cargo build --release --target aarch64-unknown-linux-gnu -p sdl2test-rocknix
```

端口 `launch.sh` 需设置 Wayland 环境变量，可参考 `sdl2test-rocknix/launch.sh`。

## 手柄按键映射

### 默认映射

| 手柄按键 | 键盘按键 |
|----------|----------|
| A | S |
| B | A |
| X | J |
| Y | U |
| L | A |
| R | D |
| L2 | W |
| R2 | S |
| L3 | O |
| R3 | L |
| Start | I |
| Select | K |

### 组合键

| 组合键 | 功能 |
|--------|------|
| Select + X | 切换鼠标模式 / 键盘模式 |
| Select + A | 切换十字方向键为 WASD 模式（`sdl2test-rocknix`；cfg 中已配置十字键映射时无效） |
| Select + Start | 切换十字方向键为 WASD 模式（`sdl2test-flip` / `sdl2test-tb` / `sdl2test-u`；cfg 中已配置十字键映射时无效） |
| Guide | 退出游戏 |

### 鼠标模式

游戏启动后**默认进入鼠标模式**。

在鼠标模式下：
- 十字方向键控制鼠标移动
- Y 键作为鼠标左键点击
- L2 / R2 仍按 keymap 映射为键盘按键

键盘模式下，手柄按键按 keymap 映射为键盘输入。十字键映射仅在键盘模式下生效；cfg 中配置了十字键后，以 cfg 为准。

## 自定义按键映射

### 映射文件位置

按以下顺序查找映射文件（以 SWF 文件名为 `{游戏名}`）：

1. **与 SWF 同目录**：`{游戏名}.cfg`（优先）
2. **当前目录下的 keymap 文件夹**：`keymap/{游戏名}.cfg`

示例：

```
/roms/ports/flash/
├── game.swf
├── game.cfg          # 优先使用
└── keymap/
    └── game.cfg      # 同目录不存在时使用
```

### 映射文件格式

每行一条映射，格式为 `按钮名=键名`，`#` 开头为注释：

```ini
# 这是注释
A=S
B=A
X=J
Y=U
L=A
R=D
L2=W
R2=S
L3=O
R3=L
Start=I
Select=K
Up=W
Down=S
Left=A
Right=D
```

### 支持的按钮名

- `A`, `B`, `X`, `Y`
- `Up`, `Down`, `Left`, `Right`（十字键；也可写 `DPadUp` 等）
- `L`, `R`
- `L2`, `R2`
- `L3`, `R3`
- `Start`, `Select`

cfg 中配置了任意十字键映射后，`Select + A` / `Select + Start` 的 WASD 切换将不再改变十字键行为。

### 支持的键名

- 字母: `A` - `Z`
- 数字: `Num0` - `Num9`
- 方向键: `Up`, `Down`, `Left`, `Right`
- 功能键: `F1` - `F12`
- 特殊键: `Space`, `Return`, `Escape`, `Tab`, `Backspace`, `Delete`, `Insert`, `Home`, `End`, `PageUp`, `PageDown`

## 使用示例

```bash
# 桌面版（W*H 为屏幕分辨率）
./sdl2test-u game.swf -W 640 -H 480

# ROCKNIX / Wayland（-W/-H 可选，省略时使用桌面分辨率）
./sdl2test-rocknix game.swf --fullscreen
```

ROCKNIX 端口可参考 `sdl2test-rocknix/launch.sh` 设置 Wayland 环境变量。


## 许可证

本项目基于 Ruffle，采用以下许可证之一：

- Apache License, Version 2.0
- MIT License

详见 [LICENSE.md](LICENSE.md)

## 致谢

基于 [Ruffle](https://github.com/ruffle-rs/ruffle) 项目开发。

---

## English

# Ruffle - Flash Player Emulator (Handheld Version)

An Flash Player emulator based on [Ruffle](https://github.com/ruffle-rs/ruffle), specifically optimized SDL2 frontend for handheld devices (Trimui Brick, Miyoo Flip) and desktop Linux.

## Features

- SDL2 + OpenGLESv2 rendering
- Gamepad control with customizable key mapping
- Optimized for ARM64 handheld devices

## Build Guide

### Prerequisites

- Java
- SDL2
- OpenGLESv2
- Cross-compilation toolchain (for ARM64 targets)


### Build Targets

#### 1. x64 Ubuntu 20 (Desktop Version)

```bash
cargo build --release -p sdl2test-u
```


#### 2. Trimui Brick (ARM64)

```bash
export PKG_CONFIG_SYSROOT_DIR="/lib-trimui"
export CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER=aarch64-none-linux-gnu-gcc
export RUSTFLAGS="-C link-arg=-L/lib-trimui -C link-arg=-Wl,-rpath-link=/lib-trimui"
cargo build --release --target aarch64-unknown-linux-gnu -p sdl2test-tb
```

#### 3. Miyoo Flip (ARM64)

```bash
export PKG_CONFIG_SYSROOT_DIR="/lib-flip"
export CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER=aarch64-none-linux-gnu-gcc
export RUSTFLAGS="-C link-arg=-L/lib-flip -C link-arg=-Wl,-rpath-link=/lib-flip"
cargo build --release --target aarch64-unknown-linux-gnu -p sdl2test-flip
```

#### 4. ROCKNIX / Wayland (ARM64)

For ROCKNIX systems running sway or other Wayland compositors. Uses SDL2 + OpenGL ES instead of direct DRM/KMS.

```bash
export TOOLCHAIN=/path/to/build.ROCKNIX-*/toolchain
export SYSROOT=$TOOLCHAIN/aarch64-rocknix-linux-gnu/sysroot
export PATH=$TOOLCHAIN/bin:$PATH
export PKG_CONFIG_SYSROOT_DIR=$SYSROOT
export PKG_CONFIG_PATH=$SYSROOT/usr/lib/pkgconfig
export CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER=aarch64-rocknix-linux-gnu-gcc
export CC_aarch64_unknown_linux_gnu=aarch64-rocknix-linux-gnu-gcc
export RUSTFLAGS="-C link-arg=-Wl,-rpath-link,$SYSROOT/usr/lib"
cargo build --release --target aarch64-unknown-linux-gnu -p sdl2test-rocknix
```

See `sdl2test-rocknix/launch.sh` for required Wayland environment variables.

## Gamepad Button Mapping

### Default Mapping

| Gamepad Button | Keyboard Key |
|----------------|--------------|
| A | S |
| B | A |
| X | J |
| Y | U |
| L | A |
| R | D |
| L2 | W |
| R2 | S |
| L3 | O |
| R3 | L |
| Start | I |
| Select | K |

### Combo Keys

| Combo Key | Function |
|-----------|----------|
| Select + X | Toggle mouse mode / keyboard mode |
| Select + A | Toggle D-pad to WASD mode (`sdl2test-rocknix`; disabled when cfg defines D-pad mappings) |
| Select + Start | Toggle D-pad to WASD mode (`sdl2test-flip` / `sdl2test-tb` / `sdl2test-u`; disabled when cfg defines D-pad mappings) |
| Guide | Quit game |

### Mouse Mode

Games **start in mouse mode by default**.

In mouse mode:
- D-pad controls mouse movement
- Y button acts as left mouse click
- L2 / R2 still map to keyboard keys via keymap

In keyboard mode, gamepad buttons are mapped to keyboard input via keymap. D-pad mappings apply in keyboard mode only; when cfg defines D-pad keys, cfg takes precedence.

## Custom Button Mapping

### Mapping File Location

Mapping files are looked up in this order (where `{name}` is the SWF file stem):

1. **Same directory as the SWF**: `{name}.cfg` (preferred)
2. **`keymap` folder relative to cwd**: `keymap/{name}.cfg`

Example:

```
/roms/ports/flash/
├── game.swf
├── game.cfg          # used first
└── keymap/
    └── game.cfg      # fallback if same-dir file is missing
```

### Mapping File Format

Each line is one mapping in the format `ButtonName=KeyName`, lines starting with `#` are comments:

```ini
# This is a comment
A=S
B=A
X=J
Y=U
L=A
R=D
L2=W
R2=S
L3=O
R3=L
Start=I
Select=K
Up=W
Down=S
Left=A
Right=D
```

### Supported Button Names

- `A`, `B`, `X`, `Y`
- `Up`, `Down`, `Left`, `Right` (D-pad; aliases `DPadUp`, etc.)
- `L`, `R`
- `L2`, `R2`
- `L3`, `R3`
- `Start`, `Select`

When cfg defines any D-pad mapping, `Select + A` / `Select + Start` no longer changes D-pad behavior.

### Supported Key Names

- Letters: `A` - `Z`
- Numbers: `Num0` - `Num9`
- Arrow keys: `Up`, `Down`, `Left`, `Right`
- Function keys: `F1` - `F12`
- Special keys: `Space`, `Return`, `Escape`, `Tab`, `Backspace`, `Delete`, `Insert`, `Home`, `End`, `PageUp`, `PageDown`

## Usage Example

```bash
# Desktop version (W*H is the screen resolution)
./sdl2test-u game.swf -W 640 -H 480

# ROCKNIX / Wayland (-W/-H optional; uses desktop resolution when omitted)
./sdl2test-rocknix game.swf --fullscreen
```

For ROCKNIX ports, see `sdl2test-rocknix/launch.sh` for required Wayland environment variables.

## License

This project is based on Ruffle and is licensed under one of the following:

- Apache License, Version 2.0
- MIT License

See [LICENSE.md](LICENSE.md) for details.

## Acknowledgments

Based on the [Ruffle](https://github.com/ruffle-rs/ruffle) project.

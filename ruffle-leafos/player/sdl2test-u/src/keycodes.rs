use sdl2::keyboard::Keycode;
use sdl2::controller::{Button, Axis};
use ruffle_core::events::{KeyDescriptor, KeyLocation, LogicalKey, NamedKey, PhysicalKey};

const DEAD_ZONE:f32   =   0.15;   // 摇杆径向死区（0.0 ～ 1.0）
const SENSITIVITY:f32  =  3.0;    // 每帧最大移动像素（越高越快）
const TRIGGER_PRESS_THRESHOLD:i16 = 0x7000;  // 按下阈值
const TRIGGER_RELEASE_THRESHOLD:i16 = 0x5000;  // 释放阈值（必须 <= PRESS）

const SOURCE_WIDTH:i16 = 640;
const SOURCE_HEIGHT:i16 = 480;

pub fn sdl2_keycode_to_ruffle_key_descriptor(key: Keycode) -> Option<KeyDescriptor> {
    // TODO: Maybe do something with `android.scan_code()`?
    let physical_key = match key {
        Keycode::Up => PhysicalKey::ArrowUp,
        Keycode::Down => PhysicalKey::ArrowDown,
        Keycode::Left => PhysicalKey::ArrowLeft,
        Keycode::Right => PhysicalKey::ArrowRight,
        Keycode::A => PhysicalKey::KeyA,
        Keycode::B => PhysicalKey::KeyB,
        Keycode::C => PhysicalKey::KeyC,
        Keycode::D => PhysicalKey::KeyD,
        Keycode::E => PhysicalKey::KeyE,
        Keycode::F => PhysicalKey::KeyF,
        Keycode::G => PhysicalKey::KeyG,
        Keycode::H => PhysicalKey::KeyH,
        Keycode::I => PhysicalKey::KeyI,
        Keycode::J => PhysicalKey::KeyJ,
        Keycode::K => PhysicalKey::KeyK,
        Keycode::L => PhysicalKey::KeyL,
        Keycode::M => PhysicalKey::KeyM,
        Keycode::N => PhysicalKey::KeyN,
        Keycode::O => PhysicalKey::KeyO,
        Keycode::P => PhysicalKey::KeyP,
        Keycode::Q => PhysicalKey::KeyQ,
        Keycode::R => PhysicalKey::KeyR,
        Keycode::S => PhysicalKey::KeyS,
        Keycode::T => PhysicalKey::KeyT,
        Keycode::U => PhysicalKey::KeyU,
        Keycode::V => PhysicalKey::KeyV,
        Keycode::W => PhysicalKey::KeyW,
        Keycode::X => PhysicalKey::KeyX,
        Keycode::Y => PhysicalKey::KeyY,
        Keycode::Z => PhysicalKey::KeyZ,
        Keycode::Num0 => PhysicalKey::Digit0,
        Keycode::Num1 => PhysicalKey::Digit1,
        Keycode::Num2 => PhysicalKey::Digit2,
        Keycode::Num3 => PhysicalKey::Digit3,
        Keycode::Num4 => PhysicalKey::Digit4,
        Keycode::Num5 => PhysicalKey::Digit5,
        Keycode::Num6 => PhysicalKey::Digit6,
        Keycode::Num7 => PhysicalKey::Digit7,
        Keycode::Num8 => PhysicalKey::Digit8,
        Keycode::Num9 => PhysicalKey::Digit9,
        Keycode::Space => PhysicalKey::Space,
        Keycode::LAlt => PhysicalKey::AltLeft,
        Keycode::LCtrl =>  PhysicalKey::ControlLeft,
        Keycode::Backspace => PhysicalKey::Backspace,

        _ => PhysicalKey::Unknown,
    };

    let logical_key = match key {
        Keycode::Up => LogicalKey::Named(NamedKey::ArrowUp),
        Keycode::Down => LogicalKey::Named(NamedKey::ArrowDown),
        Keycode::Left => LogicalKey::Named(NamedKey::ArrowLeft),
        Keycode::Right => LogicalKey::Named(NamedKey::ArrowRight),
        Keycode::Num0 => LogicalKey::Character('0'),
        Keycode::Num1 => LogicalKey::Character('1'),
        Keycode::Num2 => LogicalKey::Character('2'),
        Keycode::Num3 => LogicalKey::Character('3'),
        Keycode::Num4 => LogicalKey::Character('4'),
        Keycode::Num5 => LogicalKey::Character('5'),
        Keycode::Num6 => LogicalKey::Character('6'),
        Keycode::Num7 => LogicalKey::Character('7'),
        Keycode::Num8 => LogicalKey::Character('8'),
        Keycode::Num9 => LogicalKey::Character('9'),
        Keycode::A => LogicalKey::Character('a'),
        Keycode::B => LogicalKey::Character('b'),
        Keycode::C => LogicalKey::Character('c'),
        Keycode::D => LogicalKey::Character('d'),
        Keycode::E => LogicalKey::Character('e'),
        Keycode::F => LogicalKey::Character('f'),
        Keycode::G => LogicalKey::Character('g'),
        Keycode::H => LogicalKey::Character('h'),
        Keycode::I => LogicalKey::Character('i'),
        Keycode::J => LogicalKey::Character('j'),
        Keycode::K => LogicalKey::Character('k'),
        Keycode::L => LogicalKey::Character('l'),
        Keycode::M => LogicalKey::Character('m'),
        Keycode::N => LogicalKey::Character('n'),
        Keycode::O => LogicalKey::Character('o'),
        Keycode::P => LogicalKey::Character('p'),
        Keycode::Q => LogicalKey::Character('q'),
        Keycode::R => LogicalKey::Character('r'),
        Keycode::S => LogicalKey::Character('s'),
        Keycode::T => LogicalKey::Character('t'),
        Keycode::U => LogicalKey::Character('u'),
        Keycode::V => LogicalKey::Character('v'),
        Keycode::W => LogicalKey::Character('w'),
        Keycode::X => LogicalKey::Character('x'),
        Keycode::Y => LogicalKey::Character('y'),
        Keycode::Z => LogicalKey::Character('z'),
        Keycode::Comma => LogicalKey::Character(','),
        Keycode::Period => LogicalKey::Character('.'),
        Keycode::LAlt => LogicalKey::Named(NamedKey::Alt),
        Keycode::RAlt => LogicalKey::Named(NamedKey::Alt),
        Keycode::LShift => LogicalKey::Named(NamedKey::Shift),
        Keycode::RShift => LogicalKey::Named(NamedKey::Shift),
        Keycode::Tab => LogicalKey::Named(NamedKey::Tab),
        Keycode::Space => LogicalKey::Character(' '),
        Keycode::Return => LogicalKey::Named(NamedKey::Enter),
        Keycode::Backspace => LogicalKey::Named(NamedKey::Backspace),
        Keycode::Backquote => LogicalKey::Character('`'),
        Keycode::Minus => LogicalKey::Character('-'),
        Keycode::Equals => LogicalKey::Character('='),
        Keycode::LeftBracket => LogicalKey::Character('['),
        Keycode::RightBracket => LogicalKey::Character(']'),
        Keycode::Backslash => LogicalKey::Character('\\'),
        Keycode::Semicolon => LogicalKey::Character(';'),
        Keycode::Quote => LogicalKey::Character('\''),
        Keycode::Slash => LogicalKey::Character('/'),
        Keycode::Plus => LogicalKey::Character('+'),
        Keycode::PageUp => LogicalKey::Named(NamedKey::PageUp),
        Keycode::PageDown => LogicalKey::Named(NamedKey::PageDown),
        Keycode::Escape => LogicalKey::Named(NamedKey::Escape),
        Keycode::Delete => LogicalKey::Named(NamedKey::Delete),
        Keycode::LCtrl => LogicalKey::Named(NamedKey::Control),
        Keycode::RCtrl => LogicalKey::Named(NamedKey::Control),
        Keycode::CapsLock => LogicalKey::Named(NamedKey::CapsLock),
        Keycode::ScrollLock => LogicalKey::Named(NamedKey::ScrollLock),
        Keycode::Pause => LogicalKey::Named(NamedKey::Pause),
        Keycode::Home => LogicalKey::Named(NamedKey::Home),
        Keycode::End => LogicalKey::Named(NamedKey::End),
        Keycode::Insert => LogicalKey::Named(NamedKey::Insert),
        Keycode::F1 => LogicalKey::Named(NamedKey::F1),
        Keycode::F2 => LogicalKey::Named(NamedKey::F2),
        Keycode::F3 => LogicalKey::Named(NamedKey::F3),
        Keycode::F4 => LogicalKey::Named(NamedKey::F4),
        Keycode::F5 => LogicalKey::Named(NamedKey::F5),
        Keycode::F6 => LogicalKey::Named(NamedKey::F6),
        Keycode::F7 => LogicalKey::Named(NamedKey::F7),
        Keycode::F8 => LogicalKey::Named(NamedKey::F8),
        Keycode::F9 => LogicalKey::Named(NamedKey::F9),
        Keycode::F10 => LogicalKey::Named(NamedKey::F10),
        Keycode::F11 => LogicalKey::Named(NamedKey::F11),
        Keycode::F12 => LogicalKey::Named(NamedKey::F12),
        Keycode::NumLockClear => LogicalKey::Named(NamedKey::NumLock),
        Keycode::Kp0 => LogicalKey::Character('0'),
        Keycode::Kp1 => LogicalKey::Character('1'),
        Keycode::Kp2 => LogicalKey::Character('2'),
        Keycode::Kp3 => LogicalKey::Character('3'),
        Keycode::Kp4 => LogicalKey::Character('4'),
        Keycode::Kp5 => LogicalKey::Character('5'),
        Keycode::Kp6 => LogicalKey::Character('6'),
        Keycode::Kp7 => LogicalKey::Character('7'),
        Keycode::Kp8 => LogicalKey::Character('8'),
        Keycode::Kp9 => LogicalKey::Character('9'),
        Keycode::KpDivide => LogicalKey::Character('/'),
        Keycode::KpMultiply => LogicalKey::Character('*'),
        Keycode::KpMinus => LogicalKey::Character('-'),
        Keycode::KpPlus => LogicalKey::Character('+'),
        Keycode::KpPeriod => LogicalKey::Character('.'),
        Keycode::KpComma => LogicalKey::Character(','),
        Keycode::KpEnter => LogicalKey::Named(NamedKey::Enter),
        Keycode::KpEquals => LogicalKey::Character('='),
        _ => return None,
    };

    let key_location = match key {
        Keycode::LAlt | Keycode::LShift | Keycode::LCtrl => {
            KeyLocation::Left
        }

        Keycode::RAlt | Keycode::RShift | Keycode::RCtrl => {
            KeyLocation::Right
        }

        Keycode::NumLockClear
        | Keycode::Kp0
        | Keycode::Kp1
        | Keycode::Kp2
        | Keycode::Kp3
        | Keycode::Kp4
        | Keycode::Kp5
        | Keycode::Kp6
        | Keycode::Kp7
        | Keycode::Kp8
        | Keycode::Kp9
        | Keycode::KpDivide
        | Keycode::KpMultiply
        | Keycode::KpMinus
        | Keycode::KpPlus
        | Keycode::KpPeriod
        | Keycode::KpComma
        | Keycode::KpEnter
        | Keycode::KpEquals
        | Keycode::KpLeftParen
        | Keycode::KpRightParen => KeyLocation::Numpad,

        _ => KeyLocation::Standard,
    };

    Some(KeyDescriptor {
        physical_key,
        logical_key,
        key_location,
    })
}

#[derive(Default)]
pub struct GameControllerState {
    // pub a: bool,
    // pub b: bool,
    // pub x: bool,
    // pub y: bool,
    // start: bool,
    // back: bool,
    // left_bumper: bool,
    // right_bumper: bool,


    dpad_up: bool,
    dpad_down: bool,
    dpad_left: bool,
    dpad_right: bool,

    //扳机状态
    left_trigger_active: bool,
    right_trigger_active: bool,
    
    
    // 摇杆位置 (-32768 到 32767)
    // left_stick_x: i16,
    // left_stick_y: i16,
    // right_stick_x: i16,
    // right_stick_y: i16,
    // // 扳机 (0 到 32767)
    // left_trigger: u16,
    // right_trigger: u16,
    // joymouse_x: i16,
    // joymouse_y: i16,
}


pub fn sdl2_gamebtn_2_desc(button: Button, dpad2wasd:bool)-> Keycode {
    if dpad2wasd {
        match button {
            Button::A => Keycode::A,
            Button::B => Keycode::S,
            Button::X => Keycode::U,
            Button::Y => Keycode::J,
            Button::DPadUp => Keycode::W,
            Button::DPadDown => Keycode::S,
            Button::DPadLeft => Keycode::A,
            Button::DPadRight => Keycode::D,
            Button::LeftShoulder => Keycode::A,//L1
            Button::RightShoulder => Keycode::D,//R1
            Button::Paddle1 => Keycode::W,//L2
            Button::Paddle2 => Keycode::S,//R2
            Button::LeftStick => Keycode::O,//L3
            Button::RightStick => Keycode::L,//R3
            Button::Start => Keycode::I,
            Button::Back => Keycode::K,
            _ => Keycode::Num5,
        }
    }
    else {
        match button {
            Button::A => Keycode::A,
            Button::B => Keycode::S,
            Button::X => Keycode::U,
            Button::Y => Keycode::J,
            Button::DPadUp => Keycode::Up,
            Button::DPadDown => Keycode::Down,
            Button::DPadLeft => Keycode::Left,
            Button::DPadRight => Keycode::Right,
            Button::LeftShoulder => Keycode::A,
            Button::RightShoulder => Keycode::D,
            Button::Paddle1 => Keycode::W,
            Button::Paddle2 => Keycode::S,
            Button::LeftStick => Keycode::O,
            Button::RightStick => Keycode::L,
            Button::Start => Keycode::I,
            Button::Back => Keycode::K,
            _ => Keycode::Num5,
        }
    }    
}

use std::collections::HashMap;
use std::fs;
use std::path::Path;

pub struct KeyMap {
    mappings: HashMap<Button, Keycode>,
}

impl KeyMap {
    pub fn empty() -> Self {
        Self {
            mappings: HashMap::new(),
        }
    }

    pub fn has_custom_dpad(&self) -> bool {
        [
            Button::DPadUp,
            Button::DPadDown,
            Button::DPadLeft,
            Button::DPadRight,
        ]
        .into_iter()
        .any(|button| self.mappings.contains_key(&button))
    }

    pub fn button_to_keycode(&self, button: Button, dpad2wasd: bool) -> Keycode {
        if let Some(&kc) = self.mappings.get(&button) {
            return kc;
        }
        let dpad2wasd = if Self::is_dpad(button) && self.has_custom_dpad() {
            false
        } else {
            dpad2wasd
        };
        sdl2_gamebtn_2_desc(button, dpad2wasd)
    }

    fn is_dpad(button: Button) -> bool {
        matches!(
            button,
            Button::DPadUp | Button::DPadDown | Button::DPadLeft | Button::DPadRight
        )
    }

    pub fn load<P: AsRef<Path>>(path: P) -> Option<Self> {
        let content = fs::read_to_string(path.as_ref()).ok()?;
        let mut mappings = HashMap::new();

        for line in content.lines() {
            let line = line.trim();
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            if let Some((btn_str, key_str)) = line.split_once('=') {
                let btn_str = btn_str.trim();
                let key_str = key_str.trim();
                if let Some(button) = parse_button(btn_str) {
                    if let Some(keycode) = parse_keycode(key_str) {
                        mappings.insert(button, keycode);
                    } else {
                        log::warn!("Unknown key name in keymap: {}", key_str);
                    }
                } else {
                    log::warn!("Unknown button name in keymap: {}", btn_str);
                }
            }
        }

        if mappings.is_empty() {
            return None;
        }
        Some(Self { mappings })
    }
}

fn parse_button(s: &str) -> Option<Button> {
    match s {
        //由于掌机手柄的X,Y和A,B是反的，这里直接交换了映射
        "A" => Some(Button::B),
        "B" => Some(Button::A),
        "X" => Some(Button::Y),
        "Y" => Some(Button::X),
        "L" => Some(Button::LeftShoulder),
        "R" => Some(Button::RightShoulder),
        "L2" => Some(Button::Paddle1),
        "R2" => Some(Button::Paddle2),
        "L3" => Some(Button::LeftStick),
        "R3" => Some(Button::RightStick),
        "Start" => Some(Button::Start),
        "Select" => Some(Button::Back),
        "Up" | "DPadUp" => Some(Button::DPadUp),
        "Down" | "DPadDown" => Some(Button::DPadDown),
        "Left" | "DPadLeft" => Some(Button::DPadLeft),
        "Right" | "DPadRight" => Some(Button::DPadRight),
        _ => None,
    }
}

fn parse_keycode(s: &str) -> Option<Keycode> {
    match s {
        "A" => Some(Keycode::A),
        "B" => Some(Keycode::B),
        "C" => Some(Keycode::C),
        "D" => Some(Keycode::D),
        "E" => Some(Keycode::E),
        "F" => Some(Keycode::F),
        "G" => Some(Keycode::G),
        "H" => Some(Keycode::H),
        "I" => Some(Keycode::I),
        "J" => Some(Keycode::J),
        "K" => Some(Keycode::K),
        "L" => Some(Keycode::L),
        "M" => Some(Keycode::M),
        "N" => Some(Keycode::N),
        "O" => Some(Keycode::O),
        "P" => Some(Keycode::P),
        "Q" => Some(Keycode::Q),
        "R" => Some(Keycode::R),
        "S" => Some(Keycode::S),
        "T" => Some(Keycode::T),
        "U" => Some(Keycode::U),
        "V" => Some(Keycode::V),
        "W" => Some(Keycode::W),
        "X" => Some(Keycode::X),
        "Y" => Some(Keycode::Y),
        "Z" => Some(Keycode::Z),
        "Num0" => Some(Keycode::Num0),
        "Num1" => Some(Keycode::Num1),
        "Num2" => Some(Keycode::Num2),
        "Num3" => Some(Keycode::Num3),
        "Num4" => Some(Keycode::Num4),
        "Num5" => Some(Keycode::Num5),
        "Num6" => Some(Keycode::Num6),
        "Num7" => Some(Keycode::Num7),
        "Num8" => Some(Keycode::Num8),
        "Num9" => Some(Keycode::Num9),
        "Up" => Some(Keycode::Up),
        "Down" => Some(Keycode::Down),
        "Left" => Some(Keycode::Left),
        "Right" => Some(Keycode::Right),
        "Space" => Some(Keycode::Space),
        "Return" => Some(Keycode::Return),
        "Enter" => Some(Keycode::Return),
        "Escape" => Some(Keycode::Escape),
        "Esc" => Some(Keycode::Escape),
        "Tab" => Some(Keycode::Tab),
        "Backspace" => Some(Keycode::Backspace),
        "Delete" => Some(Keycode::Delete),
        "LCtrl" => Some(Keycode::LCtrl),
        "RCtrl" => Some(Keycode::RCtrl),
        "LShift" => Some(Keycode::LShift),
        "RShift" => Some(Keycode::RShift),
        "LAlt" => Some(Keycode::LAlt),
        "RAlt" => Some(Keycode::RAlt),
        "F1" => Some(Keycode::F1),
        "F2" => Some(Keycode::F2),
        "F3" => Some(Keycode::F3),
        "F4" => Some(Keycode::F4),
        "F5" => Some(Keycode::F5),
        "F6" => Some(Keycode::F6),
        "F7" => Some(Keycode::F7),
        "F8" => Some(Keycode::F8),
        "F9" => Some(Keycode::F9),
        "F10" => Some(Keycode::F10),
        "F11" => Some(Keycode::F11),
        "F12" => Some(Keycode::F12),
        "Comma" => Some(Keycode::Comma),
        "Period" => Some(Keycode::Period),
        "Slash" => Some(Keycode::Slash),
        "Backslash" => Some(Keycode::Backslash),
        "Semicolon" => Some(Keycode::Semicolon),
        "Quote" => Some(Keycode::Quote),
        "LeftBracket" => Some(Keycode::LeftBracket),
        "RightBracket" => Some(Keycode::RightBracket),
        "Minus" => Some(Keycode::Minus),
        "Equals" => Some(Keycode::Equals),
        "Backquote" => Some(Keycode::Backquote),
        "Kp0" => Some(Keycode::Kp0),
        "Kp1" => Some(Keycode::Kp1),
        "Kp2" => Some(Keycode::Kp2),
        "Kp3" => Some(Keycode::Kp3),
        "Kp4" => Some(Keycode::Kp4),
        "Kp5" => Some(Keycode::Kp5),
        "Kp6" => Some(Keycode::Kp6),
        "Kp7" => Some(Keycode::Kp7),
        "Kp8" => Some(Keycode::Kp8),
        "Kp9" => Some(Keycode::Kp9),
        "KpDivide" => Some(Keycode::KpDivide),
        "KpMultiply" => Some(Keycode::KpMultiply),
        "KpMinus" => Some(Keycode::KpMinus),
        "KpPlus" => Some(Keycode::KpPlus),
        "KpPeriod" => Some(Keycode::KpPeriod),
        "KpComma" => Some(Keycode::KpComma),
        "KpEnter" => Some(Keycode::KpEnter),
        "KpEquals" => Some(Keycode::KpEquals),
        "NumLockClear" => Some(Keycode::NumLockClear),
        "Home" => Some(Keycode::Home),
        "End" => Some(Keycode::End),
        "PageUp" => Some(Keycode::PageUp),
        "PageDown" => Some(Keycode::PageDown),
        "Insert" => Some(Keycode::Insert),
        "Pause" => Some(Keycode::Pause),
        "CapsLock" => Some(Keycode::CapsLock),
        "ScrollLock" => Some(Keycode::ScrollLock),
        _ => None,
    }
}



// 定义虚拟鼠标的状态结构体
#[derive(Debug)]
pub struct VirtualMouse {
    dpad_up: bool,
    dpad_down: bool,
    dpad_left: bool,
    dpad_right: bool,

    //扳机状态
    left_trigger_active: bool,
    right_trigger_active: bool,

    pub x: f32,
    pub y: f32,
    // 当前的移动速度 (像素/帧)
    vx: f32,
    vy: f32,
    // 配置参数
    pub max_speed: f32,      // 最大速度
    pub acceleration: f32,   // 加速度
    pub friction: f32,       // 摩擦力 (0.0 - 1.0)，用于减速
}

impl VirtualMouse {
    pub fn new(start_x: f32, start_y: f32) -> Self {
        VirtualMouse {
            dpad_up: false,
            dpad_down: false,
            dpad_left: false,
            dpad_right: false,
            left_trigger_active: false,
            right_trigger_active: false,
            x: start_x,
            y: start_y,
            vx: 0.0,
            vy: 0.0,
            max_speed: 14.0,      // 根据屏幕分辨率调整
            acceleration: 1.0,    // 加速快慢
            friction: 0.4,       // 松开按键后的减速系数 (越小停得越快)
        }
    }

    pub fn handle_button_down(&mut self, button: Button) {
        match button {
            // Button::A => self.a = true,
            // Button::B => self.b = true,
            // Button::X => self.x = true,
            // Button::Y => self.y = true,
            // Button::LeftShoulder => self.left_bumper = true,
            // Button::RightShoulder => self.right_bumper = true,
            // Button::Start => self.start = true,
            // Button::Back => self.back = true,
            Button::DPadUp => self.dpad_up = true,
            Button::DPadDown => self.dpad_down = true,
            Button::DPadLeft => self.dpad_left = true,
            Button::DPadRight => self.dpad_right = true,
            _ => {}
        }
    }

    pub fn handle_button_up(&mut self, button: Button) {
        match button {
            // Button::A => self.a = false,
            // Button::B => self.b = false,
            // Button::X => self.x = false,
            // Button::Y => self.y = false,
            // Button::Start => self.start = false,
            // Button::Back => self.back = false,
            // Button::LeftShoulder => self.left_bumper = false,
            // Button::RightShoulder => self.right_bumper = false,

            Button::DPadUp => self.dpad_up = false,
            Button::DPadDown => self.dpad_down = false,
            Button::DPadLeft => self.dpad_left = false,
            Button::DPadRight => self.dpad_right = false,
            _ => {}
        }
    }

    // pub fn handle_axis(&mut self, axis: Axis, value: i16){
    //     match axis {
    //         Axis::LeftX => self.left_stick_x = value,
    //         Axis::LeftY => self.left_stick_y = value,
    //         Axis::RightX => self.right_stick_x = value,
    //         Axis::RightY => self.right_stick_y = value,
            
    //     }
    // }

    // 根据摇杆状态更新鼠标位置
    // pub fn left_stick_normalized(&mut self) -> (i16, i16) {

    //     let (mut nx, mut ny ) = (0.0, 0.0);
    //     // --- 径向死区（推荐用于 2D 移动）---
    //     let fx = self.left_stick_x as f32 / 32768.0;
    //     let fy = self.left_stick_y as f32 / 32768.0;
    //     let magnitude = f32::sqrt(fx * fx + fy * fy);

    //     if magnitude > DEAD_ZONE
    //     {
    //         // 重映射有效区域到 [0, 1]
    //         let scale = (magnitude - DEAD_ZONE) / (1.0 - DEAD_ZONE);
    //         // 保持方向，缩放幅度
    //         nx = (fx / magnitude) * scale;
    //         ny = (fy / magnitude) * scale;
    //     }

    //     // 应用灵敏度（注意：Y 轴反向，因为屏幕坐标向下为正）
    //     self.joymouse_x += (nx * SENSITIVITY) as i16;
    //     self.joymouse_y += (ny * SENSITIVITY) as i16;  // 减号：上推摇杆 → 鼠标向上（Y 减小）

    //     // === 边界检测：限制在窗口内 ===
    //     if self.joymouse_x < 0 { self.joymouse_x = 0;}
    //     if self.joymouse_y < 0 { self.joymouse_y = 0;}
    //     if self.joymouse_x >= SOURCE_WIDTH-8 {self.joymouse_x = SOURCE_WIDTH-8;}
    //     if self.joymouse_y >= SOURCE_HEIGHT-8 {self.joymouse_y = SOURCE_HEIGHT-8;}

    //     (self.joymouse_x, self.joymouse_y)
    // }


    /// 核心函数：根据手柄按钮状态更新鼠标坐标
    /// 
    /// 参数:
    /// - `dt`: 时间增量 (可选，如果想做帧率无关移动)，这里简化为每帧调用
    /// - `screen_w`, `screen_h`: 屏幕边界，防止越界
    /// 
    /// 返回: 更新后的 (x, y) 坐标
    pub fn update(&mut self, screen_w: f32, screen_h: f32) -> (f32, f32) {
        let mut input_x = 0.0;
        let mut input_y = 0.0;

        // 1. 解析输入：只处理上下左右四个方向键
        if self.dpad_up {
            input_y -= 1.0;
        }
        if self.dpad_down {
            input_y += 1.0;
        }
        if self.dpad_left {
            input_x -= 1.0;
        }
        if self.dpad_right {
            input_x += 1.0;
        }

        // 2. 应用加速度
        if input_x != 0.0 {
            self.vx += input_x * self.acceleration;
        } else {
            // 如果没有输入，应用摩擦力
            self.vx *= self.friction;
        }

        if input_y != 0.0 {
            self.vy += input_y * self.acceleration;
        } else {
            self.vy *= self.friction;
        }

        // 3. 限制最大速度 (Clamping)
        self.vx = self.vx.clamp(-self.max_speed, self.max_speed);
        self.vy = self.vy.clamp(-self.max_speed, self.max_speed);

        // 4. 停止微小移动 (避免浮点数误差导致永远不停)
        if self.vx.abs() < 0.01 { self.vx = 0.0; }
        if self.vy.abs() < 0.01 { self.vy = 0.0; }

        // 5. 更新位置
        self.x += self.vx;
        self.y += self.vy;

        // 6. 边界检查 (防止移出屏幕)
        self.x = self.x.clamp(15.0, screen_w - 15.0);
        self.y = self.y.clamp(15.0, screen_h - 15.0);

        (self.x, self.y)
    }
}

//! Built-in mappings, expressed in SDL's stable keycode ABI (ASCII or scancode | 1<<30).
//! Physical labels enter here only after the frontend's single ABXY normalization.
use super::Action;

pub const UP: i32 = (1 << 30) | 82;
pub const DOWN: i32 = (1 << 30) | 81;
pub const LEFT: i32 = (1 << 30) | 80;
pub const RIGHT: i32 = (1 << 30) | 79;
pub const CTRL: i32 = (1 << 30) | 224;
pub const SHIFT: i32 = (1 << 30) | 225;
pub const PAGE_UP: i32 = (1 << 30) | 75;
pub const PAGE_DOWN: i32 = (1 << 30) | 78;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Binding {
    None, Key(i32), Chord(&'static [i32]), Combo([i32; 4], u8), LeftClick, RightClick,
    Slow, Fast, Wheel(i8), DragLock, Center, Pointer(u8),
}

impl Binding {
    pub fn actions(self) -> Vec<Action> {
        match self {
            Self::Key(key) => vec![Action::Key(key)],
            Self::Chord(keys) => keys.iter().copied().map(Action::Key).collect(),
            Self::Combo(keys, count) => keys[..usize::from(count).min(4)].iter().copied().map(Action::Key).collect(),
            Self::LeftClick => vec![Action::MouseLeft],
            Self::RightClick => vec![Action::MouseRight],
            _ => Vec::new(),
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Stick { Directions, Steering, Mouse, None, Custom }

pub struct Profile {
    pub id: &'static str,
    pub name: &'static str,
    pub dpad: Option<[i32; 4]>, // Up, Down, Left, Right; None means fine mouse movement.
    pub stick: Stick,
    pub buttons: [Binding; 10], // A B X Y L1 R1 L2 R2 Start Select
}

use Binding::{Key as K, None as N};
const ARROWS: Option<[i32; 4]> = Some([UP, DOWN, LEFT, RIGHT]);
const WASD: Option<[i32; 4]> = Some([b'w' as i32, b's' as i32, b'a' as i32, b'd' as i32]);

pub const PROFILES: [Profile; 12] = [
    Profile { id: "arrows-space", name: "方向键＋Space", dpad: ARROWS, stick: Stick::Directions,
        buttons: [K(32), K(CTRL), K(SHIFT), K(122), K(120), K(99), N, N, K(13), K(27)] },
    Profile { id: "arrows-zx", name: "方向键＋ZX", dpad: ARROWS, stick: Stick::Directions,
        buttons: [K(122), K(120), K(99), K(32), K(SHIFT), K(CTRL), N, N, K(13), K(27)] },
    Profile { id: "wasd-jkl", name: "WASD＋JKL", dpad: WASD, stick: Stick::Directions,
        buttons: [K(106), K(107), K(108), K(32), K(113), K(101), N, N, K(13), K(27)] },
    Profile { id: "arrows-jump", name: "方向键跳跃", dpad: ARROWS, stick: Stick::Directions,
        buttons: [K(UP), K(32), K(CTRL), K(SHIFT), K(122), K(120), N, N, K(13), K(27)] },
    Profile { id: "racing", name: "竞速", dpad: ARROWS, stick: Stick::Steering,
        buttons: [K(32), K(110), K(114), K(13), K(122), K(120), N, N, K(13), K(27)] },
    Profile { id: "mouse-click", name: "鼠标点击", dpad: ARROWS, stick: Stick::Mouse,
        buttons: [Binding::LeftClick, Binding::RightClick, K(13), K(32), Binding::Slow, Binding::Fast,
                  Binding::Wheel(1), Binding::Wheel(-1), K(13), K(27)] },
    Profile { id: "mouse-drag", name: "鼠标拖拽", dpad: None, stick: Stick::Mouse,
        buttons: [Binding::LeftClick, Binding::RightClick, Binding::DragLock, Binding::Center,
                  Binding::Slow, Binding::Fast, Binding::Wheel(1), Binding::Wheel(-1), K(13), K(27)] },
    Profile { id: "wasd-mouse", name: "WASD＋鼠标", dpad: WASD, stick: Stick::Mouse,
        buttons: [K(32), K(101), K(114), K(113), Binding::RightClick, Binding::LeftClick,
                  K(SHIFT), Binding::Slow, K(13), K(27)] },
    Profile { id: "arrows-mouse", name: "方向键＋鼠标", dpad: ARROWS, stick: Stick::Mouse,
        buttons: [K(32), K(122), K(120), K(13), Binding::RightClick, Binding::LeftClick,
                  K(CTRL), Binding::Slow, K(13), K(27)] },
    Profile { id: "rhythm-dfjk", name: "节奏 DFJK", dpad: ARROWS, stick: Stick::None,
        buttons: [K(100), K(102), K(106), K(107), K(115), K(108), N, N, K(13), K(27)] },
    Profile { id: "tab-navigation", name: "Tab 导航", dpad: ARROWS, stick: Stick::Mouse,
        buttons: [K(13), K(27), K(9), Binding::Chord(&[SHIFT, 9]), K(PAGE_UP), K(PAGE_DOWN),
                  K(8), Binding::LeftClick, K(13), K(27)] },
    Profile { id: "number-skills", name: "数字技能栏", dpad: ARROWS, stick: Stick::Mouse,
        buttons: [K(49), K(50), K(51), K(52), K(53), K(54), K(55), K(56), K(13), K(27)] },
];

pub const PROFILE_COUNT: usize = PROFILES.len() + 1; // zero retains the legacy cfg/mode behavior

pub fn profile(index: usize) -> Option<&'static Profile> { index.checked_sub(1).and_then(|i| PROFILES.get(i)) }

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn catalog_has_twelve_unique_profiles_and_safe_common_keys() {
        let mut ids = std::collections::BTreeSet::new();
        for p in &PROFILES {
            assert!(ids.insert(p.id));
            assert_eq!(&p.buttons[8..], &[K(13), K(27)]);
            assert!(!p.name.is_empty());
        }
        assert_eq!(ids.len(), 12);
        assert!(profile(0).is_none() && profile(13).is_none());
    }

    #[test]
    fn demanding_profiles_keep_their_distinct_controls() {
        assert_eq!(PROFILES[2].dpad, WASD);
        assert_eq!(&PROFILES[2].buttons[..4], &[K(106), K(107), K(108), K(32)]);
        assert_eq!(PROFILES[3].buttons[0], K(UP));
        assert_eq!(PROFILES[4].stick, Stick::Steering);
        assert_eq!(PROFILES[9].stick, Stick::None);
        assert_eq!(PROFILES[10].buttons[3].actions(), vec![Action::Key(SHIFT), Action::Key(9)]);
        assert_eq!(PROFILES[11].buttons[..8].iter().map(|b| b.actions()[0]).collect::<Vec<_>>(),
            (49..=56).map(Action::Key).collect::<Vec<_>>());
        for p in &PROFILES[..5] { assert_eq!(&p.buttons[6..8], &[N, N]); }
        assert_eq!(PROFILES[6].dpad, None);
        assert_eq!(PROFILES[6].buttons[2], Binding::DragLock);
        assert_eq!(PROFILES[6].buttons[3], Binding::Center);
        assert_eq!(&PROFILES[5].buttons[4..8], &[Binding::Slow, Binding::Fast, Binding::Wheel(1), Binding::Wheel(-1)]);
    }
}

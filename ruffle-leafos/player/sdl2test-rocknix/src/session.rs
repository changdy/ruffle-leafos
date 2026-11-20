//! Frontend input ownership, pause state and time-based stick movement.
//! This module has no SDL/GL dependencies so its transition tests run on the host.
use crate::session::i18n::t;
use std::collections::{BTreeMap, BTreeSet};
#[path = "profiles.rs"]
pub mod profiles;
#[path = "controls.rs"]
pub mod controls;
#[path = "cursor.rs"]
pub mod cursor;
#[path = "shortcuts.rs"]
pub mod shortcuts;
#[path = "experience.rs"]
pub mod experience;
#[path = "data_manager.rs"]
pub mod data_manager;
#[path = "graphics.rs"]
pub mod graphics;
#[path = "turbo.rs"]
pub mod turbo;
#[path = "system_volume.rs"]
pub mod system_volume;
#[path = "game_assets.rs"]
pub mod game_assets;
#[path = "i18n.rs"]
pub mod i18n;
#[path = "virtual_keyboard.rs"]
pub mod virtual_keyboard;
#[path = "resource_cache.rs"]
pub mod resource_cache;
#[path = "screenshot.rs"]
pub mod screenshot;

#[derive(Clone, Copy, Debug, Eq, PartialEq, Ord, PartialOrd)]
pub enum Source {
    Controller(u8),
    Trigger(u8),
    Keyboard(i32),
    Mouse(u8),
    Stick(u8),
    DragLock,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Ord, PartialOrd)]
pub enum Action {
    Key(i32),
    MouseLeft,
    MouseRight,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Transition {
    pub action: Action,
    pub down: bool,
}

#[derive(Default)]
pub struct InputLedger {
    sources: BTreeMap<Source, Vec<Action>>,
    owners: BTreeMap<Action, usize>,
    order: Vec<Action>,
}

impl InputLedger {
    pub fn is_down(&self, action: Action) -> bool { self.owners.contains_key(&action) }
    pub fn press(&mut self, source: Source, action: Action) -> Vec<Transition> {
        self.press_many(source, &[action])
    }

    pub fn press_many(&mut self, source: Source, actions: &[Action]) -> Vec<Transition> {
        if self.sources.contains_key(&source) {
            return Vec::new();
        }
        let mut owned = Vec::new();
        let mut result = Vec::new();
        for &action in actions {
            if owned.contains(&action) { continue; }
            owned.push(action);
            let count = self.owners.entry(action).or_default();
            *count += 1;
            if *count == 1 {
                self.order.push(action);
                result.push(Transition { action, down: true });
            }
        }
        self.sources.insert(source, owned);
        result
    }

    pub fn release(&mut self, source: Source) -> Vec<Transition> {
        let Some(actions) = self.sources.remove(&source) else {
            return Vec::new();
        };
        let mut result = Vec::new();
        for action in actions.into_iter().rev() {
            let count = self.owners.get_mut(&action).expect("owned input has a count");
            *count -= 1;
            if *count == 0 {
                self.owners.remove(&action);
                self.order.retain(|a| *a != action);
                result.push(Transition { action, down: false });
            }
        }
        result
    }

    pub fn release_all(&mut self) -> Vec<Transition> {
        let result = self.order.drain(..).rev()
            .map(|action| Transition { action, down: false }).collect();
        self.sources.clear();
        self.owners.clear();
        result
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum InputMode {
    Hybrid,
    Keyboard,
    Mouse,
}

impl InputMode {
    pub fn name(self) -> &'static str {
        match self {
            Self::Hybrid => t("混合"),
            Self::Keyboard => t("键盘"),
            Self::Mouse => t("鼠标"),
        }
    }

    pub fn next(self, direction: i32) -> Self {
        let index = match self { Self::Hybrid => 0, Self::Keyboard => 1, Self::Mouse => 2 };
        match (index + direction).rem_euclid(3) {
            0 => Self::Hybrid,
            1 => Self::Keyboard,
            _ => Self::Mouse,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum MouseSpeed {
    Slow,
    Normal,
    Fast,
}

impl MouseSpeed {
    pub fn pixels_per_second(self) -> f32 {
        match self { Self::Slow => 300.0, Self::Normal => 600.0, Self::Fast => 900.0 }
    }

    pub fn next(self, direction: i32) -> Self {
        let index = match self { Self::Slow => 0, Self::Normal => 1, Self::Fast => 2 };
        match (index + direction).clamp(0, 2) {
            0 => Self::Slow,
            1 => Self::Normal,
            _ => Self::Fast,
        }
    }
}

pub const MENU_ROWS: usize = 11;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum MenuResult {
    None,
    Resume,
    Quit,
    ModeChanged(InputMode),
    MouseSpeedChanged(MouseSpeed),
    ProfileChanged(usize),
    OpenControls,
    SlotChanged(usize),
    OpenCursor,
    OpenSaves,
    OpenSettings,
    OpenShortcuts,
    OpenAbout,
    OpenLanguage,
    OpenScreenshot,
    TakePhoto,
    SaveProgress,
}

pub struct Session {
    pub virtual_keyboard: Option<virtual_keyboard::Editor>,
    pub system_level: Option<(system_volume::Level,std::time::Instant)>,
    pub directions_swapped: bool,
    pub experience: experience::Ui,
    pub toast: Option<(String,std::time::Instant)>,
    pub cursor_settings: cursor::Settings,
    pub controls: controls::Controls,
    pub paused: bool,
    pub selected: usize,
    pub mode: InputMode,
    pub preview_mode: InputMode,
    pub mouse_speed: MouseSpeed,
    pub preview_mouse_speed: MouseSpeed,
    pub profile: usize,
    pub preview_profile: usize,
    pub drag_locked: bool,
    pub use_cfg: bool,
    pub preview_use_cfg: bool,
    pub stick: (i16, i16),
    pub right_stick: (i16, i16),
    pub ledger: InputLedger,
    held: BTreeSet<Source>,
    wait_neutral: bool,
    stick_axes: [(bool,bool); 2],
}

impl Session {
    pub fn new(mode: InputMode) -> Self {
        Self {
            virtual_keyboard: None,
            system_level: None,
            directions_swapped: false,
            experience: experience::Ui::default(),
            toast: None,
            cursor_settings: cursor::Settings::default(),
            controls: controls::Controls::default(),
            paused: false, selected: 0, mode, preview_mode: mode,
            mouse_speed: MouseSpeed::Normal, preview_mouse_speed: MouseSpeed::Normal,
            profile: 0, preview_profile: 0, drag_locked: false,
            use_cfg: true, preview_use_cfg: true,
            stick: (0, 0), right_stick: (0, 0), ledger: InputLedger::default(),
            held: BTreeSet::new(), wait_neutral: true, stick_axes: [(true,true), (false,false)],
        }
    }

    pub fn mark_down(&mut self, source: Source) -> bool {
        self.held.insert(source)
    }

    pub fn is_held(&self, source: Source) -> bool {
        self.held.contains(&source)
    }

    pub fn release(&mut self, source: Source) -> Vec<Transition> {
        self.held.remove(&source);
        self.refresh_neutral();
        self.ledger.release(source)
    }

    pub fn allow_game_input(&self) -> bool {
        !self.paused && self.virtual_keyboard.is_none() && !self.wait_neutral
    }

    pub fn require_neutral(&mut self) {
        self.wait_neutral = true;
    }

    pub fn toggle_directions(&mut self) -> Vec<Transition> {
        self.directions_swapped=!self.directions_swapped;
        self.drag_locked=false; self.wait_neutral=true;
        self.held.retain(|source| !matches!(source,Source::Stick(_)));
        self.ledger.release_all()
    }

    pub fn resync_held(&mut self, physical: impl IntoIterator<Item = Source>) -> Vec<Transition> {
        // Focus loss can hide the corresponding key-up. Rebuild from SDL's
        // current snapshot rather than waiting forever for a missing event.
        self.held = physical.into_iter().collect();
        self.wait_neutral = true;
        self.drag_locked = false;
        self.ledger.release_all()
    }

    pub fn refresh_neutral(&mut self) {
        // Stick sources are derived from the physical axis snapshot. Requiring
        // their key-up here can deadlock: update_stick releases them only after
        // allow_game_input becomes true. Real buttons/keys still need release.
        let buttons_released=self.held.iter().all(|source|matches!(source,Source::Stick(_)));
        let sticks_centered = [self.stick, self.right_stick].into_iter().zip(self.stick_axes)
            .all(|(stick, axes)| mapped_stick(stick, axes).is_none());
        if buttons_released && sticks_centered {
            self.wait_neutral = false;
        }
    }

    #[cfg(test)]
    pub fn refresh_neutral_for_axes(&mut self, used: (bool,bool)) {
        self.refresh_neutral_for_sticks([used, (false, false)]);
    }

    pub fn refresh_neutral_for_sticks(&mut self, used: [(bool,bool); 2]) {
        if self.stick_axes != used { self.wait_neutral = true; }
        self.stick_axes = used;
        self.refresh_neutral();
    }

    pub fn game_press(&mut self, source: Source, action: Action) -> Vec<Transition> {
        if self.allow_game_input() {
            self.ledger.press(source, action)
        } else {
            Vec::new()
        }
    }

    pub fn toggle_drag(&mut self) -> Vec<Transition> {
        if !self.allow_game_input() { return Vec::new(); }
        self.drag_locked = !self.drag_locked;
        if self.drag_locked { self.ledger.press(Source::DragLock, Action::MouseLeft) }
        else { self.ledger.release(Source::DragLock) }
    }

    pub fn open_menu(&mut self) -> Vec<Transition> {
        self.controls.preview_slot = self.controls.game_slot;
        self.controls.visible = false;
        self.paused = true;
        self.selected = 0;
        self.preview_mode = self.mode;
        self.preview_mouse_speed = self.mouse_speed;
        self.preview_profile = self.profile;
        self.preview_use_cfg = self.use_cfg;
        self.drag_locked = false;
        self.wait_neutral = true;
        self.held.retain(|s| !matches!(s, Source::Stick(_)));
        self.ledger.release_all()
    }

    pub fn resume(&mut self) -> MenuResult {
        self.controls.preview_slot = self.controls.game_slot;
        self.controls.visible = false;
        self.preview_mode = self.mode;
        self.preview_mouse_speed = self.mouse_speed;
        self.preview_profile = self.profile;
        self.preview_use_cfg = self.use_cfg;
        self.paused = false;
        self.wait_neutral = true;
        MenuResult::Resume
    }

    pub fn menu_move(&mut self, direction: i32) {
        self.selected = (self.selected as i32 + direction).rem_euclid(MENU_ROWS as i32) as usize;
    }

    pub fn preview(&mut self, direction: i32) -> MenuResult {
        match self.selected {
            1 => return MenuResult::SlotChanged((self.controls.game_slot as i32+direction).rem_euclid(controls::GAME_SLOTS as i32) as usize),
            _ => {}
        }
        MenuResult::None
    }

    pub fn toggle_cfg_preview(&mut self) {
    }

    pub fn accept(&mut self) -> MenuResult {
        match self.selected {
            0 => self.resume(),
            1 => MenuResult::OpenControls,
            2 => MenuResult::OpenCursor,
            3 => MenuResult::SaveProgress,
            4 => MenuResult::OpenSaves,
            5 => MenuResult::OpenSettings,
            6 => MenuResult::OpenShortcuts,
            7 => MenuResult::OpenAbout,
            8 => MenuResult::OpenLanguage,
            9 if self.experience.pocketais => MenuResult::TakePhoto,
            9 => MenuResult::OpenScreenshot,
            _ => MenuResult::Quit,
        }
    }

    pub fn disconnected(&mut self) -> Vec<Transition> {
        let result = if self.paused {self.ledger.release_all()}else{self.open_menu()};
        self.held.clear();
        self.stick = (0, 0);
        self.right_stick = (0, 0);
        result
    }
}

pub fn mapped_stick(stick: (i16, i16), axes: (bool, bool)) -> Option<(f32, f32)> {
    stick_vector(if axes.0 { stick.0 } else { 0 }, if axes.1 { stick.1 } else { 0 })
}

pub fn stick_vector(x: i16, y: i16) -> Option<(f32, f32)> {
    let x = (x as f32 / 32767.0).clamp(-1.0, 1.0);
    let y = (y as f32 / 32767.0).clamp(-1.0, 1.0);
    let radius = (x * x + y * y).sqrt();
    let deadzone = 0.18;
    if radius <= deadzone {
        return None;
    }
    let amount = ((radius.min(1.0) - deadzone) / (1.0 - deadzone)).clamp(0.0, 1.0);
    Some((x / radius * amount, y / radius * amount))
}

pub fn move_cursor(position: (f32, f32), axes: (i16, i16), seconds: f32, speed: MouseSpeed,
                   bounds: (f32, f32, f32, f32)) -> (f32, f32) {
    let Some((x, y)) = stick_vector(axes.0, axes.1) else { return position; };
    let distance = speed.pixels_per_second() * seconds.clamp(0.0, 0.05);
    ((position.0 + x * distance).clamp(bounds.0, bounds.2),
     (position.1 + y * distance).clamp(bounds.1, bounds.3))
}

#[cfg(test)]
mod tests {
    #[test]
    fn keyboard_blocks_game_inputs_until_closed_and_all_buttons_released() {
        let mut s=super::Session::new(super::InputMode::Hybrid);s.refresh_neutral();
        s.virtual_keyboard=Some(super::virtual_keyboard::Editor::new("".into(),(0,0),false,false).unwrap());
        assert!(!s.allow_game_input());
        assert!(s.game_press(super::Source::Controller(1),super::Action::Key(32)).is_empty());
        s.mark_down(super::Source::Controller(1));s.virtual_keyboard=None;s.resume();
        assert!(!s.allow_game_input());s.release(super::Source::Controller(1));assert!(s.allow_game_input());
    }
    use super::*;
    #[test]
    fn direction_toggle_releases_inputs_and_waits_for_the_stick_to_center() {
        let mut s=Session::new(InputMode::Hybrid);s.refresh_neutral();
        s.game_press(Source::Stick(0),Action::Key(119));s.toggle_drag();s.stick=(32767,0);
        let released=s.toggle_directions();assert!(s.directions_swapped);assert!(!s.drag_locked);
        assert!(released.iter().any(|t|t.action==Action::Key(119)&&!t.down));
        assert!(released.iter().any(|t|t.action==Action::MouseLeft&&!t.down));
        assert!(s.ledger.release_all().is_empty());s.refresh_neutral();assert!(!s.allow_game_input());
        s.stick=(0,0);s.refresh_neutral();assert!(s.allow_game_input());
        s.toggle_directions();assert!(!s.directions_swapped);
    }

    #[test]
    fn chords_release_in_reverse_order_and_keep_a_shared_modifier_held() {
        let shift = Action::Key(profiles::SHIFT);
        let tab = Action::Key(9);
        let mut input = InputLedger::default();
        input.press(Source::Keyboard(1), shift);
        assert_eq!(input.press_many(Source::Controller(3), &[shift, tab, tab]), vec![Transition { action: tab, down: true }]);
        assert_eq!(input.release(Source::Controller(3)), vec![Transition { action: tab, down: false }]);
        assert_eq!(input.release(Source::Keyboard(1)), vec![Transition { action: shift, down: false }]);
        input.press_many(Source::Controller(3), &[shift, tab]);
        assert_eq!(input.release_all(), vec![Transition { action: tab, down: false }, Transition { action: shift, down: false }]);
        assert!(input.release_all().is_empty());
    }

    #[test]
    fn drag_lock_shares_left_click_and_pause_or_focus_releases_it_once() {
        let mut s = Session::new(InputMode::Mouse);
        s.refresh_neutral();
        assert_eq!(s.toggle_drag(), vec![Transition { action: Action::MouseLeft, down: true }]);
        s.game_press(Source::Controller(0), Action::MouseLeft);
        assert!(s.toggle_drag().is_empty());
        assert_eq!(s.release(Source::Controller(0)).len(), 1);
        s.toggle_drag();
        assert_eq!(s.open_menu().len(), 1);
        assert!(!s.drag_locked);
        s.resume(); s.refresh_neutral();
        assert!(s.ledger.release_all().is_empty());
        s.toggle_drag();
        assert_eq!(s.resync_held([]).len(), 1);
        assert!(!s.drag_locked);
    }

    #[test]
    fn controls_row_switches_schemes_left_right_and_A_still_opens_editor() {
        let mut s=Session::new(InputMode::Hybrid);
        s.open_menu();s.menu_move(1);
        assert_eq!(s.preview(-1),MenuResult::SlotChanged(9));
        assert_eq!(s.preview(1),MenuResult::SlotChanged(1));
        s.controls.game_slot=9;assert_eq!(s.preview(1),MenuResult::SlotChanged(0));s.controls.game_slot=0;
        assert_eq!(s.controls.game_slot,0);assert_eq!(s.accept(),MenuResult::OpenControls);
    }

    #[test]
    fn editor_and_cursor_have_direct_menu_entries() {
        let mut s=Session::new(InputMode::Hybrid);s.open_menu();s.menu_move(1);
        assert_eq!(s.accept(),MenuResult::OpenControls);s.menu_move(1);assert_eq!(s.accept(),MenuResult::OpenCursor);
    }

    #[test]
    fn shared_key_is_released_only_after_last_owner() {
        let mut input = InputLedger::default();
        let action = Action::Key(13);
        assert_eq!(input.press(Source::Controller(1), action), vec![Transition { action, down: true }]);
        assert!(input.press(Source::Keyboard(13), action).is_empty());
        assert!(input.release(Source::Controller(1)).is_empty());
        assert_eq!(input.release(Source::Keyboard(13)), vec![Transition { action, down: false }]);
        assert!(input.release(Source::Keyboard(13)).is_empty());
    }

    #[test]
    fn pause_releases_drag_and_keys_and_resume_waits_for_neutral() {
        let mut s = Session::new(InputMode::Hybrid);
        s.refresh_neutral();
        s.mark_down(Source::Controller(1));
        s.game_press(Source::Controller(1), Action::MouseLeft);
        s.mark_down(Source::Controller(2));
        s.game_press(Source::Controller(2), Action::Key(32));
        let released = s.open_menu();
        assert_eq!(released.len(), 2);
        assert!(released.iter().all(|e| !e.down));
        assert!(s.game_press(Source::Controller(3), Action::Key(9)).is_empty());
        s.resume();
        assert!(!s.allow_game_input());
        s.release(Source::Controller(1));
        assert!(!s.allow_game_input());
        s.release(Source::Controller(2));
        assert!(s.allow_game_input());
        assert!(s.mark_down(Source::Controller(1)));
        assert_eq!(s.game_press(Source::Controller(1), Action::MouseLeft).len(), 1);
    }

    #[test]
    fn disconnect_preserves_unsaved_editor_and_pending_confirmation() {
        let mut s=Session::new(InputMode::Hybrid);s.open_menu();s.controls.begin(controls::Mapping::group(0));
        s.controls.nav(controls::Nav::Y);let draft=s.controls.draft.clone();s.controls.nav(controls::Nav::Next);
        s.disconnected();assert!(s.controls.visible);assert_eq!(s.controls.page,controls::Page::Confirm);assert_eq!(s.controls.draft,draft);
    }

    #[test]
    fn held_stick_blocks_resume_until_centered() {
        let mut s = Session::new(InputMode::Hybrid);
        s.stick = (20000, 0);
        s.open_menu();
        s.resume();
        s.refresh_neutral();
        assert!(!s.allow_game_input());
        s.stick = (1000, -1000);
        s.refresh_neutral();
        assert!(s.allow_game_input());
    }

    #[test]
    fn stick_movement_is_independent_of_30_or_60_hz() {
        let bounds = (0.0, 0.0, 4000.0, 4000.0);
        let mut a = (0.0, 0.0);
        let mut b = a;
        for _ in 0..30 { a = move_cursor(a, (25000, 10000), 1.0 / 30.0, MouseSpeed::Normal, bounds); }
        for _ in 0..60 { b = move_cursor(b, (25000, 10000), 1.0 / 60.0, MouseSpeed::Normal, bounds); }
        assert!((a.0 - b.0).abs() < 0.01 && (a.1 - b.1).abs() < 0.01);
    }

    #[test]
    fn drift_and_long_pause_do_not_jump_cursor() {
        let bounds = (0.0, 0.0, 960.0, 720.0);
        assert_eq!(move_cursor((10.0, 10.0), (1000, -1000), 1.0, MouseSpeed::Normal, bounds), (10.0, 10.0));
        let p = move_cursor((10.0, 10.0), (32767, 0), 120.0, MouseSpeed::Normal, bounds);
        assert!((p.0 - 40.0).abs() < 0.01);
        assert_eq!(move_cursor((959.0, 719.0), (32767, 32767), 0.05, MouseSpeed::Normal, bounds), (960.0, 720.0));
    }

    #[test]
    fn repeated_button_down_does_not_acquire_a_second_owner() {
        let mut s = Session::new(InputMode::Keyboard);
        s.refresh_neutral();
        assert!(s.mark_down(Source::Controller(0)));
        assert!(!s.mark_down(Source::Controller(0)));
        s.game_press(Source::Controller(0), Action::Key(32));
        assert!(s.game_press(Source::Controller(0), Action::Key(32)).is_empty());
        assert_eq!(s.release(Source::Controller(0)).len(), 1);
    }

    #[test]
    fn disconnect_releases_all_inputs_and_leaves_recoverable_menu() {
        let mut s = Session::new(InputMode::Mouse);
        s.refresh_neutral();
        s.mark_down(Source::Controller(0));
        s.game_press(Source::Controller(0), Action::MouseLeft);
        assert_eq!(s.disconnected().len(), 1);
        assert!(s.paused);
        assert_eq!(s.selected, 0);
        s.resume();
        s.refresh_neutral();
        assert!(s.allow_game_input());
    }

    #[test]
    fn focus_snapshot_recovers_missing_key_up_without_replaying_held_keys() {
        let mut s = Session::new(InputMode::Keyboard);
        s.refresh_neutral();
        s.mark_down(Source::Keyboard(32));
        s.game_press(Source::Keyboard(32), Action::Key(32));
        s.open_menu();
        s.resync_held([Source::Controller(1)]);
        s.resume();
        s.refresh_neutral();
        assert!(!s.allow_game_input());
        assert!(s.game_press(Source::Controller(1), Action::Key(13)).is_empty());
        s.release(Source::Controller(1));
        assert!(s.allow_game_input());
        assert!(s.mark_down(Source::Keyboard(32)));
    }

    #[test]
    fn main_cursor_row_opens_settings_without_adjusting_speed() {
        let mut s = Session::new(InputMode::Mouse);
        s.open_menu();
        s.menu_move(2);
        assert_eq!(s.preview(-1),MenuResult::None);
        assert_eq!(s.preview_mouse_speed, MouseSpeed::Normal);
        assert_eq!(s.mouse_speed, MouseSpeed::Normal);
        assert_eq!(s.preview(1),MenuResult::None);
        s.mark_down(Source::Controller(0));
        assert_eq!(s.accept(), MenuResult::OpenCursor);
        assert!(s.paused);
        assert_eq!(s.mode, InputMode::Mouse);
        s.resume();
        assert!(!s.allow_game_input());
        s.release(Source::Controller(0));
        assert!(s.allow_game_input());
        s.open_menu();
        assert_eq!(s.preview_mouse_speed, MouseSpeed::Normal);
    }

    #[test]
    fn cursor_row_does_not_quit_and_navigation_wraps_to_the_real_quit_row() {
        let mut s = Session::new(InputMode::Hybrid);
        s.open_menu();
        s.menu_move(-1);
        assert_eq!(s.accept(), MenuResult::Quit);
        s.menu_move(1);
        assert_eq!(s.selected, 0);
        s.menu_move(2);
        assert_eq!(s.accept(), MenuResult::OpenCursor);
        s.menu_move(4);assert_eq!(s.accept(),MenuResult::OpenShortcuts);
        s.menu_move(1);assert_eq!(s.accept(),MenuResult::OpenAbout);
        s.menu_move(1);assert_eq!(s.accept(),MenuResult::OpenLanguage);
        s.menu_move(1);assert_eq!(s.accept(),MenuResult::OpenScreenshot);
        s.menu_move(1);assert_eq!(s.accept(),MenuResult::Quit);
        assert_eq!(MouseSpeed::Slow.next(-1), MouseSpeed::Slow);
        assert_eq!(MouseSpeed::Fast.next(1), MouseSpeed::Fast);
    }

    #[test]
    fn pocketais_screenshot_saves_directly_from_pause() {
        let mut session = Session::new(InputMode::Hybrid);
        session.experience.pocketais = true;
        session.open_menu();
        session.menu_move(-2);
        assert_eq!(session.accept(), MenuResult::TakePhoto);
        assert!(session.paused);
        assert_eq!(session.experience.page, experience::Page::Home);
    }

    #[test]
    fn all_speeds_have_correct_distance_at_30_and_60_hz_and_keep_the_safety_clamp() {
        let bounds = (0.0, 0.0, 4000.0, 4000.0);
        for speed in [MouseSpeed::Slow, MouseSpeed::Normal, MouseSpeed::Fast] {
            for hz in [30, 60] {
                let mut cursor = (100.0, 100.0);
                for _ in 0..hz { cursor = move_cursor(cursor, (32767, 0), 1.0 / hz as f32, speed, bounds); }
                assert!((cursor.0 - 100.0 - speed.pixels_per_second()).abs() < 0.01);
                assert_eq!(cursor.1, 100.0);
            }
            let cursor = move_cursor((100.0, 100.0), (32767, 0), 30.0, speed, bounds);
            assert!((cursor.0 - 100.0 - speed.pixels_per_second() * 0.05).abs() < 0.01);
            assert_eq!(move_cursor((10.0, 10.0), (1000, -1000), 1.0, speed, bounds), (10.0, 10.0));
        }
    }

    #[test]
    fn right_stick_requires_neutral_only_on_mapped_axes() {
        let mut state = Session::new(InputMode::Hybrid);
        state.right_stick = (1000, i16::MAX);
        state.refresh_neutral_for_sticks([(true, true), (true, false)]);
        assert!(state.allow_game_input());
        state.refresh_neutral_for_sticks([(true, true), (true, true)]);
        assert!(!state.allow_game_input());
        state.right_stick = (0, 0);
        state.refresh_neutral();
        assert!(state.allow_game_input());
    }

    #[test]
    fn unused_stick_axes_do_not_block_resume_but_physical_buttons_still_do() {
        let mut s=Session::new(InputMode::Hybrid);
        let mut mapping=controls::Mapping::group(2);
        mapping.buttons[0]=profiles::Binding::Key(32);
        s.stick=(i16::MIN,i16::MAX);
        s.open_menu();s.resume();
        s.refresh_neutral_for_axes(mapping.used_stick_axes(false));
        assert!(s.allow_game_input());
        assert_eq!(s.game_press(Source::Controller(0),Action::Key(32)),vec![Transition{action:Action::Key(32),down:true}]);
        s.resync_held([Source::Controller(0)]);
        s.refresh_neutral_for_axes(mapping.used_stick_axes(false));
        assert!(!s.allow_game_input());
        s.release(Source::Controller(0));
        assert!(s.allow_game_input());
    }

    #[test]
    fn single_active_axis_keeps_its_deadzone_despite_noise_on_an_unused_axis() {
        let mut mapping=controls::Mapping::group(2);
        mapping.stick=profiles::Stick::Custom;
        mapping.axes[2]=profiles::Binding::Pointer(2);
        let used=mapping.used_stick_axes(false);
        assert_eq!(used,(true,false));
        let mut s=Session::new(InputMode::Hybrid);
        for disabled_y in [i16::MIN,0,i16::MAX] {
            s.require_neutral();s.stick=(1000,disabled_y);
            s.refresh_neutral_for_axes(used);
            assert!(s.allow_game_input());
            assert!(mapped_stick(s.stick, used).is_none());
            for active_x in [-10000,10000] {
                s.require_neutral();s.stick=(active_x,disabled_y);
                s.refresh_neutral_for_axes(used);
                assert!(!s.allow_game_input());
            }
        }
        mapping.axes[2]=profiles::Binding::None;
        mapping.axes[1]=profiles::Binding::Key(115);
        assert_eq!(mapping.used_stick_axes(false),(false,true));
        s.require_neutral();s.stick=(i16::MAX,-10000);
        s.refresh_neutral_for_axes(mapping.used_stick_axes(false));
        assert!(!s.allow_game_input());
        s.stick.1=1000;s.refresh_neutral();
        assert!(s.allow_game_input());
    }

    #[test]
    fn neutral_mask_uses_effective_directions_including_legacy_swaps() {
        let mut mapping=controls::Mapping::group(2);
        mapping.buttons[12]=profiles::Binding::Key(97);
        let mut s=Session::new(InputMode::Hybrid);
        s.stick=(20000,0);
        s.refresh_neutral_for_axes(mapping.used_stick_axes(false));
        assert!(s.allow_game_input());
        s.toggle_directions();
        s.refresh_neutral_for_axes(mapping.used_stick_axes(s.directions_swapped));
        assert!(!s.allow_game_input());
        assert_eq!(mapping.used_stick_axes(true),mapping.directions(true).used_stick_axes(false));
        s.stick=(0,20000);s.refresh_neutral();
        assert!(s.allow_game_input());

        // Frontend legacy/config snapshots always have default pointer axes,
        // but their real D-pad bindings can be empty or single-axis mappings.
        let mut legacy=controls::Mapping::group(0);
        legacy.buttons[10..14].fill(profiles::Binding::None);
        assert_eq!(legacy.used_stick_axes(false),(true,true));
        assert_eq!(legacy.used_stick_axes(true),(false,false));
        legacy.buttons[10]=profiles::Binding::Key(119);
        assert_eq!(legacy.used_stick_axes(true),(false,true));
        s.require_neutral();s.stick=(0,20000);
        s.refresh_neutral_for_axes(legacy.used_stick_axes(true));
        assert!(!s.allow_game_input());
    }

    #[test]
    fn derived_stick_sources_do_not_deadlock_the_neutral_gate_or_release_live_keys_early() {
        let mut s=Session::new(InputMode::Hybrid);
        s.mark_down(Source::Stick(0));
        s.ledger.press(Source::Stick(0),Action::Key(119));
        s.stick=(0,-20000);
        s.refresh_neutral_for_axes((true,true));
        assert!(!s.allow_game_input());
        s.stick=(0,0);s.refresh_neutral();
        assert!(s.allow_game_input());
        // The normal stick update still owns and emits the pending key-up.
        assert!(s.ledger.is_down(Action::Key(119)));
        assert_eq!(s.release(Source::Stick(0)),vec![Transition{action:Action::Key(119),down:false}]);
        s.mark_down(Source::Keyboard(13));s.require_neutral();s.refresh_neutral();
        assert!(!s.allow_game_input());
        s.release(Source::Keyboard(13));
        assert!(s.allow_game_input());
    }
}

use crate::session::i18n::t;
use super::*;
use crate::pause_menu::PauseMenu;
use crate::session::{Action, InputMode, MenuResult, Session, Source, Transition};
use crate::session::profiles::{self, Binding, Stick};
use crate::session::controls::{self, Mapping, Nav, ResultAction};
use crate::session::cursor;
use crate::session::graphics::{self, Backend as GraphicsBackend};
use crate::session::turbo::{Rate,Repeater};
use crate::session::experience::{self, Page as ExperiencePage, Command as ExperienceCommand};
use glow::HasContext;
use sdl2::keyboard::Keycode;
use std::sync::atomic::{AtomicBool, Ordering};

static EXIT_REQUESTED: AtomicBool = AtomicBool::new(false);

struct NativeStorage(Arc<Mutex<experience::Progress>>);
impl ruffle_core::backend::storage::StorageBackend for NativeStorage {
    fn get(&self,name:&str)->Option<Vec<u8>> {self.0.lock().unwrap().get(name)}
    fn put(&mut self,name:&str,value:&[u8])->bool {self.0.lock().unwrap().put(name,Some(value))}
    fn remove_key(&mut self,name:&str){self.0.lock().unwrap().remove(name);}
}

#[cfg(unix)]
extern "C" fn request_exit(_: libc::c_int) {
    // No I/O, allocation, player access or saving inside the signal handler.
    EXIT_REQUESTED.store(true, Ordering::Relaxed);
}

fn install_signals() {
    #[cfg(unix)]
    unsafe {
        libc::signal(libc::SIGTERM, request_exit as *const () as libc::sighandler_t);
        libc::signal(libc::SIGINT, request_exit as *const () as libc::sighandler_t);
    }
}

fn normalize_button(button: Button, swap: bool) -> Button {
    if !swap { return button; }
    match button {
        Button::A => Button::B, Button::B => Button::A,
        // Leaf 0.13 Loong Gamepad maps BTN_NORTH/WEST to SDL X/Y already.
        // Only SDL A/B differ from the device's physical labels.
        other => other,
    }
}

const BUTTONS: [Button; 17] = [
    Button::A, Button::B, Button::X, Button::Y, Button::Back, Button::Guide,
    Button::Start, Button::LeftShoulder, Button::RightShoulder,
    Button::DPadUp, Button::DPadDown, Button::DPadLeft, Button::DPadRight,
    Button::Paddle1, Button::Paddle2, Button::LeftStick, Button::RightStick,
];

const PROFILE_BUTTONS: [Button; 10] = [Button::A, Button::B, Button::X, Button::Y,
    Button::LeftShoulder, Button::RightShoulder, Button::Paddle1, Button::Paddle2, Button::Start, Button::Back];
const DPAD: [Button; 4] = [Button::DPadUp, Button::DPadDown, Button::DPadLeft, Button::DPadRight];

fn binding_for(keymap: &keycodes::KeyMap, index: usize, mode: InputMode, button: Button, use_cfg: bool) -> Binding {
    if matches!(button, Button::Guide | Button::LeftStick | Button::RightStick) { return Binding::None; }
    // Game-specific legacy cfg remains the highest priority for mapped buttons.
    if use_cfg {
        if let Some(key) = keymap.custom_keycode(button) { return Binding::Key(key.into_i32()); }
    }
    if let Some(p) = profiles::profile(index) {
        if let Some(i) = PROFILE_BUTTONS.iter().position(|b| *b == button) { return p.buttons[i]; }
        if let Some(i) = DPAD.iter().position(|b| *b == button) {
            return p.dpad.map(|keys| Binding::Key(keys[i])).unwrap_or(Binding::None);
        }
        return Binding::None;
    }
    match (mode, button) {
        (InputMode::Hybrid, Button::LeftShoulder) => Binding::LeftClick,
        (InputMode::Hybrid, Button::RightShoulder) => Binding::RightClick,
        (InputMode::Mouse, Button::A | Button::Y | Button::LeftShoulder) => Binding::LeftClick,
        (InputMode::Mouse, Button::B | Button::X | Button::RightShoulder) => Binding::RightClick,
        (InputMode::Mouse, Button::DPadUp | Button::DPadDown | Button::DPadLeft | Button::DPadRight) => Binding::None,
        _ => Binding::Key(if use_cfg { keymap.button_to_keycode(button, false) }
            else { keycodes::KeyMap::empty().button_to_keycode(button, false) }.into_i32()),
    }
}

fn stick_kind(index: usize, mode: InputMode) -> Stick {
    profiles::profile(index).map(|p| p.stick).unwrap_or(if mode == InputMode::Keyboard { Stick::Directions } else { Stick::Mouse })
}

fn binding_label(binding: Binding) -> String {
    let key_name = |code| match code {
        profiles::UP => "↑".into(), profiles::DOWN => "↓".into(), profiles::LEFT => "←".into(), profiles::RIGHT => "→".into(),
        profiles::SHIFT => "Shift".into(), profiles::CTRL => "Ctrl".into(), 13 => "Enter".into(), 27 => "Esc".into(),
        _ => Keycode::from_i32(code).map(|k| k.name()).unwrap_or_else(|| t("无").into()),
    };
    match binding {
        Binding::Key(code) => key_name(code), Binding::Chord(keys) => keys.iter().map(|k| key_name(*k)).collect::<Vec<_>>().join("+"),
        Binding::Combo(..) => controls::binding_label(binding),
        Binding::None => t("无").into(), Binding::LeftClick => t("左键").into(), Binding::RightClick => t("右键").into(),
        Binding::Slow => t("按住慢速").into(), Binding::Fast => t("按住快速").into(), Binding::Wheel(1) => t("滚轮上").into(),
        Binding::Wheel(_) => t("滚轮下").into(), Binding::DragLock => t("拖拽锁定").into(), Binding::Center => t("指针居中").into(),
        Binding::Pointer(_) => controls::binding_label(binding),
    }
}

fn profile_previews(keymap: &keycodes::KeyMap) -> Vec<[String; 4]> {
    (0..30).map(|variant| {
        let use_cfg = variant < 15;
        let i = variant % 15;
        let (index, mode) = if i < 3 { (0, [InputMode::Hybrid, InputMode::Keyboard, InputMode::Mouse][i]) }
            else { (i - 2, InputMode::Hybrid) };
        let label = |button| binding_label(binding_for(keymap, index, mode, button, use_cfg));
        let fine = profiles::profile(index).is_some_and(|p| p.dpad.is_none()) || (index == 0 && mode == InputMode::Mouse);
        let directions = DPAD.iter().map(|b| {
            if fine && binding_for(keymap, index, mode, *b, use_cfg) == Binding::None { t("鼠标").into() } else { label(*b) }
        }).collect::<Vec<_>>().join(" / ");
        let stick = match stick_kind(index, mode) { Stick::Directions => t("同十字键"), Stick::Steering => t("仅左右转向"), Stick::Mouse => t("鼠标"), Stick::None => t("无"), Stick::Custom => t("独立映射") };
        let overrides = PROFILE_BUTTONS.iter().chain(DPAD.iter()).filter(|b| keymap.custom_keycode(**b).is_some()).count();
        [crate::tr!("十字键 上/下/左/右：{directions}；摇杆：{stick}", "D-pad Up/Down/Left/Right: {directions}; Stick: {stick}"),
         format!("A={}   B={}   X={}   Y={}", label(Button::A), label(Button::B), label(Button::X), label(Button::Y)),
         format!("L1={}   R1={}   L2={}   R2={}", label(Button::LeftShoulder), label(Button::RightShoulder), label(Button::Paddle1), label(Button::Paddle2)),
         crate::tr!("Start={}   Select={}；cfg {}；方案行 X 切换", "Start={}   Select={}; cfg {}; X toggles on profile row", label(Button::Start), label(Button::Back),
            if use_cfg { crate::tr!("覆盖 {overrides} 项", "{overrides} overrides") } else { t("已忽略").into() })]
    }).collect()
}

struct Frontend {
    progress: Arc<Mutex<experience::Progress>>,
    settings_path: PathBuf,
    language_path: PathBuf,
    movie_size: (u32,u32),
    display_size: (u32,u32),
    restart_slot: Option<usize>,
    restart_renderer: Option<GraphicsBackend>,
    menu_device: session::shortcuts::Device,
    chords: session::shortcuts::Chords,
    menu_held: bool,
    repeater: Repeater,
    system_volume: session::system_volume::Monitor,
    blocked: std::collections::BTreeSet<u8>,
    state: Session,
    active: ActivePlayer,
    keymap: keycodes::KeyMap,
    cursor: (f32, f32),
    cursor_idle: cursor::Idle,
    cursor_visible: bool,
    bounds: ContentBounds,
    quit: bool,
    redraw: bool,
    resumed: bool,
    started: Instant,
    diagnostic: bool,
    scripted_input: bool,
    ticks: u64,
}

impl Frontend {
    fn refresh_saves(&mut self) {
        let p=self.progress.lock().unwrap();
        self.state.experience.slots=p.slots();
        self.state.experience.active=p.active.unwrap_or(0);
    }
    fn flush_progress(&mut self,announce:bool)->bool {
        if self.state.experience.app || !self.state.experience.started {return true;}
        self.progress.lock().unwrap().error=None;
        self.progress.lock().unwrap().retry_deletes();
        let core_ok=self.active.player.lock().unwrap().flush_shared_objects_checked();
        let p=self.progress.lock().unwrap();
        let error=p.error.clone().or_else(||if core_ok{None}else{Some(t("Flash 进度序列化或写盘失败").into())});let has_data=p.has_data();let active=p.active.unwrap_or(0);drop(p);
        self.refresh_saves();
        if let Some(error)=error {self.state.experience.message=crate::tr!("保存失败：{error}", "Save failed: {error}");self.toast(crate::tr!("保存失败：{error}", "Save failed: {error}"));return false;}
        if announce {self.toast(if has_data {crate::tr!("存档 {} 的原生进度已保存", "Native progress saved to slot {}",active+1)}else{t("游戏尚未产生原生进度，请先使用游戏内保存功能").into()});}
        println!("leaf-progress flush=ok slot={} has_data={has_data}",active+1);true
    }
    fn apply_av(&mut self) {
        use ruffle_core::StageScaleMode;
        use ruffle_render::quality::StageQuality;
        let s=self.state.experience.settings;
        let mut p=self.active.player.lock().unwrap();
        p.set_volume(1.0);
        p.set_scale_mode([StageScaleMode::ShowAll,StageScaleMode::ExactFit,StageScaleMode::NoScale][s.scale as usize]);
        p.set_quality([StageQuality::Low,StageQuality::Medium,StageQuality::High][s.quality as usize]);
        p.set_letterbox(if s.scale==0{ruffle_core::config::Letterbox::On}else{ruffle_core::config::Letterbox::Off});
        drop(p);
        let (w,h)=self.display_size;
        self.bounds=if s.scale==0 {ContentBounds::for_show_all(w,h,self.movie_size.0,self.movie_size.1)}
            else {ContentBounds{min_x:0.,min_y:0.,max_x:w as f32,max_y:h as f32}};
        self.cursor=self.bounds.clamp(self.cursor.0,self.cursor.1,1.);
        self.redraw=true;
        println!("leaf-av applied={s:?}");
    }
    fn experience_nav(&mut self,nav:Nav) {
        let command=self.state.experience.nav(nav);
        let result=(|| -> Result<(),String>{
            match command {
                ExperienceCommand::None=>{},
                ExperienceCommand::Language(language)=>{
                    session::i18n::select(&self.language_path,language)?;
                    self.state.experience.language=language;
                    self.state.experience.message=if language==session::i18n::Language::English{"Language saved"}else{"语言已保存"}.into();
                    self.state.controls.status.clear();
                    self.state.toast=None;
                },
                ExperienceCommand::Quit=>self.quit=true,
                ExperienceCommand::Save=>{self.flush_progress(true);},
                ExperienceCommand::Choose(n)=>{
                    if self.state.experience.started {if self.flush_progress(false){self.restart_slot=Some(n);self.quit=true;}}
                    else {
                        self.progress.lock().unwrap().select(n)?;
                        self.state.experience.started=true;self.state.experience.open(ExperiencePage::Home);self.refresh_saves();
                        let result=self.state.resume();self.menu_result(result);
                        println!("leaf-progress selected={} before_first_tick=true ticks={}",n+1,self.ticks);
                    }
                },
                ExperienceCommand::Copy(n)=>{
                    if self.flush_progress(false){self.progress.lock().unwrap().copy_to(n)?;self.refresh_saves();self.state.experience.open(ExperiencePage::Saves);self.toast(crate::tr!("原生进度已复制到存档 {}", "Native progress copied to slot {}",n+1));}
                },
                ExperienceCommand::Settings=>{
                    self.apply_av();self.state.experience.settings.save(&self.settings_path)?;
                    self.state.experience.message=t("已保存全局画面设置").into();
                },
                ExperienceCommand::Renderer(backend)=>{
                    if backend!=self.state.experience.actual_renderer&&self.flush_progress(false) {
                        self.restart_renderer=Some(backend);self.quit=true;
                    }
                },
                ExperienceCommand::Clear(kind)=>{
                    if !self.state.experience.app{return Err(t("请从应用入口管理数据").into());}
                    let p=self.progress.lock().unwrap();let root=p.root.clone();drop(p);
                    let controls=&self.state.controls.store.root;
                    let trash=root.parent().ok_or(t("缺少数据父目录"))?.join("cleared-data");
                    let game=self.state.experience.games.get(self.state.experience.game_index).cloned();
                    let mut targets=Vec::new();
                    match kind {
                        1=>targets.push(root.join("games").join(game.ok_or(t("未选择游戏"))?.id)),
                        2=>{let game=game.ok_or(t("未选择游戏"))?;targets.push(controls.join("slots").join(&game.control_id));targets.push(controls.join("games").join(format!("{}.rmap",game.control_id)));targets.push(controls.join("games").join(format!("{}.rmap.bak",game.control_id)));},
                        3=>targets.push(root.join("games")),
                        4=>{for name in ["slots","games","global.rmap","global.rmap.bak"] {targets.push(controls.join(name));}},
                        _=>return Err(t("未知清理操作").into())
                    }
                    let mut changed=false;for path in targets{changed|=experience::archive(&path,&trash)?;}
                    self.state.experience.games=experience::entries(&root);
                    self.state.experience.game_index=self.state.experience.game_index.min(self.state.experience.games.len().saturating_sub(1));
                    self.state.experience.open(ExperiencePage::Manager);
                    self.state.experience.message=if changed{t("已清空；旧数据已移到本地备份目录")}else{t("没有需要清理的数据")}.into();
                    println!("leaf-manager clear={kind} archived={changed}");
                }
            }Ok(())
        })();
        if let Err(error)=result{self.state.experience.message=error.clone();self.toast(error);}
        self.redraw=true;
    }
    fn snapshot(&self) -> Mapping {
        if let Some(active) = &self.state.controls.active { let mut m = active.clone(); m.speed = self.state.mouse_speed; return m; }
        let mut buttons = [Binding::None; 14];
        for (i, button) in PROFILE_BUTTONS.iter().chain(DPAD.iter()).enumerate() { buttons[i] = self.binding(*button); }
        let mut mapping = Mapping::group(0); mapping.buttons=buttons; mapping.speed=self.state.mouse_speed; mapping
    }

    fn stick_kind(&self) -> Stick { self.state.controls.active.as_ref().map(|m| if m.has_pointer() {Stick::Mouse} else {m.stick}).unwrap_or(Stick::Mouse) }

    fn toast(&mut self, message: String) { println!("leaf-toast {message}"); self.state.toast=Some((message,Instant::now())); self.redraw=true; }
    fn apply_mapping(&mut self, mapping: Mapping) {
        self.cancel_repeats();
        self.cursor_idle.reveal(self.started.elapsed());
        let released = self.state.ledger.release_all(); self.emit(released,false);
        for i in 0..4 {self.state.release(Source::Stick(i));}
        self.state.drag_locked=false; self.state.require_neutral();
        self.state.profile=mapping.base; self.state.preview_profile=mapping.base;
        self.state.mode=mapping.mode; self.state.preview_mode=mapping.mode;
        self.state.mouse_speed=mapping.speed; self.state.preview_mouse_speed=mapping.speed;
        self.state.use_cfg=false; self.state.preview_use_cfg=false; self.redraw=true;
        println!("leaf-controls applied source={} mapping={mapping:?}",self.state.controls.source);
    }
    fn switch_slot(&mut self, slot: usize) {
        self.state.controls.active=Some(self.snapshot());
        if let ResultAction::Apply(mapping) = self.state.controls.switch_slot(slot) {
            self.apply_mapping(mapping); self.toast(self.state.controls.status.clone());
        }
    }
    fn poll_chords(&mut self) {
        for (snapshot,at) in self.menu_device.drain() {
        self.menu_held=snapshot.menu;
        self.system_volume.observe(snapshot.volume_up,snapshot.volume_down,snapshot.menu,self.started.elapsed());
        for command in self.chords.update_at(snapshot,at) {
            if command==session::shortcuts::Command::MenuTap {
                println!("leaf-session physical_menu_tap");self.toggle_menu();continue;
            }
            if self.state.paused {continue;}
            let consumed=match command {session::shortcuts::Command::Previous=>Some(Button::A),session::shortcuts::Command::Next=>Some(Button::B),session::shortcuts::Command::SwapDirections=>Some(Button::X),session::shortcuts::Command::Drag=>Some(Button::DPadDown),_=>None};
            if let Some(button)=consumed {self.blocked.insert(button as u8);}
            // The buttons used for a Menu chord must not remain held in Flash.
            for button in DPAD.into_iter().chain([Button::A,Button::B,Button::X]) {
                let released=self.state.release(Source::Controller(button as u8)); self.emit(released,false);
            }
            use session::shortcuts::Command;
            match command {
                Command::Previous | Command::Next => {
                    if let Some(slot)=self.state.controls.shortcut_slot(if command==Command::Previous {-1} else {1}) {
                        self.switch_slot(slot);
                    } else {self.toast(t("没有其他非空按键方案").into());}
                },
                Command::SwapDirections => {
                    self.cancel_repeats();
                    let released=self.state.toggle_directions();self.emit(released,false);
                    self.toast(if self.state.directions_swapped {t("十字键 ↔ 摇杆：已互换（本次）")} else {t("十字键 ↔ 摇杆：已恢复")}.into());
                },
                Command::Drag => { if self.can_game_input() {let transitions=self.state.toggle_drag(); self.emit(transitions,false); self.toast(if self.state.drag_locked {t("鼠标左键保持：开")} else {t("鼠标左键保持：关")}.into());} }
                Command::MenuTap=>{},
            }
        }
        }
        if !self.menu_device.available() {self.menu_held=false;}
    }

    fn controls_nav(&mut self, nav: Nav) {
        match self.state.controls.nav(nav) {
            ResultAction::Apply(mapping) => {
                self.apply_mapping(mapping);
                if self.state.controls.page == controls::Page::Edit && self.state.controls.status.starts_with(t("已切换")) {self.toast(self.state.controls.status.clone());}
            }
            ResultAction::Cursor(settings) => {
                self.state.cursor_settings=settings; self.cursor_idle.reveal(self.started.elapsed());
                println!("leaf-cursor global_saved settings={settings:?}");
            },
            ResultAction::Speed(speed) => self.menu_result(MenuResult::MouseSpeedChanged(speed)),
            ResultAction::Close => println!("leaf-controls editor_closed"),
            ResultAction::None => {}
        }
        self.redraw = true;
    }
    fn can_game_input(&self) -> bool {
        self.started.elapsed() >= Duration::from_millis(500) && self.state.allow_game_input()
    }

    fn emit(&self, transitions: Vec<Transition>, text: bool) {
        if transitions.is_empty() { return; }
        let mut player = self.active.player.lock().unwrap();
        for transition in transitions {
            if self.diagnostic { println!("leaf-input {:?} down={} elapsed_us={}", transition.action, transition.down,self.started.elapsed().as_micros()); }
            match transition.action {
                Action::Key(key) => {
                    if let Some(key) = Keycode::from_i32(key).and_then(keycodes::sdl2_keycode_to_ruffle_key_descriptor) {
                        player.handle_event(if transition.down { PlayerEvent::KeyDown { key } } else { PlayerEvent::KeyUp { key } });
                        if transition.down && text {
                            if let LogicalKey::Character(codepoint) = key.logical_key {
                                player.handle_event(PlayerEvent::TextInput { codepoint });
                            }
                        }
                    }
                }
                mouse => {
                    let button = if mouse == Action::MouseLeft { RMouseBtn::Left } else { RMouseBtn::Right };
                    player.handle_event(if transition.down {
                        PlayerEvent::MouseDown { x: self.cursor.0 as f64, y: self.cursor.1 as f64, button, index: None }
                    } else {
                        PlayerEvent::MouseUp { x: self.cursor.0 as f64, y: self.cursor.1 as f64, button }
                    });
                }
            }
        }
    }

    fn open_menu(&mut self) {
        self.cancel_repeats();
        if self.state.experience.app || !self.state.experience.started {return;}
        let released = self.state.open_menu();
        self.emit(released, false);
        self.active.player.lock().unwrap().set_is_playing(false);
        self.state.experience.open(ExperiencePage::Home);
        self.flush_progress(false);
        self.redraw = true;
        println!("leaf-session pause_open audio=paused inputs=released ticks={}", self.ticks);
    }

    fn menu_result(&mut self, result: MenuResult) {
        match result {
            MenuResult::Resume => {
                self.active.player.lock().unwrap().set_is_playing(true);
                self.resumed = true;
                println!("leaf-session resume mode={:?} waiting_for_neutral=true ticks={} mouse_speed={:?}", self.state.mode, self.ticks, self.state.mouse_speed);
            }
            MenuResult::Quit => {if self.flush_progress(false){self.quit=true;}},
            MenuResult::ModeChanged(mode) => { self.cursor_idle.reveal(self.started.elapsed()); self.state.controls.active = None; self.state.controls.source = t("仅本次").into(); println!("leaf-session mode_applied={mode:?} scope=session"); },
            MenuResult::MouseSpeedChanged(speed) => {
                self.state.mouse_speed=speed;self.state.preview_mouse_speed=speed;
                if let Some(mapping) = &mut self.state.controls.active { mapping.speed = speed; self.state.controls.source = t("仅本次（速度已改）").into(); }
                println!("leaf-session mouse_speed_applied={speed:?} pixels_per_second={} scope=session", speed.pixels_per_second());
            }
            MenuResult::ProfileChanged(index) => {
                self.cursor_idle.reveal(self.started.elapsed());
                if let Some(mapping) = self.state.controls.active.as_ref().filter(|m| m.base == index && !self.state.use_cfg) {
                    self.state.mode = mapping.mode; self.state.preview_mode = mapping.mode;
                } else { self.state.controls.active = None; self.state.controls.source = t("仅本次").into(); }
                let released = self.state.ledger.release_all(); self.emit(released, false);
                println!("leaf-session profile_applied={} scope=session use_cfg={}", profiles::profile(index).map(|p| p.id).unwrap_or("legacy"), self.state.use_cfg);
            }
            MenuResult::OpenControls => { let actual = self.snapshot(); self.state.controls.begin(actual); }
            MenuResult::SlotChanged(slot) => self.switch_slot(slot),
            MenuResult::OpenCursor => { self.state.controls.begin_cursor(self.state.cursor_settings,self.state.mouse_speed); }
            MenuResult::SaveProgress => {self.flush_progress(true);},
            MenuResult::OpenSaves => {self.flush_progress(false);self.refresh_saves();self.state.experience.open(ExperiencePage::Saves);},
            MenuResult::OpenSettings => {self.state.experience.open(ExperiencePage::Settings);},
            MenuResult::OpenShortcuts => {self.state.experience.open(ExperiencePage::Shortcuts);},
            MenuResult::OpenAbout => {self.state.experience.open(ExperiencePage::About);},
            MenuResult::OpenLanguage => {self.state.experience.open(ExperiencePage::Language);},
            MenuResult::None => {}
        }
        self.redraw = true;
    }

    fn toggle_menu(&mut self) {
        if self.state.experience.app || !self.state.experience.started {return;}
        if self.state.experience.page!=ExperiencePage::Home {
            self.experience_nav(Nav::B);return;
        }
        if self.state.paused {
            if self.state.controls.visible {self.controls_nav(Nav::B);return;}
            let result = self.state.resume();
            self.menu_result(result);
        } else {
            self.open_menu();
        }
    }

    fn binding(&self, button: Button) -> Binding {
        if let Some(mapping) = &self.state.controls.active {
            if button == Button::LeftStick {return mapping.axes[4];}
            return PROFILE_BUTTONS.iter().chain(DPAD.iter()).position(|b| *b == button).map(|i|
                if self.state.directions_swapped && i>=10 {mapping.axes[i-10]} else {mapping.buttons[i]}).unwrap_or(Binding::None);
        }
        binding_for(&self.keymap, self.state.profile, self.state.mode, button, self.state.use_cfg)
    }

    fn repeat_rate(&self, button: Button) -> Rate {
        let Some(mapping)=&self.state.controls.active else {return Rate::Off;};
        if button==Button::LeftStick {return mapping.axis_turbo[4];}
        PROFILE_BUTTONS.iter().chain(DPAD.iter()).position(|b|*b==button).map(|i|if self.state.directions_swapped&&i>=10 {mapping.axis_turbo[i-10]}else{mapping.turbo[i]}).unwrap_or(Rate::Off)
    }
    fn cancel_repeats(&mut self) {let released=self.repeater.clear(&mut self.state.ledger);self.emit(released,false);}
    fn update_repeats(&mut self) {
        if !self.can_game_input() || self.menu_held {self.cancel_repeats();return;}
        let transitions=self.repeater.update(self.started.elapsed(),&mut self.state.ledger);self.emit(transitions,false);
    }
    fn apply_binding(&mut self, source: Source, binding: Binding, rate: Rate, text: bool) {
        if !self.can_game_input() { return; }
        if self.diagnostic || matches!(binding, Binding::Chord(_) | Binding::Combo(..) | Binding::DragLock | Binding::Center) {
            println!("leaf-binding source={source:?} binding={binding:?}");
        }
        let result = match binding {
            Binding::DragLock => self.state.toggle_drag(),
            Binding::Center => { self.move_pointer(self.bounds.center()); Vec::new() }
            Binding::Wheel(lines) => {
                self.active.player.lock().unwrap().handle_event(PlayerEvent::MouseWheel {
                    delta: ruffle_core::events::MouseWheelDelta::Lines(lines as f64),
                }); Vec::new()
            }
            _ => self.repeater.press(source,binding,rate,self.started.elapsed(),&mut self.state.ledger),
        };
        self.emit(result, text && !matches!(binding, Binding::Chord(_) | Binding::Combo(..)));
        self.redraw = true;
    }

    fn move_pointer(&mut self, next: (f32, f32)) {
        if next == self.cursor { return; }
        self.cursor = next;
        self.cursor_idle.moved(next, self.started.elapsed());
        self.active.player.lock().unwrap().handle_event(PlayerEvent::MouseMove { x: next.0 as f64, y: next.1 as f64 });
        self.redraw = true;
    }

    fn button(&mut self, button: Button, source: Source, down: bool) {
        self.poll_chords();
        if !down {
            let released=self.repeater.release(source,&mut self.state.ledger);self.emit(released,false);
            self.blocked.remove(&(button as u8));
            let released = self.state.release(source);
            self.emit(released, false);
            return;
        }
        if button != Button::Guide && (self.menu_held || self.blocked.contains(&(button as u8))) {
            self.blocked.insert(button as u8); return;
        }
        if button == Button::Guide {
            // The physical observer owns Menu taps when available. Leaf's
            // synthetic Guide events must neither duplicate a tap nor acquire
            // a game-held source whose release can be lost after a chord.
            if self.menu_device.available()&&!self.scripted_input {return;}
            println!("leaf-session guide_received");
            self.toggle_menu();
            return;
        }
        if !self.state.mark_down(source) { return; }
        if self.state.paused {
            if self.state.experience.page!=ExperiencePage::Home {
                let nav=match button{
                    Button::DPadUp=>Some(Nav::Up),Button::DPadDown=>Some(Nav::Down),Button::DPadLeft=>Some(Nav::Left),Button::DPadRight=>Some(Nav::Right),
                    Button::A|Button::Start=>Some(Nav::A),Button::B|Button::Back=>Some(Nav::B),Button::Y=>Some(Nav::Y),
                    Button::LeftShoulder=>Some(Nav::Prev),Button::RightShoulder=>Some(Nav::Next),_=>None};
                if let Some(nav)=nav{self.experience_nav(nav);}return;
            }
            if self.state.controls.visible {
                let nav = match button {
                    Button::DPadUp => Some(Nav::Up), Button::DPadDown => Some(Nav::Down), Button::DPadLeft => Some(Nav::Left), Button::DPadRight => Some(Nav::Right),
                    Button::A | Button::Start => Some(Nav::A), Button::B | Button::Back => Some(Nav::B), Button::X => Some(Nav::X), Button::Y => Some(Nav::Y),
                    Button::LeftShoulder => Some(Nav::Prev), Button::RightShoulder => Some(Nav::Next), _ => None,
                };
                if let Some(nav) = nav { self.controls_nav(nav); }
                return;
            }
            match button {
                Button::DPadUp => self.state.menu_move(-1),
                Button::DPadDown => self.state.menu_move(1),
                Button::DPadLeft => {let result=self.state.preview(-1);self.menu_result(result);},
                Button::DPadRight => {let result=self.state.preview(1);self.menu_result(result);},
                Button::X => self.state.toggle_cfg_preview(),
                Button::A | Button::Start => { let result = self.state.accept(); self.menu_result(result); }
                Button::B | Button::Back => { let result = self.state.resume(); self.menu_result(result); }
                _ => {}
            }
            self.redraw = true;
        } else if self.started.elapsed() >= Duration::from_millis(500) {
            self.apply_binding(source, self.binding(button), self.repeat_rate(button), true);
        }
    }

    fn keyboard(&mut self, key: Keycode, down: bool, repeat: bool) {
        let source = Source::Keyboard(key.into_i32());
        if !down {
            let released = self.state.release(source);
            self.emit(released, false);
            return;
        }
        if repeat || !self.state.mark_down(source) { return; }
        if self.state.paused&&self.state.experience.page!=ExperiencePage::Home {
            let nav=match key{Keycode::Up=>Some(Nav::Up),Keycode::Down=>Some(Nav::Down),Keycode::Left=>Some(Nav::Left),Keycode::Right=>Some(Nav::Right),
                Keycode::Return|Keycode::Space=>Some(Nav::A),Keycode::Escape|Keycode::Backspace=>Some(Nav::B),Keycode::Y=>Some(Nav::Y),
                Keycode::Q=>Some(Nav::Prev),Keycode::E=>Some(Nav::Next),_=>None};
            if let Some(nav)=nav{self.experience_nav(nav);}return;
        }
        if key == Keycode::Escape {
            self.toggle_menu();
        } else if self.state.paused {
            if self.state.controls.visible {
                let nav = match key {
                    Keycode::Up => Some(Nav::Up), Keycode::Down => Some(Nav::Down), Keycode::Left => Some(Nav::Left), Keycode::Right => Some(Nav::Right),
                    Keycode::Return => Some(Nav::A), Keycode::Backspace => Some(Nav::B), Keycode::X => Some(Nav::X), Keycode::Y => Some(Nav::Y),
                    Keycode::Q => Some(Nav::Prev), Keycode::E => Some(Nav::Next), _ => None,
                };
                if let Some(nav) = nav { self.controls_nav(nav); }
                return;
            }
            match key {
                Keycode::Up => self.state.menu_move(-1), Keycode::Down => self.state.menu_move(1),
                Keycode::Left => {let result=self.state.preview(-1);self.menu_result(result);},
                Keycode::Right => {let result=self.state.preview(1);self.menu_result(result);},
                Keycode::X => self.state.toggle_cfg_preview(),
                Keycode::Return | Keycode::Space => { let result = self.state.accept(); self.menu_result(result); }
                _ => {}
            }
            self.redraw = true;
        } else if self.started.elapsed() >= Duration::from_millis(500) {
            let result = self.state.game_press(source, Action::Key(key.into_i32()));
            self.emit(result, false);
            self.redraw = true;
        }
    }

    fn axis(&mut self, axis: Axis, value: i16) {
        match axis {
            Axis::LeftX => self.state.stick.0 = value,
            Axis::LeftY => self.state.stick.1 = value,
            Axis::TriggerLeft | Axis::TriggerRight => {
                let left = axis == Axis::TriggerLeft;
                let source = Source::Trigger(if left { 0 } else { 1 });
                let was_down = self.state.is_held(source);
                let down = if was_down { value > 8000 } else { value > 20000 };
                if down != was_down {
                    self.button(if left { Button::Paddle1 } else { Button::Paddle2 }, source, down);
                }
            }
            _ => {}
        }
    }

    fn update_stick(&mut self, seconds: f32) {
        if !self.can_game_input() || self.menu_held {return;}
        let mapping=self.snapshot().directions(self.state.directions_swapped);
        let (x,y)=session::stick_vector(self.state.stick.0,self.state.stick.1).unwrap_or((0.,0.));
        let strengths=[(-y).max(0.),y.max(0.),(-x).max(0.),x.max(0.)];
        let mut pointer=(0f32,0f32);
        let mut slow=false; let mut fast=false;
        let mut motion = |binding:Binding, amount:f32| {
            match binding {
                Binding::Pointer(0)=>pointer.1-=amount,Binding::Pointer(1)=>pointer.1+=amount,
                Binding::Pointer(2)=>pointer.0-=amount,Binding::Pointer(3)=>pointer.0+=amount,
                Binding::Slow if amount>0.3=>slow=true,Binding::Fast if amount>0.3=>fast=true,_=>{}
            }
        };
        for (i,amount) in strengths.into_iter().enumerate() {motion(mapping.axes[i],amount);}
        for button in BUTTONS {
            let trigger=match button {Button::Paddle1=>Some(0),Button::Paddle2=>Some(1),_=>None};
            if self.state.is_held(Source::Controller(button as u8)) || trigger.is_some_and(|i|self.state.is_held(Source::Trigger(i))) {motion(self.binding(button),1.);}
        }
        for (i,amount) in strengths.into_iter().enumerate() {
            let source=Source::Stick(i as u8);
            let down=amount > if self.state.is_held(source) {0.30} else {0.45};
            if down && self.state.mark_down(source) {self.apply_binding(source,mapping.axes[i],mapping.axis_turbo[i],false);}
            else if !down {let repeated=self.repeater.release(source,&mut self.state.ledger);self.emit(repeated,false);let released=self.state.release(source);self.emit(released,false);}
        }
        if mapping.fine && x==0. && y==0. {
            for (i,button) in DPAD.into_iter().enumerate() {
                if self.binding(button)==Binding::None && self.state.is_held(Source::Controller(button as u8)) {
                    match i {0=>pointer.1-=0.25,1=>pointer.1+=0.25,2=>pointer.0-=0.25,_=>pointer.0+=0.25}
                }
            }
        }
        let speed=if slow {session::MouseSpeed::Slow} else if fast {session::MouseSpeed::Fast} else {self.state.mouse_speed};
        let length=(pointer.0*pointer.0+pointer.1*pointer.1).sqrt().max(1.);
        let distance=speed.pixels_per_second()*seconds.clamp(0.,0.05);
        self.move_pointer(((self.cursor.0+pointer.0/length*distance).clamp(self.bounds.min_x,self.bounds.max_x-1.),
            (self.cursor.1+pointer.1/length*distance).clamp(self.bounds.min_y,self.bounds.max_y-1.)));
    }

}

fn push_button(events: &sdl2::EventSubsystem, button: Button, swap: bool) -> Result<(), String> {
    let button = normalize_button(button, swap);
    events.push_event(Event::ControllerButtonDown { timestamp: 0, which: 0, button })?;
    events.push_event(Event::ControllerButtonUp { timestamp: 0, which: 0, button })
}

pub async fn run() -> Result<(), String> {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();
    install_signals();
    let opt = Opt::parse();
    crate::performance::enable(opt.trace_performance);
    let mut performance = crate::performance::Performance::default();
    if opt.self_test_experience && (opt.storage_dir.is_none()||opt.controls_dir.is_none()) {
        return Err("Experience self-test requires explicit isolated --storage-dir and --controls-dir".into());
    }
    let swf_path = opt.swf.canonicalize().map_err(|e| format!("Cannot open SWF: {e}"))?;
    let keymap = keycodes::KeyMap::load_for_swf(&swf_path);
    let bytes = std::fs::read(&swf_path).map_err(|e| format!("Cannot read SWF: {e}"))?;
    let movie_url = Url::from_file_path(&swf_path).map_err(|_| "Cannot form a file URL for the selected game")?;
    let storage_dir = opt.storage_dir.clone().or_else(|| std::env::var_os("RUFFLE_STORAGE_DIR").map(PathBuf::from))
        .or_else(|| std::env::var_os("SAVES_PATH").map(|p| PathBuf::from(p).join("ruffle-leaf/shared_objects")))
        .unwrap_or_else(|| PathBuf::from("./shared_objects"));
    std::fs::create_dir_all(&storage_dir).map_err(|e| format!("Cannot create game storage: {e}"))?;
    println!("leaf-runtime source=3bc39c5+leaf-frontend storage={}", storage_dir.display());
    let controls_root = opt.controls_dir.clone().or_else(|| std::env::var_os("RUFFLE_CONTROLS_DIR").map(PathBuf::from))
        .unwrap_or_else(|| std::env::var_os("XDG_CONFIG_HOME").map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from(std::env::var_os("HOME").unwrap_or_default()).join(".config/ruffle-leaf")).join("controls"));
    if opt.self_test_slots && opt.controls_dir.is_none() { return Err("Controls self-test requires an explicit isolated --controls-dir".into()); }
    if opt.self_test_slots && controls_root.exists() && std::fs::read_dir(&controls_root).map_err(|e| e.to_string())?.next().is_some() {
        return Err("Controls self-test requires an empty isolated controls directory".into());
    }
    let language_path=session::i18n::path(&controls_root);
    let language_error=session::i18n::initialize(&language_path).err();
    if let Some(error)=&language_error{eprintln!("leaf-language initialization_save_failed={error}");}
    let control_store = controls::Store::new(controls_root, &swf_path);
    let progress=Arc::new(Mutex::new(experience::Progress::new(storage_dir.join("slots-v1"),&bytes,
        swf_path.file_stem().unwrap_or_default().to_string_lossy().into_owned(),
        control_store.game.file_stem().unwrap_or_default().to_string_lossy().into_owned(),movie_url.path().into())));
    if !opt.app_settings {progress.lock().unwrap().register()?;}
    let settings_path=experience::Settings::global_path(&control_store.root);
    let mut av=experience::Settings::load(&settings_path);
    let slots=progress.lock().unwrap().slots();
    let pick_slot=!opt.app_settings&&opt.progress_slot.is_none()&&slots.iter().any(|s|s.time!=0||s.broken);
    if !opt.app_settings&&!pick_slot {progress.lock().unwrap().select(opt.progress_slot.unwrap_or(1) as usize-1)?;}
    println!("leaf-controls startup slots=10 directory={}",control_store.root.display());

    let sdl = sdl2::init()?;
    // This frontend draws exactly one host pointer, including with a USB mouse.
    sdl.mouse().show_cursor(false);
    let video = sdl.video()?;
    // Vulkan uses a shared GBM image rather than a native Wayland VkSurface.
    println!("leaf-graphics window_driver={}", video.current_video_driver());
    let gamepads = sdl.game_controller()?;
    let events = sdl.event()?;
    let (init_width, init_height) = initial_window_size(&video, opt.width, opt.height);
    let attributes = video.gl_attr();
    attributes.set_context_profile(GLProfile::GLES);
    attributes.set_context_version(3, 0);
    attributes.set_double_buffer(true);
    attributes.set_depth_size(24);
    attributes.set_stencil_size(8);
    attributes.set_red_size(8); attributes.set_green_size(8); attributes.set_blue_size(8); attributes.set_alpha_size(8);
    let mut window = video.window("Ruffle Flash", init_width, init_height).opengl().resizable().build().map_err(|e| e.to_string())?;
    if opt.fullscreen { window.set_fullscreen(sdl2::video::FullscreenType::Desktop)?; }
    let mut size = window_drawable_size(&window);
    let context = window.gl_create_context()?;
    window.gl_make_current(&context)?;
    let gl = unsafe { glow::Context::from_loader_function(|name| video.gl_get_proc_address(name) as *const _) };
    unsafe {
        println!("GL_VENDOR={}", gl.get_parameter_string(glow::VENDOR));
        println!("GL_RENDERER={}", gl.get_parameter_string(glow::RENDERER));
        println!("GL_VERSION={}", gl.get_parameter_string(glow::VERSION));
        println!("GLSL_VERSION={}", gl.get_parameter_string(glow::SHADING_LANGUAGE_VERSION));
    }
    let gl=Arc::new(gl);
    let graphics_failure=Arc::new(Mutex::new(None::<String>));
    let requested=av.requested_renderer(opt.app_settings,opt.renderer.as_deref());
    let mut actual=requested;let mut renderer_failed=false;
    let renderer:Box<dyn RenderBackend>=if requested==GraphicsBackend::Vulkan {
        #[cfg(target_os="linux")]
        let attempt=unsafe{crate::vulkan_backend::create(gl.clone(),size.0,size.1,graphics_failure.clone()).await};
        #[cfg(not(target_os="linux"))]
        let attempt:Result<Box<dyn RenderBackend>,String>=Err("This Vulkan presentation bridge requires Linux".into());
        match attempt {
            Ok(renderer)=>renderer,
            Err(error)=>{
                if opt.renderer.is_some(){return Err(error);}
                eprintln!("leaf-graphics vulkan_startup_failed={error} fallback=opengl-es");
                actual=GraphicsBackend::OpenGlEs;renderer_failed=true;av.renderer=actual;
                if let Err(error)=av.save(&settings_path){eprintln!("leaf-graphics fallback_preference_save_failed={error}");}
                *graphics_failure.lock().unwrap()=None;
                Box::new(unsafe{GlRenderBackend::new_shared(gl.clone(),size.0,size.1).map_err(|e|format!("Renderer: {e:?}"))?})
            }
        }
    } else {Box::new(unsafe{GlRenderBackend::new_shared(gl.clone(),size.0,size.1).map_err(|e|format!("Renderer: {e:?}"))?})};
    println!("leaf-graphics actual={} renderer={}",actual.name(),renderer.debug_info());
    let font_path = std::env::var_os("RUFFLE_MENU_FONT").map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("res/menu-font.ttf"));
    let mut menu = unsafe { PauseMenu::new(gl, &font_path, &profile_previews(&keymap))? };
    let (sender, receiver) = mpsc::channel::<RuffleEvent>();
    let sender = EventSender { sender };
    let id = PlayerId::new();
    let navigator = ExternalNavigatorBackend::new(
        movie_url.clone(), None, None,
        AndroidExecutor { event_loop: sender.clone(), player_id: id }, None, true, Default::default(),
        ruffle_core::backend::navigator::SocketMode::Allow,
        Rc::new(PlayingContent::DirectFile(ContentDescriptor::new_remote(movie_url.clone()))),
        LinuxNavigatorInterface,
    );
    let ui = SdlUiBackend::new();
    let cjk_fonts = ui.cjk_font_names();
    let mut builder = PlayerBuilder::new().with_ui(ui);
    if opt.no_audio {
        builder = builder.with_audio(NullAudioBackend::new());
    } else {
        match CpalAudioBackend::new(Some("default")) {
            Ok(audio) => builder = builder.with_audio(audio),
            Err(error) => {
                log::error!("Audio device initialization failed: {error}; using silent backend");
                builder = builder.with_audio(NullAudioBackend::new());
            }
        }
    }
    let player = builder.with_boxed_renderer(renderer)
        .with_storage(Box::new(NativeStorage(progress.clone())))
        .with_navigator(navigator).with_log(FileLogBackend::new(Some(Path::new("flashlog.txt"))))
        .with_video(SoftwareVideoBackend::new()).build();
    let movie = SwfMovie::from_data(&bytes, movie_url.to_string(), None).map_err(|e| format!("Invalid SWF: {e}"))?;
    let movie_size = (movie.width().to_pixels().round() as u32, movie.height().to_pixels().round() as u32);
    {
        let mut p = player.lock().unwrap();
        if !cjk_fonts.is_empty() {
            for family in [DefaultFont::Sans, DefaultFont::Serif, DefaultFont::Typewriter,
                           DefaultFont::JapaneseGothic, DefaultFont::JapaneseGothicMono, DefaultFont::JapaneseMincho] {
                p.set_default_font(family, cjk_fonts.clone());
            }
        }
        p.mutate_with_update_context(|context| context.set_root_movie(movie));
        p.set_letterbox(ruffle_core::config::Letterbox::On);
        p.set_viewport_dimensions(ViewportDimensions { width: size.0, height: size.1, scale_factor: 1.0 });
        p.set_is_playing(!opt.app_settings&&!pick_slot);
    }
    let bounds = ContentBounds::for_show_all(size.0, size.1, movie_size.0, movie_size.1);
    let mode = opt.input_mode.clone().or_else(|| std::env::var("RUFFLE_INPUT_MODE").ok()).unwrap_or_else(|| "hybrid".into());
    let mode = match mode.as_str() { "hybrid" => InputMode::Hybrid, "keyboard" => InputMode::Keyboard, "mouse" => InputMode::Mouse,
        _ => return Err("input mode must be hybrid, keyboard or mouse".into()) };
    let swap = std::env::var("RUFFLE_SWAP_AB").map(|s| s == "1").unwrap_or(true);
    let mut front = Frontend {
        progress,settings_path,movie_size,display_size:size,restart_slot:None,restart_renderer:None,
        menu_device:session::shortcuts::Device::new(), chords:session::shortcuts::Chords::default(), menu_held:false, repeater:Repeater::default(), system_volume:session::system_volume::Monitor::new(), blocked:Default::default(),
        language_path,
        state: Session::new(mode), active: ActivePlayer { id, player }, keymap, cursor: bounds.center(), cursor_idle: cursor::Idle::default(), cursor_visible: false, bounds,
        quit: false, redraw: true, resumed: false, started: Instant::now(), diagnostic: opt.self_test_slots || opt.self_test_experience || opt.trace_input, scripted_input: opt.self_test_slots || opt.self_test_experience, ticks: 0,
    };
    let mut controller: Option<sdl2::controller::GameController> = None;
    let mut pump = sdl.event_pump()?;
    front.state.controls = controls::Controls::new(control_store);
    front.state.cursor_settings=front.state.controls.cursor_settings;
    println!("leaf-cursor startup global={:?}",front.state.cursor_settings);
    front.state.experience.title=front.progress.lock().unwrap().title.clone();
    front.state.experience.settings=av;front.state.experience.app=opt.app_settings;
    front.state.experience.actual_renderer=actual;front.state.experience.renderer_failed=renderer_failed;
    front.state.experience.started=!opt.app_settings&&!pick_slot;
    front.refresh_saves();
    if opt.app_settings {
        front.state.experience.games=experience::entries(&front.progress.lock().unwrap().root);
        front.state.experience.open(ExperiencePage::Manager);front.state.paused=true;
    } else if pick_slot {front.state.experience.open(ExperiencePage::Saves);front.state.paused=true;}
    front.apply_av();
    front.state.experience.language=session::i18n::current();
    if language_error.is_some(){front.toast(t("语言偏好未能保存，请检查存储卡").into());}
    if renderer_failed{front.toast(t("Vulkan 启动失败，已恢复 OpenGL ES").into());}
    println!("leaf-progress startup picker={pick_slot} app={} active={:?}",opt.app_settings,front.progress.lock().unwrap().active);
    front.cursor_idle.rebase(front.cursor);
    let initial=front.state.controls.mappings[front.state.controls.game_slot].clone();
    front.state.controls.active=Some(initial.clone()); front.apply_mapping(initial);
    println!("leaf-controls startup active_slot={} source={}",front.state.controls.game_slot+1,front.state.controls.source);
    let mut clock = Instant::now();
    let mut last_flush=Instant::now();
    let mut fps_at=Instant::now();let mut fps_frames=0u64;
    let d=Button::DPadDown;let a=Button::A;let b=Button::B;let r=Button::DPadRight;
    let experience_script:Vec<Vec<Button>>=if opt.app_settings {
        vec![vec![],vec![d,a],vec![b],vec![d,a],vec![d,a],vec![d,d,a],vec![d,a],
            vec![d,d,d,a],vec![d,a],vec![d,d,d,d,a],vec![d,a],vec![]]
    } else {
        vec![if pick_slot{vec![a]}else{vec![]},vec![Button::Guide],vec![d,d,d,d,d,a],
            vec![r,d,r,d,a,a],vec![],vec![b,b],vec![],vec![Button::Guide],
            vec![d,d,d,a],vec![d,a],vec![d,d,d,d,Button::Y],vec![d,a],vec![]]
    };
    let mut experience_phase=0;
    let mut script_phase = 0;
    let mut snapshots = std::collections::BTreeSet::new();
    let mut sync_focus = false;
    let mut script=vec![vec![Button::Guide],vec![d,a,Button::RightShoulder,b],vec![b],
        PROFILE_BUTTONS.iter().chain(DPAD.iter()).copied().chain([Button::LeftStick]).collect(),
        vec![Button::Guide],vec![d,a],vec![a]];
    for _ in 0..9 {script.push(vec![Button::RightShoulder]);}
    script.extend([vec![r,a],vec![r,r,r,a]]); // A=Enter, select Y
    for _ in 0..5 {script.push(vec![Button::RightShoulder]);}
    script.extend([vec![d,d,d,d,d,r,a],vec![b],vec![d,d,a], // save and leave slot 2
        vec![b],vec![Button::Guide],vec![d,d,a],
        vec![r,d,r,r,d,r],vec![d,d,d,a],vec![b,b]]);
    let ui_phases=script.len();
    let mut idle_start=None;
    let mut axis_start=(0.,0.);
    if let Some(path) = &opt.capture_dir { std::fs::create_dir_all(path).map_err(|e| e.to_string())?; }
    println!("MOVIE STARTED mode={mode:?} swap_ab={swap} swap_xy=false resolution={}x{}", size.0, size.1);

    while !front.quit && !EXIT_REQUESTED.load(Ordering::Relaxed) {
        let now = Instant::now();
        let elapsed = now.duration_since(clock);
        clock = now;
        if front.started.elapsed() < Duration::from_millis(500) { front.state.require_neutral(); }
        else { front.state.refresh_neutral(); }
        if opt.diagnostic_seconds.is_some_and(|seconds| front.started.elapsed() >= Duration::from_secs(seconds)) { break; }
        if opt.self_test_experience&&experience_phase<experience_script.len()&&front.started.elapsed().as_secs_f32()>=1.+experience_phase as f32*0.7 {
            if experience_phase==0&&pick_slot&&front.ticks!=0{return Err("Slot picker advanced the Flash VM before selection".into());}
            println!("leaf-experience phase={experience_phase} page={:?} ticks={}",front.state.experience.page,front.ticks);
            if let Some(dir)=&opt.capture_dir{unsafe{menu.capture(&dir.join(format!("experience-{experience_phase:02}.bmp")),size.0,size.1)?;}}
            for &button in &experience_script[experience_phase]{push_button(&events,button,swap)?;}
            experience_phase+=1;
        }
        if opt.self_test_slots && script_phase < ui_phases && front.started.elapsed().as_secs_f32()>=1.+script_phase as f32*0.3 {
            println!("leaf-slots test_phase={script_phase} page={:?} row={}",front.state.controls.page,front.state.controls.row);
            for &button in &script[script_phase] {push_button(&events,button,swap)?;}
            script_phase+=1;
        } else if opt.self_test_slots && script_phase==ui_phases {
            front.move_pointer((400.,300.));
            events.push_event(Event::ControllerButtonDown{timestamp:0,which:0,button:Button::LeftStick})?;
            idle_start=Some(Instant::now());script_phase+=1;
        } else if opt.self_test_slots && script_phase==ui_phases+1 && idle_start.is_some_and(|at:Instant|at.elapsed()>=Duration::from_secs(4)) {
            if front.cursor_visible || !front.state.is_held(Source::Controller(Button::LeftStick as u8)) {return Err("Idle pointer did not hide while left mouse was held".into());}
            println!("leaf-cursor idle_check=passed hidden_while_held=true");
            events.push_event(Event::ControllerButtonUp{timestamp:0,which:0,button:Button::LeftStick})?;
            front.move_pointer((420.,300.));script_phase+=1;
        } else if opt.self_test_slots && script_phase==ui_phases+2 && idle_start.is_some_and(|at:Instant|at.elapsed()>=Duration::from_millis(4400)) {
            if !front.cursor_visible {return Err("Moved pointer did not reappear".into());}
            let mut mapping=front.snapshot();mapping.stick=Stick::Custom;
            mapping.axes=[Binding::Key(119),Binding::Key(115),Binding::Pointer(2),Binding::Pointer(3),Binding::RightClick];
            front.state.controls.active=Some(mapping.clone());front.apply_mapping(mapping);axis_start=front.cursor;script_phase+=1;
        } else if opt.self_test_slots && script_phase==ui_phases+3 && idle_start.is_some_and(|at:Instant|at.elapsed()>=Duration::from_millis(4700)) {
            for (axis,value) in [(Axis::LeftY,-32767),(Axis::LeftX,20000)] {events.push_event(Event::ControllerAxisMotion{timestamp:0,which:0,axis,value})?;}
            script_phase+=1;
        } else if opt.self_test_slots && script_phase==ui_phases+4 && idle_start.is_some_and(|at:Instant|at.elapsed()>=Duration::from_millis(5000)) {
            if !front.state.ledger.is_down(Action::Key(119)) || front.cursor.0<=axis_start.0 || front.cursor.1!=axis_start.1 {return Err("Mixed independent stick axes failed".into());}
            front.switch_slot(0);
            if front.state.ledger.is_down(Action::Key(119)) {return Err("Hot switch did not release held stick key".into());}
            for axis in [Axis::LeftX,Axis::LeftY] {events.push_event(Event::ControllerAxisMotion{timestamp:0,which:0,axis,value:0})?;}
            script_phase+=1;
        } else if opt.self_test_slots && script_phase==ui_phases+5 && idle_start.is_some_and(|at:Instant|at.elapsed()>=Duration::from_millis(5300)) {
            if !front.can_game_input() {return Err("Hot switch with held stick did not recover after centering".into());}
            front.switch_slot(1);script_phase+=1;
        } else if opt.self_test_slots && script_phase==ui_phases+6 && idle_start.is_some_and(|at:Instant|at.elapsed()>=Duration::from_millis(5600)) {
            events.push_event(Event::ControllerButtonDown{timestamp:0,which:0,button:Button::LeftStick})?;script_phase+=1;
        } else if opt.self_test_slots && script_phase==ui_phases+7 && idle_start.is_some_and(|at:Instant|at.elapsed()>=Duration::from_millis(5900)) {
            if !front.state.ledger.is_down(Action::MouseRight) {return Err("Independent stick press mapping failed".into());}
            events.push_event(Event::ControllerButtonUp{timestamp:0,which:0,button:Button::LeftStick})?;
            println!("leaf-stick independent_axes=passed mixed_keyboard_pointer=true independent_press=true hot_switch_while_deflected=true");
            push_button(&events,Button::Guide,swap)?;push_button(&events,Button::DPadUp,swap)?;push_button(&events,a,swap)?;script_phase+=1;
        }
        front.poll_chords();
        if let Some(level)=front.system_volume.poll(front.started.elapsed()) {
            if front.state.system_volume.as_ref().is_none_or(|(old,_)|*old!=level) {println!("leaf-volume system_percent={level} source=leaf");}
            front.state.system_volume=Some((level,Instant::now()));front.redraw=true;
        }
        if front.state.system_volume.as_ref().is_some_and(|(_,at)|at.elapsed()>=Duration::from_millis(1500)) {front.state.system_volume=None;front.redraw=true;}
        if front.state.toast.as_ref().is_some_and(|(_,at)|at.elapsed()>=Duration::from_secs(3)) {front.state.toast=None;front.redraw=true;}
        for event in pump.poll_iter() {
            match event {
                Event::Quit { .. } => front.quit = true,
                Event::ControllerDeviceAdded { which, .. } if controller.is_none() => {
                    match gamepads.open(which) {
                        Ok(pad) => {
                            println!("leaf-input controller={} mapping={}", pad.name(), pad.mapping());
                            for raw in BUTTONS {
                                if raw!=Button::Guide && pad.button(raw) { front.state.mark_down(Source::Controller(normalize_button(raw, swap) as u8)); }
                            }
                            front.state.stick = (pad.axis(Axis::LeftX), pad.axis(Axis::LeftY));
                            front.state.require_neutral();
                            controller = Some(pad);
                        }
                        Err(error) => log::warn!("Cannot open controller: {error}"),
                    }
                }
                Event::ControllerDeviceRemoved { which, .. } => {
                    if controller.as_ref().is_some_and(|pad| pad.instance_id() == which) {
                        front.cancel_repeats();let released = front.state.disconnected(); front.emit(released, false);
                        front.active.player.lock().unwrap().set_is_playing(false);
                        front.redraw = true;
                        controller = None;
                    }
                }
                Event::ControllerButtonDown { button, which, .. } => {
                    if front.diagnostic || controller.as_ref().is_some_and(|pad| pad.instance_id() == which) {
                        let physical = normalize_button(button, swap);
                        front.button(physical, Source::Controller(physical as u8), true);
                    }
                }
                Event::ControllerButtonUp { button, which, .. } => {
                    if front.diagnostic || controller.as_ref().is_some_and(|pad| pad.instance_id() == which) {
                        let physical = normalize_button(button, swap);
                        front.button(physical, Source::Controller(physical as u8), false);
                    }
                }
                Event::ControllerAxisMotion { axis, value, which, .. } => {
                    if front.diagnostic || controller.as_ref().is_some_and(|pad| pad.instance_id() == which) { front.axis(axis, value); }
                }
                Event::KeyDown { keycode: Some(key), repeat, .. } => front.keyboard(key, true, repeat),
                Event::KeyUp { keycode: Some(key), .. } => front.keyboard(key, false, false),
                Event::TextInput { text, .. } if front.can_game_input() => {
                    let mut p = front.active.player.lock().unwrap();
                    for codepoint in text.chars() { p.handle_event(PlayerEvent::TextInput { codepoint }); }
                    front.redraw = true;
                }
                Event::MouseMotion { x, y, .. } if front.can_game_input() => {
                    let logical = window.size();
                    let next = front.bounds.clamp(x as f32 * size.0 as f32 / logical.0.max(1) as f32,
                                                 y as f32 * size.1 as f32 / logical.1.max(1) as f32, 1.0);
                    front.move_pointer(next);
                }
                Event::MouseButtonDown { mouse_btn, x, y, .. } => {
                    let logical = window.size();
                    let px = x as f32 * size.0 as f32 / logical.0.max(1) as f32;
                    let py = y as f32 * size.1 as f32 / logical.1.max(1) as f32;
                    if px >= front.bounds.min_x && px < front.bounds.max_x && py >= front.bounds.min_y && py < front.bounds.max_y {
                        let action = match mouse_btn { sdl2::mouse::MouseButton::Left => Some(Action::MouseLeft),
                            sdl2::mouse::MouseButton::Right => Some(Action::MouseRight), _ => None };
                        if let Some(action) = action {
                            let source = Source::Mouse(mouse_btn as u8);
                            front.state.mark_down(source);
                            if front.can_game_input() {
                                let result = front.state.game_press(source, action); front.emit(result, false);
                            }
                            front.redraw = true;
                        }
                    }
                }
                Event::MouseButtonUp { mouse_btn, .. } => {
                    let result = front.state.release(Source::Mouse(mouse_btn as u8)); front.emit(result, false);
                }
                Event::MouseWheel { precise_y, .. } if front.can_game_input() => {
                    front.active.player.lock().unwrap().handle_event(PlayerEvent::MouseWheel {
                        delta: ruffle_core::events::MouseWheelDelta::Lines(precise_y as f64),
                    });
                    front.redraw = true;
                }
                Event::Window { win_event: WindowEvent::FocusLost, .. } => {
                    if !front.state.paused { front.open_menu(); }
                    front.active.player.lock().unwrap().handle_event(PlayerEvent::FocusLost);
                }
                Event::Window { win_event: WindowEvent::FocusGained, .. } => {
                    front.active.player.lock().unwrap().handle_event(PlayerEvent::FocusGained);
                    sync_focus = true;
                }
                Event::Window { win_event: WindowEvent::SizeChanged(_, _), .. } => {
                    size = window_drawable_size(&window);
                    front.display_size=size;front.apply_av();
                    front.cursor = front.bounds.clamp(front.cursor.0, front.cursor.1, 1.0);
                    front.cursor_idle.rebase(front.cursor);
                    front.active.player.lock().unwrap().set_viewport_dimensions(ViewportDimensions {
                        width: size.0, height: size.1, scale_factor: 1.0,
                    });
                    front.redraw = true;
                }
                Event::Window { win_event: WindowEvent::Close, .. } => front.quit = true,
                _ => {}
            }
        }
        if sync_focus {
            let mut physical = Vec::new();
            if let Some(pad) = &controller {
                for raw in BUTTONS {
                    if raw!=Button::Guide && pad.button(raw) { physical.push(Source::Controller(normalize_button(raw, swap) as u8)); }
                }
                for (index, axis) in [(0, Axis::TriggerLeft), (1, Axis::TriggerRight)] {
                    if pad.axis(axis) > 8000 { physical.push(Source::Trigger(index)); }
                }
                front.state.stick = (pad.axis(Axis::LeftX), pad.axis(Axis::LeftY));
            }
            for scan in pump.keyboard_state().pressed_scancodes() {
                if let Some(key) = Keycode::from_scancode(scan) { physical.push(Source::Keyboard(key.into_i32())); }
            }
            let mouse = pump.mouse_state();
            for button in [sdl2::mouse::MouseButton::Left, sdl2::mouse::MouseButton::Right] {
                if mouse.is_mouse_button_pressed(button) { physical.push(Source::Mouse(button as u8)); }
            }
            front.cancel_repeats();let released = front.state.resync_held(physical);
            front.emit(released, false);
            sync_focus = false;
        }
        if front.quit { break; }
        let seconds = if front.resumed { 0.0 } else { elapsed.as_secs_f64() };
        if front.resumed {
            println!("leaf-session resume_first_dt_us=0");
            front.resumed = false;
        }
        if !front.state.paused {
            front.update_stick(seconds as f32);
            front.update_repeats();
            let tasks_at = crate::performance::Performance::start(opt.trace_performance);
            for _ in 0..64 {
                match receiver.try_recv() {
                    Ok(RuffleEvent::TaskPoll(task)) if *task.0.metadata() == front.active.id => { task.0.run(); }
                    Ok(RuffleEvent::RunContextMenuCallback(index)) => { front.active.player.lock().unwrap().run_context_menu_callback(index); }
                    Ok(RuffleEvent::ClearContextMenu) => { front.active.player.lock().unwrap().clear_custom_menu_items(); }
                    Ok(RuffleEvent::RequestContextMenu) => { let _ = front.active.player.lock().unwrap().prepare_context_menu(); }
                    Ok(_) => {}
                    Err(_) => break,
                }
            }
            performance.tasks += crate::performance::Performance::elapsed(tasks_at);
            let mut p = front.active.player.lock().unwrap();
            let tick_at = crate::performance::Performance::start(opt.trace_performance);
            p.tick(FloatDuration::from_millis(seconds * 1000.0));
            let tick_cost = crate::performance::Performance::elapsed(tick_at);
            performance.tick += tick_cost;
            performance.max_tick = performance.max_tick.max(tick_cost);
            front.ticks += 1;
            front.redraw |= p.needs_render();
        }
        if !front.state.paused&&last_flush.elapsed()>=Duration::from_secs(10){front.flush_progress(false);last_flush=Instant::now();}
        if fps_at.elapsed()>=Duration::from_secs(1) {
            let p=front.active.player.lock().unwrap();let frames=p.frames_run();
            front.state.experience.fps=if front.state.paused{0.}else{frames.saturating_sub(fps_frames) as f64/fps_at.elapsed().as_secs_f64()};
            front.state.experience.target_fps=p.frame_rate();
            if opt.trace_performance {performance.log(fps_at.elapsed(), frames.saturating_sub(fps_frames),p.frame_rate(),front.state.paused);}
            drop(p);fps_frames=frames;fps_at=Instant::now();
            if front.state.paused||front.state.experience.settings.fps{front.redraw=true;}
        }
        let cursor_visible = !front.state.paused && front.stick_kind() == Stick::Mouse && front.cursor_idle.visible(front.started.elapsed());
        if cursor_visible != front.cursor_visible {
            front.cursor_visible = cursor_visible; front.redraw = true;
            if front.diagnostic { println!("leaf-cursor visible={cursor_visible} elapsed_ms={}",front.started.elapsed().as_millis()); }
        }
        if front.redraw {
            unsafe {
                if front.state.paused {
                    if front.state.experience.started&&!front.state.experience.app {front.active.player.lock().unwrap().render();}
                    menu.draw(size.0, size.1, &front.state, None);
                } else {
                    let mut p = front.active.player.lock().unwrap();
                    let render_at = crate::performance::Performance::start(opt.trace_performance);
                    p.render();
                    let render_cost = crate::performance::Performance::elapsed(render_at);
                    performance.render += render_cost;
                    performance.max_render = performance.max_render.max(render_cost);
                    performance.draws += 1;
                    let collect_at = crate::performance::Performance::start(opt.trace_performance);
                    p.collect_static_render_resources();
                    performance.collect += crate::performance::Performance::elapsed(collect_at);
                    drop(p);
                    if front.state.experience.settings.fps || front.state.drag_locked || front.cursor_visible || front.state.toast.is_some() || front.state.system_volume.is_some() { menu.draw(size.0, size.1, &front.state, if front.cursor_visible {Some(front.cursor)} else {None}); }
                }
                if opt.self_test_slots {
                    if let Some(dir)=&opt.capture_dir {
                        let label=format!("slots-{script_phase:02}-{:?}.bmp",if front.state.controls.visible {front.state.controls.page} else {controls::Page::Edit});
                        if snapshots.insert(label.clone()) {menu.capture(&dir.join(label),size.0,size.1)?;}
                    }
                }
                if opt.self_test_experience {
                    if let Some(dir)=&opt.capture_dir {
                        let label=format!("experience-ui-{experience_phase:02}-{:?}.bmp",front.state.experience.page);
                        if snapshots.insert(label.clone()){menu.capture(&dir.join(label),size.0,size.1)?;}
                    }
                }
            }
            let swap_at = crate::performance::Performance::start(opt.trace_performance);
            window.gl_swap_window();
            performance.swap += crate::performance::Performance::elapsed(swap_at);
            front.redraw = false;
        }
        // Service repeat deadlines after rendering as well: the normal 16ms
        // loop sleep must not add another frame to a short release window.
        if !front.state.paused {front.update_repeats();}
        let cost = now.elapsed();
        if opt.trace_performance {performance.max_loop=performance.max_loop.max(cost);}
        let mut delay=Duration::from_millis(16).saturating_sub(cost);
        if let Some(repeat_delay)=front.repeater.next_delay(front.started.elapsed()) {delay=delay.min(repeat_delay);}
        if !delay.is_zero() {sleep(delay);}
        if let Some(error)=graphics_failure.lock().unwrap().as_ref() {
            eprintln!("leaf-graphics runtime_error={error}");front.quit=true;
        }
    }
    let released = front.state.ledger.release_all();
    front.emit(released, false);
    {
        let mut p = front.active.player.lock().unwrap();
        p.set_is_playing(false);
    }
    let saved=front.flush_progress(false);
    println!("leaf-session shutdown inputs=released audio=paused progress_flushed={saved}");
    if opt.self_test_experience {
        if experience_phase!=experience_script.len(){return Err("Experience UI self-test did not finish".into());}
        if !opt.app_settings {
            let settings=experience::Settings::load(&front.settings_path);
            if !settings.fps||settings.scale!=1||settings.quality!=0 {return Err(format!("Display persistence mismatch: {settings:?}"));}
            if front.progress.lock().unwrap().slots()[4].time==0{return Err("Copy to fifth progress slot failed".into());}
        }
        println!("leaf-experience self_test=passed app={} saved={saved} fps={:.1}",opt.app_settings,front.state.experience.fps);
    }
    if let Some(error)=graphics_failure.lock().unwrap().take() {
        let mut settings=front.state.experience.settings;
        if settings.recover_renderer(actual,opt.renderer.as_deref()) {
            if let Err(error)=settings.save(&front.settings_path){eprintln!("leaf-graphics recovery_preference_save_failed={error}");}
        }
        return Err(error);
    }
    if front.restart_slot.is_some()||front.restart_renderer.is_some() {
        if !saved {return Err(t("保存失败，已取消重启游戏").into());}
        if let Some(backend)=front.restart_renderer {
            let mut settings=front.state.experience.settings;settings.renderer=backend;settings.save(&front.settings_path)?;
            println!("leaf-graphics restart requested={} progress_flushed=true",backend.name());
        }
        let slot=front.restart_slot.unwrap_or_else(||front.progress.lock().unwrap().active.unwrap_or(0));
        let args=graphics::restart_args(std::env::args_os().skip(1),front.restart_renderer.is_some(),slot);
        // Retire GPU work and destroy image/context resources before exec.
        drop(front);drop(menu);drop(context);drop(window);
        #[cfg(unix)] {use std::os::unix::process::CommandExt;let error=std::process::Command::new(std::env::current_exe().map_err(|e|e.to_string())?).args(args).exec();return Err(error.to_string());}
        #[cfg(not(unix))] return Err(t("读取存档后请重新打开游戏").into());
    }
    if opt.self_test_slots {
        if script_phase!=ui_phases+8 {return Err(format!("Slots self-test incomplete: {script_phase}/{ui_phases}"));}
        let store=&front.state.controls.store;
        let saved=store.load(&store.game_slot(1))?.0;
        let expected=cursor::Settings{style:cursor::Style::Crosshair,color:cursor::Color::Red,size:cursor::Size::Large};
        if store.load_cursor()?.0!=expected || saved.mapping.buttons[0]!=Binding::Key(13) || saved.mapping.buttons[3].actions()!=vec![Action::Key(profiles::SHIFT),Action::Key(9)] || front.state.controls.game_slot!=1 {
            return Err(format!("Mapping persistence assertion failed: saved={saved:?}"));
        }
        println!("leaf-slots self_test=passed slot_switch=true tabs=9 grid=true editor=true save=true cursor=true idle=true");
    }
    if !saved{return Err(t("游戏原生进度提交失败，请检查日志和存储卡").into());}
    Ok(())
}

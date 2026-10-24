//! Local control editor and strictly data-only, versioned per-game mapping storage.
//! No SDL/GL dependencies: the same state machine is exercised on the host.
use super::{profiles::{self, Binding, Stick}, cursor, turbo::{self,Rate}, InputMode, MouseSpeed};
use std::{collections::BTreeMap, fs::{self, File, OpenOptions}, io::{Read, Write}, path::{Path, PathBuf}};

pub const LABELS: [&str; 14] = ["A", "B", "X", "Y", "L1", "R1", "L2", "R2", "Start", "Select", "十字上", "十字下", "十字左", "十字右"];
pub const AXIS_LABELS: [&str; 5] = ["摇杆上", "摇杆下", "摇杆左", "摇杆右", "摇杆按下"];
pub const CATEGORIES: [&str; 9] = ["常用", "字母", "数字", "功能", "鼠标", "Shift", "Ctrl", "Alt", "符号"];
pub const GAME_SLOTS: usize = 10;
pub const GRID_COLS: usize = 5;
const LIMIT: usize = 16384;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Mapping {
    pub base: usize,
    pub mode: InputMode,
    pub speed: MouseSpeed,
    pub buttons: [Binding; 14],
    pub axes: [Binding; 5],
    pub turbo: [Rate;14],
    pub axis_turbo: [Rate;5],
    pub stick: Stick,
    pub fine: bool,
}


impl Mapping {
    pub fn directions(&self, swapped: bool) -> Self {
        let mut mapping=self.clone();
        if swapped {
            for i in 0..4 { std::mem::swap(&mut mapping.buttons[10+i],&mut mapping.axes[i]); std::mem::swap(&mut mapping.turbo[10+i],&mut mapping.axis_turbo[i]); }
            mapping.stick=Stick::Custom; mapping.fine=false;
        }
        mapping
    }
    fn normalize_turbo(&mut self) {
        for (binding,rate) in self.buttons.iter().zip(self.turbo.iter_mut()).chain(self.axes.iter().zip(self.axis_turbo.iter_mut())) {if !turbo::supported(*binding) {*rate=Rate::Off;}}
    }
    pub fn group(slot: usize) -> Self {
        let mut m = Self::builtin(1);
        m.base = if slot == 1 {14} else {13};
        m.mode = InputMode::Hybrid; m.stick = Stick::Mouse; m.fine = false;
        m.buttons = [Binding::Key(106),Binding::Key(107),Binding::Key(108),Binding::Key(105),
            Binding::Key(32),Binding::Key(13),Binding::None,Binding::None,Binding::Key(27),Binding::Key(9),
            Binding::Key(119),Binding::Key(115),Binding::Key(97),Binding::Key(100)];
        if slot == 1 {
            for i in 0..4 { m.buttons[i] = Binding::Key(49+i as i32); }
            for (i,key) in [profiles::UP,profiles::DOWN,profiles::LEFT,profiles::RIGHT].into_iter().enumerate() { m.buttons[10+i] = Binding::Key(key); }
        }
        m.reset_axes(); m.axes[4] = Binding::LeftClick; m
    }
    pub fn reset_axes(&mut self) {
        self.axes = [Binding::None;5]; self.axis_turbo.fill(Rate::Off);
        for i in 0..4 { self.axes[i] = match self.stick {
            Stick::Mouse => Binding::Pointer(i as u8), Stick::Directions => self.buttons[10+i],
            Stick::Steering if i >= 2 => self.buttons[10+i], _ => Binding::None,
        }; }
    }
    pub fn has_pointer(&self) -> bool { self.axes.iter().chain(self.buttons.iter()).any(|b| matches!(b,Binding::Pointer(_))) }
    pub fn defaults(&self) -> Self {
        if self.base >= 13 { return Self::group(if self.base == 14 {1} else {0}); }
        if self.base != 0 { return Self::builtin(self.base); }
        let mut m = Self::builtin(1); m.base = 0; m.mode = self.mode;
        m.buttons = [Binding::Key(32),Binding::Key(13),Binding::Key(profiles::CTRL),Binding::Key(profiles::SHIFT),
            Binding::Key(1073742050),Binding::Key(32),Binding::Key(27),Binding::Key(8),Binding::Key(13),Binding::Key(9),
            Binding::Key(profiles::UP),Binding::Key(profiles::DOWN),Binding::Key(profiles::LEFT),Binding::Key(profiles::RIGHT)];
        m.stick = if self.mode == InputMode::Keyboard {Stick::Directions} else {Stick::Mouse}; m.fine = self.mode == InputMode::Mouse;
        if self.mode == InputMode::Hybrid { m.buttons[4] = Binding::LeftClick; m.buttons[5] = Binding::RightClick; }
        if self.mode == InputMode::Mouse {
            for i in [0,3,4] { m.buttons[i] = Binding::LeftClick; } for i in [1,2,5] { m.buttons[i] = Binding::RightClick; }
            m.buttons[10..].fill(Binding::None);
        }
        m.reset_axes(); m
    }
    pub fn builtin(base: usize) -> Self {
        let p = &profiles::PROFILES[base.saturating_sub(1).min(11)];
        let mut buttons = [Binding::None; 14];
        buttons[..10].copy_from_slice(&p.buttons);
        if let Some(keys) = p.dpad { for (i, key) in keys.into_iter().enumerate() { buttons[10+i] = Binding::Key(key); } }
        let mut mapping = Self { base: base.clamp(1, 12), mode: match p.stick {
            Stick::Mouse if p.dpad.is_none() => InputMode::Mouse, Stick::Mouse => InputMode::Hybrid, _ => InputMode::Keyboard,
        }, speed: MouseSpeed::Normal, buttons, turbo:[Rate::Off;14], axis_turbo:[Rate::Off;5], axes: [Binding::None;5], stick: p.stick, fine: p.dpad.is_none() };
        mapping.reset_axes(); mapping
    }
}

pub fn key_label(key: i32) -> String {
    match key {
        profiles::UP => "↑".into(), profiles::DOWN => "↓".into(), profiles::LEFT => "←".into(), profiles::RIGHT => "→".into(),
        profiles::SHIFT => "Shift".into(), profiles::CTRL => "Ctrl".into(), 1073742050 => "Alt".into(),
        8 => "Backspace".into(), 9 => "Tab".into(), 13 => "Enter".into(), 27 => "Esc".into(), 32 => "Space".into(),
        127 => "Delete".into(), profiles::PAGE_UP => "PageUp".into(), profiles::PAGE_DOWN => "PageDown".into(),
        1073741898 => "Home".into(), 1073741901 => "End".into(), 1073741897 => "Insert".into(),
        1073741882..=1073741893 => format!("F{}", key-1073741881),
        33..=126 => char::from_u32(key as u32).unwrap().to_string(), _ => "无效键".into(),
    }
}

pub fn binding_label(binding: Binding) -> String {
    match binding {
        Binding::Key(key) => key_label(key),
        Binding::Chord(_) | Binding::Combo(..) => binding.actions().iter().filter_map(|a| match a { super::Action::Key(k) => Some(key_label(*k)), _ => None }).collect::<Vec<_>>().join("+"),
        Binding::None => "无".into(), Binding::LeftClick => "鼠标左键".into(), Binding::RightClick => "鼠标右键".into(),
        Binding::Slow => "按住慢速".into(), Binding::Fast => "按住快速".into(), Binding::Wheel(1) => "滚轮上".into(),
        Binding::Wheel(_) => "滚轮下".into(), Binding::DragLock => "拖拽锁定".into(), Binding::Center => "指针居中".into(),
        Binding::Pointer(i) => format!("鼠标移{}", ["上","下","左","右"][usize::from(i).min(3)]),
    }
}

pub fn stick_label(stick: Stick) -> &'static str {
    match stick { Stick::Directions => "同十字键", Stick::Steering => "仅左右转向", Stick::Mouse => "鼠标", Stick::None => "方向禁用", Stick::Custom => "各方向独立映射" }
}

fn allowed_key(key: i32) -> bool {
    matches!(key,8|9|13|27|32|127|1073741882..=1073741893|1073741897|1073741898|1073741899|1073741901|1073741902|1073741903|1073741904|1073741905|1073741906|1073742048|1073742049|1073742050)
        || (b'a' as i32..=b'z' as i32).contains(&key) || (b'0' as i32..=b'9' as i32).contains(&key)
        || b"`-=[]\\;',./".iter().any(|b| i32::from(*b) == key)
}
fn encode_binding(binding: Binding) -> String {
    match binding {
        Binding::None => "none".into(), Binding::LeftClick => "left-click".into(), Binding::RightClick => "right-click".into(),
        Binding::Slow => "slow".into(), Binding::Fast => "fast".into(), Binding::Wheel(1) => "wheel-up".into(),
        Binding::Wheel(_) => "wheel-down".into(), Binding::DragLock => "drag-lock".into(), Binding::Center => "center".into(),
        Binding::Pointer(i) => format!("pointer-{i}"),
        _ => binding.actions().iter().filter_map(|a| match a { super::Action::Key(k) => Some(k.to_string()), _ => None }).collect::<Vec<_>>().join(","),
    }
}
fn decode_binding(text: &str) -> Result<Binding, String> {
    Ok(match text {
        "none" => Binding::None, "left-click" => Binding::LeftClick, "right-click" => Binding::RightClick,
        "slow" => Binding::Slow, "fast" => Binding::Fast, "wheel-up" => Binding::Wheel(1), "wheel-down" => Binding::Wheel(-1),
        "drag-lock" => Binding::DragLock, "center" => Binding::Center,
        "pointer-0" => Binding::Pointer(0), "pointer-1" => Binding::Pointer(1), "pointer-2" => Binding::Pointer(2), "pointer-3" => Binding::Pointer(3),
        _ => {
            let keys = text.split(',').map(|s| s.parse::<i32>().map_err(|_| "按键格式错误".to_string())).collect::<Result<Vec<_>, _>>()?;
            if keys.is_empty() || keys.len() > 4 || keys.iter().any(|k| !allowed_key(*k)) || keys.iter().enumerate().any(|(i,k)| keys[..i].contains(k)) { return Err("按键或组合键无效".into()); }
            if keys.len() == 1 { Binding::Key(keys[0]) } else {
                let mut array = [0; 4]; array[..keys.len()].copy_from_slice(&keys); Binding::Combo(array, keys.len() as u8)
            }
        }
    })
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Record { pub name: String, pub identity: String, pub mapping: Mapping }

impl Record {
    pub fn encode(&self) -> String {
        let m = &self.mapping;
        let mut text = format!("version=5\nname={}\nidentity={}\nbase={}\nmode={}\nspeed={}\nstick={}\nfine={}\n", self.name, self.identity, m.base,
            match m.mode { InputMode::Hybrid => "hybrid", InputMode::Keyboard => "keyboard", InputMode::Mouse => "mouse" },
            match m.speed { MouseSpeed::Slow => "slow", MouseSpeed::Normal => "normal", MouseSpeed::Fast => "fast" },
            match m.stick { Stick::Directions => "directions", Stick::Steering => "steering", Stick::Mouse => "mouse", Stick::None => "none", Stick::Custom => "custom" }, m.fine);
        for (i, binding) in m.buttons.iter().enumerate() { text.push_str(&format!("bind.{i}={}\n", encode_binding(*binding))); }
        for (i, binding) in m.axes.iter().enumerate() { text.push_str(&format!("axis.{i}={}\n", encode_binding(*binding))); }
        for (i,rate) in m.turbo.iter().enumerate() {text.push_str(&format!("turbo.{i}={}\n",rate.hz()));}
        for (i,rate) in m.axis_turbo.iter().enumerate() {text.push_str(&format!("axis-turbo.{i}={}\n",rate.hz()));}
        text
    }

    pub fn decode(text: &str) -> Result<Self, String> {
        if text.len() > LIMIT { return Err("配置文件过大".into()); }
        let mut fields = BTreeMap::new();
        for line in text.lines() {
            let (key, value) = line.split_once('=').ok_or("配置字段格式错误")?;
            if fields.insert(key, value).is_some() { return Err("配置字段重复".into()); }
        }
        let get = |key: &str| fields.get(key).copied().ok_or_else(|| format!("配置缺少 {key}"));
        let version = get("version")?;
        let count = match version { "1"=>22,"2"=>25,"3"=>30,"4"=>27,"5"=>46,_=>return Err("不支持的配置版本".into()) };
        if fields.len() != count { return Err("配置字段缺失或未知".into()); }
        // Old appearance fields are validated for compatibility, never applied.
        if matches!(version,"2"|"3") {
            cursor::Style::parse(get("cursor-style")?).ok_or("光标样式无效")?;
            cursor::Color::parse(get("cursor-color")?).ok_or("光标颜色无效")?;
            cursor::Size::parse(get("cursor-size")?).ok_or("光标大小无效")?;
        }
        let name = get("name")?.to_string();
        if name.trim().is_empty() || name.chars().count() > 32 || name.chars().any(|c| c.is_control() || c == '=') { return Err("配置名称无效".into()); }
        let identity = get("identity")?.to_string();
        if identity.len() > 4096 || identity.bytes().any(|b| !b.is_ascii_hexdigit()) { return Err("游戏标识无效".into()); }
        let base = get("base")?.parse::<usize>().map_err(|_| "配置基底无效")?;
        if base > if matches!(version,"3"|"4"|"5") {14} else {12} { return Err("配置基底无效".into()); }
        let mode = match get("mode")? { "hybrid" => InputMode::Hybrid, "keyboard" => InputMode::Keyboard, "mouse" => InputMode::Mouse, _ => return Err("控制方式无效".into()) };
        let speed = match get("speed")? { "slow" => MouseSpeed::Slow, "normal" => MouseSpeed::Normal, "fast" => MouseSpeed::Fast, _ => return Err("鼠标速度无效".into()) };
        let stick = match get("stick")? { "directions" => Stick::Directions, "steering" => Stick::Steering, "mouse" => Stick::Mouse, "none" => Stick::None, "custom" if matches!(version,"3"|"4"|"5") => Stick::Custom, _ => return Err("摇杆方式无效".into()) };
        let fine = match get("fine")? { "true" => true, "false" => false, _ => return Err("十字精调无效".into()) };
        let mut buttons = [Binding::None; 14];
        for (i, binding) in buttons.iter_mut().enumerate() { *binding = decode_binding(get(&format!("bind.{i}"))?)?; }
        let mut mapping = Mapping { base, mode, speed, buttons, turbo:[Rate::Off;14], axis_turbo:[Rate::Off;5], axes: [Binding::None;5], stick, fine };
        if matches!(version,"3"|"4"|"5") { for (i,binding) in mapping.axes.iter_mut().enumerate() { *binding = decode_binding(get(&format!("axis.{i}"))?)?; } }
        else { mapping.reset_axes(); }
        if version=="5" {
            for (i,rate) in mapping.turbo.iter_mut().enumerate() {*rate=Rate::parse(get(&format!("turbo.{i}"))?).ok_or("连发频率无效")?;}
            for (i,rate) in mapping.axis_turbo.iter_mut().enumerate() {*rate=Rate::parse(get(&format!("axis-turbo.{i}"))?).ok_or("摇杆连发频率无效")?;}
            for (binding,rate) in mapping.buttons.iter().zip(mapping.turbo.iter()).chain(mapping.axes.iter().zip(mapping.axis_turbo.iter())) {
                if *rate!=Rate::Off && !turbo::supported(*binding) {return Err("此动作不支持连发".into());}
            }
        }
        if mapping.buttons.iter().chain(mapping.axes.iter()).all(|b| *b == Binding::None) { return Err("全空配置已拒绝，请恢复默认".into()); }
        Ok(Self { name, identity, mapping })
    }
}

fn unique() -> String {
    static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
    format!("{}-{}-{}", std::process::id(), std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap_or_default().as_nanos(), NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed))
}
fn regular(path: &Path) -> Result<(), String> {
    if let Ok(meta) = fs::symlink_metadata(path) { if !meta.is_file() || meta.file_type().is_symlink() { return Err("目标不是普通文件".into()); } }
    Ok(())
}
fn read_record(path: &Path) -> Result<Record, String> {
    regular(path)?;
    let mut text = String::new();
    File::open(path).map_err(|e| e.to_string())?.take((LIMIT+1) as u64).read_to_string(&mut text).map_err(|e| e.to_string())?;
    Record::decode(&text)
}

fn read_cursor(path: &Path) -> Result<cursor::Settings,String> {
    regular(path)?; let mut text=String::new();
    File::open(path).map_err(|e|e.to_string())?.take(513).read_to_string(&mut text).map_err(|e|e.to_string())?;
    cursor::Settings::decode(&text)
}

#[cfg(not(windows))]
fn replace(from: &Path, to: &Path) -> std::io::Result<()> { fs::rename(from, to) }
#[cfg(windows)]
fn replace(from: &Path, to: &Path) -> std::io::Result<()> {
    use std::os::windows::ffi::OsStrExt;
    #[link(name = "kernel32")]
    extern "system" { fn MoveFileExW(from: *const u16, to: *const u16, flags: u32) -> i32; }
    let a: Vec<u16> = from.as_os_str().encode_wide().chain(Some(0)).collect();
    let b: Vec<u16> = to.as_os_str().encode_wide().chain(Some(0)).collect();
    if unsafe { MoveFileExW(a.as_ptr(), b.as_ptr(), 1 | 8) } == 0 { Err(std::io::Error::last_os_error()) } else { Ok(()) }
}
fn atomic_bytes(path: &Path, bytes: &[u8]) -> Result<(), String> {
    regular(path)?;
    let parent = path.parent().ok_or("缺少数据目录")?;
    fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    let temporary = parent.join(format!(".write-{}", unique()));
    let result = (|| -> std::io::Result<()> {
        let mut file = OpenOptions::new().create_new(true).write(true).open(&temporary)?;
        file.write_all(bytes)?; file.sync_all()?; drop(file);
        replace(&temporary, path)?;
        #[cfg(unix)]
        File::open(parent)?.sync_all()?;
        Ok(())
    })();
    if result.is_err() { let _ = fs::remove_file(&temporary); }
    result.map_err(|e| e.to_string())
}

#[derive(Clone, Debug)]
pub struct Store { pub root: PathBuf, pub identity: String, pub game: PathBuf }
impl Store {
    pub fn new(root: PathBuf, game: &Path) -> Self {
        let path = game.to_string_lossy().replace('\\', "/");
        // ROM-relative identity survives card mount changes and FLASH -> flash.
        let lower = path.to_ascii_lowercase();
        let logical = lower.find("/roms/flash/").map(|i| format!("flash/{}", &path[i+12..])).unwrap_or(path);
        let identity = logical.as_bytes().iter().map(|b| format!("{b:02x}")).collect::<String>();
        let hash = identity.bytes().fold(0xcbf29ce484222325u64, |h,b| (h ^ u64::from(b)).wrapping_mul(0x100000001b3));
        let game = root.join("games").join(format!("{hash:016x}.rmap"));
        Self { root, identity, game }
    }
    fn game_dir(&self) -> PathBuf { self.root.join("slots").join(self.game.file_stem().unwrap_or_default()) }
    pub fn game_slot(&self, slot: usize) -> PathBuf { self.game_dir().join(format!("slot-{:02}.rmap",slot+1)) }
    pub fn cursor_path(&self) -> PathBuf { self.root.join("cursor.conf") }
    pub fn load_cursor(&self) -> Result<(cursor::Settings,bool),String> {
        let path=self.cursor_path();
        read_cursor(&path).map(|s|(s,false)).or_else(|error|read_cursor(&path.with_extension("conf.bak")).map(|s|(s,true)).map_err(|_|error))
    }
    pub fn save_cursor(&self, settings: cursor::Settings) -> Result<(),String> {
        let path=self.cursor_path(); regular(&path)?;
        if path.exists() {
            let mut old=Vec::new(); File::open(&path).map_err(|e|e.to_string())?.take(513).read_to_end(&mut old).map_err(|e|e.to_string())?;
            if old.len()>512 {return Err("原光标配置过大，未覆盖".into());}
            let backup=if cursor::Settings::decode(std::str::from_utf8(&old).unwrap_or("")).is_ok() {path.with_extension("conf.bak")} else {path.with_extension(format!("invalid-{}",unique()))};
            atomic_bytes(&backup,&old)?;
        }
        atomic_bytes(&path,settings.encode().as_bytes())
    }
    fn selected_path(&self) -> PathBuf { self.game_dir().join("selected.rmap") }
    pub fn select_slot(&self, slot: usize, mapping: &Mapping) -> Result<(),String> {
        if slot >= GAME_SLOTS { return Err("方案超出范围".into()); }
        self.save(&self.selected_path(), &Record {name:format!("Slot {}",slot+1),identity:self.identity.clone(),mapping:mapping.clone()})
    }
    pub fn load_slots(&self) -> ([Mapping;GAME_SLOTS],usize,String) {
        let mut mappings = std::array::from_fn(Mapping::group);
        let mut warnings = Vec::new();
        for (i,mapping) in mappings.iter_mut().enumerate() {
            let path = self.game_slot(i);
            if path.exists() || path.with_extension("rmap.bak").exists() {
                match self.load(&path) {
                    Ok((r,recovered)) if r.identity == self.identity => { *mapping = r.mapping; if recovered {warnings.push(format!("方案{}回退备份",i+1));} },
                    _ => warnings.push(format!("方案{}损坏，使用默认；原文件保留",i+1)),
                }
            } else if i == 0 && self.game.exists() {
                if let Ok((r,_)) = self.load(&self.game) { if r.identity == self.identity { *mapping = r.mapping; warnings.push("旧游戏配置载入方案1，保存后迁移".into()); } }
            }
        }
        let selected = match self.load(&self.selected_path()) {
            Ok((r,_)) if r.identity == self.identity => r.name.strip_prefix("Slot ").and_then(|s| s.parse::<usize>().ok()).filter(|n| (1..=GAME_SLOTS).contains(n)).map(|n|n-1).unwrap_or(0),
            _ => 0,
        };
        (mappings,selected,warnings.join("；"))
    }
    pub fn save(&self, path: &Path, record: &Record) -> Result<(), String> {
        Record::decode(&record.encode())?;
        regular(path)?;
        if (path == self.game || path.starts_with(self.game_dir())) && path.exists() {
            if let Ok(old) = read_record(path) { if old.identity != self.identity { return Err("游戏标识冲突，未覆盖原配置".into()); } }
        }
        if path.exists() {
            let mut old = Vec::new();
            File::open(path).map_err(|e| e.to_string())?.take((LIMIT+1) as u64).read_to_end(&mut old).map_err(|e| e.to_string())?;
            if old.len() > LIMIT { return Err("原配置过大，未覆盖".into()); }
            if Record::decode(std::str::from_utf8(&old).unwrap_or("")).is_ok() { atomic_bytes(&path.with_extension("rmap.bak"), &old)?; }
            else { atomic_bytes(&path.with_extension(format!("invalid-{}", unique())), &old)?; }
        }
        atomic_bytes(path, record.encode().as_bytes())
    }
    pub fn load(&self, path: &Path) -> Result<(Record, bool), String> {
        match read_record(path) { Ok(r) => Ok((r,false)), Err(original) => {
            read_record(&path.with_extension("rmap.bak")).map(|r| (r,true)).map_err(|_| original)
        } }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Page { Edit, Picker, Confirm, Cursor }
#[derive(Clone, Copy, Debug)]
pub enum Nav { Up, Down, Left, Right, A, B, X, Y, Prev, Next }
pub fn grid_move(row: usize, count: usize, nav: Nav) -> usize {
    grid_columns(row,count,nav,GRID_COLS)
}
fn grid_columns(row: usize, count: usize, nav: Nav, columns: usize) -> usize {
    if count == 0 { return 0; }
    let row = row.min(count-1);
    let column = row % columns;
    let line = row / columns;
    let start = line * columns;
    let width = columns.min(count-start);
    match nav {
        Nav::Left => start + (column+width-1)%width,
        Nav::Right => start + (column+1)%width,
        Nav::Up if row >= columns => row-columns,
        Nav::Up => column + ((count-1-column)/columns)*columns,
        Nav::Down if row+columns < count => row+columns,
        Nav::Down => column,
        _ => row,
    }
}
#[derive(Clone, Debug)]
pub enum ResultAction { None, Close, Apply(Mapping), Cursor(cursor::Settings) }
#[derive(Clone, Copy, Debug)]
enum Pending { Exit, Slot(usize) }

pub const EDIT_ITEMS: usize = 22;
pub const SAVE_ROW: usize = EDIT_ITEMS;
pub const BACK_ROW: usize = EDIT_ITEMS+1;

fn edit_move(row:usize,nav:Nav)->usize {
    use Nav::*;
    if row>=EDIT_ITEMS {return match nav {
        Left|Right=>if row==SAVE_ROW {BACK_ROW}else{SAVE_ROW},
        Up=>if row==SAVE_ROW {20}else{21},
        Down=>if row==SAVE_ROW {0}else{4},_=>row,
    };}
    if matches!(nav,Down)&&row>=20 {return if row==20 {SAVE_ROW}else{BACK_ROW};}
    if matches!(nav,Up)&&row<GRID_COLS {return if row<2 {SAVE_ROW}else{BACK_ROW};}
    grid_move(row,EDIT_ITEMS,nav)
}

pub struct Controls {
    pub game_slot: usize,
    pub preview_slot: usize,
    pub mappings: [Mapping;GAME_SLOTS],
    pub active: Option<Mapping>,
    pub visible: bool,
    pub page: Page,
    pub row: usize,
    pub status: String,
    pub source: String,
    pub draft: Mapping,
    pub cursor_settings: cursor::Settings,
    pub cursor_draft: cursor::Settings,
    original: Mapping,
    pub store: Store,
    editing: usize,
    pub category: usize,
    pending: Option<Pending>,
    return_row: usize,
}
impl Default for Controls {
    fn default() -> Self { Self::new(Store::new(PathBuf::new(), Path::new(""))) }
}

impl Controls {
    pub fn new(store: Store) -> Self {
        let (mappings,game_slot,mut status) = store.load_slots();
        let cursor_settings=match store.load_cursor() {
            Ok((settings,recovered))=>{if recovered {status.push_str("；光标配置回退备份");} settings},
            Err(_) => {if store.cursor_path().exists() || store.cursor_path().with_extension("conf.bak").exists() {status.push_str("；光标配置损坏，使用默认；原文件保留");} cursor::Settings::default()},
        };
        let mapping = mappings[game_slot].clone();
        Self { game_slot, preview_slot:game_slot, mappings, active: None, visible: false, page: Page::Edit, row: 0, status: status.clone(), source: if status.is_empty() {format!("按键方案{}",game_slot+1)} else {status}, draft: mapping.clone(), original: mapping, cursor_settings, cursor_draft: cursor_settings, store,
            editing: 0, category: 0, pending:None, return_row:0 }
    }
    pub fn begin(&mut self, actual: Mapping) { self.visible = true; self.page = Page::Edit; self.row = 0; self.draft = actual.clone(); self.original = actual; self.pending=None; self.status.clear(); }
    pub fn dirty(&self)->bool {self.draft!=self.original}
    fn request(&mut self,pending:Pending)->ResultAction {
        if !self.dirty() {return self.finish(pending);}
        self.pending=Some(pending);self.return_row=self.row;self.go(Page::Confirm);self.status.clear();ResultAction::None
    }
    fn finish(&mut self,pending:Pending)->ResultAction {
        self.pending=None;self.draft=self.original.clone();
        match pending {Pending::Exit=>self.close(),Pending::Slot(slot)=>self.switch_slot(slot)}
    }
    fn cancel_pending(&mut self) {self.pending=None;self.page=Page::Edit;self.row=self.return_row;self.status.clear();}
    pub fn pending_detail(&self)->String {match self.pending {
        Some(Pending::Slot(slot))=>format!("切换到方案 {} 前，请处理当前修改",slot+1),
        _=>"返回前，请处理当前修改".into(),
    }}
    pub fn switch_slot(&mut self, slot: usize) -> ResultAction {
        if slot >= GAME_SLOTS { return ResultAction::None; }
        if let Some(active) = &self.active { self.mappings[self.game_slot] = active.clone(); }
        self.game_slot = slot; self.preview_slot = slot; self.draft = self.mappings[slot].clone(); self.original = self.draft.clone();
        self.page=Page::Edit;self.row=0;self.pending=None;
        let result = self.store.select_slot(slot,&self.draft);
        let remembered = self.failure(result,"方案选择已记忆");
        let action = self.apply(&format!("按键方案{}",slot+1));
        self.status = format!("已切换至按键方案{}{}",slot+1,if remembered {""} else {"（选择仅本次，写入失败）"}); action
    }
    pub fn begin_cursor(&mut self, actual: cursor::Settings) { self.visible=true; self.cursor_draft=actual; self.status.clear(); self.go(Page::Cursor); }
    fn close(&mut self) -> ResultAction { self.visible = false; ResultAction::Close }
    fn save_scope(&mut self) -> ResultAction {
        let result = self.store.save(&self.store.game_slot(self.game_slot), &self.record(format!("按键方案{}",self.game_slot+1),true));
        if self.failure(result,"保存成功") {
            let source = format!("方案{}（已保存）",self.game_slot+1);
            let action = self.apply(&source);
            self.original=self.draft.clone();self.status="已保存".into();
            if let Err(e) = self.store.select_slot(self.game_slot,&self.draft) { self.status = format!("方案已保存，记忆当前选择失败：{e}"); }
            action
        } else { ResultAction::None }
    }
    fn go(&mut self, page: Page) { self.page = page; self.row = 0; }
    fn record(&self, name: String, game: bool) -> Record { Record { name, identity: if game { self.store.identity.clone() } else { String::new() }, mapping: self.draft.clone() } }
    fn apply(&mut self, source: &str) -> ResultAction {
        if let Err(error) = Record::decode(&self.record("Current".into(), false).encode()) { self.status = error; return ResultAction::None; }
        self.active = Some(self.draft.clone()); self.mappings[self.game_slot] = self.draft.clone(); self.source = source.into(); self.status = format!("已应用：{source}"); ResultAction::Apply(self.draft.clone())
    }
    fn failure(&mut self, result: Result<(), String>, success: &str) -> bool {
        match result { Ok(()) => { self.status = success.into(); true }, Err(e) => { eprintln!("leaf-controls io_error={e}"); self.status = "操作失败，原配置已保留；请检查目录或空间".into(); false } }
    }
    fn choices(&self) -> Vec<Binding> {
        use Binding::*;
        let keys: Vec<i32> = match self.category {
            1 => (b'a'..=b'z').map(i32::from).collect(), 2 => (b'0'..=b'9').map(i32::from).collect(),
            3 => (1073741882..=1073741893).collect(), 5..=7 => (b'a'..=b'z').map(i32::from).chain([9,13,32,profiles::UP,profiles::DOWN,profiles::LEFT,profiles::RIGHT]).collect(),
            8 => b"`-=[]\\;',./".iter().map(|k| i32::from(*k)).collect(),
            _ => vec![32,13,27,9,8,127,profiles::UP,profiles::DOWN,profiles::LEFT,profiles::RIGHT,profiles::SHIFT,profiles::CTRL,1073742050,profiles::PAGE_UP,profiles::PAGE_DOWN,1073741898,1073741901,1073741897],
        };
        if self.category == 4 { return vec![None,LeftClick,RightClick,Wheel(1),Wheel(-1),DragLock,Center,Slow,Fast,Pointer(0),Pointer(1),Pointer(2),Pointer(3)]; }
        keys.into_iter().map(|k| if (5..=7).contains(&self.category) { Combo([match self.category { 5 => profiles::SHIFT, 6 => profiles::CTRL, _ => 1073742050 }, k,0,0],2) } else { Key(k) }).collect()
    }
    pub fn title(&self) -> &str { match self.page { Page::Edit => "按键设置", Page::Picker => "选择虚拟动作", Page::Confirm => "未保存的修改", Page::Cursor => "鼠标光标" } }
    pub fn hint(&self) -> &str { match self.page {
        Page::Edit => "L / R 切换方案  A 修改  Y 连发  B 返回",
        Page::Picker => "十字键移动  L1/R1 切换类型  A 确认  B 取消",
        Page::Confirm => "A 确定  B 继续编辑",
        Page::Cursor => "左右调整  A 确认  X 恢复此项  B 返回",
    } }
    pub fn lines(&self) -> Vec<String> { match self.page {
        Page::Edit => LABELS.iter().enumerate().map(|(i,s)| format!("{s}{}：{}",self.draft.turbo[i].badge(), binding_label(self.draft.buttons[i]))).chain([
            format!("摇杆预设：{}", stick_label(self.draft.stick))]).chain(AXIS_LABELS.iter().enumerate().map(|(i,s)| format!("{s}{}：{}",self.draft.axis_turbo[i].badge(),binding_label(self.draft.axes[i])))).chain([
            format!("十字鼠标精调：{}", if self.draft.fine {"开"} else {"关"}),
            format!("鼠标速度：{}", match self.draft.speed { MouseSpeed::Slow => "慢", MouseSpeed::Normal => "标准", MouseSpeed::Fast => "快" }),
            "保存".into(), "返回".into()]).collect(),
        Page::Cursor => vec![format!("光标样式：{}",self.cursor_draft.style.name()),format!("光标颜色：{}",self.cursor_draft.color.name()),
            format!("光标大小：{}（{}）",self.cursor_draft.size.name(),self.cursor_draft.size.pixels()),"恢复光标默认".into(),"保存并应用（全局）".into(),"返回".into()],
        Page::Picker => self.choices().into_iter().map(binding_label).collect(),
        Page::Confirm => vec!["继续编辑".into(), "舍弃修改并继续".into(), "保存并继续".into()],
    } }
    fn selected_repeat(&self) -> Option<(Binding,Rate)> {
        match self.row {0..=13=>Some((self.draft.buttons[self.row],self.draft.turbo[self.row])),15..=19=>Some((self.draft.axes[self.row-15],self.draft.axis_turbo[self.row-15])),_=>None}
    }
    pub fn detail(&self) -> String {
        if self.page == Page::Picker { return format!("{}  →  {}", if self.editing < 14 {LABELS[self.editing]} else {AXIS_LABELS[self.editing-15]}, binding_label(if self.editing < 14 {self.draft.buttons[self.editing]} else {self.draft.axes[self.editing-15]})); }
        if self.page == Page::Cursor { return "所有游戏共用一份外观；静止 3.5 秒隐藏，移动显示".into(); }
        let repeat=if self.page==Page::Edit {self.selected_repeat().map(|(binding,rate)|if turbo::supported(binding) {format!(" · {}（Y切换）",rate.label())}else{" · 此动作无连发".into()}).unwrap_or_default()}else{String::new()};
        repeat.trim_start_matches(" · ").into()
    }
    pub fn nav(&mut self, nav: Nav) -> ResultAction {
        use Nav::*;
        if matches!(nav, B) {
            match self.page {
                Page::Edit => return self.request(Pending::Exit),
                Page::Cursor => return self.close(),
                Page::Picker => { self.go(Page::Edit); self.row = self.editing; },
                Page::Confirm => self.cancel_pending(),
            }
            return ResultAction::None;
        }
        if self.page == Page::Confirm {
            if matches!(nav,Up|Down|Left|Right) {self.row=(self.row as i32+if matches!(nav,Up|Left){-1}else{1}).rem_euclid(3) as usize;}
            if matches!(nav,A) {match self.row {
                0=>self.cancel_pending(),
                1=>if let Some(pending)=self.pending {return self.finish(pending);},
                2=>if let Some(pending)=self.pending {
                    let action=self.save_scope();
                    if matches!(action,ResultAction::Apply(_)) {
                        if matches!(pending,Pending::Exit) {self.pending=None;self.close();return action;}
                        return self.finish(pending);
                    }
                },_=>{}
            }}
            return ResultAction::None;
        }
        if self.page==Page::Edit&&matches!(nav,Prev|Next) {
            let slot=(self.game_slot as i32+if matches!(nav,Prev){-1}else{1}).rem_euclid(GAME_SLOTS as i32) as usize;
            return self.request(Pending::Slot(slot));
        }
        let count = self.lines().len();
        if self.page == Page::Picker {
            if matches!(nav, Prev | Next) { self.category = (self.category as i32 + if matches!(nav,Prev) {-1} else {1}).rem_euclid(9) as usize; self.row = 0; }
            if matches!(nav,Up|Down|Left|Right) { self.row = grid_move(self.row,count,nav); }
            if matches!(nav,A) {
                let binding = self.choices()[self.row];
                if self.editing < 14 {
                    self.draft.buttons[self.editing] = binding;
                    if self.editing>=10 && (self.draft.stick==Stick::Directions || self.draft.stick==Stick::Steering && self.editing>=12) {self.draft.axes[self.editing-10]=binding;}
                }
                else { self.draft.axes[self.editing-15] = binding; self.draft.stick = Stick::Custom; }
                self.draft.normalize_turbo(); self.go(Page::Edit); self.row = self.editing;
            }
            return ResultAction::None;
        }
        if self.page == Page::Edit && matches!(nav,Up|Down|Left|Right) { self.row=edit_move(self.row,nav); return ResultAction::None; }
        if matches!(nav, Up | Down) { self.row = (self.row as i32 + if matches!(nav,Up) {-1} else {1}).rem_euclid(count as i32) as usize; return ResultAction::None; }
        let adjust = if matches!(nav,Left) {-1} else {1};
        if self.page == Page::Cursor {
            if matches!(nav,X) {
                let defaults=cursor::Settings::default();
                match self.row { 0=>self.cursor_draft.style=defaults.style,1=>self.cursor_draft.color=defaults.color,2=>self.cursor_draft.size=defaults.size,_=>{} }
                self.status="此项已恢复默认，尚未应用".into();
            }
            if matches!(nav,Left|Right|A) { match self.row {
                0=>self.cursor_draft.style=self.cursor_draft.style.next(adjust),
                1=>self.cursor_draft.color=self.cursor_draft.color.next(adjust),
                2=>self.cursor_draft.size=self.cursor_draft.size.next(adjust),
                3 if matches!(nav,A)=>{ self.cursor_draft=cursor::Settings::default(); self.status="光标已恢复默认，请应用或保存".into(); },
                4 if matches!(nav,A)=>{
                    let result=self.store.save_cursor(self.cursor_draft);
                    if self.failure(result,"全局光标已保存并应用") {self.cursor_settings=self.cursor_draft;return ResultAction::Cursor(self.cursor_settings);}
                },
                5 if matches!(nav,A)=>return self.close(),_=>{}
            } }
            return ResultAction::None;
        }
        if self.page == Page::Edit {
            if matches!(nav,Y) {
                if let Some((binding,rate))=self.selected_repeat() {
                    if turbo::supported(binding) {
                        let next=rate.next();if self.row<14 {self.draft.turbo[self.row]=next;}else{self.draft.axis_turbo[self.row-15]=next;}
                        self.status=format!("{}；请保存",next.label());
                    } else {self.status="连发支持键盘、组合键及鼠标左右键".into();}
                }
            }
            if matches!(nav, A) {
                match self.row {
                    0..=13 | 15..=19 if matches!(nav,A) => { self.editing = self.row; self.category = 0; self.go(Page::Picker); },
                    14 => { let items = [Stick::Mouse,Stick::Directions,Stick::Steering,Stick::None,Stick::Custom]; let i = items.iter().position(|s| *s == self.draft.stick).unwrap(); self.draft.stick = items[(i as i32+adjust).rem_euclid(5) as usize]; if self.draft.stick != Stick::Custom {let press=self.draft.axes[4];let rate=self.draft.axis_turbo[4]; self.draft.reset_axes();self.draft.axes[4]=press;self.draft.axis_turbo[4]=rate;} },
                    20 => self.draft.fine = !self.draft.fine,
                    21 => self.draft.speed = match self.draft.speed { MouseSpeed::Slow=>MouseSpeed::Normal,MouseSpeed::Normal=>MouseSpeed::Fast,MouseSpeed::Fast=>MouseSpeed::Slow },
                    SAVE_ROW => return self.save_scope(),
                    BACK_ROW => return self.request(Pending::Exit), _ => {}
                }
            }
        }
        self.draft.normalize_turbo();
        ResultAction::None
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    struct Fixture(Store);
    impl Fixture {
        fn new() -> Self { Self(Store::new(std::env::temp_dir().join(format!("ruffle-controls-{}",unique())), Path::new("/mnt/sdcard/Roms/flash/folder/game.swf"))) }
        fn record(&self, base: usize, game: bool) -> Record { Record { name: "测试配置".into(), identity: if game {self.0.identity.clone()} else {String::new()}, mapping: Mapping::builtin(base) } }
    }
    impl Drop for Fixture { fn drop(&mut self) { let _ = fs::remove_dir_all(&self.0.root); } }

    #[test]
    fn all_builtin_templates_roundtrip_without_losing_actions() {
        let f = Fixture::new();
        for i in 1..=12 {
            let r = f.record(i,false); let parsed = Record::decode(&r.encode()).unwrap();
            assert_eq!(parsed.encode(),r.encode());
            for n in 0..14 { assert_eq!(parsed.mapping.buttons[n].actions(),r.mapping.buttons[n].actions()); }
        }
    }
    #[test]
    fn reject_unknown_versions_fields_duplicates_and_shell_commands() {
        let f = Fixture::new(); let text = f.record(1,false).encode();
        for broken in [text.replace("version=5","version=99"),text.clone()+"command=touch /tmp/pwned\n",text.clone()+"version=1\n",text.replace("bind.0=32","bind.0=$(reboot)"),text.replace("base=1","base=99"),text.replace("bind.0=32","bind.0=1073741925"),text.replace("bind.0=32","bind.0=32,32")] { assert!(Record::decode(&broken).is_err(),"{broken}"); }
        assert!(Record::decode(&"x".repeat(LIMIT+1)).is_err());
    }
    #[test]
    fn completely_empty_mapping_cannot_be_saved_and_the_draft_is_retained() {
        let f=Fixture::new();let mut c=Controls::new(f.0.clone());c.begin(c.draft.clone());
        c.draft.buttons.fill(Binding::None); c.draft.axes.fill(Binding::None); c.draft.stick = Stick::None;
        c.row = SAVE_ROW; assert!(matches!(c.nav(Nav::A),ResultAction::None)); assert!(c.active.is_none());
        assert!(c.dirty());assert!(!f.0.game_slot(0).exists());assert_eq!(c.draft.buttons,[Binding::None;14]);
    }
    #[test]
    fn relative_game_identity_survives_mount_and_legacy_folder_case() {
        let f = Fixture::new(); let other = Store::new(f.0.root.clone(),Path::new("/media/card2/Roms/FLASH/folder/game.swf"));
        assert_eq!(f.0.identity,other.identity); assert_eq!(f.0.game,other.game);
    }
    #[test]
    fn corrupt_config_recovers_last_valid_backup_and_preserves_damage() {
        let f = Fixture::new(); f.0.save(&f.0.game,&f.record(1,true)).unwrap(); f.0.save(&f.0.game,&f.record(2,true)).unwrap();
        fs::write(&f.0.game,"damaged").unwrap();
        let (loaded,recovered) = f.0.load(&f.0.game).unwrap(); assert_eq!(loaded.mapping.base,1); assert!(recovered);
        assert_eq!(fs::read_to_string(&f.0.game).unwrap(),"damaged");
        f.0.save(&f.0.game,&f.record(3,true)).unwrap();
        assert_eq!(read_record(&f.0.game.with_extension("rmap.bak")).unwrap().mapping.base,1);
        assert!(fs::read_dir(f.0.game.parent().unwrap()).unwrap().any(|e| e.unwrap().path().extension().unwrap().to_string_lossy().starts_with("invalid-")));
    }
    #[test]
    fn identity_mismatch_refuses_to_load_or_overwrite_other_game() {
        let f = Fixture::new(); let mut r = f.record(1,true); r.identity = "deadbeef".into();
        fs::create_dir_all(f.0.game.parent().unwrap()).unwrap(); fs::write(&f.0.game,r.encode()).unwrap();
        assert_eq!(f.0.load_slots().0[0],Mapping::group(0)); assert!(f.0.save(&f.0.game,&f.record(3,true)).is_err()); assert_eq!(read_record(&f.0.game).unwrap(),r);
    }
    #[test]
    fn edit_cancel_and_removed_reset_do_not_mutate_builtin_catalog() {
        let mut c = Controls::default(); let original = Mapping::builtin(1); c.begin(original.clone());
        assert_eq!(c.page,Page::Edit); c.nav(Nav::A); assert_eq!(c.page,Page::Picker);
        c.nav(Nav::Right); c.nav(Nav::A); assert_eq!(c.draft.buttons[0],Binding::Key(13)); assert!(c.active.is_none());
        c.nav(Nav::X); assert_eq!(c.draft.buttons[0],Binding::Key(13));
        c.nav(Nav::B);assert_eq!(c.page,Page::Confirm);c.nav(Nav::B);assert_eq!(c.draft.buttons[0],Binding::Key(13));
        c.nav(Nav::B);c.nav(Nav::Down);assert!(matches!(c.nav(Nav::A),ResultAction::Close));assert_eq!(c.draft,original);
        assert_eq!(Mapping::builtin(1),original);
    }
    #[test]
    fn combo_picker_preserves_press_order_and_reverse_release() {
        let mut c = Controls::default(); c.page = Page::Picker; c.category = 5; c.row = 26; c.editing = 3; c.nav(Nav::A);
        let actions = c.draft.buttons[3].actions(); assert_eq!(actions,vec![super::super::Action::Key(profiles::SHIFT),super::super::Action::Key(9)]);
        let mut ledger = super::super::InputLedger::default(); let source = super::super::Source::Controller(3);
        ledger.press_many(source,&actions); assert_eq!(ledger.release(source).into_iter().map(|t|t.action).collect::<Vec<_>>(),vec![actions[1],actions[0]]);
    }
    #[test]
    fn write_failure_does_not_apply_or_claim_success() {
        let f = Fixture::new(); fs::create_dir_all(&f.0.root).unwrap(); fs::write(f.0.root.join("global.rmap"),"bad").unwrap();
        let mut c = Controls::new(Store::new(f.0.root.join("global.rmap"),Path::new("game.swf"))); c.row = SAVE_ROW;
        assert!(matches!(c.nav(Nav::A),ResultAction::None)); assert!(c.active.is_none()); assert!(c.status.contains("失败"));
    }
    #[test]
    fn restoring_legacy_mode_uses_legacy_defaults_including_mouse_buttons() {
        let mut m = Mapping::builtin(1); m.base=0; m.mode=InputMode::Mouse; let restored = m.defaults();
        assert_eq!(restored.base,0); assert_eq!(restored.buttons[0],Binding::LeftClick); assert_eq!(restored.buttons[1],Binding::RightClick); assert!(restored.fine);
    }

    fn legacy_record(r: &Record, version: u8) -> String {
        let mut text=(r.encode().lines().filter(|line|!line.starts_with("turbo.")&&!line.starts_with("axis-turbo.")).collect::<Vec<_>>().join("\n")+"\n").replace("version=5",&format!("version={version}"));
        if version<3 {text=text.lines().filter(|s|!s.starts_with("axis.")).collect::<Vec<_>>().join("\n")+"\n";}
        if version>1 {text.push_str("cursor-style=arrow\ncursor-color=red\ncursor-size=48\n");}
        text
    }
    #[test]
    fn old_mapping_versions_ignore_appearance_and_upgrade_only_on_save() {
        let f=Fixture::new(); let record=f.record(6,true);
        for version in 1..=3 {
            let legacy=legacy_record(&record,version);
            fs::create_dir_all(f.0.game.parent().unwrap()).unwrap();fs::write(&f.0.game,&legacy).unwrap();
            let loaded=f.0.load(&f.0.game).unwrap().0; assert_eq!(loaded,record);
            assert_eq!(fs::read_to_string(&f.0.game).unwrap(),legacy);
            assert!(!f.0.cursor_path().exists());
            f.0.save(&f.0.game,&loaded).unwrap();
            assert_eq!(fs::read_to_string(f.0.game.with_extension("rmap.bak")).unwrap(),legacy);
            let saved=fs::read_to_string(&f.0.game).unwrap(); assert!(saved.starts_with("version=5\n")); assert!(!saved.contains("cursor-"));
        }
        let text=legacy_record(&record,3);
        for bad in [text.replace("cursor-color=red","cursor-color=exec"),text.replace("cursor-size=48","cursor-size=999"),text.replace("cursor-style=arrow","cursor-style=unknown")] {assert!(Record::decode(&bad).is_err());}
    }
    #[test]
    fn global_cursor_save_restart_and_cancel_are_independent_of_all_mappings() {
        let f=Fixture::new();let mut c=Controls::new(f.0.clone());let original=cursor::Settings::default();
        c.begin_cursor(original);c.nav(Nav::Right);assert_ne!(c.cursor_draft,original);c.nav(Nav::X);assert_eq!(c.cursor_draft,original);
        c.nav(Nav::Right);assert!(matches!(c.nav(Nav::B),ResultAction::Close));assert!(!f.0.cursor_path().exists());
        c.begin_cursor(original);c.nav(Nav::Right);let expected=c.cursor_draft;c.row=4;
        assert!(matches!(c.nav(Nav::A),ResultAction::Cursor(s) if s==expected));assert!(c.active.is_none());
        for i in 0..GAME_SLOTS {c.switch_slot(i);assert_eq!(c.cursor_settings,expected);assert!(!c.store.game_slot(i).exists());}
        let other=Controls::new(Store::new(f.0.root.clone(),Path::new("another.swf")));
        assert_eq!(other.cursor_settings,expected);assert_eq!(other.mappings,std::array::from_fn(Mapping::group));
    }
    #[test]
    fn global_cursor_recovers_backup_and_write_failure_does_not_apply() {
        let f=Fixture::new();let default=cursor::Settings::default();let mut changed=default;changed.color=cursor::Color::Cyan;
        f.0.save_cursor(default).unwrap();f.0.save_cursor(changed).unwrap();
        fs::write(f.0.cursor_path(),"bad").unwrap();assert_eq!(f.0.load_cursor().unwrap(),(default,true));
        f.0.save_cursor(changed).unwrap();assert_eq!(f.0.load_cursor().unwrap(),(changed,false));
        assert_eq!(fs::read_to_string(f.0.cursor_path().with_extension("conf.bak")).unwrap(),default.encode());
        fs::remove_file(f.0.cursor_path()).unwrap();fs::create_dir(f.0.cursor_path()).unwrap();
        let mut c=Controls::new(f.0.clone());c.begin_cursor(default);c.cursor_draft=changed;c.row=4;
        assert!(matches!(c.nav(Nav::A),ResultAction::None));assert_eq!(c.cursor_settings,default);assert!(c.status.contains("失败"));
    }

    #[test]
    fn requested_groups_have_exact_keys_and_independent_stick_click() {
        let a=Mapping::group(0);let b=Mapping::group(1);
        assert_eq!(&a.buttons[..4],&[Binding::Key(106),Binding::Key(107),Binding::Key(108),Binding::Key(105)]);
        assert_eq!(&a.buttons[10..],&[Binding::Key(119),Binding::Key(115),Binding::Key(97),Binding::Key(100)]);
        assert_eq!(&b.buttons[..4],&[Binding::Key(49),Binding::Key(50),Binding::Key(51),Binding::Key(52)]);
        assert_eq!(&b.buttons[4..10],&a.buttons[4..10]);
        assert_eq!(&a.buttons[4..6],&[Binding::Key(32),Binding::Key(13)]);
        assert_eq!(&a.buttons[8..10],&[Binding::Key(27),Binding::Key(9)]);
        assert_eq!(a.axes,[Binding::Pointer(0),Binding::Pointer(1),Binding::Pointer(2),Binding::Pointer(3),Binding::LeftClick]);
        assert_eq!(b.axes,a.axes);
    }
    #[test]
    fn direction_swap_routes_both_default_schemes_and_preserves_saved_mapping() {
        for slot in 0..GAME_SLOTS {
            let original=Mapping::group(slot);let routed=original.directions(true);
            assert_eq!(&routed.buttons[10..],&original.axes[..4]);
            assert_eq!(&routed.axes[..4],&original.buttons[10..]);
            assert_eq!(&routed.buttons[..10],&original.buttons[..10]);assert_eq!(routed.axes[4],original.axes[4]);
            assert_eq!(routed.speed,original.speed);assert_eq!(original.directions(false),original);
            let twice=routed.directions(true);assert_eq!(twice.buttons,original.buttons);assert_eq!(twice.axes,original.axes);
            assert_eq!(Mapping::group(slot),original);
        }
    }
    #[test]
    fn ten_slots_survive_restart_without_affecting_other_games_or_unsaved_draft() {
        let f=Fixture::new();let mut c=Controls::new(f.0.clone());
        for i in 0..GAME_SLOTS {
            c.switch_slot(i);c.draft.buttons[0]=Binding::Key(97+i as i32);
            assert!(matches!(c.save_scope(),ResultAction::Apply(_)));
        }
        let (bank,slot,_)=f.0.load_slots();assert_eq!(slot,9);
        for (i,m) in bank.iter().enumerate() {assert_eq!(m.buttons[0],Binding::Key(97+i as i32));}
        c.switch_slot(0);c.draft.buttons[0]=Binding::Key(122);c.nav(Nav::B);
        assert_eq!(f.0.load_slots().0[0].buttons[0],Binding::Key(97));
        let other=Store::new(f.0.root.clone(),Path::new("/mnt/sdcard/Roms/flash/another.swf"));
        assert_eq!(other.load_slots().0,std::array::from_fn(Mapping::group));
    }
    #[test]
    fn startup_ignores_old_global_maps_and_migrates_only_its_own_legacy_game() {
        let f=Fixture::new();f.0.save(&f.0.root.join("global.rmap"),&f.record(8,false)).unwrap();
        assert_eq!(f.0.load_slots().0[0],Mapping::group(0));assert!(!f.0.game_dir().exists());
        f.0.save(&f.0.game,&f.record(3,true)).unwrap();let before=fs::read(&f.0.game).unwrap();
        assert_eq!(f.0.load_slots().0[0].base,3);assert!(!f.0.game_dir().exists());assert_eq!(fs::read(&f.0.game).unwrap(),before);
    }
    #[test]
    fn slot_backup_recovers_corruption_and_identity_collision_is_refused() {
        let f=Fixture::new();let path=f.0.game_slot(4);
        f.0.save(&path,&f.record(1,true)).unwrap();f.0.save(&path,&f.record(2,true)).unwrap();
        fs::write(&path,"bad").unwrap();assert_eq!(f.0.load_slots().0[4].base,1);
        assert_eq!(fs::read_to_string(&path).unwrap(),"bad");
        let mut wrong=f.record(3,true);wrong.identity="deadbeef".into();fs::write(&path,wrong.encode()).unwrap();
        assert!(f.0.save(&path,&f.record(4,true)).is_err());assert_eq!(f.0.load_slots().0[4],Mapping::group(4));
    }
    #[test]
    fn mixed_axis_actions_and_click_roundtrip_and_edit_independently() {
        let f=Fixture::new();let mut r=f.record(1,false);r.mapping.stick=Stick::Custom;
        r.mapping.axes=[Binding::Key(119),Binding::Key(115),Binding::Pointer(2),Binding::RightClick,Binding::Combo([profiles::CTRL,32,0,0],2)];
        assert_eq!(Record::decode(&r.encode()).unwrap(),r);
        let mut c=Controls::new(f.0.clone());c.begin(r.mapping.clone());c.page=Page::Edit;c.row=15;c.nav(Nav::A);
        c.nav(Nav::Right);c.nav(Nav::A);assert_eq!(c.draft.axes[0],Binding::Key(13));assert_eq!(&c.draft.axes[1..],&r.mapping.axes[1..]);
    }
    #[test]
    fn grid_navigation_keeps_every_action_reachable_without_invalid_tail_cells() {
        let mut c=Controls::default();c.page=Page::Picker;
        for category in 0..9 {
            c.category=category;let count=c.choices().len();let mut reached=std::collections::BTreeSet::from([0]);
            loop {let before=reached.len();for row in reached.clone() {for nav in [Nav::Up,Nav::Down,Nav::Left,Nav::Right] {let next=grid_move(row,count,nav);assert!(next<count);reached.insert(next);}}if reached.len()==before {break;}}
            assert_eq!(reached.len(),count);assert!(count<=35);
        }
    }

    #[test]
    fn editor_opens_directly_and_grid_reaches_all_keys_and_commands() {
        let mut c=Controls::default(); c.begin(Mapping::group(0));
        assert_eq!(c.page,Page::Edit); assert_eq!(c.lines().len(),24);
        let mut reached=std::collections::BTreeSet::from([0]);
        loop { let before=reached.len(); for row in reached.clone() { for nav in [Nav::Up,Nav::Down,Nav::Left,Nav::Right] {
            c.row=row; c.nav(nav); assert!(c.row<24); reached.insert(c.row);
        }} if reached.len()==before {break;} }
        assert_eq!(reached.len(),24);
        c.row=19; c.nav(Nav::A); assert_eq!(c.page,Page::Picker);
        c.nav(Nav::B); assert_eq!((c.page,c.row),(Page::Edit,19));
        c.row=BACK_ROW;
        assert!(matches!(c.nav(Nav::B),ResultAction::Close)); assert!(!c.visible);
    }

    #[test]
    fn repeat_is_per_button_and_per_scheme_and_survives_restart() {
        let f=Fixture::new();let mut c=Controls::new(f.0.clone());c.begin(Mapping::group(0));
        c.nav(Nav::Y);c.nav(Nav::Y);assert_eq!(c.draft.turbo[0],Rate::Normal);
        assert!(c.draft.turbo[1..].iter().all(|rate|*rate==Rate::Off));assert_eq!(c.draft.axis_turbo,[Rate::Off;5]);
        assert!(c.lines()[0].contains("连10"));c.row=SAVE_ROW;c.nav(Nav::A);
        c.switch_slot(1);assert_eq!(c.draft.turbo,[Rate::Off;14]);
        let restarted=Controls::new(f.0.clone());assert_eq!(restarted.mappings[0].turbo[0],Rate::Normal);
        assert_eq!(restarted.mappings[1],Mapping::group(1));
        let mut c=Controls::new(f.0.clone());c.switch_slot(0);c.begin(c.draft.clone());c.row=0;c.nav(Nav::X);
        assert_eq!(c.draft.turbo[0],Rate::Normal);c.nav(Nav::Y);c.nav(Nav::Y);assert_eq!(c.draft.turbo[0],Rate::Off);
    }
    #[test]
    fn clean_shoulder_switches_wrap_all_ten_schemes_and_keep_editor_open() {
        let f=Fixture::new();let mut c=Controls::new(f.0.clone());c.begin(c.draft.clone());
        assert!(matches!(c.nav(Nav::Prev),ResultAction::Apply(_)));assert_eq!(c.game_slot,9);
        for slot in 0..GAME_SLOTS {assert!(matches!(c.nav(Nav::Next),ResultAction::Apply(_)));assert_eq!(c.game_slot,slot);assert!(c.visible);assert!(!c.dirty());assert_eq!(c.row,0);}
        assert_eq!(Controls::new(f.0.clone()).game_slot,9);
    }
    #[test]
    fn dirty_switch_can_cancel_discard_or_save_before_continuing() {
        let f=Fixture::new();let mut c=Controls::new(f.0.clone());c.begin(c.draft.clone());c.row=3;c.nav(Nav::Y);
        let draft=c.draft.clone();let before=fs::read(f.0.selected_path()).ok();
        assert!(matches!(c.nav(Nav::Next),ResultAction::None));assert_eq!(c.page,Page::Confirm);assert_eq!(c.game_slot,0);
        assert_eq!(fs::read(f.0.selected_path()).ok(),before);c.nav(Nav::A);assert_eq!(c.row,3);assert_eq!(c.draft,draft);
        c.nav(Nav::Next);c.nav(Nav::Down);assert!(matches!(c.nav(Nav::A),ResultAction::Apply(_)));
        assert_eq!(c.game_slot,1);assert_eq!(c.mappings[0],Mapping::group(0));assert!(!f.0.game_slot(0).exists());
        c.nav(Nav::Y);let saved=c.draft.clone();c.nav(Nav::Next);c.nav(Nav::Down);c.nav(Nav::Down);
        assert!(matches!(c.nav(Nav::A),ResultAction::Apply(_)));assert_eq!(c.game_slot,2);assert!(c.visible);assert!(!c.dirty());
        assert_eq!(Controls::new(f.0.clone()).mappings[1],saved);
    }
    #[test]
    fn dirty_back_and_return_button_both_prompt_and_save_exit_applies() {
        let f=Fixture::new();let mut c=Controls::new(f.0.clone());c.begin(c.draft.clone());c.nav(Nav::Y);let saved=c.draft.clone();
        c.nav(Nav::B);assert_eq!(c.page,Page::Confirm);c.nav(Nav::B);assert!(c.visible);assert_eq!(c.draft,saved);
        c.row=BACK_ROW;c.nav(Nav::A);assert_eq!(c.page,Page::Confirm);c.nav(Nav::Down);c.nav(Nav::Down);
        assert!(matches!(c.nav(Nav::A),ResultAction::Apply(_)));assert!(!c.visible);assert!(!c.dirty());assert_eq!(c.active,Some(saved.clone()));
        assert_eq!(Controls::new(f.0.clone()).mappings[0],saved);
        c.begin(saved);c.nav(Nav::Y);c.row=SAVE_ROW;assert!(matches!(c.nav(Nav::A),ResultAction::Apply(_)));assert!(c.visible);assert!(!c.dirty());
        assert!(matches!(c.nav(Nav::B),ResultAction::Close));
    }
    #[test]
    fn failed_pending_save_never_switches_or_closes_and_keeps_the_draft() {
        let f=Fixture::new();let mut c=Controls::new(f.0.clone());c.begin(c.draft.clone());c.nav(Nav::Y);let draft=c.draft.clone();
        let blocked=f.0.root.join("blocked");fs::create_dir_all(&f.0.root).unwrap();fs::write(&blocked,b"file").unwrap();c.store.root=blocked;
        for nav in [Nav::Next,Nav::B] {
            c.nav(nav);c.nav(Nav::Down);c.nav(Nav::Down);assert!(matches!(c.nav(Nav::A),ResultAction::None));
            assert_eq!(c.page,Page::Confirm);assert!(c.visible);assert_eq!(c.game_slot,0);assert_eq!(c.draft,draft);assert!(c.active.is_none());assert!(c.status.contains("失败"));
            c.nav(Nav::B);assert_eq!(c.page,Page::Edit);
        }
    }
    #[test]
    fn old_v4_has_repeat_disabled_and_invalid_repeat_fields_are_rejected() {
        let f=Fixture::new();let r=Record{mapping:Mapping::group(0),..f.record(1,true)};let text=r.encode();
        let old=text.lines().filter(|line|!line.starts_with("turbo.")&&!line.starts_with("axis-turbo.")).collect::<Vec<_>>().join("\n").replace("version=5","version=4")+"\n";
        assert_eq!(Record::decode(&old).unwrap(),r);
        for bad in [text.replace("turbo.0=0","turbo.0=99"),text.replace("axis-turbo.0=0","axis-turbo.0=10"),text.replace("turbo.0=0\n",""),text.clone()+"turbo.0=0\n"] {assert!(Record::decode(&bad).is_err());}
        let mut c=Controls::default();c.begin(Mapping::group(0));c.row=15;c.nav(Nav::Y);
        assert_eq!(c.draft.axis_turbo[0],Rate::Off);assert!(c.status.contains("支持"));
    }
    #[test]
    fn direction_swap_moves_repeat_rates_with_the_assigned_actions() {
        let mut m=Mapping::group(0);m.turbo[10]=Rate::Fast;m.axes[0]=Binding::Key(13);m.axis_turbo[0]=Rate::Slow;
        let swapped=m.directions(true);assert_eq!(swapped.axis_turbo[0],Rate::Fast);assert_eq!(swapped.turbo[10],Rate::Slow);
        assert_eq!(swapped.axes[4],m.axes[4]);assert_eq!(swapped.axis_turbo[4],m.axis_turbo[4]);
    }

}

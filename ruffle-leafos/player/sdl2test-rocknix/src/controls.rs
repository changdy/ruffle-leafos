//! Local control editor and strictly data-only, versioned template storage.
//! No SDL/GL dependencies: the same state machine is exercised on the host.
use super::{profiles::{self, Binding, Stick}, cursor, InputMode, MouseSpeed};
use std::{collections::BTreeMap, fs::{self, File, OpenOptions}, io::{Read, Write}, path::{Path, PathBuf}};

pub const LABELS: [&str; 14] = ["A", "B", "X", "Y", "L1", "R1", "L2", "R2", "Start", "Select", "十字上", "十字下", "十字左", "十字右"];
pub const AXIS_LABELS: [&str; 5] = ["摇杆上", "摇杆下", "摇杆左", "摇杆右", "摇杆按下"];
pub const CATEGORIES: [&str; 9] = ["常用", "字母", "数字", "功能", "鼠标", "Shift", "Ctrl", "Alt", "符号"];
pub const GAME_SLOTS: usize = 10;
pub const GRID_COLS: usize = 5;
const LIMIT: usize = 16384;
const SLOTS: usize = 16;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Mapping {
    pub cursor: cursor::Settings,
    pub base: usize,
    pub mode: InputMode,
    pub speed: MouseSpeed,
    pub buttons: [Binding; 14],
    pub axes: [Binding; 5],
    pub stick: Stick,
    pub fine: bool,
}


impl Mapping {
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
        self.axes = [Binding::None;5];
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
        let mut mapping = Self { cursor: cursor::Settings::default(), base: base.clamp(1, 12), mode: match p.stick {
            Stick::Mouse if p.dpad.is_none() => InputMode::Mouse, Stick::Mouse => InputMode::Hybrid, _ => InputMode::Keyboard,
        }, speed: MouseSpeed::Normal, buttons, axes: [Binding::None;5], stick: p.stick, fine: p.dpad.is_none() };
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
        let mut text = format!("version=3\nname={}\nidentity={}\nbase={}\nmode={}\nspeed={}\nstick={}\nfine={}\n", self.name, self.identity, m.base,
            match m.mode { InputMode::Hybrid => "hybrid", InputMode::Keyboard => "keyboard", InputMode::Mouse => "mouse" },
            match m.speed { MouseSpeed::Slow => "slow", MouseSpeed::Normal => "normal", MouseSpeed::Fast => "fast" },
            match m.stick { Stick::Directions => "directions", Stick::Steering => "steering", Stick::Mouse => "mouse", Stick::None => "none", Stick::Custom => "custom" }, m.fine);
        for (i, binding) in m.buttons.iter().enumerate() { text.push_str(&format!("bind.{i}={}\n", encode_binding(*binding))); }
        for (i, binding) in m.axes.iter().enumerate() { text.push_str(&format!("axis.{i}={}\n", encode_binding(*binding))); }
        text.push_str(&format!("cursor-style={}\ncursor-color={}\ncursor-size={}\n",m.cursor.style.id(),m.cursor.color.id(),m.cursor.size.pixels()));
        text
    }

    pub fn decode(text: &str) -> Result<Self, String> {
        if text.len() > LIMIT { return Err("模板文件过大".into()); }
        let mut fields = BTreeMap::new();
        for line in text.lines() {
            let (key, value) = line.split_once('=').ok_or("模板字段格式错误")?;
            if fields.insert(key, value).is_some() { return Err("模板字段重复".into()); }
        }
        let get = |key: &str| fields.get(key).copied().ok_or_else(|| format!("模板缺少 {key}"));
        let version = get("version")?;
        let count = match version { "1"=>22,"2"=>25,"3"=>30,_=>return Err("不支持的模板版本".into()) };
        if fields.len() != count { return Err("模板字段缺失或未知".into()); }
        let cursor = if version == "1" { cursor::Settings::default() } else { cursor::Settings {
            style: cursor::Style::parse(get("cursor-style")?).ok_or("光标样式无效")?,
            color: cursor::Color::parse(get("cursor-color")?).ok_or("光标颜色无效")?,
            size: cursor::Size::parse(get("cursor-size")?).ok_or("光标大小无效")?,
        } };
        let name = get("name")?.to_string();
        if name.trim().is_empty() || name.chars().count() > 32 || name.chars().any(|c| c.is_control() || c == '=') { return Err("模板名称无效".into()); }
        let identity = get("identity")?.to_string();
        if identity.len() > 4096 || identity.bytes().any(|b| !b.is_ascii_hexdigit()) { return Err("游戏标识无效".into()); }
        let base = get("base")?.parse::<usize>().map_err(|_| "模板基底无效")?;
        if base > if version == "3" {14} else {12} { return Err("模板基底无效".into()); }
        let mode = match get("mode")? { "hybrid" => InputMode::Hybrid, "keyboard" => InputMode::Keyboard, "mouse" => InputMode::Mouse, _ => return Err("控制方式无效".into()) };
        let speed = match get("speed")? { "slow" => MouseSpeed::Slow, "normal" => MouseSpeed::Normal, "fast" => MouseSpeed::Fast, _ => return Err("鼠标速度无效".into()) };
        let stick = match get("stick")? { "directions" => Stick::Directions, "steering" => Stick::Steering, "mouse" => Stick::Mouse, "none" => Stick::None, "custom" if version == "3" => Stick::Custom, _ => return Err("摇杆方式无效".into()) };
        let fine = match get("fine")? { "true" => true, "false" => false, _ => return Err("十字精调无效".into()) };
        let mut buttons = [Binding::None; 14];
        for (i, binding) in buttons.iter_mut().enumerate() { *binding = decode_binding(get(&format!("bind.{i}"))?)?; }
        let mut mapping = Mapping { cursor, base, mode, speed, buttons, axes: [Binding::None;5], stick, fine };
        if version == "3" { for (i,binding) in mapping.axes.iter_mut().enumerate() { *binding = decode_binding(get(&format!("axis.{i}"))?)?; } }
        else { mapping.reset_axes(); }
        if mapping.buttons.iter().chain(mapping.axes.iter()).all(|b| *b == Binding::None) { return Err("全空模板已拒绝，请恢复默认".into()); }
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
    pub fn template(&self, slot: usize) -> PathBuf { self.root.join("templates").join(format!("{:02}.rmap", slot+1)) }
    fn game_dir(&self) -> PathBuf { self.root.join("slots").join(self.game.file_stem().unwrap_or_default()) }
    pub fn game_slot(&self, slot: usize) -> PathBuf { self.game_dir().join(format!("slot-{:02}.rmap",slot+1)) }
    fn selected_path(&self) -> PathBuf { self.game_dir().join("selected.rmap") }
    pub fn select_slot(&self, slot: usize, mapping: &Mapping) -> Result<(),String> {
        if slot >= GAME_SLOTS { return Err("槽位超出范围".into()); }
        self.save(&self.selected_path(), &Record {name:format!("Slot {}",slot+1),identity:self.identity.clone(),mapping:mapping.clone()})
    }
    pub fn load_slots(&self) -> ([Mapping;GAME_SLOTS],usize,String) {
        let mut mappings = std::array::from_fn(Mapping::group);
        let mut warnings = Vec::new();
        for (i,mapping) in mappings.iter_mut().enumerate() {
            let path = self.game_slot(i);
            if path.exists() || path.with_extension("rmap.bak").exists() {
                match self.load(&path) {
                    Ok((r,recovered)) if r.identity == self.identity => { *mapping = r.mapping; if recovered {warnings.push(format!("槽位{}回退备份",i+1));} },
                    _ => warnings.push(format!("槽位{}损坏，使用默认；原文件保留",i+1)),
                }
            } else if i == 0 && self.game.exists() {
                if let Ok((r,_)) = self.load(&self.game) { if r.identity == self.identity { *mapping = r.mapping; warnings.push("旧游戏配置载入槽位1，保存后迁移".into()); } }
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
    pub fn startup(&self) -> (Option<Record>, String) {
        let mut warnings = Vec::new();
        for (path, source) in [(&self.game, "此游戏默认"), (&self.root.join("global.rmap"), "全局默认")] {
            if !path.exists() { continue; }
            match self.load(path) {
                Ok((r, recovered)) if path != &self.game || r.identity == self.identity => {
                    return (Some(r), format!("{}{}{}", warnings.join("；"), source, if recovered { "（已回退备份）" } else { "" }));
                }
                _ => warnings.push(format!("{source}损坏，原文件保留；")),
            }
        }
        (None, format!("{}内置默认", warnings.join("")))
    }
    pub fn archive(&self, path: &Path) -> Result<(), String> {
        if !path.exists() { return Ok(()); }
        regular(path)?;
        let deleted = self.root.join("deleted"); fs::create_dir_all(&deleted).map_err(|e| e.to_string())?;
        fs::rename(path, deleted.join(format!("{}-{}", unique(), path.file_name().unwrap().to_string_lossy()))).map_err(|e| e.to_string())?;
        let backup = path.with_extension("rmap.bak");
        if backup.exists() { regular(&backup)?; fs::rename(&backup, deleted.join(format!("{}-backup.rmap", unique()))).map_err(|e| e.to_string())?; }
        Ok(())
    }
    pub fn imports(&self) -> Vec<PathBuf> {
        let mut paths: Vec<_> = fs::read_dir(self.root.join("imports")).into_iter().flatten().filter_map(Result::ok)
            .filter(|e| e.file_type().is_ok_and(|t| t.is_file() && !t.is_symlink()))
            .map(|e| e.path()).filter(|p| p.extension().is_some_and(|s| s == "rmap")).take(32).collect();
        paths.sort(); paths
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Page { Hub, Edit, Picker, Templates, Template, Name, Imports, Confirm, Cursor, Slots }
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
pub enum ResultAction { None, Close, Apply(Mapping) }

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
    original: Mapping,
    pub store: Store,
    slot: usize,
    editing: usize,
    pub category: usize,
    confirm: usize,
    name: String,
    rename: bool,
    cursor: usize,
    cursor_direct: bool,
}
impl Default for Controls {
    fn default() -> Self { Self::new(Store::new(PathBuf::new(), Path::new(""))) }
}

impl Controls {
    pub fn new(store: Store) -> Self {
        let (mappings,game_slot,status) = store.load_slots();
        let mapping = mappings[game_slot].clone();
        Self { game_slot, preview_slot:game_slot, mappings, active: None, visible: false, page: Page::Hub, row: 0, status: status.clone(), source: if status.is_empty() {format!("按键映射槽位{}",game_slot+1)} else {status}, draft: mapping.clone(), original: mapping, store,
            slot: 0, editing: 0, category: 0, confirm: 0, name: String::new(), rename: false, cursor: 0, cursor_direct: false }
    }
    pub fn begin(&mut self, actual: Mapping) { self.visible = true; self.cursor_direct = false; self.page = Page::Hub; self.row = 0; self.draft = actual.clone(); self.original = actual; self.status.clear(); }
    pub fn switch_slot(&mut self, slot: usize) -> ResultAction {
        if slot >= GAME_SLOTS { return ResultAction::None; }
        if let Some(active) = &self.active { self.mappings[self.game_slot] = active.clone(); }
        self.game_slot = slot; self.preview_slot = slot; self.draft = self.mappings[slot].clone(); self.original = self.draft.clone();
        let result = self.store.select_slot(slot,&self.draft);
        let remembered = self.failure(result,"槽位选择已记忆");
        let action = self.apply(&format!("按键映射槽位{}",slot+1));
        self.status = format!("已切换至按键映射槽位{}{}",slot+1,if remembered {""} else {"（选择仅本次，写入失败）"}); action
    }
    pub fn begin_cursor(&mut self, actual: Mapping) { self.begin(actual); self.cursor_direct = true; self.go(Page::Cursor); }
    fn leave_cursor(&mut self) -> ResultAction {
        if self.cursor_direct { self.visible = false; ResultAction::Close }
        else { self.go(Page::Edit); self.row = 22; ResultAction::None }
    }
    fn save_scope(&mut self, _game: bool) -> ResultAction {
        let result = self.store.save(&self.store.game_slot(self.game_slot), &self.record(format!("按键映射槽位{}",self.game_slot+1),true));
        if self.failure(result,"保存成功") {
            let source = format!("槽位{}（已保存）",self.game_slot+1);
            let action = self.apply(&source);
            if let Err(e) = self.store.select_slot(self.game_slot,&self.draft) { self.status = format!("槽位已保存，记忆当前选择失败：{e}"); }
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
    fn slots(&self) -> Vec<usize> { (0..SLOTS).filter(|i| self.store.template(*i).exists()).collect() }
    fn template_name(&self, slot: usize) -> String { self.store.load(&self.store.template(slot)).map(|(r, _)| r.name).unwrap_or_else(|_| format!("损坏模板 {:02}（可删除）", slot+1)) }
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
    pub fn title(&self) -> &str { match self.page { Page::Hub => "按键映射与模板", Page::Edit => "编辑按键", Page::Picker => "选择虚拟动作", Page::Templates => "模板管理", Page::Template => "用户模板", Page::Name => "模板名称", Page::Imports => "导入模板", Page::Confirm => "确认操作", Page::Cursor => "鼠标光标", Page::Slots => "此游戏的 10 个映射槽位" } }
    pub fn hint(&self) -> &str { match self.page {
        Page::Edit => "A 编辑  X 恢复此项  B 返回；Menu 始终可用",
        Page::Picker => "十字键移动  L1/R1 切换类型  A 确认  B 取消",
        Page::Name => "上下选字符  左右移位  X 删除  Y 加空格  A 保存  B 取消",
        Page::Confirm => "A 确认  B 取消；仅执行当前所选操作",
        Page::Cursor => "左右调整  A 确认  X 恢复此项  B 返回",
        _ => "上下选择  左右调整  A 确认  B 返回；仅保存后才会记忆",
    } }
    pub fn lines(&self) -> Vec<String> { match self.page {
        Page::Hub => vec![format!("按键映射槽位{}（A 选择其他槽位）", self.game_slot+1), "编辑按键".into(), "应用到当前槽位（本次）".into(), "保存当前槽位并应用".into(), "另存用户模板".into(), "模板管理".into(), "恢复当前槽位默认".into(), "返回（丢弃未应用修改）".into()],
        Page::Edit => LABELS.iter().enumerate().map(|(i,s)| format!("{s}：{}", binding_label(self.draft.buttons[i]))).chain([
            format!("摇杆预设：{}", stick_label(self.draft.stick))]).chain(AXIS_LABELS.iter().enumerate().map(|(i,s)| format!("{s}：{}",binding_label(self.draft.axes[i])))).chain([format!("十字键鼠标精调：{}", if self.draft.fine {"开"} else {"关"}),
            format!("鼠标速度：{}", match self.draft.speed { MouseSpeed::Slow => "慢", MouseSpeed::Normal => "标准", MouseSpeed::Fast => "快" }), "光标样式、颜色与大小".into(), "返回".into()]).collect(),
        Page::Cursor => vec![format!("光标样式：{}",self.draft.cursor.style.name()),format!("光标颜色：{}",self.draft.cursor.color.name()),
            format!("光标大小：{}（{}）",self.draft.cursor.size.name(),self.draft.cursor.size.pixels()),"恢复光标默认".into(),"应用到当前槽位（本次）".into(),"保存当前槽位并应用".into(),"返回".into()],
        Page::Slots => (0..GAME_SLOTS).map(|i|format!("按键映射槽位{}{}",i+1,if i==self.game_slot {"  ● 当前"} else {""})).collect(),
        Page::Picker => self.choices().into_iter().map(binding_label).collect(),
        Page::Templates => profiles::PROFILES.iter().map(|p| format!("内置：{}", p.name)).chain(self.slots().into_iter().map(|s| format!("用户：{}", self.template_name(s)))).chain(["导入本地模板".into(),"返回".into()]).collect(),
        Page::Template => vec![format!("载入：{}", self.template_name(self.slot)), "用当前编辑内容覆盖".into(), "重命名".into(), "导出到 exports 目录".into(), "删除此用户模板".into(), "返回".into()],
        Page::Name => vec![format!("名称：{}", self.name), format!("正在编辑第 {} 个字符", self.cursor+1), "可输入英文字母、数字、空格、横线、下划线".into()],
        Page::Imports => self.store.imports().iter().map(|p| p.file_name().unwrap().to_string_lossy().chars().take(40).collect()).chain(["返回".into()]).collect(),
        Page::Confirm => vec![match self.confirm { 0 => "恢复基底默认，替换当前编辑内容？", 1 => "删除所选用户模板？", 2 => "清除此游戏默认？", 3 => "用编辑内容覆盖所选用户模板？", _ => "清除全局默认？" }.into(), "A 确认操作".into(), "B 取消操作".into()],
    } }
    pub fn detail(&self) -> String {
        if self.page == Page::Picker { return format!("{}  →  {}", if self.editing < 14 {LABELS[self.editing]} else {AXIS_LABELS[self.editing-15]}, binding_label(if self.editing < 14 {self.draft.buttons[self.editing]} else {self.draft.axes[self.editing-15]})); }
        if self.page == Page::Name { return "名称不影响模板编号；导入支持中文名称".into(); }
        if self.page == Page::Cursor { return "静止 3.5 秒自动隐藏；再次移动立即显示".into(); }
        format!("当前生效来源：{}；编辑内容{}", self.source, if self.active.as_ref().unwrap_or(&self.original) == &self.draft { "已应用" } else { "尚未应用" })
    }
    pub fn nav(&mut self, nav: Nav) -> ResultAction {
        use Nav::*;
        if self.page == Page::Name { return self.name_nav(nav); }
        if matches!(nav, B) {
            if self.page == Page::Cursor { return self.leave_cursor(); }
            match self.page { Page::Hub => { self.visible = false; return ResultAction::Close; }, Page::Picker => { self.go(Page::Edit); self.row = self.editing; }, Page::Template => self.go(Page::Templates), Page::Confirm => self.go(if matches!(self.confirm,1|3) {Page::Template} else {Page::Hub}), Page::Imports => self.go(Page::Templates), _ => self.go(Page::Hub) }
            return ResultAction::None;
        }
        if self.page == Page::Confirm && !matches!(nav,A) { return ResultAction::None; }
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
                self.go(Page::Edit); self.row = self.editing;
            }
            return ResultAction::None;
        }
        if self.page == Page::Slots && matches!(nav,Up|Down|Left|Right) {self.row=grid_columns(self.row,count,nav,2);return ResultAction::None;}
        if matches!(nav, Up | Down) { self.row = (self.row as i32 + if matches!(nav,Up) {-1} else {1}).rem_euclid(count as i32) as usize; return ResultAction::None; }
        let adjust = if matches!(nav,Left) {-1} else {1};
        if self.page == Page::Cursor {
            if matches!(nav,X) {
                let defaults=cursor::Settings::default();
                match self.row { 0=>self.draft.cursor.style=defaults.style,1=>self.draft.cursor.color=defaults.color,2=>self.draft.cursor.size=defaults.size,_=>{} }
                self.status="此项已恢复默认，尚未应用".into();
            }
            if matches!(nav,Left|Right|A) { match self.row {
                0=>self.draft.cursor.style=self.draft.cursor.style.next(adjust),
                1=>self.draft.cursor.color=self.draft.cursor.color.next(adjust),
                2=>self.draft.cursor.size=self.draft.cursor.size.next(adjust),
                3 if matches!(nav,A)=>{ self.draft.cursor=cursor::Settings::default(); self.status="光标已恢复默认，请应用或保存".into(); },
                4 if matches!(nav,A)=>return self.apply("仅本次"),
                5 if matches!(nav,A)=>return self.save_scope(true),
                6 if matches!(nav,A)=>return self.leave_cursor(),_=>{}
            } }
            return ResultAction::None;
        }
        if self.page == Page::Edit {
            if matches!(nav, X) {
                let defaults = Mapping::group(self.game_slot);
                match self.row { 0..=13 => self.draft.buttons[self.row] = defaults.buttons[self.row], 14 => { self.draft.stick = defaults.stick; self.draft.axes = defaults.axes; }, 15..=19 => {self.draft.axes[self.row-15] = defaults.axes[self.row-15];self.draft.stick=Stick::Custom;}, 20 => self.draft.fine = defaults.fine, 21 => self.draft.speed = defaults.speed, _ => {} }
                self.status = "此项已恢复槽位默认，尚未应用".into();
                if (10..14).contains(&self.row) && (self.draft.stick==Stick::Directions || self.draft.stick==Stick::Steering && self.row>=12) {self.draft.axes[self.row-10]=self.draft.buttons[self.row];}
            }
            if matches!(nav, Left | Right | A) {
                match self.row {
                    0..=13 | 15..=19 if matches!(nav,A) => { self.editing = self.row; self.category = 0; self.go(Page::Picker); },
                    14 => { let items = [Stick::Mouse,Stick::Directions,Stick::Steering,Stick::None,Stick::Custom]; let i = items.iter().position(|s| *s == self.draft.stick).unwrap(); self.draft.stick = items[(i as i32+adjust).rem_euclid(5) as usize]; if self.draft.stick != Stick::Custom {let press=self.draft.axes[4]; self.draft.reset_axes();self.draft.axes[4]=press;} },
                    20 => self.draft.fine = !self.draft.fine,
                    21 => self.draft.speed = self.draft.speed.next(adjust),
                    22 if matches!(nav,A) => { self.cursor_direct = false; self.go(Page::Cursor); },
                    23 if matches!(nav,A) => self.go(Page::Hub), _ => {}
                }
            }
        }
        if !matches!(nav,A) { return ResultAction::None; }
        match self.page {
            Page::Hub => match self.row {
                0 => { self.go(Page::Slots); self.row = self.game_slot; },
                1 => self.go(Page::Edit), 2 => return self.apply("仅本次"),
                3 => return self.save_scope(true),
                4 => {
                    if let Some(slot) = (0..SLOTS).find(|i| !self.store.template(*i).exists()) {
                        self.slot = slot; self.name = format!("Template {:02}",slot+1); self.rename = false; self.cursor = 0; self.go(Page::Name);
                    } else { self.status = "用户模板已满（16 个），请先删除或覆盖".into(); }
                }
                5 => self.go(Page::Templates), 6 => { self.confirm = 0; self.go(Page::Confirm); },
                7 => { self.visible = false; return ResultAction::Close; }, _ => {}
            },
            Page::Slots => { let slot = self.row; let action = self.switch_slot(slot); self.go(Page::Hub); return action; },
            Page::Templates => {
                let slots = self.slots();
                if self.row < 12 { self.draft = Mapping::builtin(self.row+1); self.go(Page::Hub); self.status = "内置模板已载入编辑区，尚未应用".into(); }
                else if self.row < 12+slots.len() { self.slot = slots[self.row-12]; self.go(Page::Template); }
                else if self.row == 12+slots.len() { self.go(Page::Imports); }
                else { self.go(Page::Hub); }
            }
            Page::Template => {
                let path = self.store.template(self.slot);
                match self.row {
                    0 => match self.store.load(&path) { Ok((r, recovered)) => { self.draft = r.mapping; self.go(Page::Hub); self.status = if recovered {"已载入备份模板；原损坏文件保留"} else {"用户模板已载入编辑区，尚未应用"}.into(); }, Err(_) => self.status = "模板损坏；可返回删除或载入内置模板".into() },
                    1 => { self.confirm = 3; self.go(Page::Confirm); },
                    2 => { self.name = self.template_name(self.slot); self.rename = true; self.cursor = 0; self.go(Page::Name); },
                    3 => match self.store.load(&path) { Ok((r,_)) => { let result = self.store.save(&self.store.root.join("exports").join(format!("template-{:02}.rmap",self.slot+1)), &Record {identity: String::new(), ..r}); self.failure(result,"已导出到 exports 目录"); }, Err(_) => self.status = "模板损坏，无法导出".into() },
                    4 => { self.confirm = 1; self.go(Page::Confirm); }, _ => self.go(Page::Templates),
                }
            }
            Page::Imports => {
                if let Some(path) = self.store.imports().get(self.row) {
                    match read_record(path) { Ok(mut r) => {
                        if let Some(slot) = (0..SLOTS).find(|i| !self.store.template(*i).exists()) {
                            r.identity.clear(); let result = self.store.save(&self.store.template(slot), &r);
                            if self.failure(result,"导入成功，原文件保留") { self.go(Page::Templates); }
                        } else { self.status = "用户模板已满，请先删除或覆盖".into(); }
                    }, Err(e) => self.status = format!("拒绝导入：{e}") }
                } else { self.go(Page::Templates); }
            }
            Page::Confirm => match self.confirm {
                0 => { self.draft = Mapping::group(self.game_slot); self.go(Page::Hub); self.status = "已恢复槽位默认；请应用或保存".into(); },
                1 => { let result = self.store.archive(&self.store.template(self.slot)); if self.failure(result,"用户模板已删除，当前游戏映射保留") { self.go(Page::Templates); } },
                2 | 4 => {
                    let path = if self.confirm == 2 {self.store.game.clone()} else {self.store.root.join("global.rmap")};
                    let result = self.store.archive(&path); if self.failure(result,"已清除默认；下次启动生效，当前映射保留") { self.go(Page::Hub); }
                }
                3 => { let record = self.record(self.template_name(self.slot),false); let result = self.store.save(&self.store.template(self.slot), &record); if self.failure(result,"用户模板已更新") { self.go(Page::Template); } }, _ => {}
            }, _ => {}
        }
        ResultAction::None
    }
    fn name_nav(&mut self, nav: Nav) -> ResultAction {
        use Nav::*;
        let mut chars: Vec<char> = self.name.chars().collect();
        if matches!(nav,B) { self.go(if self.rename {Page::Template} else {Page::Hub}); return ResultAction::None; }
        if matches!(nav, Left) { self.cursor = self.cursor.saturating_sub(1); }
        if matches!(nav, Right) { self.cursor = (self.cursor+1).min(chars.len().min(23)); }
        if matches!(nav, X) && self.cursor < chars.len() { chars.remove(self.cursor); self.cursor = self.cursor.min(chars.len()); }
        if matches!(nav, Y) && chars.len() < 24 { chars.insert(self.cursor.min(chars.len()),' '); self.cursor = (self.cursor+1).min(23); }
        if matches!(nav, Up | Down) {
            let alphabet: Vec<char> = " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_".chars().collect();
            if self.cursor >= chars.len() { chars.push(' '); }
            let pos = alphabet.iter().position(|c| *c == chars[self.cursor]).unwrap_or(0);
            chars[self.cursor] = alphabet[(pos as i32 + if matches!(nav,Up) {1} else {-1}).rem_euclid(alphabet.len() as i32) as usize];
        }
        self.name = chars.into_iter().collect();
        if matches!(nav,A) {
            let mut record = self.record(self.name.trim().to_string(),false);
            if self.rename { match self.store.load(&self.store.template(self.slot)) { Ok((r,_)) => record.mapping = r.mapping, Err(_) => { self.status = "损坏模板无法重命名".into(); return ResultAction::None; } } }
            let result = self.store.save(&self.store.template(self.slot), &record);
            if self.failure(result,"模板已保存") { self.go(if self.rename {Page::Template} else {Page::Hub}); }
        }
        ResultAction::None
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    struct Fixture(Store);
    impl Fixture {
        fn new() -> Self { Self(Store::new(std::env::temp_dir().join(format!("ruffle-controls-{}",unique())), Path::new("/mnt/sdcard/Roms/flash/folder/game.swf"))) }
        fn record(&self, base: usize, game: bool) -> Record { Record { name: "测试模板".into(), identity: if game {self.0.identity.clone()} else {String::new()}, mapping: Mapping::builtin(base) } }
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
        for broken in [text.replace("version=3","version=99"),text.clone()+"command=touch /tmp/pwned\n",text.clone()+"version=1\n",text.replace("bind.0=32","bind.0=$(reboot)"),text.replace("base=1","base=99"),text.replace("bind.0=32","bind.0=1073741925"),text.replace("bind.0=32","bind.0=32,32")] { assert!(Record::decode(&broken).is_err(),"{broken}"); }
        assert!(Record::decode(&"x".repeat(LIMIT+1)).is_err());
    }
    #[test]
    fn completely_empty_mapping_is_rejected_but_menu_can_still_restore() {
        let mut c = Controls::default(); c.draft.buttons.fill(Binding::None); c.draft.axes.fill(Binding::None); c.draft.stick = Stick::None;
        c.row = 2; assert!(matches!(c.nav(Nav::A),ResultAction::None)); assert!(c.active.is_none());
        c.row = 6; c.nav(Nav::A); c.nav(Nav::A); assert_eq!(c.draft,Mapping::group(0));
        c.row = 2; assert!(matches!(c.nav(Nav::A),ResultAction::Apply(_)));
    }
    #[test]
    fn game_override_wins_global_and_never_affects_another_game() {
        let f = Fixture::new(); f.0.save(&f.0.root.join("global.rmap"),&f.record(2,false)).unwrap();
        f.0.save(&f.0.game,&f.record(3,true)).unwrap();
        assert_eq!(f.0.startup().0.unwrap().mapping.base,3);
        let other = Store::new(f.0.root.clone(),Path::new("/mnt/sdcard/Roms/flash/other/game.swf"));
        assert_ne!(f.0.game,other.game); assert_eq!(other.startup().0.unwrap().mapping.base,2);
        f.0.archive(&f.0.game).unwrap(); assert_eq!(f.0.startup().0.unwrap().mapping.base,2);
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
        let (loaded,source) = f.0.startup(); assert_eq!(loaded.unwrap().mapping.base,1); assert!(source.contains("备份"));
        assert_eq!(fs::read_to_string(&f.0.game).unwrap(),"damaged");
        f.0.save(&f.0.game,&f.record(3,true)).unwrap();
        assert_eq!(read_record(&f.0.game.with_extension("rmap.bak")).unwrap().mapping.base,1);
        assert!(fs::read_dir(f.0.game.parent().unwrap()).unwrap().any(|e| e.unwrap().path().extension().unwrap().to_string_lossy().starts_with("invalid-")));
    }
    #[test]
    fn corrupt_game_falls_through_to_global_and_no_files_are_deleted() {
        let f = Fixture::new(); f.0.save(&f.0.root.join("global.rmap"),&f.record(6,false)).unwrap();
        fs::create_dir_all(f.0.game.parent().unwrap()).unwrap(); fs::write(&f.0.game,"bad").unwrap();
        assert_eq!(f.0.startup().0.unwrap().mapping.base,6); assert_eq!(fs::read_to_string(&f.0.game).unwrap(),"bad");
    }
    #[test]
    fn identity_mismatch_refuses_to_load_or_overwrite_other_game() {
        let f = Fixture::new(); let mut r = f.record(1,true); r.identity = "deadbeef".into();
        fs::create_dir_all(f.0.game.parent().unwrap()).unwrap(); fs::write(&f.0.game,r.encode()).unwrap();
        assert!(f.0.startup().0.is_none()); assert!(f.0.save(&f.0.game,&f.record(3,true)).is_err()); assert_eq!(read_record(&f.0.game).unwrap(),r);
    }
    #[test]
    fn edit_cancel_apply_and_reset_do_not_mutate_builtin_catalog() {
        let mut c = Controls::default(); let original = Mapping::builtin(1); c.begin(original.clone());
        c.row = 1; c.nav(Nav::A); c.nav(Nav::A); assert_eq!(c.page,Page::Picker);
        c.nav(Nav::Right); c.nav(Nav::A); assert_eq!(c.draft.buttons[0],Binding::Key(13)); assert!(c.active.is_none());
        c.nav(Nav::X); assert_eq!(c.draft.buttons[0],Binding::Key(106));
        c.nav(Nav::B); c.row = 2; assert!(matches!(c.nav(Nav::A),ResultAction::Apply(_)));
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
    fn template_save_rename_export_import_and_delete_preserve_current_mapping() {
        let f = Fixture::new(); let mut c = Controls::new(f.0.clone()); c.begin(Mapping::builtin(7));
        c.row = 4; c.nav(Nav::A); c.nav(Nav::A); assert!(c.store.template(0).exists());
        c.go(Page::Template); c.slot = 0; c.row = 2; c.nav(Nav::A); c.name = "Renamed".into(); c.nav(Nav::A);
        assert_eq!(read_record(&c.store.template(0)).unwrap().name,"Renamed");
        c.row = 3; c.nav(Nav::A); let export = c.store.root.join("exports/template-01.rmap"); assert!(export.exists());
        fs::create_dir_all(c.store.root.join("imports")).unwrap(); fs::copy(export,c.store.root.join("imports/copy.rmap")).unwrap();
        c.go(Page::Imports); c.nav(Nav::A); assert!(c.store.template(1).exists());
        c.active = Some(c.draft.clone()); c.go(Page::Template); c.row = 4; c.nav(Nav::A); c.nav(Nav::B); assert!(c.store.template(0).exists());
        c.row = 4; c.nav(Nav::A); c.nav(Nav::A); assert!(!c.store.template(0).exists()); assert!(c.active.is_some());
        assert!(!c.store.template(0).with_extension("rmap.bak").exists());
    }
    #[test]
    fn write_failure_does_not_apply_or_claim_success() {
        let f = Fixture::new(); fs::create_dir_all(&f.0.root).unwrap(); fs::write(f.0.root.join("global.rmap"),"bad").unwrap();
        let mut c = Controls::new(Store::new(f.0.root.join("global.rmap"),Path::new("game.swf"))); c.row = 3;
        assert!(matches!(c.nav(Nav::A),ResultAction::None)); assert!(c.active.is_none()); assert!(c.status.contains("失败"));
    }
    #[test]
    fn restoring_legacy_mode_uses_legacy_defaults_including_mouse_buttons() {
        let mut m = Mapping::builtin(1); m.base=0; m.mode=InputMode::Mouse; let restored = m.defaults();
        assert_eq!(restored.base,0); assert_eq!(restored.buttons[0],Binding::LeftClick); assert_eq!(restored.buttons[1],Binding::RightClick); assert!(restored.fine);
    }

    #[test]
    fn version_one_loads_without_rewriting_and_new_cursor_values_roundtrip() {
        let f=Fixture::new(); let r=f.record(6,true);
        let legacy=r.encode().lines().filter(|s|!s.starts_with("cursor-") && !s.starts_with("axis.")).collect::<Vec<_>>().join("\n").replace("version=3","version=1")+"\n";
        fs::create_dir_all(f.0.game.parent().unwrap()).unwrap(); fs::write(&f.0.game,&legacy).unwrap();
        let loaded=f.0.startup().0.unwrap(); assert_eq!(loaded.mapping.cursor,cursor::Settings::default()); assert_eq!(fs::read_to_string(&f.0.game).unwrap(),legacy);
        let mut updated=loaded; updated.mapping.cursor=cursor::Settings{style:cursor::Style::Crosshair,color:cursor::Color::Cyan,size:cursor::Size::ExtraLarge};
        f.0.save(&f.0.game,&updated).unwrap(); assert_eq!(f.0.load(&f.0.game).unwrap().0,updated);
        assert_eq!(fs::read_to_string(f.0.game.with_extension("rmap.bak")).unwrap(),legacy);
    }
    #[test]
    fn cursor_format_rejects_unknown_colors_sizes_and_incomplete_versions() {
        let f=Fixture::new(); let text=f.record(1,false).encode();
        for invalid in [text.replace("cursor-color=white","cursor-color=exec"),text.replace("cursor-size=24","cursor-size=99999"),text.replace("cursor-style=ring-dot","cursor-style=unknown"),text.replace("version=3","version=1"),text.replace("cursor-size=24\n","")] { assert!(Record::decode(&invalid).is_err()); }
    }
    #[test]
    fn saved_arrow_maps_to_ring_without_rewriting_and_preserves_other_fields() {
        let f=Fixture::new();let expected=f.record(6,true);
        let legacy=expected.encode().replace("cursor-style=ring-dot","cursor-style=arrow");
        fs::create_dir_all(f.0.game.parent().unwrap()).unwrap();fs::write(&f.0.game,&legacy).unwrap();
        let loaded=f.0.load(&f.0.game).unwrap().0;
        assert_eq!(loaded,expected);assert_eq!(fs::read_to_string(&f.0.game).unwrap(),legacy);
        assert!(loaded.encode().contains("cursor-style=ring-dot\n"));
        f.0.save(&f.0.game,&loaded).unwrap();
        assert_eq!(f.0.load(&f.0.game).unwrap().0,expected);
        assert_eq!(fs::read_to_string(f.0.game.with_extension("rmap.bak")).unwrap(),legacy);
    }
    #[test]
    fn cursor_preview_cancel_and_single_reset_never_apply_or_save() {
        let mut c=Controls::default(); let original=Mapping::builtin(6); c.begin_cursor(original.clone());
        c.nav(Nav::Right); assert_ne!(c.draft.cursor,original.cursor); assert!(c.active.is_none());
        c.nav(Nav::X); assert_eq!(c.draft.cursor,original.cursor);
        c.nav(Nav::Down); c.nav(Nav::Right); assert!(matches!(c.nav(Nav::B),ResultAction::Close)); assert!(c.active.is_none());
    }
    #[test]
    fn cursor_page_saves_current_game_slot_and_template_with_same_values() {
        let f=Fixture::new();let mut c=Controls::new(f.0.clone());c.begin_cursor(Mapping::group(0));
        c.nav(Nav::Right);c.nav(Nav::Down);c.nav(Nav::Right);c.nav(Nav::Down);c.nav(Nav::Right);
        let expected=c.draft.cursor;c.row=5;assert!(matches!(c.nav(Nav::A),ResultAction::Apply(_)));
        assert_eq!(c.store.load_slots().0[0].cursor,expected);assert!(!c.store.root.join("global.rmap").exists());
        c.go(Page::Hub);c.row=4;c.nav(Nav::A);c.nav(Nav::A);
        assert_eq!(c.store.load(&c.store.template(0)).unwrap().0.mapping.cursor,expected);
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
    fn ten_slots_survive_restart_without_affecting_other_games_or_unsaved_draft() {
        let f=Fixture::new();let mut c=Controls::new(f.0.clone());
        for i in 0..GAME_SLOTS {
            c.switch_slot(i);c.draft.buttons[0]=Binding::Key(97+i as i32);
            assert!(matches!(c.save_scope(true),ResultAction::Apply(_)));
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

}

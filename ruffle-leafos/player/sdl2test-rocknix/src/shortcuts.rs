//! Read-only physical state: Leaf's tap-only Menu proxy intentionally hides
//! Menu-down from SDL. Never grab the device or change system shortcut settings.
#[derive(Clone,Copy,Debug,Default)]
pub struct Snapshot { pub menu: bool, pub a: bool, pub b: bool, pub button_x: bool, pub x: i32, pub y: i32 }
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub enum Command { Previous, Next, Drag, SwapDirections }
#[derive(Default)]
pub struct Chords { held: [bool;4], armed: bool }
impl Chords {
    pub fn update(&mut self, s: Snapshot) -> Vec<Command> {
        if !s.menu { self.armed = true; self.held = [false;4]; return Vec::new(); }
        if !self.armed { return Vec::new(); }
        // Physical A = previous, B = next. Opposing buttons cancel rather
        // than switching twice; the old Menu+hat left/right is inactive.
        let single=[s.a,s.b,s.y>0,s.button_x].into_iter().filter(|held|*held).count()==1;
        let now = [s.a&&single,s.b&&single,s.y>0&&single,s.button_x&&single];
        let mut commands = Vec::new();
        for (i,command) in [Command::Previous,Command::Next,Command::Drag,Command::SwapDirections].into_iter().enumerate() {
            if now[i] && !self.held[i] { commands.push(command); }
        }
        self.held = now; commands
    }
}

#[cfg(all(target_os="linux",not(test)))]
pub struct Device { file: Option<std::fs::File> }
#[cfg(all(target_os="linux",not(test)))]
impl Device {
    pub fn new() -> Self {
        use std::os::fd::AsRawFd;
        let physical = std::fs::read_dir("/sys/class/input").into_iter().flatten().filter_map(Result::ok).find_map(|entry| {
            let name = entry.file_name(); let name = name.to_str()?;
            if !name.starts_with("event") { return None; }
            let path = entry.path();
            if !std::fs::canonicalize(&path).ok()?.to_string_lossy().contains("/devices/platform/") {return None;}
            if std::fs::read_to_string(path.join("device/name")).ok()?.trim() != "Loong Gamepad" { return None; }
            // Leaf's roster namespace hides the physical event node to avoid
            // duplicate controller input. We only query key/hat state: no reads,
            // event grabs, event writes, mounts or system configuration changes.
            for (prefix,host_view) in [("/dev/input",false),("/proc/1/root/dev/input",true)] {
                if let Ok(file)=std::fs::File::open(format!("{prefix}/{name}")) {
                    let mut label=[0u8;128];let mut phys=[0u8;128];let mut id=[0u16;4];
                    let valid=unsafe {libc::ioctl(file.as_raw_fd(),0x80804506u64,&mut label)>=0
                        && libc::ioctl(file.as_raw_fd(),0x80804507u64,&mut phys)>=0
                        && libc::ioctl(file.as_raw_fd(),0x80084502u64,&mut id)>=0};
                    if valid && label.split(|b|*b==0).next()==Some(b"Loong Gamepad".as_slice())
                        && phys.split(|b|*b==0).next()==Some(b"loong1_joypad/input0".as_slice()) && id[1..3]==[0x9903,0x9913] {
                        return Some((file,host_view));
                    }
                }
            }
            None
        });
        println!("leaf-shortcuts physical_menu_observer={} access=read-only-state host_view={}",physical.is_some(),physical.as_ref().is_some_and(|(_,host)|*host));
        Self {file:physical.map(|(file,_)|file)}
    }
    pub fn snapshot(&self) -> Snapshot {
        use std::os::fd::AsRawFd;
        let Some(file) = &self.file else {return Snapshot::default();};
        let mut keys = [0u8;96]; let mut x = [0i32;6]; let mut y = [0i32;6];
        // Linux EVIOCGKEY(96), EVIOCGABS(ABS_HAT0X/Y); input_absinfo is six i32s.
        let ok = unsafe { libc::ioctl(file.as_raw_fd(),0x80604518u64,&mut keys) >= 0
            && libc::ioctl(file.as_raw_fd(),0x80184550u64,&mut x) >= 0
            && libc::ioctl(file.as_raw_fd(),0x80184551u64,&mut y) >= 0 };
        // BTN_EAST is the target's physical A, BTN_SOUTH its physical B.
        // This is the same physical-label convention as leaf_input_tap.c.
        if ok {Snapshot {menu:keys[316/8] & (1 << (316%8)) != 0,
            a:keys[305/8] & (1 << (305%8)) != 0,b:keys[304/8] & (1 << (304%8)) != 0,
            button_x:keys[307/8] & (1 << (307%8)) != 0,x:x[0],y:y[0]}} else {Snapshot::default()}
    }
}
#[cfg(any(not(target_os="linux"),test))]
pub struct Device;
#[cfg(any(not(target_os="linux"),test))]
impl Device {
    pub fn new() -> Self {Self}
    pub fn snapshot(&self) -> Snapshot {Snapshot::default()}
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn menu_x_is_edge_triggered_and_ambiguous_chords_are_ignored() {
        let mut c=Chords::default();let swap=Snapshot{menu:true,button_x:true,..Default::default()};
        assert!(c.update(swap).is_empty());c.update(Snapshot::default());
        assert_eq!(c.update(swap),vec![Command::SwapDirections]);assert!(c.update(swap).is_empty());
        c.update(Snapshot{menu:true,..Default::default()});assert_eq!(c.update(swap),vec![Command::SwapDirections]);
        assert!(c.update(Snapshot{a:true,..swap}).is_empty());
        c.update(Snapshot::default());assert!(c.update(Snapshot{button_x:true,..Default::default()}).is_empty());
    }
    #[test]
    fn combos_trigger_once_and_never_trigger_on_startup_held_menu() {
        let mut c=Chords::default();
        let b=Snapshot{menu:true,b:true,..Default::default()};
        assert!(c.update(b).is_empty()); c.update(Snapshot::default());
        assert_eq!(c.update(b),vec![Command::Next]); assert!(c.update(b).is_empty());
        c.update(Snapshot{menu:true,..Default::default()});
        assert_eq!(c.update(Snapshot{menu:true,a:true,..Default::default()}),vec![Command::Previous]);
        assert!(c.update(Snapshot{menu:true,a:true,..Default::default()}).is_empty());
        c.update(Snapshot{menu:true,..Default::default()});
        assert_eq!(c.update(Snapshot{menu:true,y:1,..Default::default()}),vec![Command::Drag]);
        c.update(Snapshot::default()); assert!(c.update(Snapshot{a:true,..Default::default()}).is_empty());
    }
    #[test]
    fn old_hat_shortcuts_are_disabled_and_opposing_buttons_do_not_switch_twice() {
        let mut c=Chords::default();c.update(Snapshot::default());
        for x in [-1,0,1] {assert!(c.update(Snapshot{menu:true,x,..Default::default()}).is_empty());}
        assert!(c.update(Snapshot{menu:true,a:true,b:true,..Default::default()}).is_empty());
        assert_eq!(c.update(Snapshot{menu:true,a:true,..Default::default()}),vec![Command::Previous]);
        c.update(Snapshot::default());
        assert_eq!(c.update(Snapshot{menu:true,b:true,..Default::default()}),vec![Command::Next]);
    }
}

//! Read-only physical input observer. Menu taps and chords have their own
//! lifecycle, independent of SDL's virtual controller and game-held keys.
use std::time::Duration;

pub fn is_pocketais() -> bool {
    std::env::var_os("RUFFLE_POCKET_AIS").is_some_and(|value| value == "1")
}
#[derive(Clone,Copy,Debug,Default,PartialEq,Eq)]
pub struct Snapshot { pub menu: bool, pub a: bool, pub b: bool, pub button_x: bool, pub x: i32, pub y: i32, pub other: bool, pub volume_up: bool, pub volume_down: bool, pub brightness_up: bool, pub brightness_down: bool }
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub enum Command { Previous, Next, Drag, SwapDirections, BrightnessUp, BrightnessDown, MenuTap }
#[derive(Default)]
pub struct Chords { held: [bool;6], armed: bool, menu_press: Option<(Duration,bool)> }
impl Chords {
    #[cfg(test)]
    pub fn update(&mut self, s: Snapshot) -> Vec<Command> {
        self.update_at(s,Duration::ZERO)
    }
    pub fn update_at(&mut self, s: Snapshot, now: Duration) -> Vec<Command> {
        if !s.menu {
            let tap=self.menu_press.take().is_some_and(|(at,alone)|alone && now.saturating_sub(at)<Duration::from_secs(3));
            self.armed = true; self.held = [false;6];
            return if tap {vec![Command::MenuTap]}else{Vec::new()};
        }
        if !self.armed { return Vec::new(); }
        let alone=!s.other&&!s.a&&!s.b&&!s.button_x&&s.x==0&&s.y==0&&!s.volume_up&&!s.volume_down&&!s.brightness_up&&!s.brightness_down;
        let press=self.menu_press.get_or_insert((now,alone));press.1&=alone;
        // Physical A = previous, B = next. Opposing buttons cancel rather
        // than switching twice; the old Menu+hat left/right is inactive.
        let pressed = [s.a, s.b, s.y > 0, s.button_x, s.brightness_up, s.brightness_down];
        let single = pressed.into_iter().filter(|held| *held).count() == 1;
        let now = pressed.map(|held| held && single);
        let mut commands = Vec::new();
        for (i,command) in [Command::Previous,Command::Next,Command::Drag,Command::SwapDirections,Command::BrightnessUp,Command::BrightnessDown].into_iter().enumerate() {
            if now[i] && !self.held[i] { commands.push(command); }
        }
        self.held = now; commands
    }
}

#[cfg(all(target_os="linux",not(test)))]
#[derive(Clone, Copy)]
enum Pad { Leaf, PocketAis }

#[cfg(all(target_os="linux",not(test)))]
pub struct Device {
    receiver: std::sync::mpsc::Receiver<(Snapshot,Duration)>,
    stop: std::sync::Arc<std::sync::atomic::AtomicBool>,
    worker: Option<std::thread::JoinHandle<()>>,
    available: bool,
}
#[cfg(all(target_os="linux",not(test)))]
impl Device {
    pub fn new() -> Self {
        use std::os::fd::AsRawFd;
        let pad = if is_pocketais() { Pad::PocketAis } else { Pad::Leaf };
        let physical = std::fs::read_dir("/sys/class/input").into_iter().flatten().filter_map(Result::ok).find_map(|entry| {
            let name = entry.file_name(); let name = name.to_str()?;
            if !name.starts_with("event") { return None; }
            let path = entry.path();
            if !std::fs::canonicalize(&path).ok()?.to_string_lossy().contains("/devices/platform/") {return None;}
            let label = std::fs::read_to_string(path.join("device/name")).ok()?;
            match pad {
                Pad::PocketAis => {
                    if label.trim() != "play_joystick" { return None; }
                    return std::fs::File::open(format!("/dev/input/{name}")).ok().map(|file| (file, false));
                }
                Pad::Leaf if label.trim() != "Loong Gamepad" => return None,
                Pad::Leaf => {}
            }
            // Leaf's roster namespace hides the physical event node to avoid
            // duplicate controller input. EVIOCGKEY/ABS remains readable while
            // Leaf owns EVIOCGRAB; an additional event reader receives nothing.
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
        // Pocket AIS exposes the chassis volume buttons on adc-keys, not the pad.
        let volume = if matches!(pad, Pad::PocketAis) {
            std::fs::read_dir("/sys/class/input").into_iter().flatten().filter_map(Result::ok).find_map(|entry| {
                let name = entry.file_name();
                let name = name.to_str()?.strip_prefix("event")?;
                if name.is_empty() || !name.bytes().all(|b| b.is_ascii_digit()) { return None; }
                let path = entry.path();
                if !std::fs::canonicalize(&path).ok()?.to_string_lossy().contains("/devices/platform/adc-keys/") { return None; }
                if std::fs::read_to_string(path.join("device/name")).ok()?.trim() != "adc-keys" { return None; }
                std::fs::File::open(format!("/dev/input/event{name}")).ok()
            })
        } else { None };
        println!("leaf-shortcuts physical_menu_observer={} access=read-only-state-thread host_view={}",physical.is_some(),physical.as_ref().is_some_and(|(_,host)|*host));
        let (sender,receiver)=std::sync::mpsc::channel();
        let stop=std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
        let available=physical.is_some();let stopped=stop.clone();
        let worker=physical.map(|(file,_)|std::thread::spawn(move||observe(file,volume,pad,sender,stopped)));
        Self {receiver,stop,worker,available}
    }
    pub fn available(&self) -> bool {self.available}
    pub fn drain(&mut self) -> Vec<(Snapshot,Duration)> {
        let mut samples=Vec::new();
        loop {match self.receiver.try_recv() {
            Ok(sample)=>samples.push(sample),
            Err(std::sync::mpsc::TryRecvError::Empty)=>break,
            Err(std::sync::mpsc::TryRecvError::Disconnected)=>{self.available=false;break;},
        }}
        samples
    }
}
#[cfg(all(target_os="linux",not(test)))]
impl Drop for Device {
    fn drop(&mut self) {self.stop.store(true,std::sync::atomic::Ordering::Relaxed);if let Some(worker)=self.worker.take(){let _=worker.join();}}
}
#[cfg(all(target_os="linux",not(test)))]
fn observe(file:std::fs::File,volume:Option<std::fs::File>,pad:Pad,sender:std::sync::mpsc::Sender<(Snapshot,Duration)>,stop:std::sync::Arc<std::sync::atomic::AtomicBool>) {
    use std::os::fd::AsRawFd;
    let fd = file.as_raw_fd();
    let mut keys = [0u8; 96];
    let mut axes = [[0i32; 6]; 4];
    let (menu_key, x_key, axes_codes) = match pad {
        Pad::Leaf => (316, 307, [0, 1, 16, 17].as_slice()),
        Pad::PocketAis => (102, 307, [0, 1, 3, 4].as_slice()),
    };
    let read_axes = |axes: &mut [[i32; 6]; 4]| {
        axes.iter_mut().zip(axes_codes).all(|(axis, code)| {
            unsafe { libc::ioctl(fd, 0x80184540u64 + code, axis) >= 0 }
        })
    };
    let down = |keys: &[u8; 96], code: usize| keys[code / 8] & (1 << (code % 8)) != 0;
    if unsafe { libc::ioctl(fd, 0x80604518u64, &mut keys) } < 0 || !read_axes(&mut axes) { return; }
    let read_volume = || {
        let Some(device) = &volume else { return (false, false); };
        let mut keys = [0u8; 96];
        if unsafe { libc::ioctl(device.as_raw_fd(), 0x80604518u64, &mut keys) } < 0 { return (false, false); }
        (down(&keys, 115), down(&keys, 114))
    };
    let snapshot = |keys: &[u8; 96], axes: &[[i32; 6]; 4]| {
        let pressed = |code| down(keys, code);
        let stick_axes = if matches!(pad, Pad::Leaf) { &axes[..2] } else { &axes[..] };
        let other = (0..768).any(|code| code != menu_key && pressed(code)) || stick_axes.iter().any(|axis| {
            let center = (i64::from(axis[1]) + i64::from(axis[2])) / 2;
            (i64::from(axis[0]) - center).abs() > ((i64::from(axis[2]) - i64::from(axis[1])) * 18 / 200).max(i64::from(axis[4]))
        });
        let (x, y, brightness_up, brightness_down) = match pad {
            Pad::Leaf => (axes[2][0], axes[3][0], false, false),
            Pad::PocketAis => (
                i32::from(pressed(547)) - i32::from(pressed(546)),
                i32::from(pressed(545)) - i32::from(pressed(544)),
                pressed(311), pressed(310),
            ),
        };
        let (volume_up, volume_down) = match pad {
            Pad::Leaf => (pressed(115), pressed(114)),
            Pad::PocketAis => read_volume(),
        };
        Snapshot { menu: pressed(menu_key), a: pressed(305), b: pressed(304), button_x: pressed(x_key),
            x, y, other, volume_up, volume_down, brightness_up, brightness_down }
    };
    let mut previous=snapshot(&keys,&axes);
    if sender.send((previous,Duration::ZERO)).is_err(){return;}
    let started=std::time::Instant::now();
    while !stop.load(std::sync::atomic::Ordering::Relaxed) {
        // Independent of the Flash/render thread: a 40ms Menu tap survives a
        // long VM frame. Query state instead of reading Leaf's grabbed stream.
        std::thread::sleep(Duration::from_millis(4));
        if unsafe{libc::ioctl(fd,0x80604518u64,&mut keys)}<0 {break;}
        if (down(&keys, menu_key) || previous.menu) && !read_axes(&mut axes) { break; }
        let current=snapshot(&keys,&axes);
        if current!=previous {
            if sender.send((current,started.elapsed())).is_err(){return;}
            previous=current;
        }
    }
}
#[cfg(any(not(target_os="linux"),test))]
pub struct Device;
#[cfg(any(not(target_os="linux"),test))]
impl Device {
    pub fn new() -> Self {Self}
    pub fn available(&self) -> bool {false}
    pub fn drain(&mut self) -> Vec<(Snapshot,Duration)> {Vec::new()}
}

#[cfg(test)]
mod tests {
    use super::*;
    fn at(ms:u64)->Duration {Duration::from_millis(ms)}
    #[test]
    fn brightness_chord_consumes_the_menu_tap() {
        for (chord, expected) in [
            (Snapshot { brightness_up: true, ..Snapshot::default() }, Command::BrightnessUp),
            (Snapshot { brightness_down: true, ..Snapshot::default() }, Command::BrightnessDown),
        ] {
            let mut chords = Chords::default();
            chords.update_at(Snapshot::default(), at(0));
            chords.update_at(Snapshot { menu: true, ..Snapshot::default() }, at(10));
            let held = Snapshot { menu: true, ..chord };
            assert_eq!(chords.update_at(held, at(30)), vec![expected]);
            assert!(chords.update_at(held, at(60)).is_empty());
            assert!(chords.update_at(Snapshot::default(), at(100)).is_empty());
        }
    }

    #[test]
    fn tap_after_either_chord_release_order_never_depends_on_game_held_keys() {
        for menu_first in [false,true] {
            let mut c=Chords::default();c.update_at(Snapshot::default(),at(0));
            c.update_at(Snapshot{menu:true,..Default::default()},at(100));
            assert_eq!(c.update_at(Snapshot{menu:true,b:true,..Default::default()},at(120)),vec![Command::Next]);
            let release=if menu_first{Snapshot{b:true,..Default::default()}}else{Snapshot{menu:true,..Default::default()}};
            assert!(c.update_at(release,at(200)).is_empty());assert!(c.update_at(Snapshot::default(),at(210)).is_empty());
            // These queued samples can be drained together after a slow VM frame.
            c.update_at(Snapshot{menu:true,..Default::default()},at(240));
            assert_eq!(c.update_at(Snapshot::default(),at(310)),vec![Command::MenuTap]);
            c.update_at(Snapshot{menu:true,..Default::default()},at(350));
            assert_eq!(c.update_at(Snapshot::default(),at(410)),vec![Command::MenuTap]);
        }
    }
    #[test]
    fn startup_held_menu_long_hold_and_system_chords_never_generate_menu_taps() {
        let mut c=Chords::default();c.update_at(Snapshot{menu:true,..Default::default()},at(0));
        assert!(c.update_at(Snapshot::default(),at(100)).is_empty());
        for blocker in [Snapshot{other:true,..Default::default()},Snapshot{volume_up:true,..Default::default()},Snapshot{x:1,..Default::default()}] {
            c.update_at(Snapshot{menu:true,..Default::default()},at(200));
            c.update_at(Snapshot{menu:true,..blocker},at(220));
            c.update_at(Snapshot{menu:true,..Default::default()},at(230));
            assert!(c.update_at(Snapshot::default(),at(300)).is_empty());
        }
        c.update_at(Snapshot{menu:true,..Default::default()},at(400));
        assert!(c.update_at(Snapshot::default(),at(3400)).is_empty());
    }
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

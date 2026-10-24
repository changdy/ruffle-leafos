//! Read-only physical input observer. Menu taps and chords have their own
//! lifecycle, independent of SDL's virtual controller and game-held keys.
use std::time::Duration;
#[derive(Clone,Copy,Debug,Default)]
pub struct Snapshot { pub menu: bool, pub a: bool, pub b: bool, pub button_x: bool, pub x: i32, pub y: i32, pub other: bool, pub volume_up: bool, pub volume_down: bool }
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub enum Command { Previous, Next, Drag, SwapDirections, MenuTap }
#[derive(Default)]
pub struct Chords { held: [bool;4], armed: bool, menu_press: Option<(Duration,bool)> }
impl Chords {
    #[cfg(test)]
    pub fn update(&mut self, s: Snapshot) -> Vec<Command> {
        self.update_at(s,Duration::ZERO)
    }
    pub fn update_at(&mut self, s: Snapshot, now: Duration) -> Vec<Command> {
        if !s.menu {
            let tap=self.menu_press.take().is_some_and(|(at,alone)|alone && now.saturating_sub(at)<Duration::from_secs(3));
            self.armed = true; self.held = [false;4];
            return if tap {vec![Command::MenuTap]}else{Vec::new()};
        }
        if !self.armed { return Vec::new(); }
        let alone=!s.other&&!s.a&&!s.b&&!s.button_x&&s.x==0&&s.y==0&&!s.volume_up&&!s.volume_down;
        let press=self.menu_press.get_or_insert((now,alone));press.1&=alone;
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
        let physical = std::fs::read_dir("/sys/class/input").into_iter().flatten().filter_map(Result::ok).find_map(|entry| {
            let name = entry.file_name(); let name = name.to_str()?;
            if !name.starts_with("event") { return None; }
            let path = entry.path();
            if !std::fs::canonicalize(&path).ok()?.to_string_lossy().contains("/devices/platform/") {return None;}
            if std::fs::read_to_string(path.join("device/name")).ok()?.trim() != "Loong Gamepad" { return None; }
            // Leaf's roster namespace hides the physical event node to avoid
            // duplicate controller input. A separate read-only evdev client
            // observes events without consuming Leaf's copy or grabbing input.
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
        println!("leaf-shortcuts physical_menu_observer={} access=read-only-events host_view={}",physical.is_some(),physical.as_ref().is_some_and(|(_,host)|*host));
        let (sender,receiver)=std::sync::mpsc::channel();
        let stop=std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
        let available=physical.is_some();let stopped=stop.clone();
        let worker=physical.map(|(file,_)|std::thread::spawn(move||observe(file,sender,stopped)));
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
fn observe(file:std::fs::File,sender:std::sync::mpsc::Sender<(Snapshot,Duration)>,stop:std::sync::Arc<std::sync::atomic::AtomicBool>) {
    use std::os::fd::AsRawFd;
    let fd=file.as_raw_fd();let mut keys=[0u8;96];let mut axes=[[0i32;6];4];
    if unsafe{libc::ioctl(fd,0x80604518u64,&mut keys)}<0 {return;}
    for (i,code) in [0u64,1,16,17].into_iter().enumerate() {if unsafe{libc::ioctl(fd,0x80184540u64+code,&mut axes[i])}<0{return;}}
    let snapshot=|keys:&[u8;96],axes:&[[i32;6];4]| {
        let down=|code:usize| keys[code/8]&(1<<(code%8))!=0;
        let other=(0..768).any(|code|code!=316&&down(code)) || axes[..2].iter().any(|a| {
            let center=(i64::from(a[1])+i64::from(a[2]))/2;
            (i64::from(a[0])-center).abs()>((i64::from(a[2])-i64::from(a[1]))*18/200).max(i64::from(a[4]))
        });
        Snapshot{menu:down(316),a:down(305),b:down(304),button_x:down(307),x:axes[2][0],y:axes[3][0],other,volume_up:down(115),volume_down:down(114)}
    };
    if sender.send((snapshot(&keys,&axes),Duration::ZERO)).is_err(){return;}
    // Change only this observer fd's timestamp clock, never the device grab.
    let monotonic:libc::c_int=libc::CLOCK_MONOTONIC;
    unsafe{libc::ioctl(fd,0x400445a0u64,&monotonic);}
    let mut events:[libc::input_event;64]=unsafe{std::mem::zeroed()};
    while !stop.load(std::sync::atomic::Ordering::Relaxed) {
        let mut poll=libc::pollfd{fd,events:libc::POLLIN,revents:0};
        let ready=unsafe{libc::poll(&mut poll,1,25)};
        if ready<0 {if std::io::Error::last_os_error().kind()==std::io::ErrorKind::Interrupted{continue;}break;}
        if ready==0{continue;}
        if poll.revents&(libc::POLLERR|libc::POLLHUP|libc::POLLNVAL)!=0{break;}
        let count=unsafe{libc::read(fd,events.as_mut_ptr().cast(),std::mem::size_of_val(&events))};
        if count<=0{break;}
        for event in events.iter().take(count as usize/std::mem::size_of::<libc::input_event>()) {
            let changed=match event.type_ {
                1 if event.value!=2 && usize::from(event.code)<768=>{
                    let code=usize::from(event.code);if event.value>0{keys[code/8]|=1<<(code%8);}else{keys[code/8]&=!(1<<(code%8));}true
                },
                3=>{if let Some(i)=[0u16,1,16,17].iter().position(|code|*code==event.code){axes[i][0]=event.value;keys[316/8]&(1<<(316%8))!=0}else{false}},
                // Reconnect/resync rather than inventing a tap after dropped events.
                0 if event.code==3=>return,
                _=>false,
            };
            if changed {
                let at=Duration::from_secs(event.time.tv_sec.max(0) as u64)+Duration::from_micros(event.time.tv_usec.max(0) as u64);
                if sender.send((snapshot(&keys,&axes),at)).is_err(){return;}
            }
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

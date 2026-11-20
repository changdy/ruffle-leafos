//! Observe system volume and brightness after the firmware handles the keys.
use std::time::Duration;

#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub enum Kind { Volume, Brightness }
impl Kind {
    pub fn name(self)->&'static str {match self {Self::Volume=>"volume",Self::Brightness=>"brightness"}}
    fn field(self)->&'static str {match self {Self::Volume=>"\"volume_percent\"",Self::Brightness=>"\"brightness_percent\""}}
}
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub struct Level { pub kind:Kind, pub percent:u8 }

pub fn pocketais_brightness(up: bool) -> Option<Level> {
    let status = std::process::Command::new("/usr/bin/brightness")
        .arg(if up { "up" } else { "down" }).status().ok()?;
    if !status.success() { return None; }
    let sysfs = std::path::Path::new("/sys/class/backlight/backlight");
    let read = |name| std::fs::read_to_string(sysfs.join(name)).ok()?.trim().parse::<u32>().ok();
    let current = read("brightness")?;
    let max = read("max_brightness")?;
    if max == 0 { return None; }
    let percent = (u64::from(current.min(max)) * 100 + u64::from(max) / 2) / u64::from(max);
    Some(Level { kind: Kind::Brightness, percent: percent as u8 })
}

fn parse_level(text:&str,kind:Kind)->Option<Level> {
    let body=text.trim().strip_prefix('{')?.strip_suffix('}')?;
    let mut response_kind=None;let mut level=None;
    for field in body.split(',') {
        let (key,value)=field.split_once(':')?;let value=value.trim();
        if key.trim()=="\"type\"" {if response_kind.replace(value).is_some(){return None;}}
        else if key.trim()==kind.field() {if level.is_some(){return None;}let n=value.parse::<u8>().ok()?;if n>100{return None;}level=Some(n);}
    }
    if response_kind!=Some("\"platform-levels\""){return None;}level.map(|percent|Level{kind,percent})
}

fn parse_sink_volume(text: &str) -> Option<Level> {
    let channels = text.lines().next()?.trim().strip_prefix("Volume:")?;
    let percent = channels.split_whitespace().filter_map(|word| {
        word.strip_suffix('%')?.parse::<u32>().ok()
    }).max()?.min(100) as u8;
    Some(Level { kind: Kind::Volume, percent })
}

#[derive(Default)]
struct Watch { held:bool, kind:Option<Kind>, until:Duration, next:Duration }
impl Watch {
    fn observe(&mut self,up:bool,down:bool,menu:bool,now:Duration) {
        let held=up||down;
        if held {
            let kind=if menu {Kind::Brightness}else{Kind::Volume};
            if self.kind!=Some(kind) {self.next=now;}
            self.kind=Some(kind);
        }
        // Keep observing briefly after release, using the last key's kind even
        // when Menu is released first. Leaf may finish its update asynchronously.
        if held || self.held {self.until=now+Duration::from_millis(350);}
        self.held=held;
    }
    fn due(&mut self,now:Duration)->Option<Kind> {
        if self.held {self.until=now+Duration::from_millis(350);}
        if now>=self.until || now<self.next {return None;}
        self.next=now+Duration::from_millis(100);self.kind
    }
}

pub struct Monitor {
    watch:Watch,
    #[cfg(all(target_os="linux",not(test)))]
    sender:std::sync::mpsc::SyncSender<Kind>,
    #[cfg(all(target_os="linux",not(test)))]
    receiver:std::sync::mpsc::Receiver<Level>,
    #[cfg(all(target_os="linux",not(test)))]
    stop:std::sync::Arc<std::sync::atomic::AtomicBool>,
    #[cfg(all(target_os="linux",not(test)))]
    worker:Option<std::thread::JoinHandle<()>>,
}
impl Monitor {
    pub fn new()->Self {
        #[cfg(all(target_os="linux",not(test)))]
        {
            let pocketais = super::shortcuts::is_pocketais();
            let (sender,requests)=std::sync::mpsc::sync_channel(1);let (results,receiver)=std::sync::mpsc::channel();
            let stop=std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));let stopped=stop.clone();
            let worker=std::thread::spawn(move||while !stopped.load(std::sync::atomic::Ordering::Relaxed) {
                match requests.recv_timeout(Duration::from_millis(25)) {
                    Ok(kind)=>{
                        let level = if pocketais { read_pocketais_volume() } else { read_leaf_level(kind) };
                        if let Some(level)=level {if results.send(level).is_err(){break;}}
                    },
                    Err(std::sync::mpsc::RecvTimeoutError::Disconnected)=>break,
                    _=>{},
                }
            });
            Self {watch:Watch::default(),sender,receiver,stop,worker:Some(worker)}
        }
        #[cfg(any(not(target_os="linux"),test))]
        {Self{watch:Watch::default()}}
    }
    pub fn observe(&mut self,up:bool,down:bool,menu:bool,now:Duration) {self.watch.observe(up,down,menu,now);}
    pub fn poll(&mut self,now:Duration)->Option<Level> {
        #[cfg(all(target_os="linux",not(test)))]
        {
            if let Some(kind)=self.watch.due(now){let _=self.sender.try_send(kind);}
            // A response for the previous kind must not replace the active OSD.
            self.receiver.try_iter().filter(|level|Some(level.kind)==self.watch.kind).last()
        }
        #[cfg(any(not(target_os="linux"),test))]
        {let _=self.watch.due(now);None}
    }
}
#[cfg(all(target_os="linux",not(test)))]
impl Drop for Monitor {
    fn drop(&mut self){self.stop.store(true,std::sync::atomic::Ordering::Relaxed);if let Some(worker)=self.worker.take(){let _=worker.join();}}
}
#[cfg(all(target_os="linux",not(test)))]
fn read_leaf_level(kind:Kind)->Option<Level> {
    use std::{io::{Read,Write},os::unix::net::UnixStream};
    let mut socket=UnixStream::connect("/tmp/jawaka-runtime/jawakad.sock").ok()?;
    socket.set_read_timeout(Some(Duration::from_millis(200))).ok()?;
    socket.set_write_timeout(Some(Duration::from_millis(200))).ok()?;
    let request=br#"{"type":"platform-levels"}"#;
    socket.write_all(&(request.len() as u32).to_be_bytes()).ok()?;socket.write_all(request).ok()?;
    let mut length=[0u8;4];socket.read_exact(&mut length).ok()?;let length=u32::from_be_bytes(length) as usize;
    if length>8192{return None;}
    let mut bytes=vec![0;length];socket.read_exact(&mut bytes).ok()?;
    parse_level(std::str::from_utf8(&bytes).ok()?,kind)
}

#[cfg(all(target_os="linux",not(test)))]
fn read_pocketais_volume() -> Option<Level> {
    // AisOS exposes PipeWire's PulseAudio server on the system runtime socket.
    // Query off the render thread and bound the wait if the service is restarting.
    let runtime = std::env::var_os("XDG_RUNTIME_DIR").unwrap_or_else(|| "/var/run/0-runtime-dir".into());
    let mut server = std::ffi::OsString::from("--server=unix:");
    server.push(std::path::PathBuf::from(runtime).join("pulse/native"));
    let output = std::process::Command::new("/usr/bin/timeout")
        .args(["-k", "1", "1", "/usr/bin/pactl"])
        .arg(server).args(["get-sink-volume", "@DEFAULT_SINK@"]).env("LC_ALL", "C").output().ok()?;
    if !output.status.success() { return None; }
    parse_sink_volume(std::str::from_utf8(&output.stdout).ok()?)
}

#[cfg(test)]
mod tests {
    use super::*;
    fn at(n:u64)->Duration {Duration::from_millis(n)}
    #[test]
    fn aisos_volume_reads_loudest_channel_and_clamps_amplification() {
        assert_eq!(parse_sink_volume("Volume: front-left: 32768 /  50% / -18.06 dB, front-right: 36045 / 55% / -15.58 dB\n  balance 0.05"), Some(Level { kind: Kind::Volume, percent: 55 }));
        assert_eq!(parse_sink_volume("Volume: mono: 0 / 0% / -inf dB"), Some(Level { kind: Kind::Volume, percent: 0 }));
        assert_eq!(parse_sink_volume("Volume: mono: 78643 / 120% / 4.75 dB"), Some(Level { kind: Kind::Volume, percent: 100 }));
        assert_eq!(parse_sink_volume("Connection failed"), None);
    }
    #[test]
    fn only_real_valid_levels_are_displayed() {
        let response=r#"{"type":"platform-levels","brightness_percent":35,"volume_percent":5,"hdmi_revert_seconds":0}"#;
        assert_eq!(parse_level(response,Kind::Volume),Some(Level{kind:Kind::Volume,percent:5}));
        assert_eq!(parse_level(response,Kind::Brightness),Some(Level{kind:Kind::Brightness,percent:35}));
        for kind in [Kind::Volume,Kind::Brightness] {
            for value in ["null","-1","101","5.5"] {
                assert_eq!(parse_level(&format!(r#"{{"type":"platform-levels",{}:{value}}}"#,kind.field()),kind),None);
            }
            assert_eq!(parse_level(&format!(r#"{{"type":"platform-levels",{}:5,{}:8}}"#,kind.field(),kind.field()),kind),None);
            assert_eq!(parse_level(&format!(r#"{{"type":"error",{}:5}}"#,kind.field()),kind),None);
            assert_eq!(parse_level(r#"{"type":"platform-levels"}"#,kind),None);
        }
    }
    #[test]
    fn hardware_keys_select_the_real_level_and_poll_after_release() {
        let mut w=Watch::default();assert_eq!(w.due(at(0)),None);
        w.observe(false,false,true,at(0));assert_eq!(w.due(at(0)),None);
        w.observe(true,false,true,at(20));assert_eq!(w.due(at(20)),Some(Kind::Brightness));
        assert_eq!(w.due(at(21)),None);
        assert_eq!(w.due(at(120)),Some(Kind::Brightness));
        w.observe(false,false,false,at(130));
        assert_eq!(w.due(at(220)),Some(Kind::Brightness));assert_eq!(w.due(at(500)),None);
        w.observe(false,true,false,at(510));assert_eq!(w.due(at(510)),Some(Kind::Volume));
        w.observe(false,false,false,at(520));assert_eq!(w.due(at(610)),Some(Kind::Volume));
        assert_eq!(w.due(at(900)),None);
    }
    #[test]
    fn changing_menu_while_volume_is_held_immediately_switches_the_observation() {
        let mut w=Watch::default();
        w.observe(true,false,false,at(0));assert_eq!(w.due(at(0)),Some(Kind::Volume));
        w.observe(true,false,true,at(10));assert_eq!(w.due(at(10)),Some(Kind::Brightness));
        w.observe(true,false,false,at(20));assert_eq!(w.due(at(20)),Some(Kind::Volume));
    }
}

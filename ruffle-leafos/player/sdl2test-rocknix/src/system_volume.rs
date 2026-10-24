//! Observe Leaf's real system level without applying the hardware key twice.
use std::time::Duration;

fn parse_level(text:&str)->Option<u8> {
    let body=text.trim().strip_prefix('{')?.strip_suffix('}')?;
    let mut kind=None;let mut level=None;
    for field in body.split(',') {
        let (key,value)=field.split_once(':')?;let value=value.trim();
        match key.trim() {
            "\"type\""=>{if kind.replace(value).is_some(){return None;}},
            "\"volume_percent\""=>{if level.is_some(){return None;}let n=value.parse::<u8>().ok()?;if n>100{return None;}level=Some(n);},
            _=>{},
        }
    }
    if kind!=Some("\"platform-levels\""){return None;}level
}

#[derive(Default)]
struct Watch { held:bool, until:Duration, next:Duration }
impl Watch {
    fn observe(&mut self,up:bool,down:bool,menu:bool,now:Duration) {
        let held=(up||down)&&!menu;
        if held || self.held&&!menu {self.until=now+Duration::from_millis(350);}
        self.held=held;
    }
    fn due(&mut self,now:Duration)->bool {
        if self.held {self.until=now+Duration::from_millis(350);}
        if now>=self.until || now<self.next {return false;}
        self.next=now+Duration::from_millis(100);true
    }
}

pub struct Monitor {
    watch:Watch,
    #[cfg(all(target_os="linux",not(test)))]
    sender:std::sync::mpsc::SyncSender<()>,
    #[cfg(all(target_os="linux",not(test)))]
    receiver:std::sync::mpsc::Receiver<u8>,
    #[cfg(all(target_os="linux",not(test)))]
    stop:std::sync::Arc<std::sync::atomic::AtomicBool>,
    #[cfg(all(target_os="linux",not(test)))]
    worker:Option<std::thread::JoinHandle<()>>,
}
impl Monitor {
    pub fn new()->Self {
        #[cfg(all(target_os="linux",not(test)))]
        {
            let (sender,requests)=std::sync::mpsc::sync_channel(1);let (results,receiver)=std::sync::mpsc::channel();
            let stop=std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));let stopped=stop.clone();
            let worker=std::thread::spawn(move||while !stopped.load(std::sync::atomic::Ordering::Relaxed) {
                match requests.recv_timeout(Duration::from_millis(25)) {
                    Ok(())=>{if let Some(level)=read_level(){if results.send(level).is_err(){break;}}},
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
    pub fn poll(&mut self,now:Duration)->Option<u8> {
        #[cfg(all(target_os="linux",not(test)))]
        {
            if self.watch.due(now){let _=self.sender.try_send(());}
            self.receiver.try_iter().last()
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
fn read_level()->Option<u8> {
    use std::{io::{Read,Write},os::unix::net::UnixStream};
    let mut socket=UnixStream::connect("/tmp/jawaka-runtime/jawakad.sock").ok()?;
    socket.set_read_timeout(Some(Duration::from_millis(200))).ok()?;
    socket.set_write_timeout(Some(Duration::from_millis(200))).ok()?;
    let request=br#"{"type":"platform-levels"}"#;
    socket.write_all(&(request.len() as u32).to_be_bytes()).ok()?;socket.write_all(request).ok()?;
    let mut length=[0u8;4];socket.read_exact(&mut length).ok()?;let length=u32::from_be_bytes(length) as usize;
    if length>8192{return None;}
    let mut bytes=vec![0;length];socket.read_exact(&mut bytes).ok()?;
    parse_level(std::str::from_utf8(&bytes).ok()?)
}

#[cfg(test)]
mod tests {
    use super::*;
    fn at(n:u64)->Duration {Duration::from_millis(n)}
    #[test]
    fn only_real_valid_levels_are_displayed() {
        assert_eq!(parse_level(r#"{"type":"platform-levels","brightness_percent":35,"volume_percent":5,"hdmi_revert_seconds":0}"#),Some(5));
        for bad in [r#"{"type":"error","volume_percent":5}"#,r#"{"type":"platform-levels","volume_percent":null}"#,r#"{"type":"platform-levels","volume_percent":101}"#,r#"{"type":"platform-levels","volume_percent":5,"volume_percent":8}"#] {assert_eq!(parse_level(bad),None);}
    }
    #[test]
    fn hardware_volume_is_polled_without_reapplying_and_brightness_chords_are_ignored() {
        let mut w=Watch::default();w.observe(true,false,true,at(0));assert!(!w.due(at(0)));
        w.observe(false,false,false,at(10));assert!(!w.due(at(10)));
        w.observe(true,false,false,at(20));assert!(w.due(at(20)));assert!(!w.due(at(21)));
        assert!(w.due(at(120)));w.observe(false,false,false,at(130));
        assert!(w.due(at(220)));assert!(!w.due(at(500)));
    }
}

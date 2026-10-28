//! Opt-in device regression against scripts/make_keyboard_fixtures.py movies.
use super::*;

#[derive(Default)]
pub(super) struct Test { pub phase: usize, next: Option<Instant>, frozen_ticks: u64 }

fn check(ok:bool,message:&str)->Result<(),String> {if ok {Ok(())}else{Err(format!("Keyboard self-test: {message}"))}}
fn tap(front:&mut Frontend,button:Button) {
    let source=Source::Controller(button as u8);
    front.button(button,source,true);front.button(button,source,false);
}
fn click(front:&mut Frontend,y:f32) {
    front.move_pointer((890.,y));
    let source=Source::Mouse(sdl2::mouse::MouseButton::Left as u8);
    front.state.mark_down(source);front.apply_binding(source,Binding::LeftClick,Rate::Off,false);
    front.sync_virtual_keyboard();let released=front.state.release(source);front.emit(released,false);
    front.state.refresh_neutral();
}
fn text(front:&Frontend)->String {front.focused_text().map(|t|t.text).unwrap_or_default()}
fn draft(front:&mut Frontend)->Result<&mut virtual_keyboard::Editor,String> {front.state.virtual_keyboard.as_mut().ok_or("Keyboard self-test: expected overlay".into())}

impl Test {
    pub fn step(&mut self,front:&mut Frontend)->Result<(),String> {
        if front.started.elapsed()<Duration::from_secs(1) || self.next.is_some_and(|at|Instant::now()<at) || self.phase>=12 {return Ok(());}
        match self.phase {
            0=>{
                if front.state.virtual_keyboard.is_none() {click(front,140.);}
                check(draft(front)?.text=="seed","initial field text")?;
                // Script focus may start at 0 or select all, whereas a mouse
                // click near the right edge starts at the end. Normalize only
                // this diagnostic draft before checking append behavior.
                draft(front)?.input(KeyboardInput::End);self.frozen_ticks=front.ticks;
            },
            1=>{
                check(front.ticks==self.frozen_ticks&&front.state.ledger.release_all().is_empty(),"VM advanced or game input held during keyboard")?;
                for button in [Button::A,Button::Back,Button::A,Button::B,Button::LeftShoulder,Button::X] {tap(front,button);}
                check(draft(front)?.text=="seed q","layout/case/delete/cursor/space")?;
                tap(front,Button::Guide);check(draft(front)?.help,"Menu help")?;tap(front,Button::Guide);
            },
            2=>{
                let source=Source::Controller(Button::Start as u8);front.button(Button::Start,source,true);
                check(front.state.virtual_keyboard.is_none()&&!front.can_game_input(),"confirm leaked held Start")?;
                front.button(Button::Start,source,false);check(text(front)=="seed q","confirmed text")?;
            },
            3=>{click(front,140.);draft(front)?.insert("discard");tap(front,Button::Y);check(text(front)=="seed q","cancel changed field or same-field reopen failed")?;},
            4=>{click(front,240.);let e=draft(front)?;check(e.password&&!e.preview().contains("secret"),"password exposed")?;e.insert("中😀");},
            5=>{tap(front,Button::Start);check(text(front)=="secret中😀","Unicode commit")?;},
            6=>{click(front,340.);draft(front)?.insert("a345");tap(front,Button::Start);check(text(front)=="1234","restrict/maxChars not applied")?;},
            7=>{click(front,440.);check(draft(front)?.multiline,"multiline flag")?;tap(front,Button::Paddle2);draft(front)?.insert("中😀");tap(front,Button::Start);check(text(front).replace('\r',"\n")=="line\n中😀","multiline commit")?;},
            8=>{click(front,540.);check(front.state.virtual_keyboard.is_none(),"read-only field opened keyboard")?;},
            9=>{click(front,140.);while !draft(front)?.text.is_empty() {tap(front,Button::B);}tap(front,Button::Start);check(text(front).is_empty(),"delete all failed")?;},
            10=>{
                click(front,640.);
                if front.state.virtual_keyboard.is_some() {
                    draft(front)?.insert("q");tap(front,Button::Start);check(text(front)=="keep","AVM2 preventDefault ignored")?;
                    println!("leaf-keyboard-test avm2_prevent_default=passed");
                }
            },
            11=>{
                click(front,140.);draft(front)?.insert("discard");front.finish_keyboard(false);front.open_menu();
                front.active.player.lock().unwrap().handle_event(PlayerEvent::FocusLost);
                check(front.state.virtual_keyboard.is_none()&&front.state.paused,"focus-loss cancellation")?;
                let result=front.state.resume();front.menu_result(result);front.state.refresh_neutral();
                click(front,140.);check(draft(front)?.text.is_empty(),"focus loss changed field")?;
                front.finish_keyboard(false);let released=front.state.disconnected();front.emit(released,false);
                check(front.state.virtual_keyboard.is_none()&&front.state.paused,"disconnect cancellation")?;
                println!("leaf-keyboard-test passed confirm cancel unicode password restrict max_chars multiline readonly empty help neutral focus_loss disconnect");
                front.quit=true;
            },
            _=>{},
        }
        println!("leaf-keyboard-test phase={} passed ticks={}",self.phase,front.ticks);
        self.phase+=1;self.next=Some(Instant::now()+Duration::from_millis(650));front.redraw=true;
        Ok(())
    }
}

//! Execute small SWFs with real VM/event dispatch and null platform backends.
use super::*;
use crate::backend::log::LogBackend;
use std::cell::RefCell;

#[derive(Clone, Default)]
pub(super) struct TraceLog(Rc<RefCell<Vec<String>>>);

impl LogBackend for TraceLog {
    fn avm_trace(&self, message: &str) {
        self.0.borrow_mut().push(message.to_owned());
    }

    fn avm_warning(&self, message: &str) {
        self.0.borrow_mut().push(format!("WARNING: {message}"));
    }
}

impl TraceLog {
    pub(super) fn take(&self) -> Vec<String> {
        std::mem::take(&mut *self.0.borrow_mut())
    }
}

pub(super) fn player_for_swf(data: &[u8]) -> (Arc<Mutex<Player>>, TraceLog) {
    let movie = SwfMovie::from_data(
        data,
        "file:///compatibility-test.swf".to_owned(),
        None,
        None,
    ).unwrap();
    let trace = TraceLog::default();
    let player = PlayerBuilder::new()
        .with_movie(movie)
        .with_log(trace.clone())
        .with_viewport_dimensions(960, 720, 1.0)
        .with_autoplay(true)
        .build();
    (player, trace)
}

pub(super) fn mouse_probe() -> (Arc<Mutex<Player>>, TraceLog) {
    let (player, trace) = player_for_swf(include_bytes!("tests/mouse-events.swf"));
    {
        let mut player = player.lock().unwrap();
        player.preload(&mut ExecutionLimit::none());
        player.run_frame();
        player.run_frame();
    }
    assert!(trace.take().iter().any(|line| line == "MOUSE_PROBE_READY"), "fixture did not initialize");
    (player, trace)
}

pub(super) fn move_to(player: &mut Player, x: f64, y: f64) {
    player.handle_event(PlayerEvent::MouseMove { x, y });
}

pub(super) fn press(player: &mut Player, x: f64, y: f64) {
    player.handle_event(PlayerEvent::MouseDown {
        x, y, button: MouseButton::Left, index: None,
    });
}

pub(super) fn release(player: &mut Player, x: f64, y: f64) {
    player.handle_event(PlayerEvent::MouseUp {
        x, y, button: MouseButton::Left,
    });
}

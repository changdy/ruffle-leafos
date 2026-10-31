/// SDL2 + OpenGL ES (Glow) Ruffle player for ROCKNIX / Wayland compositors
extern crate sdl2;

use clap::Parser;

use std::rc::Rc;
use std::{
    fs::File,
    io::{Error, Read},
    path::{Path, PathBuf},
    sync::{mpsc, mpsc::Sender, Arc, Mutex},
    thread::sleep,
    time::{Duration, Instant},
};

use sdl2::controller::{Axis, Button};
use sdl2::event::{Event, WindowEvent};
use sdl2::video::GLProfile;

use ruffle_common::duration::FloatDuration;
use ruffle_core::{
    backend::navigator::OwnedFuture,
    events::{LogicalKey, MouseButton as RMouseBtn, PlayerEvent},
    tag_utils::SwfMovie,
    Player, PlayerBuilder, ViewportDimensions,
};
use ruffle_render::backend::RenderBackend;
use url::Url;

use ruffle_core::backend::audio::NullAudioBackend;
use ruffle_core::font::DefaultFont;
use ruffle_frontend_utils::backends::audio::CpalAudioBackend;
use ruffle_frontend_utils::content::PlayingContent;
use ruffle_frontend_utils::{
    backends::navigator::{ExternalNavigatorBackend, FutureSpawner},
    content::ContentDescriptor,
};
use ruffle_video_software::backend::SoftwareVideoBackend;

mod custom_event;
mod gl_backend;
#[cfg(target_os = "linux")]
mod vulkan_backend;
mod keycodes;
mod navigator;
mod trace;
mod ui_backend;

use custom_event::RuffleEvent;
use gl_backend::GlRenderBackend;
use navigator::LinuxNavigatorInterface;
use trace::FileLogBackend;
use ui_backend::SdlUiBackend;

#[derive(Copy, Clone, Eq, PartialEq)]
struct PlayerId(i64);

impl PlayerId {
    fn new() -> Self {
        use std::sync::atomic::{AtomicI64, Ordering};

        static NEXT: AtomicI64 = AtomicI64::new(0);
        let id = NEXT.fetch_add(1, Ordering::Relaxed);
        assert!(id >= 0, "PlayerId overflowed!");
        Self(id)
    }
}

pub struct PlayerRunnable(async_task::Runnable<PlayerId>);

struct ActivePlayer {
    id: PlayerId,
    player: Arc<Mutex<Player>>,
}

#[derive(Clone)]
pub struct EventSender {
    sender: Sender<RuffleEvent>,
}

impl EventSender {
    pub fn send(&self, event: RuffleEvent) {
        let _ = self.sender.send(event);
    }
}

struct AndroidExecutor {
    event_loop: EventSender,
    player_id: PlayerId,
}

impl<E: std::error::Error + 'static> FutureSpawner<E> for AndroidExecutor {
    fn spawn(&self, future: OwnedFuture<(), E>) {
        let future = async {
            if let Err(e) = future.await {
                log::error!("Async error: {}", e);
            }
        };

        let event_loop = self.event_loop.clone();
        let scheduler = move |task| {
            let event = RuffleEvent::TaskPoll(PlayerRunnable(task));
            event_loop.send(event)
        };

        let (runnable, task) = async_task::Builder::new()
            .metadata(self.player_id)
            .spawn_local(|_| future, scheduler);

        task.detach();
        runnable.schedule();
    }
}

fn read_file_to_vec<P: AsRef<Path>>(path: P) -> Result<Vec<u8>, Error> {
    let mut file = File::open(path)?;
    let mut contents = Vec::new();
    file.read_to_end(&mut contents)?;
    Ok(contents)
}

fn default_display_size(video: &sdl2::VideoSubsystem) -> (u32, u32) {
    match video.desktop_display_mode(0) {
        Ok(mode) => {
            let w = mode.w.max(1) as u32;
            let h = mode.h.max(1) as u32;
            log::info!("Using desktop display mode: {w}x{h}");
            (w, h)
        }
        Err(e) => {
            log::warn!("Failed to query desktop display mode: {e}, falling back to 800x600");
            (800, 600)
        }
    }
}

fn initial_window_size(
    video: &sdl2::VideoSubsystem,
    width: Option<u32>,
    height: Option<u32>,
) -> (u32, u32) {
    match (width, height) {
        (Some(w), Some(h)) => (w.max(1), h.max(1)),
        (None, None) => default_display_size(video),
        (Some(w), None) => (w.max(1), (w * 3 / 4).max(1)),
        (None, Some(h)) => ((h * 4 / 3).max(1), h.max(1)),
    }
}

fn window_drawable_size(window: &sdl2::video::Window) -> (u32, u32) {
    let (w, h) = window.drawable_size();
    (w.max(1), h.max(1))
}

fn apply_viewport(playerbox: &Option<ActivePlayer>, width: u32, height: u32) {
    if let Some(player) = playerbox.as_ref() {
        player.player.lock().unwrap().set_viewport_dimensions(ViewportDimensions {
            width,
            height,
            scale_factor: 1.0,
        });
    }
}

/// Visible SWF area after ShowAll letterboxing (matches Ruffle stage math).
#[derive(Clone, Copy, Debug)]
struct ContentBounds {
    min_x: f32,
    min_y: f32,
    max_x: f32,
    max_y: f32,
}

impl ContentBounds {
    fn for_show_all(viewport_w: u32, viewport_h: u32, movie_w: u32, movie_h: u32) -> Self {
        let vw = viewport_w.max(1) as f64;
        let vh = viewport_h.max(1) as f64;
        let mw = movie_w.max(1) as f64;
        let mh = movie_h.max(1) as f64;

        let scale = if (vw / vh) > (mw / mh) {
            vh / mh
        } else {
            vw / mw
        };

        let display_w = mw * scale;
        let display_h = mh * scale;
        let tx = (vw - display_w) / 2.0;
        let ty = (vh - display_h) / 2.0;

        Self {
            min_x: tx as f32,
            min_y: ty as f32,
            max_x: (tx + display_w) as f32,
            max_y: (ty + display_h) as f32,
        }
    }

    fn center(self) -> (f32, f32) {
        ((self.min_x + self.max_x) / 2.0, (self.min_y + self.max_y) / 2.0)
    }

    fn clamp(self, x: f32, y: f32, margin: f32) -> (f32, f32) {
        let min_x = self.min_x + margin;
        let min_y = self.min_y + margin;
        let max_x = (self.max_x - margin).max(min_x);
        let max_y = (self.max_y - margin).max(min_y);
        (x.clamp(min_x, max_x), y.clamp(min_y, max_y))
    }
}

#[derive(Parser, Debug)]
#[clap(name = "sdl2test-rocknix", author, version)]
struct Opt {
    #[clap(name = "swf")]
    swf: PathBuf,

    #[clap(short = 'W', long = "width")]
    width: Option<u32>,

    #[clap(short = 'H', long = "height")]
    height: Option<u32>,

    #[clap(long = "no-audio", action)]
    no_audio: bool,

    #[clap(long = "fullscreen", action)]
    fullscreen: bool,

    #[clap(long)]
    storage_dir: Option<PathBuf>,
    #[arg(long)]
    controls_dir: Option<PathBuf>,
    #[arg(long)]
    app_settings: bool,
    #[arg(long, value_parser=clap::value_parser!(u8).range(1..=5))]
    progress_slot: Option<u8>,
    #[arg(long)]
    self_test_experience: bool,
    /// Run isolated keyboard fixture assertions; never use with a user game.
    #[arg(long)]
    self_test_keyboard: bool,
    #[arg(long)]
    trace_input: bool,
    /// Log per-second CPU wall times and static artwork rebuild counters.
    #[arg(long)]
    trace_performance: bool,
    #[arg(long, aliases=["self-test-controls","self-test-cursor","self-test-menu","self-test-profiles"])]
    self_test_slots: bool,
    #[clap(long)]
    capture_dir: Option<PathBuf>,
    #[clap(long)]
    diagnostic_seconds: Option<u64>,
    #[clap(long, value_parser = ["hybrid", "keyboard", "mouse"])]
    input_mode: Option<String>,
    #[arg(long, value_parser = ["opengl-es", "vulkan"])]
    renderer: Option<String>,
}

mod session;
mod pause_menu;
mod screenshot_png;
mod leaf;
mod performance;

#[tokio::main]
async fn main() -> Result<(), String> { leaf::run().await }

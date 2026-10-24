/// SDL2 + OpenGL ES (Glow) Ruffle player
extern crate sdl2;

use clap::Parser;

use std::rc::Rc;
use std::{
    sync::{mpsc, mpsc::Sender, Arc, Mutex},
    thread::sleep,
    time::Duration,
    time::Instant,
};

use sdl2::controller::{Button, Axis};
use sdl2::event::{Event, WindowEvent};
use sdl2::keyboard::Keycode;
use sdl2::mouse::MouseButton;
use sdl2::video::GLProfile;

use ruffle_render::backend::RenderBackend;
mod custom_event;
mod gl_backend;
mod keycodes;
mod navigator;
mod trace;
mod ui_backend;
use custom_event::RuffleEvent;
use gl_backend::GlRenderBackend;

use ruffle_common::duration::FloatDuration;
use ruffle_core::{
    backend::navigator::OwnedFuture,
    events::{LogicalKey, MouseButton as RMouseBtn, PlayerEvent},
    tag_utils::SwfMovie,
    Player, PlayerBuilder, ViewportDimensions,
};
use url::Url;

use ruffle_frontend_utils::backends::audio::CpalAudioBackend;
use ruffle_frontend_utils::backends::storage::DiskStorageBackend;
use ruffle_frontend_utils::content::PlayingContent;
use ruffle_frontend_utils::{
    backends::navigator::{ExternalNavigatorBackend, FutureSpawner},
    content::ContentDescriptor,
};
use ruffle_video_external::backend::ExternalVideoBackend;

use crate::navigator::LinuxNavigatorInterface;
use crate::trace::FileLogBackend;
use crate::ui_backend::SdlUiBackend;
use ruffle_core::backend::audio::NullAudioBackend;
use ruffle_core::font::DefaultFont;

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

/// A `Player`-bound future that is currently running.
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
        if self.sender.send(event).is_ok() {
            //println!("sender");
        }
    }
}

// impl PollRequester for EventSender {
//     fn request_poll(&self) {
//         self.send(RuffleEvent::TaskPoll);
// 		//println!("TaskPoll");
//     }
// }

/// A bare-bones executor that schedules tasks on the winit event loop.
struct AndroidExecutor {
    event_loop: EventSender,
    player_id: PlayerId,
}

impl<E: std::error::Error + 'static> FutureSpawner<E> for AndroidExecutor {
    fn spawn(&self, future: OwnedFuture<(), E>) {
        // Discard any errors.
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

        // The future should run in the background.
        task.detach();
        // Immediately schedule the future to be polled for the first time.
        runnable.schedule();
    }
}

use std::fs::File;
use std::io::{Error, Read};
use std::path::Path;
use std::path::PathBuf;

fn read_file_to_vec<P: AsRef<Path>>(path: P) -> Result<Vec<u8>, Error> {
    let mut file = File::open(path)?;
    let mut contents = Vec::new();
    file.read_to_end(&mut contents)?;
    Ok(contents)
}

#[derive(Parser, Debug)]
#[clap(name = "Offscreen", author, version)]
struct Opt {
    // The swf file
    #[clap(name = "swf")]
    swf: PathBuf,

    #[clap(short = 'W', long = "width", default_value = "640")]
    width: u32,

    #[clap(short = 'H', long = "height", default_value = "480")]
    height: u32,

    #[clap(long = "no-audio", action)]
    no_audio: bool,
}

//const TARGET_FPS: u64 = 60; // 目标帧率
const FRAME_TIME: Duration = Duration::from_millis(16);
const TRIGGER_PRESS_THRESHOLD:i16 = 0x7000;  // 按下阈值
const TRIGGER_RELEASE_THRESHOLD:i16 = 0x5000;  // 释放阈值（必须 <= PRESS）

#[tokio::main]
async fn main() -> Result<(), String> {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();

    let mut count = 0;

    let opt: Opt = Opt::parse();
    let swf_path = opt.swf.canonicalize().unwrap_or_else(|_| opt.swf.clone());
    let path = swf_path.to_str().unwrap();

    let swf_name = opt
        .swf
        .file_stem()
        .and_then(|s| s.to_str())
        .unwrap_or("unknown");
    let keymap_path = format!("keymap/{}.cfg", swf_name);
    let keymap = keycodes::KeyMap::load(&keymap_path).unwrap_or_else(|| {
        log::error!("No custom keymap found at {}, using defaults", keymap_path);
        keycodes::KeyMap::empty()
    });

    let mut last_frame_time = Instant::now();
    let mut last_time = Instant::now();
    let mut next_frame_time = Some(Instant::now());

    let sdl_context = sdl2::init()?;
    let video_subsystem = sdl_context.video().unwrap();
    let game_controller_subsystem = sdl_context.game_controller().unwrap();

    let (width, height) = (opt.width as u32, opt.height as u32);

    let gl_attr = video_subsystem.gl_attr();
    gl_attr.set_context_profile(GLProfile::Core);
    gl_attr.set_context_version(4, 1);

    let window = video_subsystem
        .window("Ruffle SDL2 Player", width, height)
        .opengl()
        .build()
        .map_err(|e| e.to_string())?;

    log::info!("gl_create_context 1");

    // 设置 framebuffer 属性（非常重要！）
    gl_attr.set_double_buffer(true);
    gl_attr.set_depth_size(24);
    gl_attr.set_stencil_size(8);
    gl_attr.set_red_size(8);
    gl_attr.set_green_size(8);
    gl_attr.set_blue_size(8);
    gl_attr.set_alpha_size(8);

    let _gl_context = window.gl_create_context().map_err(|e| e.to_string())?;
    log::info!("gl_create_context 2");
    window
        .gl_make_current(&_gl_context)
        .map_err(|e| e.to_string())?;

    let gl = unsafe {
        glow::Context::from_loader_function(|s| video_subsystem.gl_get_proc_address(s) as *const _)
    };

    let (sender, receiver) = mpsc::channel::<RuffleEvent>();
    let mut playerbox: Option<ActivePlayer> = None;
    let sender = EventSender { sender };

    let viewport_scale_factor: f64 = 1.0;
    let dimensions = ViewportDimensions {
        width: width as u32,
        height: height as u32,
        scale_factor: viewport_scale_factor,
    };

    if playerbox.is_none() {
        let renderer = unsafe { GlRenderBackend::new(gl, width, height).unwrap() };

        println!("render {}", renderer.debug_info().to_string());
        let movie_url = Url::from_file_path(&swf_path).unwrap();
        let player_id = PlayerId::new();

        let future_spawner = AndroidExecutor {
            event_loop: sender.clone(),
            player_id,
        };

        let navigator = ExternalNavigatorBackend::new(
            movie_url.clone(),
            None,
            None,
            future_spawner,
            None,
            true,
            Default::default(),
            ruffle_core::backend::navigator::SocketMode::Deny,
            Rc::new(PlayingContent::DirectFile(ContentDescriptor::new_remote(
                movie_url,
            ))),
            LinuxNavigatorInterface,
        );

        let ui_backend = SdlUiBackend::new();
        let cjk_font_names = ui_backend.cjk_font_names();

        let mut builder = PlayerBuilder::new().with_ui(ui_backend);

        if opt.no_audio {
            log::warn!("Audio disabled by --no-audio; using NullAudioBackend");
            builder = builder.with_audio(NullAudioBackend::new());
        } else {
            match CpalAudioBackend::new(Some("default")) {
                Ok(audio) => {
                    builder = builder.with_audio(audio);
                }
                Err(e) => {
                    log::error!("Unable to create audio device: {}", e);
                    log::warn!("Falling back to NullAudioBackend");
                    builder = builder.with_audio(NullAudioBackend::new());
                }
            };
        }

        playerbox = Some(ActivePlayer {
            id: player_id,
            player: builder
                .with_renderer(renderer)
                .with_storage(Box::new(DiskStorageBackend::new(PathBuf::from("./"))))
                .with_navigator(navigator)
                .with_log(FileLogBackend::new(Some(Path::new("flashlog.txt"))))
                .with_video(
                    ruffle_video_software::backend::SoftwareVideoBackend::new(),
                    //ExternalVideoBackend::new(),
                )
                .build(),
        });

        let player = &playerbox.as_ref().unwrap().player;
        let mut player_lock = player.lock().unwrap();

        if !cjk_font_names.is_empty() {
            let mut default_font_names = cjk_font_names;
            default_font_names.push("Noto Sans".to_string());
            player_lock.set_default_font(DefaultFont::Sans, default_font_names.clone());
            player_lock.set_default_font(DefaultFont::Serif, default_font_names.clone());
            player_lock.set_default_font(DefaultFont::Typewriter, default_font_names.clone());
            player_lock.set_default_font(DefaultFont::JapaneseGothic, default_font_names.clone());
            player_lock
                .set_default_font(DefaultFont::JapaneseGothicMono, default_font_names.clone());
            player_lock.set_default_font(DefaultFont::JapaneseMincho, default_font_names);
        }

        let url = Url::from_file_path(&swf_path).unwrap().to_string();

        match read_file_to_vec(&path) {
            Ok(bytes) => {
                // 你可以在这里处理文件内容
                let movie = SwfMovie::from_data(&bytes, url, None).unwrap();
                println!(
                    "Loaded SWF version {}, resolution {}x{}",
                    movie.version(),
                    movie.width(),
                    movie.height(),
                );
                player_lock.mutate_with_update_context(|context| {
                    context.set_root_movie(movie);
                    println!("@ {} FPS", context.frame_rate,);
                });
            }
            Err(e) => {
                eprintln!("Error reading file: {}", e);
                player_lock.fetch_root_movie(url.to_string(), Vec::new(), Box::new(|_| {}))
            }
        }

        player_lock.set_is_playing(true); // Desktop player will auto-play.

        player_lock.set_letterbox(ruffle_core::config::Letterbox::On);

        player_lock.set_viewport_dimensions(dimensions);

        last_frame_time = Instant::now();
        next_frame_time = Some(Instant::now());

        println!("MOVIE STARTED");
    }

    // 尝试打开第一个控制器
    let mut controller = None;
    let mut event_pump = sdl_context.event_pump()?;
    let mut use_mouse = false;
    let mut mouse_x = 100.0;
    let mut mouse_y = 100.0;
    let mut backbtn_pressed = false;
    let mut dpad2wasd = false;
    let mut v_mouse = keycodes::VirtualMouse::new((width / 2) as f32, (height / 2) as f32);
    let mut left_trigger_active = false;
    let mut right_trigger_active =false;

    //let cr = cursor_renderer.unwrap();

    'running: loop {
        let mut needs_redraw = false;

        //if let Some(event) = event_pump.poll_event() {
        for event in event_pump.poll_iter() {
            match event {
                // 控制器设备连接
                Event::ControllerDeviceAdded { which, .. } => {
                    println!("控制器已连接: {}", which);
                    if controller.is_none() {
                        controller = Some(game_controller_subsystem.open(which).unwrap());
                    }
                }

                // 控制器设备移除
                Event::ControllerDeviceRemoved { which, .. } => {
                    println!("控制器已移除: {}", which);
                    if let Some(ref c) = controller {
                        if c.instance_id() == which {
                            controller = None;
                        }
                    }
                }
                Event::Window {
                    window_id: _,
                    win_event: WindowEvent::SizeChanged(width, height),
                    ..
                } => {
                    println!("resize");
                    if let Some(player) = playerbox.as_ref() {
                        player
                            .player
                            .lock()
                            .unwrap()
                            .set_viewport_dimensions(ViewportDimensions {
                                width: width as u32,
                                height: height as u32,
                                scale_factor: 1.0,
                            });
                    }
                }
                Event::Window {
                    window_id: _,
                    win_event: WindowEvent::Close,
                    ..
                } => {
                    println!("close");
                    break 'running;
                }
                Event::Window {
                    window_id: _,
                    win_event: WindowEvent::Moved(_x, _y),
                    ..
                } => {}
                Event::MouseMotion {
                    window_id: _,
                    which: _,
                    mousestate: _,
                    x,
                    y,
                    ..
                } => {
                    let x0: f64 = x.into();
                    let y0: f64 = y.into();
                    let ruffle_event = PlayerEvent::MouseMove { x: x0, y: y0 };
                    if let Some(player) = playerbox.as_ref() {
                        player.player.lock().unwrap().handle_event(ruffle_event);
                    }
                }
                Event::MouseButtonDown {
                    window_id: _,
                    which: _,
                    mouse_btn,
                    clicks: _,
                    x,
                    y,
                    ..
                } => {
                    let x0: f64 = x.into();
                    let y0: f64 = y.into();
                    match mouse_btn {
                        MouseButton::Left => {
                            let ruffle_event = PlayerEvent::MouseDown {
                                //按下左键
                                x: x0,
                                y: y0,
                                button: RMouseBtn::Left, // TODO
                                index: None,             // TODO
                            };
                            if let Some(player) = playerbox.as_ref() {
                                player.player.lock().unwrap().handle_event(ruffle_event);
                                player.player.lock().unwrap().render();
                            };
                            println!("鼠标左键按下: {}, {}", x, y);
                        }
                        MouseButton::Right => {
                            //按下右键
                            if let Some(player) = playerbox.as_ref() {
                                let items = player.player.lock().unwrap().prepare_context_menu();
                                println!("items : {}", items.len());

                                for (i, item) in items.iter().enumerate() {
                                    println!("菜单{}:{}", i, item.caption);

                                    if i != 0 && item.separator_before {
                                        println!("{}:分隔符", i);
                                    }
                                    if item.checked {
                                        //被选中的复选框
                                        //Checkbox::new(&mut true, &item.caption).ui(ui).clicked()//如果被点击，返回true
                                        println!("选中{}", i);
                                    } else {
                                        //let button = Button::new(&item.caption).wrap_mode(egui::TextWrapMode::Extend);

                                        //ui.add_enabled(item.enabled, button).clicked()
                                    };
                                    if true {
                                        //sender.send(RuffleEvent::RunContextMenuCallback(5));
                                        //player.player.lock().unwrap().run_context_menu_callback(5);
                                    }
                                }

                                if player.player.lock().unwrap().is_playing() {
                                    player.player.lock().unwrap().set_is_playing(false);
                                } else {
                                    player.player.lock().unwrap().set_is_playing(true);
                                }
                                //player.player.lock().unwrap().run_context_menu_callback(0);
                            };
                        }
                        _ => {}
                    };
                }
                Event::MouseButtonUp {
                    window_id: _,
                    which: _,
                    mouse_btn,
                    clicks: _,
                    x,
                    y,
                    ..
                } => {
                    let x0: f64 = x.into();
                    let y0: f64 = y.into();
                    match mouse_btn {
                        MouseButton::Left => {
                            let ruffle_event = PlayerEvent::MouseUp {
                                x: x0,
                                y: y0,
                                button: RMouseBtn::Left, // TODO
                            };
                            if let Some(player) = playerbox.as_ref() {
                                player.player.lock().unwrap().handle_event(ruffle_event);
                            };
                        }
                        _ => {}
                    };
                }

                Event::KeyDown { keycode, .. } => {
                    if let Some(player) = playerbox.as_ref() {
                        if let Some(Keycode::Escape) = keycode {
                            println!("quit");
                            break 'running;
                        }

                        if let Some(Keycode::Y) = keycode {
                            use_mouse = !use_mouse;
                        }

                        match keycodes::sdl2_keycode_to_ruffle_key_descriptor(keycode.unwrap()) {
                            Some(key_descriptor) => {
                                // 如果元组成功解构，发送键盘事件
                                // sender.send(RuffleEvent::VirtualKeyEvent {
                                //     down: true,
                                //     key_descriptor,
                                // });
                                let ruffle_event = PlayerEvent::KeyDown {
                                    key: key_descriptor,
                                };
                                player.player.lock().unwrap().handle_event(ruffle_event);
                                if let LogicalKey::Character(c) = key_descriptor.logical_key {
                                    let event = PlayerEvent::TextInput { codepoint: c };
                                    log::info!("faking text input: {:?}", c);
                                    player.player.lock().unwrap().handle_event(event);
                                }

                                needs_redraw = true;
                            }
                            None => {
                                // 如果元组解构失败（即返回的是None），执行这里的代码
                                println!("Failed to convert SDL2 keycode {} to Ruffle keycode and character.", keycode.unwrap());
                            }
                        }
                    }
                }

                Event::KeyUp { keycode, .. } => {
                    if let Some(player) = playerbox.as_ref() {
                        match keycodes::sdl2_keycode_to_ruffle_key_descriptor(keycode.unwrap()) {
                            Some(key_descriptor) => {
                                // 如果元组成功解构，发送键盘事件
                                // sender.send(RuffleEvent::VirtualKeyEvent {
                                //     down: false,
                                //     key_descriptor,
                                // });
                                let ruffle_event = PlayerEvent::KeyUp {
                                    key: key_descriptor,
                                };
                                player.player.lock().unwrap().handle_event(ruffle_event);
                            }
                            None => {
                                // 如果元组解构失败（即返回的是None），执行这里的代码
                                println!("Failed to convert SDL2 keycode {} to Ruffle keycode and character.", keycode.unwrap());
                            }
                        }
                    }
                }

                Event::ControllerButtonDown { button, .. } => {
                    v_mouse.handle_button_down(button);
                    println!("按钮按下: {:?}", button);

                    if button == Button::Back {
                        backbtn_pressed = true;
                    }

                    if use_mouse && button == Button::Y {
                        let ruffle_event = PlayerEvent::MouseDown {
                            //按下鼠标左键
                            x: mouse_x as f64,
                            y: mouse_y as f64,
                            button: RMouseBtn::Left, // TODO
                            index: None,             // TODO
                        };
                        if let Some(player) = playerbox.as_ref() {
                            player.player.lock().unwrap().handle_event(ruffle_event);
                        };
                        println!("按下鼠标 mouse_x: {}, mouse_y:{}", mouse_x, mouse_y);
                    }

                    if !use_mouse {
                        if let Some(player) = playerbox.as_ref() {
                            let kc = keymap.button_to_keycode(button, dpad2wasd);
                            match keycodes::sdl2_keycode_to_ruffle_key_descriptor(kc) {
                                Some(key_descriptor) => {
                                    let ruffle_event = PlayerEvent::KeyDown {
                                        key: key_descriptor,
                                    };
                                    player.player.lock().unwrap().handle_event(ruffle_event);
                                    if let LogicalKey::Character(c) = key_descriptor.logical_key {
                                        let event = PlayerEvent::TextInput { codepoint: c };
                                        log::info!("faking text input: {:?}", c);
                                        player.player.lock().unwrap().handle_event(event);
                                    }

                                    needs_redraw = true;
                                }
                                None => {
                                    println!("Failed to convert SDL2 keycode {} to Ruffle keycode and character.", kc);
                                }
                            }
                        }
                    }

                    match button {
                        Button::Guide => {
                            println!("Guide 键按下退出");
                            break 'running;
                        }
                        Button::X => {
                            if backbtn_pressed {
                                use_mouse = !use_mouse;
                                backbtn_pressed = false;
                            }
                        }
                        Button::Start => {
                            if backbtn_pressed {
                                if !keymap.has_custom_dpad() {
                                    dpad2wasd = !dpad2wasd;
                                }
                                backbtn_pressed = false;
                            }
                        }
                        _ => {}
                    }
                }

                // 控制器按钮释放
                Event::ControllerButtonUp { button, .. } => {
                    v_mouse.handle_button_up(button);
                    println!("按钮释放: {:?}", button);

                    if button == Button::Back {
                        backbtn_pressed = false;
                    }

                    if use_mouse && button == Button::Y {
                        let ruffle_event = PlayerEvent::MouseUp {
                            x: mouse_x as f64,
                            y: mouse_y as f64,
                            button: RMouseBtn::Left, // TODO
                        };
                        if let Some(player) = playerbox.as_ref() {
                            player.player.lock().unwrap().handle_event(ruffle_event);
                        };
                        println!("释放鼠标 mouse_x: {}, mouse_y:{}", mouse_x, mouse_y);
                    }
                    if !use_mouse {
                        if let Some(player) = playerbox.as_ref() {
                            let kc = keymap.button_to_keycode(button, dpad2wasd);
                            match keycodes::sdl2_keycode_to_ruffle_key_descriptor(kc) {
                                Some(key_descriptor) => {
                                    let ruffle_event = PlayerEvent::KeyUp {
                                        key: key_descriptor,
                                    };
                                    player.player.lock().unwrap().handle_event(ruffle_event);
                                }
                                None => {
                                    println!("Failed to convert SDL2 keycode {} to Ruffle keycode and character.", kc);
                                }
                            }
                        }
                    }
                }

                // 控制器轴移动（摇杆）
                Event::ControllerAxisMotion { axis, value, .. } => {
                    //println!("轴移动: {:?} = {}", axis, value);
                    //v_mouse.handle_axis(axis, value);

                    match axis {
                        Axis::TriggerLeft => {
                            if !left_trigger_active && value > TRIGGER_PRESS_THRESHOLD {
                                left_trigger_active = true;
                                //按下扳机
                                if let Some(player) = playerbox.as_ref() {
                                    let kc = keymap.button_to_keycode(Button::Paddle1, dpad2wasd);
                                    match keycodes::sdl2_keycode_to_ruffle_key_descriptor(kc) {
                                        Some(key_descriptor) => {
                                            let ruffle_event = PlayerEvent::KeyDown {
                                                key: key_descriptor,
                                            };
                                            player.player.lock().unwrap().handle_event(ruffle_event);
                                            if let LogicalKey::Character(c) = key_descriptor.logical_key {
                                                let event = PlayerEvent::TextInput { codepoint: c };
                                                log::info!("faking text input: {:?}", c);
                                                player.player.lock().unwrap().handle_event(event);
                                            }

                                            needs_redraw = true;
                                        }
                                        None => {
                                            println!("Failed to convert SDL2 keycode {} to Ruffle keycode and character.", kc);
                                        }
                                    }
                                }
                            }
                            else if left_trigger_active && value < TRIGGER_RELEASE_THRESHOLD {
                                left_trigger_active = false;
                                // 释放扳机
                                if let Some(player) = playerbox.as_ref() {
                                    let kc = keymap.button_to_keycode(Button::Paddle1, dpad2wasd);
                                    match keycodes::sdl2_keycode_to_ruffle_key_descriptor(kc) {
                                        Some(key_descriptor) => {
                                            let ruffle_event = PlayerEvent::KeyUp {
                                                key: key_descriptor,
                                            };
                                            player.player.lock().unwrap().handle_event(ruffle_event);
                                        }
                                        None => {
                                            println!("Failed to convert SDL2 keycode {} to Ruffle keycode and character.", kc);
                                        }
                                    }
                                }
                            }
                        },
                        Axis::TriggerRight => {
                            if !right_trigger_active && value > TRIGGER_PRESS_THRESHOLD {
                                right_trigger_active = true;
                                //按下扳机
                                if let Some(player) = playerbox.as_ref() {
                                    let kc = keymap.button_to_keycode(Button::Paddle2, dpad2wasd);
                                    match keycodes::sdl2_keycode_to_ruffle_key_descriptor(kc) {
                                        Some(key_descriptor) => {
                                            let ruffle_event = PlayerEvent::KeyDown {
                                                key: key_descriptor,
                                            };
                                            player.player.lock().unwrap().handle_event(ruffle_event);
                                            if let LogicalKey::Character(c) = key_descriptor.logical_key {
                                                let event = PlayerEvent::TextInput { codepoint: c };
                                                log::info!("faking text input: {:?}", c);
                                                player.player.lock().unwrap().handle_event(event);
                                            }

                                            needs_redraw = true;
                                        }
                                        None => {
                                            println!("Failed to convert SDL2 keycode {} to Ruffle keycode and character.", kc);
                                        }
                                    }
                                }
                            }
                            else if right_trigger_active && value < TRIGGER_RELEASE_THRESHOLD {
                                right_trigger_active = false;
                                // 释放扳机
                                if let Some(player) = playerbox.as_ref() {
                                    let kc = keymap.button_to_keycode(Button::Paddle2, dpad2wasd);
                                    match keycodes::sdl2_keycode_to_ruffle_key_descriptor(kc) {
                                        Some(key_descriptor) => {
                                            let ruffle_event = PlayerEvent::KeyUp {
                                                key: key_descriptor,
                                            };
                                            player.player.lock().unwrap().handle_event(ruffle_event);
                                        }
                                        None => {
                                            println!("Failed to convert SDL2 keycode {} to Ruffle keycode and character.", kc);
                                        }
                                    }
                                }
                            }
                        },
                        _ => {}
                    }

                }

                Event::Quit { .. } => {
                    println!("quit");
                    break 'running;
                }
                _ => {}
            }
        }

        if use_mouse
        {
            // let (lx, ly) = v_mouse.left_stick_normalized();
            // mouse_x = lx as f32;
            // mouse_y = ly as f32;
            (mouse_x, mouse_y) = v_mouse.update(width as f32, height as f32);
        }

        //处理往sender里发的事件
        // match receiver.recv_timeout(
        //         next_frame_time
        //             .and_then(|next| next.checked_duration_since(last_frame_time))
        //             .unwrap_or_else(|| Duration::from_millis(100),))
        match receiver.try_recv() {
            Err(_) => {}
            Ok(RuffleEvent::TaskPoll(task)) => {
                if let Some(player) = playerbox.as_ref() {
                    if *task.0.metadata() == player.id {
                        task.0.run();
                    }
                }
            }
            Ok(RuffleEvent::VirtualKeyEvent {
                down,
                key_descriptor,
            }) => {
                if let Some(player) = playerbox.as_ref() {
                    let ruffle_event = match down {
                        true => PlayerEvent::KeyDown {
                            key: key_descriptor,
                        },
                        false => PlayerEvent::KeyUp {
                            key: key_descriptor,
                        },
                    };
                    player.player.lock().unwrap().handle_event(ruffle_event);
                    if down {
                        // NOTE: this is a HACK
                        if let LogicalKey::Character(c) = key_descriptor.logical_key {
                            let event = PlayerEvent::TextInput { codepoint: c };
                            log::info!("faking text input: {:?}", c);
                            player.player.lock().unwrap().handle_event(event);
                        }
                    }

                    needs_redraw = true;
                }
            }
            Ok(RuffleEvent::RunContextMenuCallback(index)) => {
                if let Some(player) = playerbox.as_ref() {
                    player
                        .player
                        .lock()
                        .unwrap()
                        .run_context_menu_callback(index);
                }
            }
            Ok(RuffleEvent::ClearContextMenu) => {
                if let Some(player) = playerbox.as_ref() {
                    player.player.lock().unwrap().clear_custom_menu_items();
                }
            }
            Ok(RuffleEvent::RequestContextMenu) => {
                //java获取rust中player的菜单，rust会回调java的函数，把结果传给java
                if let Some(player) = playerbox.as_ref() {
                    println!("preparing context menu!");
                    let _items = player.player.lock().unwrap().prepare_context_menu();
                }
            }
        }

        let new_time = Instant::now();
        let dt = new_time.duration_since(last_frame_time).as_micros();
        if dt > 0 {
            last_frame_time = new_time;
            if let Some(player) = playerbox.as_ref() {
                if let Ok(mut player) = player.player.lock() {
                    player.tick(FloatDuration::from_millis(dt as f64 / 1000.0));
                    //next_frame_time = Some(new_time + player.time_til_next_frame());
                    needs_redraw = player.needs_render();
                    // let audio = player
                    //     .audio_mut()
                    //     .downcast_mut::<CpalAudioBackend>()
                    //     .unwrap();
                    // audio.recreate_stream_if_needed();
                }
            } else {
                next_frame_time = None;
            }
        }

        if needs_redraw {
            if let Some(player) = playerbox.as_ref() {
                if let Ok(mut player) = player.player.lock() {
                    //count += 1;
                    //player.render();
                    //log::info!("render 1");
                    if use_mouse {
                        //println!("render_mouse mouse_x: {}, mouse_y:{}", mouse_x, mouse_y);
                        player.render_mouse(mouse_x, mouse_y);
                    } else {
                        player.render();
                    }

                    //if count == 20 { break 'running; }
                }
            }
            window.gl_swap_window();
        }

        // 获取当前时间
        let now = Instant::now();
        // 计算帧耗时
        let elapsed = now.duration_since(last_time);

        // 如果帧耗时小于目标帧时间，则进行延迟
        if elapsed < FRAME_TIME {
            //println!("等待");
            sleep(FRAME_TIME - elapsed);
        }
        // 更新上一次帧时间
        last_time = Instant::now();
    }

    Ok(())
}

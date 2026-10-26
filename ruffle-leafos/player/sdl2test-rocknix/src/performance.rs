use std::time::{Duration, Instant};
use std::cell::Cell;

thread_local! {
    static ENABLED: Cell<bool> = const { Cell::new(false) };
    static VULKAN: Cell<[Duration; 3]> = const { Cell::new([Duration::ZERO; 3]) };
}

pub fn enable(enabled: bool) {
    ENABLED.set(enabled);
    VULKAN.set([Duration::ZERO; 3]);
    ruffle_core::render_metrics::set_enabled(enabled);
}

pub fn vulkan_start() -> Option<Instant> {
    Performance::start(ENABLED.get())
}

pub fn vulkan_end(stage: usize, start: Option<Instant>) {
    if let Some(start) = start {
        let mut times = VULKAN.get();
        times[stage] += start.elapsed();
        VULKAN.set(times);
    }
}

#[derive(Default)]
pub struct Performance {
    pub tasks: Duration,
    pub tick: Duration,
    pub render: Duration,
    pub collect: Duration,
    pub swap: Duration,
    pub draws: u64,
    pub max_tick: Duration,
    pub max_render: Duration,
    pub max_loop: Duration,
}

impl Performance {
    pub fn start(enabled: bool) -> Option<Instant> {
        enabled.then(Instant::now)
    }

    pub fn elapsed(start: Option<Instant>) -> Duration {
        start.map(|start| start.elapsed()).unwrap_or_default()
    }

    pub fn log(&mut self, elapsed: Duration, frames: u64, target: f64, paused: bool) {
        let metrics = ruffle_core::render_metrics::take();
        let vulkan = VULKAN.replace([Duration::ZERO; 3]);
        println!(concat!("leaf-perf window_ms={} frames={} target_fps={:.2} paused={} draws={} ",
            "tasks_ms={:.3} tick_ms={:.3} render_ms={:.3} collect_ms={:.3} swap_ms={:.3} ",
            "max_tick_ms={:.3} max_render_ms={:.3} max_loop_ms={:.3} ",
            "shapes={} shape_ms={:.3} bitmaps={} bitmap_reloads={} bitmap_rgba_mib={:.3} ",
            "decode_ms={:.3} upload_ms={:.3} evictions={} vk_submit_ms={:.3} vk_poll_ms={:.3} vk_present_ms={:.3}"),
            elapsed.as_millis(), frames, target, paused, self.draws,
            self.tasks.as_secs_f64()*1000., self.tick.as_secs_f64()*1000.,
            self.render.as_secs_f64()*1000., self.collect.as_secs_f64()*1000.,
            self.swap.as_secs_f64()*1000., self.max_tick.as_secs_f64()*1000.,
            self.max_render.as_secs_f64()*1000., self.max_loop.as_secs_f64()*1000.,
            metrics.shapes, metrics.shape_us as f64/1000., metrics.bitmaps,
            metrics.bitmap_reloads, metrics.bitmap_rgba_bytes as f64/1048576.,
            metrics.decode_us as f64/1000., metrics.upload_us as f64/1000., metrics.evictions,
            vulkan[0].as_secs_f64()*1000., vulkan[1].as_secs_f64()*1000., vulkan[2].as_secs_f64()*1000.);
        *self = Self::default();
    }
}

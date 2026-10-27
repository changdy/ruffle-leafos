//! Opt-in, per-thread diagnostics for rebuilding static Flash artwork.
//! Shape time includes bitmap decode/upload time; these values are not additive.
use std::cell::Cell;
use std::time::Instant;

#[derive(Clone, Copy, Default, Debug)]
pub struct RenderMetrics {
    pub shapes: u64,
    pub shape_us: u64,
    pub bitmaps: u64,
    pub bitmap_reloads: u64,
    pub bitmap_rgba_bytes: u64,
    pub decode_us: u64,
    pub upload_us: u64,
    pub evictions: u64,
    pub phases_us: [u64; 8],
}

#[derive(Clone, Copy)]
pub(crate) enum Phase {
    Preload, Enter, Construct, Scripts, Exit, Audio, Mouse, Gc,
}

pub(crate) fn phase(phase: Phase, start: Option<Instant>) {
    if let Some(start) = start {
        let mut metrics = METRICS.get();
        metrics.phases_us[phase as usize] += start.elapsed().as_micros() as u64;
        METRICS.set(metrics);
    }
}

thread_local! {
    static ENABLED: Cell<bool> = const { Cell::new(false) };
    static METRICS: Cell<RenderMetrics> = Cell::new(RenderMetrics::default());
}

pub fn set_enabled(enabled: bool) {
    ENABLED.set(enabled);
    METRICS.set(RenderMetrics::default());
}

pub fn take() -> RenderMetrics {
    METRICS.take()
}

pub(crate) fn start() -> Option<Instant> {
    ENABLED.get().then(Instant::now)
}

pub(crate) fn shape(start: Option<Instant>) {
    if let Some(start) = start {
        let mut metrics = METRICS.get();
        metrics.shapes += 1;
        metrics.shape_us += start.elapsed().as_micros() as u64;
        METRICS.set(metrics);
    }
}

pub(crate) fn bitmap(start: Option<Instant>, upload: Option<Instant>, bytes: u64, reload: bool) {
    if let (Some(start), Some(upload)) = (start, upload) {
        let mut metrics = METRICS.get();
        metrics.bitmaps += 1;
        metrics.bitmap_reloads += u64::from(reload);
        metrics.bitmap_rgba_bytes += bytes;
        metrics.decode_us += upload.duration_since(start).as_micros() as u64;
        metrics.upload_us += upload.elapsed().as_micros() as u64;
        METRICS.set(metrics);
    }
}

pub(crate) fn evicted() {
    if ENABLED.get() {
        let mut metrics = METRICS.get();
        metrics.evictions += 1;
        METRICS.set(metrics);
    }
}

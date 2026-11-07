//! CPU-only diagnostic of the production pixel cache. Setup is outside timing.
//! Allocation counts are requests during maintenance, not RSS or GPU memory.
use ruffle_render::bitmap::{Bitmap, BitmapFormat};
use ruffle_render::bitmap_cache::{BitmapDecoder, DecodeCache};
use ruffle_render::error::Error;
use std::alloc::{GlobalAlloc, Layout, System};
use std::hint::black_box;
use std::sync::{Arc, atomic::{AtomicBool, AtomicUsize, Ordering::Relaxed}};
use std::time::{Duration, Instant};

struct Allocator;
static TRACK: AtomicBool = AtomicBool::new(false);
static ALLOCS: AtomicUsize = AtomicUsize::new(0);
fn count() { if TRACK.load(Relaxed) { ALLOCS.fetch_add(1, Relaxed); } }
unsafe impl GlobalAlloc for Allocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        count(); unsafe { System.alloc(layout) }
    }
    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        count(); unsafe { System.alloc_zeroed(layout) }
    }
    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, size: usize) -> *mut u8 {
        count(); unsafe { System.realloc(ptr, layout, size) }
    }
    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        unsafe { System.dealloc(ptr, layout) }
    }
}
#[global_allocator]
static ALLOCATOR: Allocator = Allocator;

#[derive(Debug)]
struct Decoder;
impl BitmapDecoder for Decoder {
    fn decode(&self) -> Result<Bitmap<'static>, Error> {
        Ok(Bitmap::new(2, 1, BitmapFormat::Rgba, vec![7; 8]))
    }
    fn decode_speculative(&self) -> Result<Bitmap<'static>, Error> { self.decode() }
    fn predecode_bytes(&self) -> Option<usize> { Some(8) }
}

fn main() {
    let rounds: usize = std::env::args().nth(1).map(|s| s.parse().unwrap()).unwrap_or(200);
    assert!(rounds > 0);
    println!("mode,entries,rounds,mean_us,allocation_requests_per_round");
    for entries in [64, 256, 1024] {
        for mode in ["pressure_clear", "unloaded_sources", "live_sources"] {
            let mut total = Duration::ZERO;
            let mut allocations = 0;
            for _ in 0..rounds {
                let sources: Vec<Arc<dyn BitmapDecoder>> = (0..entries)
                    .map(|_| Arc::new(Decoder) as Arc<dyn BitmapDecoder>).collect();
                let mut cache = DecodeCache::new(entries * 8, false);
                for source in &sources { black_box(cache.decode(source).unwrap()); }
                let sources = if mode == "unloaded_sources" { drop(sources); None } else { Some(sources) };
                ALLOCS.store(0, Relaxed);
                TRACK.store(true, Relaxed);
                let start = Instant::now();
                if mode == "pressure_clear" { cache.set_limit(0); } else { cache.maintain(); }
                total += start.elapsed();
                TRACK.store(false, Relaxed);
                allocations += ALLOCS.load(Relaxed);
                let metrics = cache.take_metrics();
                let kept = if mode == "live_sources" { entries } else { 0 };
                assert_eq!(metrics.bytes, kept * 8);
                assert_eq!(metrics.evictions, (entries - kept) as u64);
                black_box(sources);
            }
            println!("{mode},{entries},{rounds},{:.3},{:.3}",
                total.as_secs_f64() * 1e6 / rounds as f64, allocations as f64 / rounds as f64);
        }
    }
}

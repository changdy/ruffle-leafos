//! Bounded CPU pixels for immutable SWF bitmap fills. No renderer or GC state
//! crosses the worker boundary. Frontends opt in for the lifetime of a player.
use crate::bitmap::Bitmap;
use crate::error::Error;
use std::cell::RefCell;
use std::collections::HashMap;
use std::fmt::Debug;
use std::sync::{Arc, Weak, mpsc};
use std::thread::JoinHandle;
use std::time::Instant;

pub trait BitmapDecoder: Debug + Send + Sync {
    fn decode(&self) -> Result<Bitmap<'static>, Error>;
    fn decode_speculative(&self) -> Result<Bitmap<'static>, Error>;
    /// Upper bound for the output of an eligible speculative decode. Returning
    /// None excludes the source from background work, but not demand caching.
    fn predecode_bytes(&self) -> Option<usize>;
}

type Source = Arc<dyn BitmapDecoder>;

#[derive(Clone, Copy, Debug, Default)]
pub struct Metrics {
    pub hits: u64,
    pub misses: u64,
    pub evictions: u64,
    pub queued: u64,
    pub ready_hits: u64,
    pub late: u64,
    pub unused: u64,
    pub failed: u64,
    pub worker_us: u64,
    pub bytes: usize,
    pub reserved: usize,
}

struct Entry {
    source: Weak<dyn BitmapDecoder>,
    pixels: Arc<Bitmap<'static>>,
    used: u64,
    speculative: bool,
}

struct Job {
    source: Source,
    bytes: usize,
}

struct Completed {
    source: Source,
    pixels: Option<Bitmap<'static>>,
    micros: u64,
}

struct Worker {
    jobs: mpsc::SyncSender<Job>,
    completed: mpsc::Receiver<Completed>,
    thread: JoinHandle<()>,
}

pub struct DecodeCache {
    limit: usize,
    entries: HashMap<usize, Entry>,
    // Reservations remain charged even when the main thread has had to decode
    // the same source. This bounds queued, running and uncollected output too.
    pending: HashMap<usize, usize>,
    // A strict speculative decoder can reject an otherwise demand-compatible
    // source. Do not retry that immutable stream on every placement/frame.
    rejected: HashMap<usize, Weak<dyn BitmapDecoder>>,
    worker: Option<Worker>,
    clock: u64,
    metrics: Metrics,
}

fn key(source: &Source) -> usize {
    // Every stored key also has a Weak or Arc, preventing address reuse.
    Arc::as_ptr(source) as *const () as usize
}

impl DecodeCache {
    pub fn new(limit: usize, predecode: bool) -> Self {
        let worker = (limit != 0 && predecode).then(|| {
            let (jobs, receiver) = mpsc::sync_channel::<Job>(3);
            let (sender, completed) = mpsc::channel();
            std::thread::Builder::new().name("ruffle-bitmap".into()).spawn(move || {
                while let Ok(job) = receiver.recv() {
                    let start = Instant::now();
                    let pixels = job.source.decode_speculative().ok()
                        .filter(|pixels| pixels.allocation_size() <= job.bytes);
                    if sender.send(Completed { source: job.source, pixels,
                        micros: start.elapsed().as_micros() as u64 }).is_err() {
                        break;
                    }
                }
            }).ok().map(|thread| Worker { jobs, completed, thread })
        }).flatten();
        Self { limit, entries: HashMap::new(), pending: HashMap::new(), rejected: HashMap::new(), worker,
            clock: 0, metrics: Metrics::default() }
    }

    fn remove(&mut self, id: usize) {
        if let Some(entry) = self.entries.remove(&id) {
            self.metrics.bytes -= entry.pixels.allocation_size();
            self.metrics.evictions += 1;
            self.metrics.unused += u64::from(entry.speculative);
        }
    }

    fn room(&mut self, bytes: usize, evict: bool) -> bool {
        if self.limit == 0 || bytes > self.limit.saturating_sub(self.metrics.reserved) {
            return false;
        }
        while self.entries.len() >= 1024 || self.metrics.bytes + self.metrics.reserved + bytes > self.limit {
            if !evict { return false; }
            let Some(id) = self.entries.iter().min_by_key(|(_, e)| e.used).map(|(id, _)| *id)
                else { return false };
            self.remove(id);
        }
        true
    }

    fn insert(&mut self, source: &Source, pixels: Arc<Bitmap<'static>>, speculative: bool) -> bool {
        let bytes = pixels.allocation_size();
        if self.entries.contains_key(&key(source)) || !self.room(bytes, !speculative) {
            return false;
        }
        self.clock += 1;
        self.entries.insert(key(source), Entry { source: Arc::downgrade(source), pixels,
            used: self.clock, speculative });
        self.metrics.bytes += bytes;
        true
    }

    fn complete(&mut self, done: Completed) {
        if let Some(bytes) = self.pending.remove(&key(&done.source)) {
            self.metrics.reserved -= bytes;
        }
        self.metrics.worker_us += done.micros;
        if let Some(pixels) = done.pixels {
            // The job is the sole owner after its movie unloads.
            if Arc::strong_count(&done.source) == 1
                || !self.insert(&done.source, Arc::new(pixels), true) {
                self.metrics.unused += 1;
            }
        } else {
            self.metrics.failed += 1;
            self.rejected.insert(key(&done.source), Arc::downgrade(&done.source));
        }
    }

    fn drain(&mut self) {
        loop {
            let result = self.worker.as_ref().map(|worker| worker.completed.try_recv());
            match result {
                Some(Ok(done)) => self.complete(done),
                Some(Err(mpsc::TryRecvError::Disconnected)) => {
                    self.pending.clear();
                    self.metrics.reserved = 0;
                    self.worker = None;
                    break;
                }
                _ => break,
            }
        }
    }

    pub fn decode(&mut self, source: &Source) -> Result<Arc<Bitmap<'static>>, Error> {
        self.drain();
        self.clock += 1;
        if let Some(entry) = self.entries.get_mut(&key(source)) {
            entry.used = self.clock;
            self.metrics.hits += 1;
            self.metrics.ready_hits += u64::from(entry.speculative);
            entry.speculative = false;
            return Ok(entry.pixels.clone());
        }
        self.metrics.misses += 1;
        self.metrics.late += u64::from(self.pending.contains_key(&key(source)));
        // Never wait for a speculative job: demand keeps the original decoder
        // and error behavior, and a late worker result cannot replace live pixels.
        let pixels = Arc::new(source.decode()?);
        self.insert(source, pixels.clone(), false);
        Ok(pixels)
    }

    pub fn predecode(&mut self, source: &Source) {
        if self.worker.is_none() { return; }
        self.drain();
        if self.entries.contains_key(&key(source)) || self.pending.contains_key(&key(source))
            || self.rejected.contains_key(&key(source)) || self.pending.len() >= 4
            || self.rejected.len() + self.pending.len() >= 1024 { return; }
        let Some(bytes) = source.predecode_bytes().filter(|bytes| *bytes > 0 && *bytes <= 1024 * 1024)
            else { return };
        // Speculation cannot evict a useful demand entry.
        if !self.room(bytes, false) { return; }
        if self.worker.as_ref().is_some_and(|worker|
            worker.jobs.try_send(Job { source: source.clone(), bytes }).is_ok()) {
            self.pending.insert(key(source), bytes);
            self.metrics.reserved += bytes;
            self.metrics.queued += 1;
        }
    }

    pub fn maintain(&mut self) {
        self.drain();
        let dead: Vec<_> = self.entries.iter().filter(|(_, e)| e.source.strong_count() == 0)
            .map(|(id, _)| *id).collect();
        for id in dead { self.remove(id); }
        self.rejected.retain(|_, source| source.strong_count() != 0);
    }

    /// Shrink retained pixels without waiting for or restarting the worker.
    /// In-flight reservations can temporarily exceed a reduced limit; their
    /// completed pixels must pass the new limit before entering the cache.
    pub fn set_limit(&mut self, bytes: usize) {
        self.limit = bytes;
        self.drain();
        while !self.entries.is_empty()
            && (self.limit == 0 || self.metrics.bytes > self.limit.saturating_sub(self.metrics.reserved)) {
            let Some(id) = self.entries.iter().min_by_key(|(_, e)| e.used).map(|(id, _)| *id)
                else { break };
            self.remove(id);
        }
    }

    pub fn take_metrics(&mut self) -> Metrics {
        self.drain();
        let metrics = self.metrics;
        self.metrics = Metrics { bytes: metrics.bytes, reserved: metrics.reserved, ..Metrics::default() };
        metrics
    }
}

impl Drop for DecodeCache {
    fn drop(&mut self) {
        if let Some(worker) = self.worker.take() {
            // Disconnect results first: the worker abandons queued work after
            // its current bounded task. The player's shutdown joins that task.
            drop(worker.completed);
            drop(worker.jobs);
            let _ = worker.thread.join();
        }
    }
}

thread_local! {
    static CACHE: RefCell<Option<DecodeCache>> = const { RefCell::new(None) };
}

/// Keep this guard inside the player run scope, so no pixels/jobs survive a
/// player restart. Other frontends remain on their original decoding path.
pub struct CacheGuard;

pub fn configure(bytes: usize, predecode: bool) -> CacheGuard {
    CACHE.with_borrow_mut(|cache| *cache = (bytes != 0).then(|| DecodeCache::new(bytes, predecode)));
    CacheGuard
}

impl Drop for CacheGuard {
    fn drop(&mut self) { CACHE.with_borrow_mut(|cache| *cache = None); }
}

pub fn decode(source: &Source) -> Result<Arc<Bitmap<'static>>, Error> {
    CACHE.with_borrow_mut(|cache| match cache {
        Some(cache) => cache.decode(source),
        None => source.decode().map(Arc::new),
    })
}

pub fn predecode(source: &Source) {
    CACHE.with_borrow_mut(|cache| { if let Some(cache) = cache { cache.predecode(source); } });
}

pub fn predecode_enabled() -> bool {
    CACHE.with_borrow(|cache| cache.as_ref().is_some_and(|cache| cache.limit != 0 && cache.worker.is_some()))
}

pub fn maintain() {
    CACHE.with_borrow_mut(|cache| { if let Some(cache) = cache { cache.maintain(); } });
}

pub fn set_limit(bytes: usize) {
    CACHE.with_borrow_mut(|cache| { if let Some(cache) = cache { cache.set_limit(bytes); } });
}

pub fn take_metrics() -> Metrics {
    CACHE.with_borrow_mut(|cache| cache.as_mut().map(DecodeCache::take_metrics).unwrap_or_default())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::bitmap::BitmapFormat;
    use std::sync::{Mutex, atomic::{AtomicUsize, Ordering}};
    use std::time::Duration;

    #[derive(Debug)]
    struct Decoder { calls: AtomicUsize, color: u8 }
    impl BitmapDecoder for Decoder {
        fn decode(&self) -> Result<Bitmap<'static>, Error> {
            self.calls.fetch_add(1, Ordering::Relaxed);
            Ok(Bitmap::new(2, 1, BitmapFormat::Rgba, vec![self.color; 8]))
        }
        fn decode_speculative(&self) -> Result<Bitmap<'static>, Error> { self.decode() }
        fn predecode_bytes(&self) -> Option<usize> { Some(8) }
    }
    fn source(color: u8) -> Source { Arc::new(Decoder { calls: AtomicUsize::new(0), color }) }

    #[test]
    fn pixels_are_lru_bounded_and_sources_are_weak() {
        let mut cache = DecodeCache::new(16, false);
        let a = source(1); let b = source(2); let c = source(3);
        let first = cache.decode(&a).unwrap();
        cache.decode(&b).unwrap();
        assert!(Arc::ptr_eq(&first, &cache.decode(&a).unwrap()));
        cache.decode(&c).unwrap();
        assert!(cache.entries.contains_key(&key(&a)));
        assert!(!cache.entries.contains_key(&key(&b)));
        assert_eq!(cache.metrics.bytes, 16);
        drop(a);
        cache.maintain();
        assert_eq!(cache.metrics.bytes, 8);
        assert_eq!(first.data(), &[1; 8], "an upload may finish after eviction");
        let mut tiny = DecodeCache::new(7, false);
        assert_eq!(tiny.decode(&b).unwrap().data(), &[2; 8]);
        assert!(tiny.entries.is_empty(), "oversize demand still decodes without caching");
    }

    #[derive(Debug)]
    struct Gated {
        entered: mpsc::Sender<()>,
        release: Mutex<mpsc::Receiver<()>>,
    }
    impl BitmapDecoder for Gated {
        fn decode(&self) -> Result<Bitmap<'static>, Error> {
            Ok(Bitmap::new(2, 1, BitmapFormat::Rgba, vec![7; 8]))
        }
        fn decode_speculative(&self) -> Result<Bitmap<'static>, Error> {
            self.entered.send(()).unwrap();
            self.release.lock().unwrap().recv_timeout(Duration::from_secs(5)).unwrap();
            self.decode()
        }
        fn predecode_bytes(&self) -> Option<usize> { Some(8) }
    }
    fn gated() -> (Source, mpsc::Receiver<()>, mpsc::Sender<()>) {
        let (entered, ready) = mpsc::channel();
        let (release, wait) = mpsc::channel();
        (Arc::new(Gated { entered, release: Mutex::new(wait) }), ready, release)
    }
    fn finish(cache: &mut DecodeCache) {
        let done = cache.worker.as_ref().unwrap().completed.recv_timeout(Duration::from_secs(5)).unwrap();
        cache.complete(done);
    }

    #[test]
    fn worker_is_bounded_deduplicated_and_never_blocks_demand() {
        let mut cache = DecodeCache::new(16, true);
        let (a, ready, release) = gated();
        cache.predecode(&a);
        ready.recv_timeout(Duration::from_secs(5)).unwrap();
        cache.predecode(&a);
        assert_eq!(cache.pending.len(), 1);
        let pixels = cache.decode(&a).unwrap(); // worker is still held by the gate
        assert_eq!(cache.metrics.late, 1);
        assert_eq!(cache.metrics.bytes + cache.metrics.reserved, 16);
        cache.predecode(&source(8));
        assert_eq!(cache.pending.len(), 1, "reservations consume the same budget");
        release.send(()).unwrap();
        finish(&mut cache);
        assert!(Arc::ptr_eq(&pixels, &cache.decode(&a).unwrap()), "late work cannot replace demand pixels");
        assert_eq!(cache.metrics.unused, 1, "late duplicate output is discarded work");
    }

    #[test]
    fn ready_pixels_are_reused_and_speculation_cannot_evict_demand() {
        let mut cache = DecodeCache::new(8, true);
        let (a, ready, release) = gated();
        cache.predecode(&a);
        ready.recv_timeout(Duration::from_secs(5)).unwrap();
        release.send(()).unwrap();
        finish(&mut cache);
        assert_eq!(cache.decode(&a).unwrap().data(), &[7; 8]);
        assert_eq!(cache.metrics.ready_hits, 1);
        cache.predecode(&source(9));
        assert!(cache.pending.is_empty());
        assert_eq!(cache.metrics.bytes, 8);
    }

    #[derive(Debug)]
    struct Overallocated;
    impl BitmapDecoder for Overallocated {
        fn decode(&self) -> Result<Bitmap<'static>, Error> {
            let mut data = Vec::with_capacity(32);
            data.extend_from_slice(&[9; 8]);
            Ok(Bitmap::new(2, 1, BitmapFormat::Rgba, data))
        }
        fn decode_speculative(&self) -> Result<Bitmap<'static>, Error> { self.decode() }
        fn predecode_bytes(&self) -> Option<usize> { Some(8) }
    }

    #[test]
    fn pixel_allocations_not_only_logical_lengths_consume_the_budget() {
        let a: Source = Arc::new(Overallocated);
        let mut tiny = DecodeCache::new(8, false);
        let pixels = tiny.decode(&a).unwrap();
        assert_eq!(pixels.data().len(), 8);
        assert_eq!(pixels.allocation_size(), 32);
        assert!(tiny.entries.is_empty(), "oversize allocations stay on the demand path");
        assert_eq!(tiny.metrics.bytes, 0);

        let mut cache = DecodeCache::new(32, false);
        cache.decode(&a).unwrap();
        assert_eq!(cache.metrics.bytes, 32);
        cache.decode(&source(4)).unwrap();
        assert!(!cache.entries.contains_key(&key(&a)));
        assert_eq!(cache.metrics.bytes, 8);
    }

    #[test]
    fn rejected_speculation_is_not_retried_and_demand_remains_compatible() {
        let a: Source = Arc::new(Overallocated);
        let mut cache = DecodeCache::new(32, true);
        cache.predecode(&a);
        finish(&mut cache);
        assert_eq!(cache.metrics.failed, 1, "output capacity exceeds its reservation");
        assert_eq!(cache.metrics.reserved, 0);
        cache.predecode(&a);
        assert_eq!(cache.metrics.queued, 1, "the same rejected stream is not retried");
        assert!(cache.pending.is_empty());
        assert_eq!(cache.decode(&a).unwrap().data(), &[9; 8]);
        assert_eq!(cache.metrics.bytes, 32);
        drop(a);
        cache.maintain();
        assert!(cache.rejected.is_empty(), "rejection tracking does not own the movie");
        assert_eq!(cache.metrics.bytes, 0);
    }

    #[test]
    fn unloaded_sources_do_not_repopulate_the_pixel_cache() {
        let mut cache = DecodeCache::new(8, true);
        let (a, ready, release) = gated();
        cache.predecode(&a);
        ready.recv_timeout(Duration::from_secs(5)).unwrap();
        drop(a);
        release.send(()).unwrap();
        finish(&mut cache);
        assert!(cache.entries.is_empty());
        assert_eq!(cache.metrics.reserved, 0);
        assert_eq!(cache.metrics.unused, 1);
    }

    #[test]
    fn limit_can_shrink_and_restore_without_restarting_the_worker() {
        let a = source(1); let b = source(2);
        let mut cache = DecodeCache::new(16, false);
        cache.decode(&a).unwrap(); cache.decode(&b).unwrap();
        cache.set_limit(8);
        assert!(!cache.entries.contains_key(&key(&a)));
        assert!(cache.entries.contains_key(&key(&b)));
        cache.set_limit(0);
        assert_eq!(cache.metrics.bytes, 0);
        cache.decode(&a).unwrap();
        assert!(cache.entries.is_empty());
        cache.set_limit(16);
        cache.decode(&a).unwrap();
        assert_eq!(cache.metrics.bytes, 8);

        let mut cache = DecodeCache::new(8, true);
        let (a, ready, release) = gated();
        cache.predecode(&a);
        ready.recv_timeout(Duration::from_secs(5)).unwrap();
        cache.set_limit(0); // The bounded worker is still held by the gate.
        assert_eq!(cache.metrics.reserved, 8);
        release.send(()).unwrap();
        finish(&mut cache);
        assert_eq!(cache.metrics.reserved, 0);
        assert!(cache.entries.is_empty(), "late output respects the reduced limit");
        assert_eq!(cache.metrics.unused, 1);
        cache.set_limit(8);
        let b = source(2);
        cache.predecode(&b);
        finish(&mut cache);
        assert!(cache.worker.is_some());
        assert_eq!(cache.decode(&b).unwrap().data(), &[2; 8]);
        assert_eq!(cache.metrics.ready_hits, 1);
    }

    #[test]
    fn errors_do_not_populate_cache_and_guard_clears_player_state() {
        #[derive(Debug)] struct Invalid;
        impl BitmapDecoder for Invalid {
            fn decode(&self) -> Result<Bitmap<'static>, Error> { Err(Error::TooLarge) }
            fn decode_speculative(&self) -> Result<Bitmap<'static>, Error> { self.decode() }
            fn predecode_bytes(&self) -> Option<usize> { Some(8) }
        }
        let mut cache = DecodeCache::new(8, false);
        assert!(cache.decode(&(Arc::new(Invalid) as Source)).is_err());
        assert!(cache.entries.is_empty());
        let a = source(3);
        let guard = configure(8, false);
        decode(&a).unwrap(); decode(&a).unwrap();
        assert_eq!(take_metrics().hits, 1);
        drop(guard);
        assert_eq!(take_metrics().bytes, 0);
    }
}

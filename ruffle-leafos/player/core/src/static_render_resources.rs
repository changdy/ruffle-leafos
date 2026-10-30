//! Track allocated static shapes without keeping unloaded movies alive. An
//! optional byte budget retains idle shapes by recency; zero preserves the
//! original one-unused-frame release policy.
use ruffle_render::backend::ShapeHandle;
use std::cell::{Cell, RefCell};
use std::rc::{Rc, Weak};

#[derive(Default)]
pub(crate) struct StaticRenderResource {
    pub handle: RefCell<Option<ShapeHandle>>,
    rendered: Cell<bool>,
    queued: Cell<bool>,
}

impl StaticRenderResource {
    pub fn mark_rendered(&self) {
        self.rendered.set(true);
    }
}

#[derive(Default)]
pub(crate) struct StaticRenderResources {
    active: Vec<ActiveResource>,
    frame: u64,
}

struct ActiveResource {
    resource: Weak<StaticRenderResource>,
    last_used: u64,
}

/// The cache bounds estimated payload retained for idle static shapes only.
/// Current-frame resources, in-flight commands, driver overhead and other
/// allocations are outside that budget. Shared textures may be charged more
/// than once, so these fields are not physical GPU-memory measurements.
#[derive(Clone, Copy, Debug, Default)]
pub struct StaticRenderResourceCacheStats {
    pub idle_bytes: u64,
    pub idle_entries: usize,
    pub current_bytes: u64,
    pub current_entries: usize,
    pub evicted: u64,
    pub unknown_evictions: u64,
}

// Also bound metadata for empty/tiny shapes independently of the byte budget.
const MAX_IDLE_ENTRIES: usize = 1024;

fn evict(resource: &StaticRenderResource, stats: &mut StaticRenderResourceCacheStats) {
    if resource.handle.borrow_mut().take().is_some() {
        crate::render_metrics::evicted();
        stats.evicted += 1;
    }
    resource.queued.set(false);
}

impl StaticRenderResources {
    pub fn track(&mut self, resource: &Rc<StaticRenderResource>) {
        if resource.handle.borrow().is_some() && !resource.queued.replace(true) {
            self.active.push(ActiveResource {
                resource: Rc::downgrade(resource),
                last_used: self.frame,
            });
        }
    }

    pub fn collect(&mut self) {
        self.collect_with_budget(0);
    }

    pub fn collect_with_budget(&mut self, budget: u64) -> StaticRenderResourceCacheStats {
        self.frame = self.frame.saturating_add(1);
        let frame = self.frame;
        let mut stats = StaticRenderResourceCacheStats::default();
        let mut idle_bytes = 0u128;
        let mut idle = Vec::new();
        self.active.retain_mut(|entry| {
            let Some(resource) = entry.resource.upgrade() else { return false };
            let Some(bytes) = resource.handle.borrow().as_ref()
                .map(|handle| handle.0.estimated_cache_bytes()) else {
                resource.queued.set(false);
                return false;
            };
            if resource.rendered.replace(false) {
                entry.last_used = frame;
                stats.current_entries += 1;
                stats.current_bytes = stats.current_bytes.saturating_add(bytes.unwrap_or(0));
                return true;
            }
            // Unmeasured/oversize handles cannot enter a bounded idle cache.
            if budget == 0 || bytes.is_none_or(|bytes| bytes > budget) {
                stats.unknown_evictions += u64::from(budget != 0 && bytes.is_none());
                evict(&resource, &mut stats);
                return false;
            }
            let bytes = bytes.unwrap();
            idle_bytes += u128::from(bytes);
            stats.idle_entries += 1;
            idle.push((entry.last_used, bytes, resource));
            true
        });
        if idle_bytes > u128::from(budget) || stats.idle_entries > MAX_IDLE_ENTRIES {
            idle.sort_unstable_by_key(|(last_used, _, _)| *last_used);
            for (_, bytes, resource) in idle {
                if idle_bytes <= u128::from(budget) && stats.idle_entries <= MAX_IDLE_ENTRIES {
                    break;
                }
                evict(&resource, &mut stats);
                idle_bytes -= u128::from(bytes);
                stats.idle_entries -= 1;
            }
            self.active.retain(|entry| entry.resource.upgrade()
                .is_some_and(|resource| resource.queued.get()));
        }
        stats.idle_bytes = idle_bytes as u64;
        stats
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use ruffle_render::backend::ShapeHandleImpl;
    use std::sync::Arc;

    #[derive(Debug)]
    struct TestShape;
    impl ShapeHandleImpl for TestShape {}

    fn shape() -> ShapeHandle { ShapeHandle(Arc::new(TestShape)) }

    #[derive(Debug)]
    struct SizedShape(u64);
    impl ShapeHandleImpl for SizedShape {
        fn estimated_cache_bytes(&self) -> Option<u64> { Some(self.0) }
    }

    fn cached(cache: &mut StaticRenderResources, bytes: u64) -> Rc<StaticRenderResource> {
        let resource = Rc::new(StaticRenderResource::default());
        *resource.handle.borrow_mut() = Some(ShapeHandle(Arc::new(SizedShape(bytes))));
        resource.mark_rendered();
        cache.track(&resource);
        resource
    }

    #[test]
    fn static_idle_cache_reuses_recent_shapes_and_evicts_least_recent() {
        let mut cache = StaticRenderResources::default();
        let first = cached(&mut cache, 6);
        let initial = first.handle.borrow().as_ref().unwrap().clone();
        cache.collect_with_budget(10);
        let second = cached(&mut cache, 4);
        cache.collect_with_budget(10);
        let stats = cache.collect_with_budget(10);
        assert_eq!((stats.idle_bytes, stats.idle_entries), (10, 2));
        first.mark_rendered();
        cache.collect_with_budget(10);
        assert!(Arc::ptr_eq(&initial.0, &first.handle.borrow().as_ref().unwrap().0),
            "an idle hit must reuse the original GPU handle");
        let third = cached(&mut cache, 4);
        cache.collect_with_budget(10);
        let stats = cache.collect_with_budget(10);
        assert_eq!((stats.idle_bytes, stats.idle_entries, stats.evicted), (10, 2, 1));
        assert!(second.handle.borrow().is_none(), "least recently rendered shape goes first");
        assert!(first.handle.borrow().is_some());
        assert!(third.handle.borrow().is_some());
    }

    #[test]
    fn static_idle_budget_excludes_current_frame_and_can_shrink_to_zero() {
        let mut cache = StaticRenderResources::default();
        let big = cached(&mut cache, 100);
        let stats = cache.collect_with_budget(8);
        assert_eq!((stats.current_bytes, stats.current_entries, stats.idle_bytes), (100, 1, 0));
        assert!(big.handle.borrow().is_some(), "current frame is never evicted to fit idle budget");
        let stats = cache.collect_with_budget(8);
        assert_eq!((stats.idle_bytes, stats.evicted), (0, 1));
        assert!(big.handle.borrow().is_none(), "oversize idle shapes are not retained");
        let small = cached(&mut cache, 8);
        cache.collect_with_budget(8);
        assert_eq!(cache.collect_with_budget(8).idle_bytes, 8);
        assert_eq!(cache.collect_with_budget(0).idle_bytes, 0);
        assert!(small.handle.borrow().is_none());
    }

    #[test]
    fn static_idle_cache_does_not_retain_unknown_backends_or_unloaded_movies() {
        let mut cache = StaticRenderResources::default();
        let unknown = Rc::new(StaticRenderResource::default());
        *unknown.handle.borrow_mut() = Some(shape());
        unknown.mark_rendered();
        cache.track(&unknown);
        cache.collect_with_budget(8);
        let stats = cache.collect_with_budget(8);
        assert_eq!((stats.idle_entries, stats.unknown_evictions), (0, 1));
        assert!(unknown.handle.borrow().is_none());
        let unloaded = cached(&mut cache, 8);
        cache.collect_with_budget(8);
        cache.collect_with_budget(8);
        let weak = Rc::downgrade(&unloaded);
        drop(unloaded);
        assert!(weak.upgrade().is_none(), "idle cache must not own the movie's resource");
        assert_eq!(cache.collect_with_budget(8).idle_bytes, 0);
    }

    #[test]
    fn static_idle_eviction_preserves_in_flight_ownership_and_requeues() {
        let mut cache = StaticRenderResources::default();
        let resource = cached(&mut cache, 8);
        let command = resource.handle.borrow().as_ref().unwrap().clone();
        let weak = Arc::downgrade(&command.0);
        cache.collect_with_budget(8);
        cache.collect_with_budget(8);
        cache.collect_with_budget(0);
        assert!(resource.handle.borrow().is_none());
        assert!(weak.upgrade().is_some());
        drop(command);
        assert!(weak.upgrade().is_none());
        *resource.handle.borrow_mut() = Some(ShapeHandle(Arc::new(SizedShape(8))));
        resource.mark_rendered();
        cache.track(&resource);
        cache.track(&resource);
        assert_eq!(cache.active.len(), 1);
        cache.collect_with_budget(8);
        assert_eq!(cache.collect_with_budget(8).idle_bytes, 8);
    }

    #[test]
    fn static_idle_cache_bounds_empty_metadata_and_avoids_byte_overflow() {
        let mut cache = StaticRenderResources::default();
        let resources: Vec<_> = (0..MAX_IDLE_ENTRIES + 1).map(|_| cached(&mut cache, 0)).collect();
        cache.collect_with_budget(8);
        let stats = cache.collect_with_budget(8);
        assert_eq!((stats.idle_entries, stats.evicted), (MAX_IDLE_ENTRIES, 1));
        assert_eq!(resources.iter().filter(|r| r.handle.borrow().is_some()).count(), MAX_IDLE_ENTRIES);

        let mut cache = StaticRenderResources::default();
        let _a = cached(&mut cache, u64::MAX);
        let _b = cached(&mut cache, u64::MAX);
        cache.collect_with_budget(u64::MAX);
        let stats = cache.collect_with_budget(u64::MAX);
        assert_eq!((stats.idle_bytes, stats.idle_entries, stats.evicted), (u64::MAX, 1, 1));
    }

    #[test]
    fn static_active_set_deduplicates_evicts_and_requeues() {
        let mut cache = StaticRenderResources::default();
        let resource = Rc::new(StaticRenderResource::default());
        cache.track(&resource);
        assert!(cache.active.is_empty(), "unallocated shapes must not enter the scan");
        *resource.handle.borrow_mut() = Some(shape());
        resource.mark_rendered();
        cache.track(&resource);
        cache.track(&resource);
        assert_eq!(cache.active.len(), 1);
        cache.collect();
        assert!(resource.handle.borrow().is_some(), "keep the completed frame");
        cache.collect();
        assert!(resource.handle.borrow().is_none());
        assert!(cache.active.is_empty());
        *resource.handle.borrow_mut() = Some(shape());
        resource.mark_rendered();
        cache.track(&resource);
        assert_eq!(cache.active.len(), 1, "a reloaded animation must rejoin collection");
        cache.collect();
        assert!(resource.handle.borrow().is_some());
    }

    #[test]
    fn static_active_set_does_not_keep_unloaded_resources_alive() {
        let mut cache = StaticRenderResources::default();
        let resource = Rc::new(StaticRenderResource::default());
        *resource.handle.borrow_mut() = Some(shape());
        resource.mark_rendered();
        let weak = Rc::downgrade(&resource);
        cache.track(&resource);
        drop(resource);
        assert!(weak.upgrade().is_none());
        cache.collect();
        assert!(cache.active.is_empty());
    }

    #[test]
    fn static_active_set_preserves_in_flight_command_ownership() {
        let mut cache = StaticRenderResources::default();
        let resource = Rc::new(StaticRenderResource::default());
        let command = shape();
        let weak = Arc::downgrade(&command.0);
        *resource.handle.borrow_mut() = Some(command.clone());
        resource.mark_rendered();
        cache.track(&resource);
        cache.collect();
        cache.collect();
        assert!(resource.handle.borrow().is_none());
        assert!(weak.upgrade().is_some(), "commands retain their own shape handles");
        drop(command);
        assert!(weak.upgrade().is_none());
    }
}

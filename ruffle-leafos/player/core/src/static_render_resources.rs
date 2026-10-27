//! Track only allocated static shapes, without keeping unloaded movies alive.
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
    active: Vec<Weak<StaticRenderResource>>,
}

impl StaticRenderResources {
    pub fn track(&mut self, resource: &Rc<StaticRenderResource>) {
        if resource.handle.borrow().is_some() && !resource.queued.replace(true) {
            self.active.push(Rc::downgrade(resource));
        }
    }

    pub fn collect(&mut self) {
        self.active.retain(|weak| {
            let Some(resource) = weak.upgrade() else { return false };
            if resource.rendered.replace(false) {
                return true;
            }
            if resource.handle.borrow_mut().take().is_some() {
                crate::render_metrics::evicted();
            }
            resource.queued.set(false);
            false
        });
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

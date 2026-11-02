use crate::descriptors::Descriptors;
use crate::globals::Globals;
use fnv::FnvHashMap;
use std::fmt::{Debug, Formatter};
use std::ops::Deref;
use std::sync::{Arc, Mutex, Weak};

struct PoolState<T> {
    available: Vec<T>,
    under_pressure: bool,
    generation: u64,
    retired: bool,
}

type PoolInner<T> = Mutex<PoolState<T>>;
type Constructor<Type, Description> = Box<dyn Fn(&Descriptors, &Description) -> Type>;

#[derive(Debug, Default)]
pub struct TexturePool {
    pools: FnvHashMap<TextureKey, BufferPool<(wgpu::Texture, wgpu::TextureView), AlwaysCompatible>>,
    globals_cache: FnvHashMap<GlobalsKey, Arc<Globals>>,
    under_pressure: bool,
}

impl TexturePool {
    pub fn new() -> Self {
        Default::default()
    }

    /// Release cached resources while preserving the current retention policy.
    /// Borrowed entries and command buffers keep their own resource references.
    pub fn clear(&mut self) {
        self.pools.clear();
        self.globals_cache.clear();
    }

    pub fn set_memory_pressure(&mut self, under_pressure: bool) {
        self.under_pressure = under_pressure;
        for pool in self.pools.values() {
            pool.set_memory_pressure(under_pressure);
        }
        if under_pressure {
            self.globals_cache.clear();
        }
    }

    pub fn get_texture(
        &mut self,
        descriptors: &Descriptors,
        size: wgpu::Extent3d,
        usage: wgpu::TextureUsages,
        format: wgpu::TextureFormat,
        sample_count: u32,
    ) -> PoolEntry<(wgpu::Texture, wgpu::TextureView), AlwaysCompatible> {
        let key = TextureKey {
            size,
            usage,
            format,
            sample_count,
        };
        let under_pressure = self.under_pressure;
        let pool = self.pools.entry(key).or_insert_with(|| {
            let label = if cfg!(feature = "render_debug_labels") {
                use std::sync::atomic::{AtomicU32, Ordering};
                static ID_COUNT: AtomicU32 = AtomicU32::new(0);
                let id = ID_COUNT.fetch_add(1, Ordering::Relaxed);
                create_debug_label!("Pooled texture {}", id)
            } else {
                None
            };
            let pool = BufferPool::new(Box::new(move |descriptors, _description| {
                let texture = descriptors.device.create_texture(&wgpu::TextureDescriptor {
                    label: label.as_deref(),
                    size,
                    mip_level_count: 1,
                    sample_count,
                    dimension: wgpu::TextureDimension::D2,
                    format,
                    view_formats: &[format],
                    usage,
                });
                let view = texture.create_view(&Default::default());
                (texture, view)
            }));
            pool.set_memory_pressure(under_pressure);
            pool
        });
        pool.take(descriptors, AlwaysCompatible)
    }

    pub fn get_globals(
        &mut self,
        descriptors: &Descriptors,
        viewport_width: u32,
        viewport_height: u32,
    ) -> Arc<Globals> {
        if self.under_pressure {
            return Arc::new(Globals::new(
                &descriptors.device,
                &descriptors.bind_layouts.globals,
                viewport_width,
                viewport_height,
            ));
        }
        self.globals_cache
            .entry(GlobalsKey {
                viewport_width,
                viewport_height,
            })
            .or_insert_with(|| {
                Arc::new(Globals::new(
                    &descriptors.device,
                    &descriptors.bind_layouts.globals,
                    viewport_width,
                    viewport_height,
                ))
            })
            .clone()
    }
}

#[derive(Copy, Clone, Debug, Hash, Eq, PartialEq)]
struct TextureKey {
    size: wgpu::Extent3d,
    usage: wgpu::TextureUsages,
    format: wgpu::TextureFormat,
    sample_count: u32,
}

#[derive(Copy, Clone, Debug, Hash, Eq, PartialEq)]
struct GlobalsKey {
    viewport_width: u32,
    viewport_height: u32,
}

pub trait BufferDescription: Clone + Debug {
    type Cost: Ord;

    /// If the potential buffer represented by this description (`self`)
    /// fits another existing buffer and its description (`other`),
    /// return the cost to use that buffer instead of making a new one.
    ///
    /// Cost is an arbitrary unit, but lower is better.
    /// None means that the other buffer cannot be used in place of this one.
    fn cost_to_use(&self, other: &Self) -> Option<Self::Cost>;
}

#[derive(Clone, Debug)]
pub struct AlwaysCompatible;

impl BufferDescription for AlwaysCompatible {
    type Cost = ();

    fn cost_to_use(&self, _other: &Self) -> Option<()> {
        Some(())
    }
}

pub struct BufferPool<Type, Description: BufferDescription> {
    available: Arc<PoolInner<(Type, Description)>>,
    constructor: Constructor<Type, Description>,
}

impl<Type, Description: BufferDescription> Debug for BufferPool<Type, Description> {
    fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("BufferPool").finish()
    }
}

impl<Type, Description: BufferDescription> BufferPool<Type, Description> {
    pub fn new(constructor: Constructor<Type, Description>) -> Self {
        Self {
            available: Arc::new(Mutex::new(PoolState {
                available: vec![],
                under_pressure: false,
                generation: 0,
                retired: false,
            })),
            constructor,
        }
    }

    /// Trim only idle entries. Keep the policy in the shared state so deferred
    /// readbacks holding an older Arc<BufferPool> also obey it.
    pub fn set_memory_pressure(&self, under_pressure: bool) {
        self.update_memory_policy(under_pressure, false);
    }

    /// Deferred users may still allocate from this pool, but can never leave
    /// idle resources behind after the backend moves to a new pool.
    pub fn retire(&self) {
        self.update_memory_policy(true, true);
    }

    fn update_memory_policy(&self, under_pressure: bool, retire: bool) {
        let mut state = self
            .available
            .lock()
            .expect("Should not be able to lock recursively");
        state.retired |= retire;
        let under_pressure = under_pressure || state.retired;
        if under_pressure && !state.under_pressure {
            // An entry borrowed before pressure must not repopulate the cache
            // after recovery, even if it is returned much later.
            state.generation = state.generation.wrapping_add(1);
        }
        state.under_pressure = under_pressure;
        let retired = if under_pressure {
            std::mem::take(&mut state.available)
        } else {
            Vec::new()
        };
        drop(state);
        drop(retired);
    }

    pub fn take(
        &self,
        descriptors: &Descriptors,
        description: Description,
    ) -> PoolEntry<Type, Description> {
        let mut guard = self
            .available
            .lock()
            .expect("Should not be able to lock recursively");
        let mut best: Option<(Description::Cost, usize)> = None;
        for i in 0..guard.available.len() {
            if let Some(cost) = description.cost_to_use(&guard.available[i].1) {
                if let Some(best) = &mut best {
                    if best.0 > cost {
                        *best = (cost, i);
                    }
                } else if best.is_none() {
                    best = Some((cost, i));
                }
            }
        }

        let (item, used_description) = if let Some((_, best)) = best {
            guard.available.swap_remove(best)
        } else {
            let item = (self.constructor)(descriptors, &description);
            (item, description)
        };
        PoolEntry {
            item: Some(item),
            description: used_description,
            pool: Arc::downgrade(&self.available),
            generation: guard.generation,
            cacheable: !guard.under_pressure,
        }
    }
}

pub struct PoolEntry<Type, Description: BufferDescription> {
    item: Option<Type>,
    description: Description,
    pool: Weak<PoolInner<(Type, Description)>>,
    generation: u64,
    cacheable: bool,
}

impl<Type, Description: BufferDescription> Debug for PoolEntry<Type, Description>
where
    Type: Debug,
{
    fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
        f.debug_tuple("PoolEntry").field(&self.item).finish()
    }
}

impl<Type, Description: BufferDescription> Drop for PoolEntry<Type, Description> {
    fn drop(&mut self) {
        if let Some(item) = self.item.take()
            && let Some(pool) = self.pool.upgrade()
        {
            let mut state = pool
                .lock()
                .expect("Should not be able to lock recursively");
            if self.cacheable && !state.under_pressure && self.generation == state.generation {
                state.available.push((item, self.description.clone()));
            }
        }
    }
}

impl<Type, Description: BufferDescription> Deref for PoolEntry<Type, Description> {
    type Target = Type;

    fn deref(&self) -> &Self::Target {
        self.item.as_ref().expect("Item should exist until dropped")
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicUsize, Ordering};

    fn descriptors() -> Descriptors {
        let instance = wgpu::Instance::new(wgpu::InstanceDescriptor {
            backends: wgpu::Backends::NOOP,
            backend_options: wgpu::BackendOptions {
                noop: wgpu::NoopBackendOptions { enable: true, ..Default::default() },
                ..Default::default()
            },
            ..wgpu::InstanceDescriptor::new_without_display_handle()
        });
        let adapter = futures::executor::block_on(instance.request_adapter(&Default::default())).unwrap();
        let (device, queue) = futures::executor::block_on(adapter.request_device(&Default::default())).unwrap();
        Descriptors::new(instance, adapter, device, queue)
    }

    struct DropCounter {
        id: usize,
        drops: Arc<AtomicUsize>,
    }

    impl Drop for DropCounter {
        fn drop(&mut self) {
            self.drops.fetch_add(1, Ordering::Relaxed);
        }
    }

    fn counter_pool(
        created: &Arc<AtomicUsize>,
        drops: &Arc<AtomicUsize>,
    ) -> Arc<BufferPool<DropCounter, AlwaysCompatible>> {
        let created = created.clone();
        let drops = drops.clone();
        Arc::new(BufferPool::new(Box::new(move |_, _| DropCounter {
            id: created.fetch_add(1, Ordering::Relaxed),
            drops: drops.clone(),
        })))
    }

    #[test]
    fn pressure_drops_idle_entries_but_keeps_borrowed_resources_usable() {
        let descriptors = descriptors();
        let created = Arc::new(AtomicUsize::new(0));
        let drops = Arc::new(AtomicUsize::new(0));
        let pool = counter_pool(&created, &drops);
        let idle = pool.take(&descriptors, AlwaysCompatible);
        let borrowed = pool.take(&descriptors, AlwaysCompatible);
        drop(idle);
        assert_eq!(drops.load(Ordering::Relaxed), 0, "normal returns are retained");

        pool.set_memory_pressure(true);
        assert_eq!(drops.load(Ordering::Relaxed), 1, "idle resources are released immediately");
        assert_eq!(borrowed.id, 1, "borrowed resources remain usable");
        drop(borrowed);
        assert_eq!(drops.load(Ordering::Relaxed), 2, "pressure prevents late returns from being retained");

        for _ in 0..2 {
            pool.set_memory_pressure(true);
            drop(pool.take(&descriptors, AlwaysCompatible));
        }
        assert_eq!(created.load(Ordering::Relaxed), 4);
        assert_eq!(drops.load(Ordering::Relaxed), 4, "pressure also prevents newly borrowed resources from accumulating");
    }

    #[test]
    fn pressure_generations_reject_old_returns_after_recovery_and_restore_new_reuse() {
        let descriptors = descriptors();
        let created = Arc::new(AtomicUsize::new(0));
        let drops = Arc::new(AtomicUsize::new(0));
        let pool = counter_pool(&created, &drops);
        let before_pressure = pool.take(&descriptors, AlwaysCompatible);
        pool.set_memory_pressure(true);
        let during_pressure = pool.take(&descriptors, AlwaysCompatible);
        pool.set_memory_pressure(false);
        drop(before_pressure);
        drop(during_pressure);
        assert_eq!(drops.load(Ordering::Relaxed), 2, "recovery must not retain stale or pressure-era returns");

        let fresh = pool.take(&descriptors, AlwaysCompatible);
        let id = fresh.id;
        drop(fresh);
        let reused = pool.take(&descriptors, AlwaysCompatible);
        assert_eq!(reused.id, id, "new resources regain normal reuse after recovery");
        assert_eq!(created.load(Ordering::Relaxed), 3);
    }

    #[test]
    fn retired_pool_arcs_allow_deferred_reads_without_caching_even_after_recovery() {
        let descriptors = descriptors();
        let created = Arc::new(AtomicUsize::new(0));
        let drops = Arc::new(AtomicUsize::new(0));
        let pool = counter_pool(&created, &drops);
        // Mirrors a NotCopied readback holding an Arc before the backend retires it.
        let delayed_reader = pool.clone();
        let idle = pool.take(&descriptors, AlwaysCompatible);
        let borrowed = pool.take(&descriptors, AlwaysCompatible);
        drop(idle);
        pool.retire();
        assert_eq!(drops.load(Ordering::Relaxed), 1);
        pool.set_memory_pressure(false);
        assert_eq!(borrowed.id, 1);
        drop(borrowed);

        let late = delayed_reader.take(&descriptors, AlwaysCompatible);
        assert_eq!(late.id, 2, "retirement must not disable deferred reads");
        drop(late);
        let another = delayed_reader.take(&descriptors, AlwaysCompatible);
        assert_eq!(another.id, 3, "old Arc users must not recreate an idle pool");
        drop(another);
        assert_eq!(drops.load(Ordering::Relaxed), 4);
    }

    #[test]
    fn texture_pool_inherits_pressure_for_new_keys_and_preserves_it_across_clear() {
        let descriptors = descriptors();
        let mut pool = TexturePool::new();
        let get_texture = |pool: &mut TexturePool, width| pool.get_texture(
            &descriptors,
            wgpu::Extent3d { width, height: 8, depth_or_array_layers: 1 },
            wgpu::TextureUsages::RENDER_ATTACHMENT,
            wgpu::TextureFormat::Rgba8Unorm,
            1,
        );

        let first = get_texture(&mut pool, 8);
        let first_texture = first.0.clone();
        drop(first);
        let borrowed = get_texture(&mut pool, 8);
        assert_eq!(borrowed.0, first_texture, "normal texture returns are reused");
        pool.set_memory_pressure(true);
        pool.set_memory_pressure(false);
        drop(borrowed);
        let fresh = get_texture(&mut pool, 8);
        assert_ne!(fresh.0, first_texture, "a pre-pressure borrowed texture must not return after recovery");
        drop(fresh);

        pool.set_memory_pressure(true);
        pool.clear();
        let under_pressure = get_texture(&mut pool, 16);
        let pressure_texture = under_pressure.0.clone();
        drop(under_pressure);
        let late = get_texture(&mut pool, 16);
        assert_ne!(late.0, pressure_texture, "new texture keys must inherit pressure after a frame/resize clear");
        let late_texture = late.0.clone();
        pool.set_memory_pressure(false);
        drop(late);
        let recovered = get_texture(&mut pool, 16);
        assert_ne!(recovered.0, late_texture, "a pressure-era borrow must not return after recovery");
        let recovered_texture = recovered.0.clone();
        drop(recovered);
        assert_eq!(get_texture(&mut pool, 16).0, recovered_texture);
    }

    #[test]
    fn pressure_does_not_pin_globals_while_borrowed_globals_remain_valid() {
        let descriptors = descriptors();
        let mut pool = TexturePool::new();
        let globals = pool.get_globals(&descriptors, 16, 16);
        assert!(Arc::ptr_eq(&globals, &pool.get_globals(&descriptors, 16, 16)));
        let weak = Arc::downgrade(&globals);
        pool.set_memory_pressure(true);
        assert!(weak.upgrade().is_some(), "the caller still owns its globals");
        drop(globals);
        assert!(weak.upgrade().is_none(), "the pool must release cached globals under pressure");

        let temporary = pool.get_globals(&descriptors, 32, 32);
        let weak = Arc::downgrade(&temporary);
        assert!(!Arc::ptr_eq(&temporary, &pool.get_globals(&descriptors, 32, 32)));
        drop(temporary);
        assert!(weak.upgrade().is_none(), "new dimensions must not create another idle globals cache");
        pool.set_memory_pressure(false);
        let recovered = pool.get_globals(&descriptors, 32, 32);
        assert!(Arc::ptr_eq(&recovered, &pool.get_globals(&descriptors, 32, 32)));
    }
}

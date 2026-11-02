use crate::avm1::Object as Avm1Object;
use crate::avm2::{
    Activation as Avm2Activation, Avm2, ClassObject as Avm2ClassObject,
    StageObject as Avm2StageObject,
};
use crate::context::{RenderContext, UpdateContext};
use crate::display_object::{BoundsMode, DisplayObjectBase};
use crate::drawing::Drawing;
use crate::library::MovieLibrarySource;
use crate::prelude::*;
use crate::tag_utils::SwfMovie;
use crate::tessellation_cache::TessellationCache;
use crate::vminterface::Instantiator;
use core::fmt;
use gc_arena::barrier::unlock;
use gc_arena::lock::Lock;
use gc_arena::{Collect, Gc, Mutation};
use ruffle_common::utils::HasPrefixField;
use ruffle_render::backend::ShapeHandle;
use ruffle_render::commands::CommandHandler;
use std::cell::{OnceCell, RefCell, RefMut};
use std::rc::Rc;
use std::sync::Arc;
use crate::static_render_resources::{StaticRenderResource, StaticRenderResources};

#[derive(Clone, Collect, Copy)]
#[collect(no_drop)]
pub struct Graphic<'gc>(Gc<'gc, GraphicData<'gc>>);

impl fmt::Debug for Graphic<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Graphic")
            .field("ptr", &Gc::as_ptr(self.0))
            .finish()
    }
}

#[derive(Clone, Collect, HasPrefixField)]
#[collect(no_drop)]
#[repr(C, align(8))]
pub struct GraphicData<'gc> {
    base: DisplayObjectBase<'gc>,
    shared: Lock<Gc<'gc, GraphicShared>>,
    class: Lock<Option<Avm2ClassObject<'gc>>>,
    avm2_object: Lock<Option<Avm2StageObject<'gc>>>,
    /// This is lazily allocated on demand, to make `GraphicData` smaller in the common case.
    #[collect(require_static)]
    drawing: OnceCell<Box<RefCell<Drawing>>>,
}

impl<'gc> Graphic<'gc> {
    /// Construct a `Graphic` from it's associated `Shape` tag.
    pub fn from_swf_tag(
        context: &mut UpdateContext<'gc>,
        swf_shape: swf::Shape,
        movie: Arc<SwfMovie>,
    ) -> Self {
        let shared = GraphicShared {
            id: swf_shape.id,
            shape_bounds: swf_shape.shape_bounds,
            edge_bounds: swf_shape.edge_bounds,
            // Upload only when visible; every scale variant shares the same
            // reclaimable resource accounting used by the idle-memory budget.
            render_resource: Rc::default(),
            bitmap_ids: OnceCell::new(),
            shape: swf_shape,
            movie,
            scaled_handle: RefCell::new(TessellationCache::new()),
        };

        Graphic(Gc::new(
            context.gc(),
            GraphicData {
                base: Default::default(),
                shared: Lock::new(Gc::new(context.gc(), shared)),
                class: Lock::new(None),
                avm2_object: Lock::new(None),
                drawing: OnceCell::new(),
            },
        ))
    }

    /// Construct an empty `Graphic`.
    pub fn empty(context: &mut UpdateContext<'gc>) -> Self {
        let shared = GraphicShared {
            id: 0,
            shape_bounds: Default::default(),
            edge_bounds: Default::default(),
            render_resource: Rc::default(),
            bitmap_ids: OnceCell::new(),
            shape: swf::Shape {
                version: 32,
                id: 0,
                shape_bounds: Default::default(),
                edge_bounds: Default::default(),
                flags: swf::ShapeFlag::empty(),
                styles: swf::ShapeStyles {
                    fill_styles: Vec::new(),
                    line_styles: Vec::new(),
                },
                shape: Vec::new(),
            },
            movie: context.root_swf.clone(),
            scaled_handle: RefCell::new(TessellationCache::new()),
        };

        Graphic(Gc::new(
            context.gc(),
            GraphicData {
                base: Default::default(),
                shared: Lock::new(Gc::new(context.gc(), shared)),
                class: Lock::new(None),
                avm2_object: Lock::new(None),
                drawing: OnceCell::new(),
            },
        ))
    }

    pub fn instantiate(self, mc: &Mutation<'gc>) -> Self {
        Self(Gc::new(mc, (*self.0).clone()))
    }

    pub fn drawing_mut(&self) -> RefMut<'_, Drawing> {
        self.0.drawing.get_or_init(Default::default).borrow_mut()
    }

    pub fn set_avm2_class(self, mc: &Mutation<'gc>, class: Avm2ClassObject<'gc>) {
        unlock!(Gc::write(mc, self.0), GraphicData, class).set(Some(class));
    }

    fn set_shared(self, mc: &Mutation<'gc>, shared: Gc<'gc, GraphicShared>) {
        unlock!(Gc::write(mc, self.0), GraphicData, shared).set(shared);
    }

    /// Seed collection once if a frontend enables it after the first render.
    pub(crate) fn track_cached_render_resource(self, resources: &mut StaticRenderResources) {
        let shared = self.0.shared.get();
        resources.track(&shared.render_resource);
        for resource in shared.scaled_handle.borrow().values() {
            resources.track(resource);
        }
    }

    fn predecode_bitmap_fills(self, context: &UpdateContext<'gc>) {
        if !ruffle_render::bitmap_cache::predecode_enabled() { return; }
        if self.0.drawing.get().is_some() { return; }
        let shared = self.0.shared.get();
        if shared.render_resource.handle.borrow().is_some()
            || shared.scaled_handle.borrow().values().any(|resource| resource.handle.borrow().is_some()) {
            return;
        }
        let Some(library) = context.library.library_for_movie(shared.movie.clone()) else { return };
        // Only placed shapes request work; preloading a movie never expands all
        // of its artwork. Include styles introduced inside a shape as well.
        let ids = shared.bitmap_ids.get_or_init(|| {
            let mut ids = Vec::new();
            let mut collect = |styles: &swf::ShapeStyles| {
                for style in styles.fill_styles.iter().chain(styles.line_styles.iter().map(|line| line.fill_style())) {
                    if let swf::FillStyle::Bitmap { id, .. } = style {
                        ids.push(*id);
                    }
                }
            };
            collect(&shared.shape.styles);
            for record in &shared.shape.shape {
                if let swf::ShapeRecord::StyleChange(change) = record {
                    if let Some(styles) = &change.new_styles { collect(styles); }
                }
            }
            ids.sort_unstable();
            ids.dedup();
            ids
        });
        for id in ids {
            if let Some(crate::character::Character::Bitmap(bitmap)) = library.character_by_id(*id) {
                bitmap.predecode();
            }
        }
    }

    /// Retessellate for the current display scale while keeping every variant
    /// reclaimable by the frontend's global static-resource memory budget.
    fn get_or_retessellate_handle(
        self,
        context: &mut RenderContext,
        current_scale: f32,
    ) -> Option<ShapeHandle> {
        let shared = self.0.shared.get();
        {
            let mut cache = shared.scaled_handle.borrow_mut();
            // The cache keeps resource containers, not a second ShapeHandle.
            // Drop reclaimed entries so a reupload is keyed at its actual scale.
            cache.retain(|resource| resource.handle.borrow().is_some());
            if let Some(resource) = cache.find_near_and_touch(current_scale) {
                resource.mark_rendered();
                context.library.track_static_render_resource(&resource);
                return resource.handle.borrow().clone();
            }
        }

        let library = context.library.library_for_movie(shared.movie.clone())?;
        let start = crate::render_metrics::start();
        let handle = context.renderer.register_shape_with_scale(
            (&shared.shape).into(),
            &MovieLibrarySource { library },
            current_scale,
        );
        crate::render_metrics::shape(start);
        let resource = if shared.render_resource.handle.borrow().is_none() {
            shared.render_resource.clone()
        } else {
            Rc::default()
        };
        *resource.handle.borrow_mut() = Some(handle.clone());
        resource.mark_rendered();
        context.library.track_static_render_resource(&resource);
        let mut cache = shared.scaled_handle.borrow_mut();
        tracing::debug!(
            "Graphic id={} retessellated: new_scale={:.2}, cache_size={}",
            shared.id,
            current_scale,
            cache.len()
        );
        cache.insert(current_scale, resource);
        Some(handle)
    }
}

impl<'gc> TDisplayObject<'gc> for Graphic<'gc> {
    fn base(self) -> Gc<'gc, DisplayObjectBase<'gc>> {
        HasPrefixField::as_prefix_gc(self.0)
    }

    fn id(self) -> CharacterId {
        self.0.shared.get().id
    }

    fn self_bounds(self, mode: BoundsMode) -> Rectangle<Twips> {
        let include_strokes = mode.includes_strokes();

        if let Some(drawing) = self.0.drawing.get() {
            drawing.borrow().self_bounds(include_strokes)
        } else if include_strokes {
            self.0.shared.get().shape_bounds
        } else {
            self.0.shared.get().edge_bounds
        }
    }

    fn construct_frame(self, context: &mut UpdateContext<'gc>) {
        self.predecode_bitmap_fills(context);
        if self.object2().is_none() && self.0.shared.get().movie.is_action_script_3() {
            let class_object = self
                .0
                .class
                .get()
                .unwrap_or_else(|| context.avm2.classes().shape);

            let mut activation = Avm2Activation::from_nothing(context);

            match Avm2StageObject::for_display_object_childless(
                &mut activation,
                self.into(),
                class_object,
            ) {
                Ok(object) => self.set_object2(activation.context, object),
                Err(err) => {
                    Avm2::uncaught_error(
                        &mut activation,
                        Some(self.into()),
                        err,
                        "Error running AVM2 construction for shape",
                    );
                }
            }

            self.on_construction_complete(context);
        }
    }

    fn replace_with(self, context: &mut UpdateContext<'gc>, id: CharacterId) {
        // Static assets like Graphics can replace themselves via a PlaceObject tag with PlaceObjectAction::Replace.
        // This does not create a new instance, but instead swaps out the underlying static data to point to the new art.
        if let Some(new_graphic) = context
            .library
            .library_for_movie_mut(self.movie())
            .get_graphic(id)
        {
            self.set_shared(context.gc(), new_graphic.0.shared.get());
        } else {
            tracing::warn!("PlaceObject: expected Graphic at character ID {}", id);
        }
        self.invalidate_cached_bitmap();
        self.predecode_bitmap_fills(context);
    }

    fn render_self(self, context: &mut RenderContext) {
        if !context.is_offscreen
            && !self
                .world_bounds(BoundsMode::Engine)
                .intersects(&context.stage.view_bounds())
        {
            // Off-screen; culled
            return;
        }

        if let Some(drawing) = self.0.drawing.get() {
            drawing.borrow().render(context);
        } else if !self.0.shared.get().shape.shape.is_empty() {
            let transform = context.transform_stack.transform();
            // Match upstream's scale-dependent tessellation and nearest-scale LRU.
            let matrix = &transform.matrix;
            let scale_x = f32::abs(matrix.a + matrix.c);
            let scale_y = f32::abs(matrix.b + matrix.d);
            let current_scale = ((scale_x * scale_x + scale_y * scale_y) / 2.0).sqrt();
            if let Some(handle) = self.get_or_retessellate_handle(context, current_scale) {
                context.commands.render_shape(handle, transform);
            }
        }
    }

    fn hit_test_shape(
        self,
        _context: &mut UpdateContext<'gc>,
        point: Point<Twips>,
        options: HitTestOptions,
    ) -> bool {
        // Transform point to local coordinates and test.
        if (!options.contains(HitTestOptions::SKIP_INVISIBLE) || self.visible())
            && self.world_bounds(BoundsMode::Engine).contains(point)
        {
            let Some(local_matrix) = self.global_to_local_matrix() else {
                return false;
            };
            let point = local_matrix * point;
            if let Some(drawing) = self.0.drawing.get() {
                if drawing.borrow().hit_test(point, &local_matrix) {
                    return true;
                }
            } else {
                let shape = &self.0.shared.get().shape;
                return ruffle_render::shape_utils::shape_hit_test(shape, point, &local_matrix);
            }
        }

        false
    }

    fn post_instantiation(
        self,
        context: &mut UpdateContext<'gc>,
        _init_object: Option<Avm1Object<'gc>>,
        _instantiated_by: Instantiator,
        _run_frame: bool,
    ) {
        self.predecode_bitmap_fills(context);
        if self.movie().is_action_script_3() {
            self.set_default_instance_name(context);
        }
    }

    fn movie(self) -> Arc<SwfMovie> {
        self.0.shared.get().movie.clone()
    }

    fn object1(self) -> Option<Avm1Object<'gc>> {
        None
    }

    fn object2(self) -> Option<Avm2StageObject<'gc>> {
        self.0.avm2_object.get()
    }

    fn set_object2(self, context: &mut UpdateContext<'gc>, to: Avm2StageObject<'gc>) {
        let mc = context.gc();
        unlock!(Gc::write(mc, self.0), GraphicData, avm2_object).set(Some(to));
    }

    fn as_drawing(&self) -> Option<RefMut<'_, Drawing>> {
        Some(self.drawing_mut())
    }
}

/// Data shared between all instances of a Graphic.
#[derive(Collect)]
#[collect(require_static)]
struct GraphicShared {
    id: CharacterId,
    shape: swf::Shape,
    /// Shared by every instance; only allocate GPU resources on first render.
    render_resource: Rc<StaticRenderResource>,
    bitmap_ids: OnceCell<Vec<CharacterId>>,
    shape_bounds: Rectangle<Twips>,
    edge_bounds: Rectangle<Twips>,
    movie: Arc<SwfMovie>,
    #[collect(require_static)]
    scaled_handle: RefCell<TessellationCache<Rc<StaticRenderResource>>>,
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::player::PlayerBuilder;
    use ruffle_render::commands::CommandList;
    use ruffle_render::transform::TransformStack;

    #[test]
    fn static_cache_is_lazy_shared_and_releases_unused_frames() {
        let player = PlayerBuilder::new().with_movie(SwfMovie::empty(9, None)).build();
        player.lock().unwrap().mutate_with_update_context(|context| {
            let graphic = Graphic::from_swf_tag(context, swf::Shape {
                version: 1, id: 1, shape_bounds: Default::default(),
                edge_bounds: Default::default(), flags: swf::ShapeFlag::empty(),
                styles: swf::ShapeStyles { fill_styles: vec![], line_styles: vec![] },
                shape: vec![swf::ShapeRecord::StraightEdge { delta: Default::default() }],
            }, context.root_swf.clone());
            let instance = Graphic(Gc::new(context.gc(), graphic.0.as_ref().clone()));
            let shared = graphic.0.shared.get();
            context.library.collect_static_render_resources();
            assert!(shared.render_resource.handle.borrow().is_none());
            graphic.hit_test_shape(context, Point::default(), HitTestOptions::empty());
            assert!(shared.render_resource.handle.borrow().is_none());
            let weak;
            {
                let mut cache_draws = vec![];
                let mut transform_stack = TransformStack::new();
                let mut render = RenderContext {
                    gc_context: context.gc(), stage: context.stage,
                    renderer: context.renderer, library: context.library,
                    ui: &*context.ui,
                    commands: CommandList::new(), cache_draws: &mut cache_draws,
                    transform_stack: &mut transform_stack,
                    is_offscreen: true, use_bitmap_cache: true,
                };
                graphic.render_self(&mut render);
                weak = Arc::downgrade(&shared.render_resource.handle.borrow().as_ref().unwrap().0);
                instance.render_self(&mut render);
                assert!(Arc::ptr_eq(&weak.upgrade().unwrap(),
                    &instance.0.shared.get().render_resource.handle.borrow().as_ref().unwrap().0));
            }
            context.library.collect_static_render_resources();
            assert!(weak.upgrade().is_some(), "current frame must retain its resources");
            context.library.collect_static_render_resources();
            assert!(weak.upgrade().is_none(), "unused animation frames must release resources");
        });
    }

    fn render_at_scale<'gc>(
        graphic: Graphic<'gc>,
        context: &mut UpdateContext<'gc>,
        scale: f32,
    ) -> ShapeHandle {
        let mut cache_draws = vec![];
        let mut transform_stack = TransformStack::new();
        transform_stack.push(&ruffle_render::transform::Transform {
            matrix: ruffle_render::matrix::Matrix {
                a: scale,
                d: scale,
                ..Default::default()
            },
            ..Default::default()
        });
        let mut render = RenderContext {
            gc_context: context.gc(), stage: context.stage,
            renderer: context.renderer, library: context.library,
            ui: &*context.ui,
            commands: CommandList::new(), cache_draws: &mut cache_draws,
            transform_stack: &mut transform_stack,
            is_offscreen: true, use_bitmap_cache: true,
        };
        graphic.render_self(&mut render);
        let shared = graphic.0.shared.get();
        let resource = shared.scaled_handle.borrow_mut().find_near_and_touch(scale).unwrap();
        resource.handle.borrow().as_ref().unwrap().clone()
    }

    #[test]
    fn scaled_static_cache_shares_nearby_scales_and_releases_every_variant() {
        let player = PlayerBuilder::new().with_movie(SwfMovie::empty(9, None)).build();
        player.lock().unwrap().mutate_with_update_context(|context| {
            let graphic = Graphic::from_swf_tag(context, swf::Shape {
                version: 1, id: 1, shape_bounds: Default::default(),
                edge_bounds: Default::default(), flags: swf::ShapeFlag::empty(),
                styles: swf::ShapeStyles { fill_styles: vec![], line_styles: vec![] },
                shape: vec![swf::ShapeRecord::StraightEdge { delta: Default::default() }],
            }, context.root_swf.clone());
            let instance = graphic.instantiate(context.gc());
            context.library.collect_static_render_resources();
            let first = render_at_scale(graphic, context, 4.0);
            let nearby = render_at_scale(instance, context, 5.0);
            assert!(Arc::ptr_eq(&first.0, &nearby.0), "instances reuse a nearby tessellation");
            let distant = render_at_scale(instance, context, 16.0);
            assert!(!Arc::ptr_eq(&first.0, &distant.0), "large scale changes retessellate");
            let first_weak = Arc::downgrade(&first.0);
            let distant_weak = Arc::downgrade(&distant.0);
            drop((first, nearby, distant));
            let stats = context.library.collect_static_render_resources_with_budget(0);
            assert_eq!(stats.current_entries, 2, "all rendered scale variants join accounting");
            assert!(first_weak.upgrade().is_some() && distant_weak.upgrade().is_some());
            let stats = context.library.collect_static_render_resources_with_budget(0);
            assert_eq!(stats.evicted, 2);
            assert!(first_weak.upgrade().is_none() && distant_weak.upgrade().is_none(),
                "scale metadata must not keep evicted GPU handles alive");
            let reloaded = render_at_scale(graphic, context, 4.0);
            assert_eq!(graphic.0.shared.get().scaled_handle.borrow().len(), 1,
                "reclaimed scales must not retain stale tessellation entries");
            assert_eq!(context.library.collect_static_render_resources_with_budget(0).current_entries, 1,
                "a newly uploaded scale rejoins collection");
            drop(reloaded);
            context.library.collect_static_render_resources();
            assert!(graphic.0.shared.get().render_resource.handle.borrow().is_none());
        });
    }
}

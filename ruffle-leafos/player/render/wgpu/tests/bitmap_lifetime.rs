use std::cell::{Cell, RefCell};
use std::sync::{Arc, Weak};

use ruffle_render::backend::RenderBackend;
use ruffle_render::bitmap::{Bitmap, BitmapFormat, BitmapHandle, BitmapHandleImpl, BitmapSize, BitmapSource, PixelSnapping};
use ruffle_render::commands::{CommandHandler, CommandList};
use ruffle_render::shape_utils::{DistilledShape, DrawCommand, DrawPath, FillRule};
use ruffle_render::transform::Transform;
use ruffle_render_wgpu::backend::WgpuRenderBackend;
use ruffle_render_wgpu::descriptors::Descriptors;
use ruffle_render_wgpu::target::TextureTarget;
use swf::{Color, FillStyle, Matrix, Point, Twips};

#[derive(Default)]
struct WeakBitmapSource {
    handle: RefCell<Option<Weak<dyn BitmapHandleImpl>>>,
    uploads: Cell<usize>,
}

impl BitmapSource for WeakBitmapSource {
    fn bitmap_size(&self, id: u16) -> Option<BitmapSize> {
        (id == 1).then_some(BitmapSize { width: 1, height: 1 })
    }

    fn bitmap_handle(&self, id: u16, renderer: &mut dyn RenderBackend) -> Option<BitmapHandle> {
        self.bitmap_size(id)?;
        if let Some(handle) = self.handle.borrow().as_ref().and_then(Weak::upgrade) {
            return Some(BitmapHandle(handle));
        }
        let handle = renderer.register_bitmap(Bitmap::new(1, 1, BitmapFormat::Rgba, vec![255; 4])).unwrap();
        *self.handle.borrow_mut() = Some(Arc::downgrade(&handle.0));
        self.uploads.set(self.uploads.get() + 1);
        Some(handle)
    }
}

fn renderer() -> WgpuRenderBackend<TextureTarget> {
    let instance = wgpu::Instance::new(&wgpu::InstanceDescriptor {
        backends: wgpu::Backends::NOOP,
        backend_options: wgpu::BackendOptions {
            noop: wgpu::NoopBackendOptions { enable: true },
            ..Default::default()
        },
        ..Default::default()
    });
    let adapter = futures::executor::block_on(instance.request_adapter(&Default::default())).unwrap();
    let (device, queue) = futures::executor::block_on(adapter.request_device(&Default::default())).unwrap();
    let target = TextureTarget::new(&device, (16, 16)).unwrap();
    let descriptors = Arc::new(Descriptors::new(instance, adapter, device, queue));
    WgpuRenderBackend::new(descriptors, target).unwrap()
}

fn shape(style: &FillStyle, id: u16) -> DistilledShape<'_> {
    let point = |x, y| Point::new(Twips::new(x), Twips::new(y));
    DistilledShape {
        id,
        shape_bounds: Default::default(),
        edge_bounds: Default::default(),
        paths: vec![DrawPath::Fill {
            style,
            winding_rule: FillRule::EvenOdd,
            commands: vec![
                DrawCommand::MoveTo(point(0, 0)),
                DrawCommand::LineTo(point(20, 0)),
                DrawCommand::LineTo(point(20, 20)),
                DrawCommand::LineTo(point(0, 20)),
                DrawCommand::LineTo(point(0, 0)),
            ],
        }],
    }
}

#[test]
fn multiple_bitmap_fills_share_while_a_shape_is_being_built() {
    let mut renderer = renderer();
    let source = WeakBitmapSource::default();
    let smooth = FillStyle::Bitmap { id: 1, matrix: Matrix::IDENTITY, is_smoothed: true, is_repeating: false };
    let nearest = FillStyle::Bitmap { id: 1, matrix: Matrix::IDENTITY, is_smoothed: false, is_repeating: false };
    let mut multiple_fills = shape(&smooth, 1);
    multiple_fills.paths.extend(shape(&nearest, 1).paths);
    let shape = renderer.register_shape(multiple_fills, &source);
    let weak = source.handle.borrow().as_ref().unwrap().clone();
    assert_eq!(source.uploads.get(), 1, "pending bitmap draws must share one cache entry");
    assert!(weak.upgrade().is_some());
    drop(shape);
    assert!(weak.upgrade().is_none(), "dropping the shape must release its handle");
}

#[test]
fn bitmap_fills_share_until_the_last_shape_and_command_release_them() {
    let mut renderer = renderer();
    let source = WeakBitmapSource::default();
    let style = FillStyle::Bitmap { id: 1, matrix: Matrix::IDENTITY, is_smoothed: true, is_repeating: false };
    let first = renderer.register_shape(shape(&style, 1), &source);
    let weak = source.handle.borrow().as_ref().unwrap().clone();
    let second = renderer.register_shape(shape(&style, 2), &source);
    assert_eq!(source.uploads.get(), 1, "two distinct shapes must share one uploaded bitmap");
    drop(first);
    let third = renderer.register_shape(shape(&style, 3), &source);
    assert_eq!(source.uploads.get(), 1, "releasing one shape must not invalidate other users");
    let mut commands = CommandList::new();
    commands.render_shape(second.clone(), Transform::default());
    drop(second);
    drop(third);
    assert!(weak.upgrade().is_some(), "queued commands must retain the shape and bitmap");
    drop(commands);
    assert!(weak.upgrade().is_none(), "the cache must not pin unused bitmap handles");
    let reloaded = renderer.register_shape(shape(&style, 4), &source);
    assert_eq!(source.uploads.get(), 2, "fully released textures must be reloadable");
    drop(reloaded);
}

#[test]
fn direct_bitmap_bindings_do_not_keep_their_own_handle_alive() {
    let mut renderer = renderer();
    let source = WeakBitmapSource::default();
    let bitmap = source.bitmap_handle(1, &mut renderer).unwrap();
    let weak = Arc::downgrade(&bitmap.0);
    let mut commands = CommandList::new();
    commands.render_bitmap(bitmap, Transform::default(), true, PixelSnapping::Never);
    renderer.submit_frame(Color::TRANSPARENT, commands, vec![]);
    assert!(weak.upgrade().is_none(), "a bitmap's own bind-group cache must not create an Arc cycle");
}

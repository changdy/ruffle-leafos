use super::*;
use ruffle_render::backend::{null::NullRenderer, ViewportDimensions};
use ruffle_render::bitmap::SyncHandle;
use std::cell::Cell;
use std::rc::Rc;

#[derive(Debug)]
struct PendingReadback(Rc<Cell<bool>>);

impl SyncHandle for PendingReadback {}

impl Drop for PendingReadback {
    fn drop(&mut self) { self.0.set(true); }
}

fn renderer() -> NullRenderer {
    // NullRenderer rejects GPU readbacks. Tests that need old GPU pixels must
    // reach that rejection; a complete overwrite must finish without it.
    NullRenderer::new(ViewportDimensions { width: 2, height: 2, scale_factor: 1.0 })
}

fn gpu_modified<'gc>(mc: &Mutation<'gc>, bitmap: BitmapData<'gc>) -> Rc<Cell<bool>> {
    let dropped = Rc::new(Cell::new(false));
    let whole = PixelRegion::for_whole_size(bitmap.width(), bitmap.height());
    bitmap.overwrite_cpu_pixels_from_gpu(mc).0.borrow_mut(mc)
        .set_gpu_dirty(mc, Box::new(PendingReadback(dropped.clone())), whole);
    dropped
}

#[test]
fn whole_copy_replaces_stale_pixels_and_discards_pending_readback() {
    let arena = gc_arena::Arena::<gc_arena::Rootable![()]>::new(|_| ());
    arena.mutate(|mc, _| {
        let source = BitmapData::new(mc, 2, 2, true, 0x80402010);
        let dest = BitmapData::new(mc, 2, 2, true, 0xffaabbcc);
        let dropped = gpu_modified(mc, dest);
        let whole = PixelRegion::for_whole_size(2, 2);
        let mut renderer = renderer();
        copy_on_cpu(mc, &mut renderer, source, dest, whole, whole, false);
        assert!(dropped.get(), "the discarded GPU result must be released");
        let expected = source.read_area(whole, &mut renderer).get_pixel32_raw(0, 0);
        let pixels = dest.read_area(whole, &mut renderer);
        for y in 0..2 { for x in 0..2 { assert_eq!(pixels.get_pixel32_raw(x, y), expected); } }
        assert!(dest.can_read(whole));
    });
}

#[test]
fn opaque_copy_can_replace_the_whole_destination_without_blending() {
    let arena = gc_arena::Arena::<gc_arena::Rootable![()]>::new(|_| ());
    arena.mutate(|mc, _| {
        let source = BitmapData::new(mc, 2, 2, false, 0x11223344);
        let dest = BitmapData::new(mc, 2, 2, false, 0xffaabbcc);
        let dropped = gpu_modified(mc, dest);
        let whole = PixelRegion::for_whole_size(2, 2);
        let mut renderer = renderer();
        copy_on_cpu(mc, &mut renderer, source, dest, whole, whole, true);
        assert!(dropped.get(), "an opaque source does not need destination blending");
        assert_eq!(u32::from(dest.read_area(whole, &mut renderer).get_pixel32_raw(1, 1)), 0xff223344);
    });
}

#[test]
fn whole_destination_copy_can_read_an_offset_area_of_a_larger_source() {
    let arena = gc_arena::Arena::<gc_arena::Rootable![()]>::new(|_| ());
    arena.mutate(|mc, _| {
        let source = BitmapData::new(mc, 3, 3, true, 0xff112233);
        let dest = BitmapData::new(mc, 2, 2, true, 0xffaabbcc);
        let dropped = gpu_modified(mc, dest);
        let whole = PixelRegion::for_whole_size(2, 2);
        let mut renderer = renderer();
        set_pixel32(mc, &mut renderer, source, 2, 2, 0xff445566);
        copy_on_cpu(mc, &mut renderer, source, dest,
            PixelRegion::for_region(1, 1, 2, 2), whole, false);
        assert!(dropped.get());
        let pixels = dest.read_area(whole, &mut renderer);
        assert_eq!(u32::from(pixels.get_pixel32_raw(0, 0)), 0xff112233);
        assert_eq!(u32::from(pixels.get_pixel32_raw(1, 1)), 0xff445566);
    });
}

#[test]
fn identical_self_copy_keeps_the_gpu_pixels_and_pending_readback() {
    let arena = gc_arena::Arena::<gc_arena::Rootable![()]>::new(|_| ());
    arena.mutate(|mc, _| {
        let bitmap = BitmapData::new(mc, 2, 2, true, 0);
        let dropped = gpu_modified(mc, bitmap);
        let whole = PixelRegion::for_whole_size(2, 2);
        copy_on_cpu(mc, &mut renderer(), bitmap, bitmap, whole, whole, false);
        assert!(!dropped.get());
        assert!(!bitmap.can_read(whole));
    });
}

#[test]
#[should_panic(expected = "Failed to sync BitmapData")]
fn partial_copy_still_reads_back_untouched_destination_pixels() {
    let arena = gc_arena::Arena::<gc_arena::Rootable![()]>::new(|_| ());
    arena.mutate(|mc, _| {
        let source = BitmapData::new(mc, 2, 2, true, 0);
        let dest = BitmapData::new(mc, 2, 2, true, 0);
        gpu_modified(mc, dest);
        let top = PixelRegion::for_region(0, 0, 2, 1);
        copy_on_cpu(mc, &mut renderer(), source, dest, top, top, false);
    });
}

#[test]
#[should_panic(expected = "Failed to sync BitmapData")]
fn blended_whole_copy_still_reads_back_destination_pixels() {
    let arena = gc_arena::Arena::<gc_arena::Rootable![()]>::new(|_| ());
    arena.mutate(|mc, _| {
        let source = BitmapData::new(mc, 2, 2, true, 0x80402010);
        let dest = BitmapData::new(mc, 2, 2, true, 0);
        gpu_modified(mc, dest);
        let whole = PixelRegion::for_whole_size(2, 2);
        copy_on_cpu(mc, &mut renderer(), source, dest, whole, whole, true);
    });
}

#[test]
#[should_panic(expected = "Failed to sync BitmapData")]
fn whole_overwrite_still_reads_back_a_gpu_modified_source() {
    let arena = gc_arena::Arena::<gc_arena::Rootable![()]>::new(|_| ());
    arena.mutate(|mc, _| {
        let source = BitmapData::new(mc, 2, 2, true, 0);
        let dest = BitmapData::new(mc, 2, 2, true, 0);
        gpu_modified(mc, source);
        let whole = PixelRegion::for_whole_size(2, 2);
        copy_on_cpu(mc, &mut renderer(), source, dest, whole, whole, false);
    });
}

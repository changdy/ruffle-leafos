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

#[test]
fn self_copy_matches_flash_reference_in_all_directions_and_clipping() {
    // Flash Player reference output and input matrix from ruffle-rs/ruffle
    // #24676, head a211d2b00fc8bd21d50221d648c65f084edd8784:
    // tests/tests/swfs/avm2/bitmapdata_copypixels_self/{Test.as,output.txt}.
    // In particular, dx<0/dy>0 intentionally reads overwritten rows in Flash;
    // a snapshot or unconditional memmove of the whole rectangle is incorrect.
    let expected = include_str!("test_data/copypixels_self_flash.txt");
    let positions = [
        (2, 2), (3, 3), (1, 1), (3, 1), (1, 3), (2, 3), (2, 1),
        (3, 2), (1, 2), (6, 6), (6, 2), (2, 6), (-2, -2),
        (-2, 2), (2, -2), (6, -2), (-2, 6),
    ];
    let mut lines = expected.lines().filter(|line| !line.is_empty());
    let mut cases = 0;
    while let Some(label) = lines.next() {
        let merge_alpha = label.starts_with("+mergeAlpha");
        let transparent = label.contains(", +transparent");
        let dest_point = positions[cases % positions.len()];
        let mut expected_pixels = Vec::new();
        for _ in 0..8 {
            expected_pixels.extend(
                lines.next().unwrap().split_whitespace()
                    .map(|pixel| u32::from_str_radix(pixel, 16).unwrap()),
            );
        }
        let arena = gc_arena::Arena::<gc_arena::Rootable![()]>::new(|_| ());
        arena.mutate(|mc, _| {
            let bitmap = BitmapData::new(mc, 8, 8, transparent, 0x7f000000);
            let mut renderer = renderer();
            for y in 0..8 {
                for x in 0..8 {
                    set_pixel(
                        mc, &mut renderer, bitmap, x, y,
                        Color::from((y + 1) * 16 + (x + 1)),
                    );
                }
            }
            let mut source_region = PixelRegion::for_whole_size(8, 8);
            let mut dest_region = PixelRegion::for_whole_size(8, 8);
            dest_region.clamp_with_intersection(
                dest_point, (2, 2), (4, 4), &mut source_region,
            );
            copy_on_cpu(
                mc, &mut renderer, bitmap, bitmap,
                source_region, dest_region, merge_alpha,
            );
            for y in 0..8 {
                for x in 0..8 {
                    let actual = if transparent {
                        get_pixel32(bitmap, &mut renderer, x, y)
                    } else {
                        get_pixel(bitmap, &mut renderer, x, y)
                    };
                    assert_eq!(actual, expected_pixels[(y * 8 + x) as usize],
                        "{label}, pixel ({x}, {y})");
                }
            }
        });
        cases += 1;
    }
    assert_eq!(cases, 68);
}

#[test]
fn partial_copy_respects_both_row_strides_and_preserves_neighbors() {
    let arena = gc_arena::Arena::<gc_arena::Rootable![()]>::new(|_| ());
    arena.mutate(|mc, _| {
        let source = BitmapData::new(mc, 4, 3, true, 0);
        let dest = BitmapData::new(mc, 5, 4, true, 0xffaabbcc);
        let mut renderer = renderer();
        for (x, y, color) in [
            (1, 1, 0xff112233), (2, 1, 0xff445566),
            (1, 2, 0xff778899), (2, 2, 0xffddeeff),
        ] {
            set_pixel32(mc, &mut renderer, source, x, y, color);
        }
        copy_on_cpu(
            mc, &mut renderer, source, dest,
            PixelRegion::for_region(1, 1, 2, 2),
            PixelRegion::for_region(2, 1, 2, 2), false,
        );
        let expected = [
            0xffaabbcc, 0xffaabbcc, 0xffaabbcc, 0xffaabbcc, 0xffaabbcc,
            0xffaabbcc, 0xffaabbcc, 0xff112233, 0xff445566, 0xffaabbcc,
            0xffaabbcc, 0xffaabbcc, 0xff778899, 0xffddeeff, 0xffaabbcc,
            0xffaabbcc, 0xffaabbcc, 0xffaabbcc, 0xffaabbcc, 0xffaabbcc,
        ];
        for y in 0..4 {
            for x in 0..5 {
                assert_eq!(get_pixel32(dest, &mut renderer, x, y),
                    expected[(y * 5 + x) as usize], "pixel ({x}, {y})");
            }
        }
    });
}

#[test]
#[should_panic(expected = "Failed to sync BitmapData")]
fn overlapping_self_copy_still_reads_back_gpu_modified_pixels() {
    let arena = gc_arena::Arena::<gc_arena::Rootable![()]>::new(|_| ());
    arena.mutate(|mc, _| {
        let bitmap = BitmapData::new(mc, 2, 2, true, 0);
        gpu_modified(mc, bitmap);
        copy_on_cpu(
            mc, &mut renderer(), bitmap, bitmap,
            PixelRegion::for_region(0, 0, 1, 2),
            PixelRegion::for_region(1, 0, 1, 2), false,
        );
    });
}

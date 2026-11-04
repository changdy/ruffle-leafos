//! Compare DefineFont2/3 metadata loading with eager outline decoding.
//! Pass SWF files or directories. Verification expands every deferred outline;
//! timings and allocation requests cover font parsing only, excluding SWF I/O,
//! decompression, ASCII preloading, rendering, and later first-use decoding.

use std::alloc::{GlobalAlloc, Layout, System};
use std::hint::black_box;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering::Relaxed};
use std::time::Instant;
use swf::read::Reader;

struct CountingAllocator;
static TRACK: AtomicBool = AtomicBool::new(false);
static REQUESTED: AtomicUsize = AtomicUsize::new(0);

fn count(size: usize) {
    if TRACK.load(Relaxed) {
        REQUESTED.fetch_add(size, Relaxed);
    }
}

// This single-threaded executable counts requested bytes, not RSS or peak heap.
unsafe impl GlobalAlloc for CountingAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        count(layout.size());
        unsafe { System.alloc(layout) }
    }

    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        count(layout.size());
        unsafe { System.alloc_zeroed(layout) }
    }

    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        unsafe { System.dealloc(ptr, layout) }
    }

    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        count(new_size);
        unsafe { System.realloc(ptr, layout, new_size) }
    }
}

#[global_allocator]
static ALLOCATOR: CountingAllocator = CountingAllocator;

fn files(path: &Path, out: &mut Vec<PathBuf>) -> std::io::Result<()> {
    if path.is_dir() {
        for entry in std::fs::read_dir(path)? {
            let entry = entry?;
            if !entry.file_type()?.is_symlink() {
                files(&entry.path(), out)?;
            }
        }
    } else if path.extension().is_some_and(|s| s.eq_ignore_ascii_case("swf")) {
        out.push(path.to_owned());
    }
    Ok(())
}

fn font_tags<'a>(data: &'a [u8], version: u8, out: &mut Vec<(&'a [u8], u8)>) -> Result<(), String> {
    let mut reader = Reader::new(data, version);
    while !reader.get_ref().is_empty() {
        let (code, len) = reader.read_tag_code_and_length().map_err(|e| e.to_string())?;
        if len > reader.get_ref().len() {
            return Err(format!("Truncated tag {code}"));
        }
        let (body, rest) = reader.get_ref().split_at(len);
        *reader.get_mut() = rest;
        match code {
            0 => break,
            48 => out.push((body, 2)),
            75 => out.push((body, 3)),
            39 if body.len() >= 4 => font_tags(&body[4..], version, out)?,
            _ => {}
        }
    }
    Ok(())
}

fn measure(mut run: impl FnMut()) -> (u128, usize) {
    const REPEATS: usize = 5;
    REQUESTED.store(0, Relaxed);
    TRACK.store(true, Relaxed);
    let at = Instant::now();
    for _ in 0..REPEATS {
        run();
    }
    let elapsed = at.elapsed().as_micros() / REPEATS as u128;
    TRACK.store(false, Relaxed);
    (elapsed, REQUESTED.load(Relaxed) / REPEATS)
}

fn main() {
    let mut inputs = Vec::new();
    for path in std::env::args_os().skip(1) {
        files(Path::new(&path), &mut inputs).expect("input path");
    }
    assert!(!inputs.is_empty(), "Pass SWF files or directories");
    inputs.sort();
    inputs.dedup();
    println!("file\tfonts\tfallbacks\tglyphs\trecords\teager_us\tlazy_us\teager_requested_bytes\tlazy_requested_bytes");
    for path in inputs {
        let bytes = std::fs::read(&path).unwrap();
        let movie = swf::decompress_swf(&bytes[..]).expect("SWF decompression");
        let version = movie.header.version();
        let mut tags = Vec::new();
        if let Err(error) = font_tags(&movie.data, version, &mut tags) {
            eprintln!("Skipping {}: {error}", path.display());
            continue;
        }
        let (mut glyphs, mut records, mut fallbacks) = (0, 0, 0);
        for &(body, font_version) in &tags {
            let eager = Reader::new(body, version).read_define_font_2(font_version).unwrap();
            glyphs += eager.glyphs.len();
            records += eager.glyphs.iter().map(|g| g.shape_records.len()).sum::<usize>();
            match Reader::new(body, version).read_define_font_2_encoded(font_version) {
                Ok(mut lazy) => {
                    for (glyph, outline) in lazy.font.glyphs.iter_mut().zip(lazy.glyph_shapes) {
                        glyph.shape_records = Reader::new(outline, version).read_font_glyph_shape().unwrap();
                    }
                    assert_eq!(lazy.font, eager, "{}", path.display());
                }
                Err(_) => fallbacks += 1,
            }
        }
        if tags.is_empty() {
            continue;
        }
        let (eager_us, eager_bytes) = measure(|| {
            for &(body, v) in &tags {
                black_box(Reader::new(body, version).read_define_font_2(v).unwrap());
            }
        });
        let (lazy_us, lazy_bytes) = measure(|| {
            for &(body, v) in &tags {
                match Reader::new(body, version).read_define_font_2_encoded(v) {
                    Ok(font) => { black_box(font); }
                    Err(_) => { black_box(Reader::new(body, version).read_define_font_2(v).unwrap()); }
                }
            }
        });
        println!("{}\t{}\t{fallbacks}\t{glyphs}\t{records}\t{eager_us}\t{lazy_us}\t{eager_bytes}\t{lazy_bytes}", path.display(), tags.len());
    }
}

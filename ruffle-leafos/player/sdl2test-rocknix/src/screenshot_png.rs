//! Bounded PNG encoding of an OpenGL bottom-up RGBA readback. The output is
//! opaque RGB; only one RGB row and the streaming encoder buffers are retained.
use std::io::{self, Write};

pub fn write_rgb(output: impl Write, width: u32, height: u32, rgba: &[u8]) -> io::Result<()> {
    let stride = (width as usize).checked_mul(4).ok_or_else(|| io::Error::other("Screenshot is too large"))?;
    if width == 0 || height == 0 || stride.checked_mul(height as usize) != Some(rgba.len()) {
        return Err(io::Error::new(io::ErrorKind::InvalidInput, "Invalid screenshot dimensions or pixels"));
    }
    let row_len = width as usize * 3;
    let mut row = Vec::new();
    row.try_reserve_exact(row_len).map_err(io::Error::other)?;
    row.resize(row_len, 0);
    let mut encoder = png::Encoder::new(output, width, height);
    encoder.set_color(png::ColorType::Rgb);
    encoder.set_depth(png::BitDepth::Eight);
    let mut writer = encoder.write_header().map_err(io::Error::other)?;
    let mut stream = writer.stream_writer_with_size(32 * 1024).map_err(io::Error::other)?;
    for source in rgba.chunks_exact(stride).rev() {
        for (pixel, rgb) in source.chunks_exact(4).zip(row.chunks_exact_mut(3)) {
            rgb.copy_from_slice(&pixel[..3]);
        }
        stream.write_all(&row)?;
    }
    stream.finish().map_err(io::Error::other)?;
    writer.finish().map_err(io::Error::other)
}

use std::cell::RefCell;
use std::sync::{Arc, Weak};

use crate::backend::audio::SoundHandle;
use crate::binary_data::BinaryData;
use crate::display_object::{
    Avm1Button, Avm2Button, BitmapClass, EditText, Graphic, MorphShape, MovieClip, Text, Video,
};
use crate::font::Font;
use gc_arena::barrier::unlock;
use gc_arena::lock::Lock;
use gc_arena::{Collect, Gc, Mutation};
use ruffle_render::backend::RenderBackend;
use ruffle_render::bitmap::{Bitmap as RenderBitmap, BitmapHandle, BitmapHandleImpl, BitmapSize};
use ruffle_render::error::Error as RenderError;
use swf::DefineBitsLossless;

#[derive(Copy, Clone, Collect, Debug)]
#[collect(no_drop)]
pub enum Character<'gc> {
    EditText(EditText<'gc>),
    Graphic(Graphic<'gc>),
    MovieClip(MovieClip<'gc>),
    Bitmap(Gc<'gc, BitmapCharacter<'gc>>),
    Avm1Button(Avm1Button<'gc>),
    Avm2Button(Avm2Button<'gc>),
    Font(Font<'gc>),
    MorphShape(MorphShape<'gc>),
    Text(Text<'gc>),
    Sound(#[collect(require_static)] SoundHandle),
    Video(Video<'gc>),
    BinaryData(Gc<'gc, BinaryData>),
}

#[derive(Collect, Debug)]
#[collect(no_drop)]
pub struct BitmapCharacter<'gc> {
    #[collect(require_static)]
    compressed: Arc<CompressedBitmap>,
    /// A lazily constructed GPU handle, used when performing fills with this bitmap
    #[collect(require_static)]
    handle: RefCell<Option<Weak<dyn BitmapHandleImpl>>>,
    /// The bitmap class set by `SymbolClass` - this is used when we instantaite
    /// a `Bitmap` displayobject.
    avm2_class: Lock<BitmapClass<'gc>>,
}

impl<'gc> BitmapCharacter<'gc> {
    pub fn new(compressed: CompressedBitmap) -> Self {
        Self {
            compressed: Arc::new(compressed),
            handle: RefCell::default(),
            avm2_class: Lock::new(BitmapClass::NoSubclass),
        }
    }

    pub fn compressed(&self) -> &CompressedBitmap {
        &self.compressed
    }

    pub(crate) fn predecode(&self) {
        if self.handle.borrow().as_ref().and_then(Weak::upgrade).is_none() {
            let source: Arc<dyn ruffle_render::bitmap_cache::BitmapDecoder> = self.compressed.clone();
            ruffle_render::bitmap_cache::predecode(&source);
        }
    }

    pub fn avm2_class(&self) -> BitmapClass<'gc> {
        self.avm2_class.get()
    }

    pub fn set_avm2_class(this: Gc<'gc, Self>, bitmap_class: BitmapClass<'gc>, mc: &Mutation<'gc>) {
        unlock!(Gc::write(mc, this), Self, avm2_class).set(bitmap_class);
    }

    pub fn bitmap_handle(
        &self,
        backend: &mut dyn RenderBackend,
    ) -> Result<BitmapHandle, RenderError> {
        if let Some(handle) = self.handle.borrow().as_ref().and_then(Weak::upgrade) {
            return Ok(BitmapHandle(handle));
        }
        let start = crate::render_metrics::start();
        let source: Arc<dyn ruffle_render::bitmap_cache::BitmapDecoder> = self.compressed.clone();
        let decoded = ruffle_render::bitmap_cache::decode(&source)?;
        let upload = crate::render_metrics::start();
        let new_handle = backend.register_bitmap(RenderBitmap::new(
            decoded.width(), decoded.height(), decoded.format(), decoded.data()))?;
        let size = self.compressed.size();
        crate::render_metrics::bitmap(start, upload,
            u64::from(size.width) * u64::from(size.height) * 4,
            self.handle.borrow().is_some());
        // Shapes and in-flight commands own the texture. The library keeps only
        // a weak reference so releasing unused shapes also releases bitmap fills.
        *self.handle.borrow_mut() = Some(Arc::downgrade(&new_handle.0));
        Ok(new_handle)
    }
}

/// Holds a bitmap from an SWF tag, plus the decoded width/height.
/// We avoid decompressing the image until it's actually needed - some pathological SWFS
/// like 'House' have thousands of highly-compressed (mostly empty) bitmaps, which can
/// take over 10GB of ram if we decompress them all during preloading.
#[derive(Clone, Debug)]
pub enum CompressedBitmap {
    Jpeg {
        data: Vec<u8>,
        alpha: Option<Vec<u8>>,
        width: u32,
        height: u32,
    },
    Lossless(DefineBitsLossless<'static>),
}

impl CompressedBitmap {
    pub fn size(&self) -> BitmapSize {
        match self {
            CompressedBitmap::Jpeg { width, height, .. } => BitmapSize {
                width: *width,
                height: *height,
            },
            CompressedBitmap::Lossless(define_bits_lossless) => BitmapSize {
                width: define_bits_lossless.width.into(),
                height: define_bits_lossless.height.into(),
            },
        }
    }
    pub fn decode(&self) -> Result<RenderBitmap<'static>, RenderError> {
        match self {
            CompressedBitmap::Jpeg {
                data,
                alpha,
                width: _,
                height: _,
            } => ruffle_render::utils::decode_define_bits_jpeg(data, alpha.as_deref()),
            CompressedBitmap::Lossless(define_bits_lossless) => {
                ruffle_render::utils::decode_define_bits_lossless(define_bits_lossless)
            }
        }
    }
}

impl ruffle_render::bitmap_cache::BitmapDecoder for CompressedBitmap {
    fn decode(&self) -> Result<RenderBitmap<'static>, RenderError> { self.decode() }

    fn predecode_bytes(&self) -> Option<usize> {
        // Only Lossless has a bounded speculative decoder. JPEG/PNG/GIF remain
        // demand decoded, and may still benefit from the pixel cache afterwards.
        if let Self::Lossless(tag) = self {
            if tag.data.len() <= 1024 * 1024 {
                return usize::try_from(u64::from(tag.width) * u64::from(tag.height) * 4).ok();
            }
        }
        None
    }

    fn decode_speculative(&self) -> Result<RenderBitmap<'static>, RenderError> {
        match self {
            Self::Lossless(tag) => ruffle_render::utils::decode_define_bits_lossless_bounded(tag, 1024 * 1024),
            _ => Err(RenderError::TooLarge),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use flate2::{Compression, write::ZlibEncoder};
    use ruffle_render::backend::{ViewportDimensions, null::NullRenderer};
    use std::{borrow::Cow, io::Write};

    #[test]
    fn static_bitmap_cache_does_not_keep_unused_textures_alive() {
        let mut data = ZlibEncoder::new(Vec::new(), Compression::default());
        data.write_all(&[255, 20, 30, 40]).unwrap();
        let bitmap = BitmapCharacter::new(CompressedBitmap::Lossless(DefineBitsLossless {
            version: 2, id: 1, format: swf::BitmapFormat::Rgb32,
            width: 1, height: 1, data: Cow::Owned(data.finish().unwrap()),
        }));
        let mut renderer = NullRenderer::new(ViewportDimensions {
            width: 16, height: 16, scale_factor: 1.0,
        });
        let first = bitmap.bitmap_handle(&mut renderer).unwrap();
        let second = bitmap.bitmap_handle(&mut renderer).unwrap();
        assert_eq!(first, second, "live fills must share a texture");
        let weak = Arc::downgrade(&first.0);
        drop(first);
        assert!(weak.upgrade().is_some());
        drop(second);
        assert!(weak.upgrade().is_none(), "compressed assets must not pin GPU memory");
        assert!(bitmap.bitmap_handle(&mut renderer).is_ok(), "evicted textures must reload");
    }
}

use super::*;
use crate::font::{Font, FontLike, FontType};
use crate::tag_utils::SwfMovie;
use std::sync::Arc;
use ruffle_render::backend::ViewportDimensions;
use ruffle_render::backend::null::NullRenderer;

fn movie(version: u8) -> Arc<SwfMovie> {
    let square = |size| {
        vec![
            swf::ShapeRecord::StyleChange(Box::new(swf::StyleChangeData {
                move_to: Some(swf::Point::new(Twips::ZERO, Twips::ZERO)),
                fill_style_0: Some(1),
                fill_style_1: None,
                line_style: None,
                new_styles: None,
            })),
            swf::ShapeRecord::StraightEdge {
                delta: swf::PointDelta::new(Twips::new(size), Twips::ZERO),
            },
            swf::ShapeRecord::StraightEdge {
                delta: swf::PointDelta::new(Twips::ZERO, Twips::new(size)),
            },
            swf::ShapeRecord::StraightEdge {
                delta: swf::PointDelta::new(Twips::new(-size), Twips::ZERO),
            },
            swf::ShapeRecord::StraightEdge {
                delta: swf::PointDelta::new(Twips::ZERO, Twips::new(-size)),
            },
        ]
    };
    let font = swf::Font {
        version,
        id: 1,
        name: swf::SwfStr::from_utf8_str("Lazy test"),
        language: swf::Language::Unknown,
        flags: swf::FontFlag::HAS_LAYOUT | swf::FontFlag::HAS_WIDE_CODES,
        glyphs: [65u16, 0x4e2d, 0x4e2d, 0xd800]
            .into_iter()
            .enumerate()
            .map(|(i, code)| swf::Glyph {
                shape_records: square(1000 + i as i32 * 100),
                code,
                advance: 1000 + i as u16,
                bounds: Some(swf::Rectangle {
                    x_min: Twips::ZERO,
                    x_max: Twips::new(1400),
                    y_min: Twips::ZERO,
                    y_max: Twips::new(1400),
                }),
            })
            .collect(),
        layout: Some(swf::FontLayout {
            ascent: 2000,
            descent: 200,
            leading: -4,
            kerning: vec![swf::KerningRecord {
                left_code: 65,
                right_code: 0x4e2d,
                adjustment: Twips::new(-20),
            }],
        }),
    };
    let mut bytes = Vec::new();
    swf::write_swf(
        &swf::Header {
            compression: swf::Compression::None,
            version: 10,
            stage_size: Default::default(),
            frame_rate: swf::Fixed8::from_f32(30.0),
            num_frames: 1,
        },
        &[
            swf::Tag::DefineFont2(Box::new(font)),
            swf::Tag::Unknown {
                tag_code: 255,
                data: &[0x10, 0xff],
            },
        ],
        &mut bytes,
    )
    .unwrap();
    Arc::new(SwfMovie::from_data(&bytes, "file:///lazy-font-test.swf".into(), None, None).unwrap())
}

fn renderer() -> NullRenderer {
    NullRenderer::new(ViewportDimensions {
        width: 640,
        height: 480,
        scale_factor: 1.0,
    })
}

fn font_body(movie: &SwfMovie) -> &[u8] {
    let mut reader = swf::read::Reader::new(movie.data(), movie.version());
    let (_, len) = reader.read_tag_code_and_length().unwrap();
    &reader.get_ref()[..len]
}

#[test]
fn lazy_glyphs_preserve_metrics_indices_hit_tests_and_cache_first_use() {
    for version in [2, 3] {
        gc_arena::arena::rootless_mutate(|mc| {
            let movie = movie(version);
            let body = font_body(&movie);
            let eager_tag = swf::read::Reader::new(body, 10)
                .read_define_font_2(version)
                .unwrap();
            let encoded = swf::read::Reader::new(body, 10)
                .read_define_font_2_encoded(version)
                .unwrap();
            let mut renderer = renderer();
            let eager = Font::from_swf_tag(
                mc,
                &mut renderer,
                eager_tag,
                swf::UTF_8,
                FontType::Embedded,
            );
            let lazy = Font::from_encoded_swf_tag(
                mc,
                &mut renderer,
                encoded,
                movie.clone(),
                swf::UTF_8,
                FontType::Embedded,
            );
            assert_eq!(lazy.scale(), eager.scale());
            assert_eq!(lazy.metrics().ascent, eager.metrics().ascent);
            assert_eq!(lazy.metrics().descent, eager.metrics().descent);
            assert_eq!(lazy.metrics().leading, eager.metrics().leading);
            assert_eq!(lazy.descriptor().name(), eager.descriptor().name());
            assert_eq!(
                lazy.get_kerning_offset('A', '中'),
                eager.get_kerning_offset('A', '中')
            );
            // A duplicate Unicode code resolves to the last glyph, as before.
            assert_eq!(
                lazy.get_glyph_for_char('中').unwrap().advance(),
                Twips::new(1002)
            );
            assert_eq!(
                lazy.get_glyph(3).unwrap().character(),
                char::REPLACEMENT_CHARACTER
            );
            let ascii = lazy.get_glyph(0).unwrap();
            let GlyphShape::Swf(ascii) = &ascii.shape else {
                panic!("Expected SWF glyph")
            };
            assert!(matches!(&*ascii.borrow(), SwfGlyphOrShape::Shape { handle: Some(_), .. }));
            assert!(lazy.get_glyph_for_char('Z').is_none());
            assert!(lazy.get_glyph(4).is_none());
            let glyph = lazy.get_glyph(1).unwrap();
            let GlyphShape::Swf(state) = &glyph.shape else {
                panic!("Expected SWF glyph")
            };
            assert!(matches!(&*state.borrow(), SwfGlyphOrShape::Encoded { .. }));
            assert_eq!(glyph.advance(), eager.get_glyph(1).unwrap().advance());
            // The encoded outline owns its movie even after the loader's
            // original strong reference goes away.
            drop(movie);
            for (x, y) in [(500, 500), (-100, 500), (2000, 500)] {
                let point = swf::Point::new(Twips::new(x), Twips::new(y));
                assert_eq!(
                    glyph.hit_test(point, &Matrix::IDENTITY),
                    eager
                        .get_glyph(1)
                        .unwrap()
                        .hit_test(point, &Matrix::IDENTITY)
                );
            }
            let records = match &*state.borrow() {
                SwfGlyphOrShape::Shape { shape, handle } => {
                    assert!(handle.is_none());
                    shape.shape.as_ptr()
                }
                _ => panic!("Expected decoded outline"),
            };
            assert!(glyph.glyph_render_data(&mut renderer).is_some());
            assert!(glyph.glyph_render_data(&mut renderer).is_some());
            let state = state.borrow();
            let SwfGlyphOrShape::Shape { shape, handle } = &*state else {
                panic!("Expected cached shape")
            };
            assert_eq!(shape.shape.as_ptr(), records);
            assert!(handle.is_some());
        });
    }
}

#[test]
fn malformed_deferred_outline_is_cached_empty_without_affecting_other_glyphs() {
    gc_arena::arena::rootless_mutate(|mc| {
        let movie = movie(3);
        let body = font_body(&movie);
        let mut tag = swf::read::Reader::new(body, 10)
            .read_define_font_2_encoded(3)
            .unwrap();
        let end = movie.data().len();
        tag.glyph_shapes[1] = &movie.data()[end - 4..end - 2];
        assert_eq!(tag.glyph_shapes[1], &[0x10, 0xff]);
        let mut renderer = renderer();
        let font = Font::from_encoded_swf_tag(
            mc,
            &mut renderer,
            tag,
            movie.clone(),
            swf::UTF_8,
            FontType::Embedded,
        );
        let glyph = font.get_glyph(1).unwrap();
        let point = swf::Point::new(Twips::new(500), Twips::new(500));
        assert!(!glyph.hit_test(point, &Matrix::IDENTITY));
        assert!(!glyph.hit_test(point, &Matrix::IDENTITY));
        let GlyphShape::Swf(state) = &glyph.shape else {
            panic!("Expected SWF glyph")
        };
        assert!(
            matches!(&*state.borrow(), SwfGlyphOrShape::Shape { shape, .. } if shape.shape.is_empty())
        );
        assert_eq!(glyph.advance(), Twips::new(1001));
        assert!(
            font.get_glyph(2)
                .unwrap()
                .hit_test(point, &Matrix::IDENTITY)
        );
    });
}

#[test]
fn player_preloads_encoded_fonts_and_falls_back_for_irregular_offsets() {
    use crate::character::Character;
    use crate::limits::ExecutionLimit;
    use crate::player::PlayerBuilder;

    for version in [2, 3] {
        for fallback in [false, true] {
            let original = movie(version);
            let mut body = font_body(&original).to_vec();
            if fallback {
                // Insert padding before the outlines and adjust all offsets.
                // The legacy reader accepts this nonstandard offset table.
                let table = 7 + usize::from(body[4]);
                let glyphs = usize::from(u16::from_le_bytes(body[table - 2..table].try_into().unwrap()));
                assert_eq!(body[2] & swf::FontFlag::HAS_WIDE_OFFSETS.bits(), 0);
                for i in 0..=glyphs {
                    let offset = table + i * 2;
                    let value = u16::from_le_bytes(body[offset..offset + 2].try_into().unwrap());
                    body[offset..offset + 2].copy_from_slice(&(value + 1).to_le_bytes());
                }
                body.insert(table + (glyphs + 1) * 2, 0);
            }
            let mut bytes = Vec::new();
            swf::write_swf(
                &swf::Header {
                    compression: swf::Compression::None,
                    version: 10,
                    stage_size: Default::default(),
                    frame_rate: swf::Fixed8::from_f32(30.0),
                    num_frames: 1,
                },
                &[
                    swf::Tag::Unknown { tag_code: if version == 3 { 75 } else { 48 }, data: &body },
                    swf::Tag::ShowFrame,
                    swf::Tag::End,
                ],
                &mut bytes,
            ).unwrap();
            let movie = SwfMovie::from_data(&bytes, "file:///font-preload.swf".into(), None, None).unwrap();
            let player = PlayerBuilder::new().with_movie(movie).build();
            let mut player = player.lock().unwrap();
            player.preload(&mut ExecutionLimit::none());
            player.mutate_with_update_context(|context| {
                let library = context.library.library_for_movie(context.root_swf.clone()).unwrap();
                let Some(Character::Font(font)) = library.character_by_id(1) else {
                    panic!("Font was not registered");
                };
                let glyph = font.get_glyph(1).unwrap();
                let GlyphShape::Swf(state) = &glyph.shape else {
                    panic!("Expected SWF glyph");
                };
                assert_eq!(matches!(&*state.borrow(), SwfGlyphOrShape::Encoded { .. }), !fallback);
                assert!(glyph.hit_test(Point::new(Twips::new(500), Twips::new(500)), &Matrix::IDENTITY));
                assert_eq!(glyph.advance(), Twips::new(1001));
                assert_eq!(font.scale(), if version == 3 { 20480.0 } else { 1024.0 });
            });
        }
    }
}

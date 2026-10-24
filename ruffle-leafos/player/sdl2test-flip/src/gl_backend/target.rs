use ruffle_core::swf::Color;
use ruffle_render::commands::RenderBlendMode;

use super::MaskState;

#[derive(Clone, Copy, Debug)]
pub(super) enum GlTarget {
    DefaultFramebuffer,
    Texture(glow::Texture),
}

#[derive(Clone, Copy, Debug)]
pub(super) struct GlSurface {
    pub(super) target: GlTarget,
    pub(super) width: i32,
    pub(super) height: i32,
    pub(super) clear_color: Option<[f32; 4]>,
    pub(super) y_flip: bool,
}

impl GlSurface {
    pub(super) fn screen(
        width: i32,
        height: i32,
        clear: Color,
        is_transparent: bool,
        normalize_color: impl FnOnce(Color) -> [f32; 4],
    ) -> Self {
        Self {
            target: GlTarget::DefaultFramebuffer,
            width: width.max(1),
            height: height.max(1),
            clear_color: Some(if is_transparent {
                [0.0, 0.0, 0.0, 0.0]
            } else {
                normalize_color(clear)
            }),
            y_flip: true,
        }
    }

    pub(super) fn texture(
        texture: glow::Texture,
        width: u32,
        height: u32,
        clear_color: Option<Color>,
        normalize_color: impl Fn(Color) -> [f32; 4],
    ) -> Self {
        Self {
            target: GlTarget::Texture(texture),
            width: width.max(1) as i32,
            height: height.max(1) as i32,
            clear_color: clear_color.map(normalize_color),
            y_flip: false,
        }
    }

    pub(super) fn view_matrix(self) -> [[f32; 4]; 4] {
        let width = self.width.max(1) as f32;
        let height = self.height.max(1) as f32;
        if self.y_flip {
            [
                [1.0 / (width / 2.0), 0.0, 0.0, 0.0],
                [0.0, -1.0 / (height / 2.0), 0.0, 0.0],
                [0.0, 0.0, 1.0, 0.0],
                [-1.0, 1.0, 0.0, 1.0],
            ]
        } else {
            [
                [1.0 / (width / 2.0), 0.0, 0.0, 0.0],
                [0.0, 1.0 / (height / 2.0), 0.0, 0.0],
                [0.0, 0.0, 1.0, 0.0],
                [-1.0, -1.0, 0.0, 1.0],
            ]
        }
    }
}

#[derive(Debug)]
pub(super) struct GlSurfaceState {
    pub(super) renderbuffer_width: i32,
    pub(super) renderbuffer_height: i32,
    pub(super) view_matrix: [[f32; 4]; 4],
    pub(super) blend_modes: Vec<RenderBlendMode>,
    pub(super) mask_state: MaskState,
    pub(super) num_masks: u32,
    pub(super) active_surface: Option<GlSurface>,
}

use std::any::Any;
use std::cell::Cell;
use std::collections::HashMap;
use std::num::NonZeroU32;
use std::sync::{Arc, Weak};
use std::borrow::Cow;

use bytemuck::{Pod, Zeroable};
use glow::HasContext;
use ruffle_core::swf::{self, BlendMode, Color, GradientInterpolation, GradientSpread, Twips};
use ruffle_render::backend::{
    BitmapCacheEntry, Context3D, Context3DProfile, PixelBenderOutput, PixelBenderTarget,
    RenderBackend, ShapeHandle, ShapeHandleImpl, ViewportDimensions,
};
use ruffle_render::bitmap::{
    Bitmap, BitmapFormat, BitmapHandle, BitmapHandleImpl, BitmapSource, PixelRegion, PixelSnapping,
    RgbaBufRead, SyncHandle,
};
use ruffle_render::commands::{CommandHandler, CommandList, RenderBlendMode};
use ruffle_render::error::Error as BitmapError;
use ruffle_render::filters::{DisplacementMapFilterMode, Filter};
use ruffle_render::matrix::Matrix;
use ruffle_render::quality::StageQuality;
use ruffle_render::shape_utils::{DistilledShape, GradientType};
use ruffle_render::tessellator::{
    Gradient as TessGradient, ShapeTessellator, Vertex as TessVertex,
};
use ruffle_render::transform::Transform;

const COLOR_VERTEX_GLSL: &str = include_str!("../shaders/color.vert");
const COLOR_FRAGMENT_GLSL: &str = include_str!("../shaders/color.frag");
const TEXTURE_VERTEX_GLSL: &str = include_str!("../shaders/texture.vert");
const GRADIENT_FRAGMENT_GLSL: &str = include_str!("../shaders/gradient.frag");
const BITMAP_FRAGMENT_GLSL: &str = include_str!("../shaders/bitmap.frag");
const FILTER_COMMON_VERTEX_GLSL: &str = include_str!("../shaders/filter_common.vert");
const FILTER_BLUR_VERTEX_GLSL: &str = include_str!("../shaders/filter_blur.vert");
const FILTER_BLUR_FRAGMENT_GLSL: &str = include_str!("../shaders/filter_blur.frag");
const FILTER_GLOW_VERTEX_GLSL: &str = include_str!("../shaders/filter_glow.vert");
const FILTER_GLOW_FRAGMENT_GLSL: &str = include_str!("../shaders/filter_glow.frag");
const FILTER_COLOR_MATRIX_FRAGMENT_GLSL: &str =
    include_str!("../shaders/filter_color_matrix.frag");
const FILTER_BEVEL_VERTEX_GLSL: &str = include_str!("../shaders/filter_bevel.vert");
const FILTER_BEVEL_FRAGMENT_GLSL: &str = include_str!("../shaders/filter_bevel.frag");
const FILTER_DISPLACEMENT_MAP_VERTEX_GLSL: &str =
    include_str!("../shaders/filter_displacement_map.vert");
const FILTER_DISPLACEMENT_MAP_FRAGMENT_GLSL: &str =
    include_str!("../shaders/filter_displacement_map.frag");
const NUM_VERTEX_ATTRIBUTES: u32 = 2;
const MAX_GRADIENT_COLORS: usize = 15;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum MaskState {
    NoMask,
    DrawMaskStencil,
    DrawMaskedContent,
    ClearMaskStencil,
}

#[repr(C)]
#[derive(Copy, Clone, Debug, Pod, Zeroable)]
struct Vertex {
    position: [f32; 2],
    color: u32,
}

impl From<TessVertex> for Vertex {
    fn from(vertex: TessVertex) -> Self {
        Self {
            position: [vertex.x, vertex.y],
            color: u32::from_le_bytes([
                vertex.color.r,
                vertex.color.g,
                vertex.color.b,
                vertex.color.a,
            ]),
        }
    }
}

pub struct GlRenderBackend {
    gl: Arc<glow::Context>,

    msaa_buffers: Option<MsaaBuffers>,
    msaa_sample_count: u32,

    color_program: ShaderProgram,
    bitmap_program: ShaderProgram,
    gradient_program: ShaderProgram,
    filter_blur_program: FilterProgram,
    filter_glow_program: FilterProgram,
    filter_color_matrix_program: FilterProgram,
    filter_bevel_program: FilterProgram,
    filter_displacement_map_program: FilterProgram,

    shape_tessellator: ShapeTessellator,

    color_quad_draws: Vec<Draw>,
    bitmap_quad_draws: Vec<Draw>,
    filter_common_quad: FilterQuad,
    filter_blur_quad: FilterQuad,
    filter_glow_quad: FilterQuad,
    filter_bevel_quad: FilterQuad,
    filter_displacement_quad: FilterQuad,

    mask_state: MaskState,
    num_masks: u32,
    mask_state_dirty: bool,
    is_transparent: bool,

    active_program: *const ShaderProgram,
    blend_modes: Vec<RenderBlendMode>,
    mult_color: Option<[f32; 4]>,
    add_color: Option<[f32; 4]>,

    renderbuffer_width: i32,
    renderbuffer_height: i32,
    view_matrix: [[f32; 4]; 4],

    viewport_scale_factor: f64,
    max_texture_size: u32,

    offscreen_fbo: glow::Framebuffer,
    offscreen_stencil_rbo: glow::Renderbuffer,
    offscreen_stencil_size: (i32, i32),
    active_texture_2d: Cell<Option<glow::Texture>>,
    pending_readbacks: HashMap<u64, PendingReadback>,
    reusable_readback_pbos: Vec<ReadbackPbo>,
    texture_resources: Vec<TextureResource>,
    scratch_texture_pool: Vec<ScratchTexture>,
    next_readback_id: u64,
}

// These should match the uniform names in the shaders.
const NUM_UNIFORMS: usize = 12;
const UNIFORM_NAMES: [&str; NUM_UNIFORMS] = [
    "world_matrix",
    "view_matrix",
    "mult_color",
    "add_color",
    "u_matrix",
    "u_gradient_type",
    "u_ratios",
    "u_colors",
    "u_repeat_mode",
    "u_focal_point",
    "u_interpolation",
    "u_texture",
];

enum ShaderUniform {
    WorldMatrix = 0,
    ViewMatrix,
    MultColor,
    AddColor,
    TextureMatrix,
    GradientType,
    GradientRatios,
    GradientColors,
    GradientRepeatMode,
    GradientFocalPoint,
    GradientInterpolation,
    BitmapTexture,
}

struct ShaderProgram {
    program: glow::Program,
    uniforms: [Option<glow::UniformLocation>; NUM_UNIFORMS],
    vertex_position_location: u32,
    vertex_color_location: u32,
    num_vertex_attributes: u32,
}

impl ShaderProgram {
    fn new(
        gl: &glow::Context,
        vertex_shader: &glow::Shader,
        fragment_shader: &glow::Shader,
    ) -> Result<Self, GlError> {
        unsafe {
            let program = gl
                .create_program()
                .map_err(|_| GlError::UnableToCreateProgram)?;
            gl.attach_shader(program, *vertex_shader);
            gl.attach_shader(program, *fragment_shader);
            gl.link_program(program);

            if !gl.get_program_link_status(program) {
                let msg = format!(
                    "Error linking shader program: {:?}",
                    gl.get_program_info_log(program)
                );
                log::error!("{msg}");
                return Err(GlError::LinkingShaderProgram(msg));
            }

            let mut uniforms: [Option<glow::UniformLocation>; NUM_UNIFORMS] = Default::default();
            for i in 0..NUM_UNIFORMS {
                uniforms[i] = gl.get_uniform_location(program, UNIFORM_NAMES[i]);
            }

            let vertex_position_location = gl
                .get_attrib_location(program, "position")
                .unwrap_or(0xffff_ffff);
            let vertex_color_location = gl
                .get_attrib_location(program, "color")
                .unwrap_or(0xffff_ffff);
            let num_vertex_attributes = if vertex_position_location != 0xffff_ffff {
                1
            } else {
                0
            } + if vertex_color_location != 0xffff_ffff {
                1
            } else {
                0
            };

            Ok(ShaderProgram {
                program,
                uniforms,
                vertex_position_location,
                vertex_color_location,
                num_vertex_attributes,
            })
        }
    }

    fn uniform1f(&self, gl: &glow::Context, uniform: ShaderUniform, value: f32) {
        unsafe {
            gl.uniform_1_f32(self.uniforms[uniform as usize].as_ref(), value);
        }
    }

    fn uniform1fv(&self, gl: &glow::Context, uniform: ShaderUniform, values: &[f32]) {
        unsafe {
            gl.uniform_1_f32_slice(self.uniforms[uniform as usize].as_ref(), values);
        }
    }

    fn uniform1i(&self, gl: &glow::Context, uniform: ShaderUniform, value: i32) {
        unsafe {
            gl.uniform_1_i32(self.uniforms[uniform as usize].as_ref(), value);
        }
    }

    fn uniform4fv(&self, gl: &glow::Context, uniform: ShaderUniform, values: &[f32]) {
        unsafe {
            gl.uniform_4_f32_slice(self.uniforms[uniform as usize].as_ref(), values);
        }
    }

    fn uniform_matrix3fv(
        &self,
        gl: &glow::Context,
        uniform: ShaderUniform,
        values: &[[f32; 3]; 3],
    ) {
        unsafe {
            gl.uniform_matrix_3_f32_slice(
                self.uniforms[uniform as usize].as_ref(),
                false,
                bytemuck::cast_slice(values),
            );
        }
    }

    fn uniform_matrix4fv(
        &self,
        gl: &glow::Context,
        uniform: ShaderUniform,
        values: &[[f32; 4]; 4],
    ) {
        unsafe {
            gl.uniform_matrix_4_f32_slice(
                self.uniforms[uniform as usize].as_ref(),
                false,
                bytemuck::cast_slice(values),
            );
        }
    }
}

struct FilterProgram {
    program: glow::Program,
    uniforms: HashMap<&'static str, Option<glow::UniformLocation>>,
    attributes: HashMap<&'static str, u32>,
}

impl FilterProgram {
    fn new(
        gl: &glow::Context,
        vertex_shader: &glow::Shader,
        fragment_shader: &glow::Shader,
        uniform_names: &[&'static str],
        attribute_names: &[&'static str],
    ) -> Result<Self, GlError> {
        unsafe {
            let program = gl
                .create_program()
                .map_err(|_| GlError::UnableToCreateProgram)?;
            gl.attach_shader(program, *vertex_shader);
            gl.attach_shader(program, *fragment_shader);
            gl.link_program(program);

            if !gl.get_program_link_status(program) {
                let msg = format!(
                    "Error linking filter shader program: {:?}",
                    gl.get_program_info_log(program)
                );
                log::error!("{msg}");
                return Err(GlError::LinkingShaderProgram(msg));
            }

            let uniforms = uniform_names
                .iter()
                .map(|name| (*name, gl.get_uniform_location(program, name)))
                .collect();
            let attributes = attribute_names
                .iter()
                .filter_map(|name| gl.get_attrib_location(program, name).map(|loc| (*name, loc)))
                .collect();

            Ok(Self {
                program,
                uniforms,
                attributes,
            })
        }
    }

    fn uniform(&self, name: &'static str) -> Option<&glow::UniformLocation> {
        self.uniforms.get(name).and_then(Option::as_ref)
    }

    fn attribute(&self, name: &'static str) -> Option<u32> {
        self.attributes.get(name).copied()
    }
}

struct FilterQuad {
    vao: glow::VertexArray,
    vertex_buffer: glow::Buffer,
}

#[derive(thiserror::Error, Debug)]
pub enum GlError {
    #[error("Couldn't create GL context")]
    CantCreateGLContext,

    #[error("Couldn't create frame buffer")]
    UnableToCreateFrameBuffer,

    #[error("Couldn't create program")]
    UnableToCreateProgram,

    #[error("Couldn't create texture")]
    UnableToCreateTexture,

    #[error("Couldn't compile shader")]
    UnableToCreateShader,

    #[error("Couldn't create render buffer")]
    UnableToCreateRenderBuffer,

    #[error("Couldn't create vertex array object")]
    UnableToCreateVAO,

    #[error("Couldn't create buffer")]
    UnableToCreateBuffer,

    #[error("Couldn't link shader program: {0}")]
    LinkingShaderProgram(String),

    #[error("GL Error in {0}")]
    GLError(String),
}

impl From<GlError> for BitmapError {
    fn from(e: GlError) -> Self {
        BitmapError::Unimplemented(format!("{}", e).into())
    }
}

#[derive(Clone, Debug)]
struct RegistryData {
    gl: Arc<glow::Context>,
    texture: glow::Texture,
    width: Cell<u32>,
    height: Cell<u32>,
    min_filter: Cell<i32>,
    mag_filter: Cell<i32>,
    wrap_s: Cell<i32>,
    wrap_t: Cell<i32>,
}

impl Drop for RegistryData {
    fn drop(&mut self) {
        unsafe {
            self.gl.delete_texture(self.texture);
        }
    }
}

impl BitmapHandleImpl for RegistryData {}

impl RegistryData {
    fn new(gl: Arc<glow::Context>, texture: glow::Texture, width: u32, height: u32) -> Self {
        Self {
            gl,
            texture,
            width: Cell::new(width),
            height: Cell::new(height),
            min_filter: Cell::new(i32::MIN),
            mag_filter: Cell::new(i32::MIN),
            wrap_s: Cell::new(i32::MIN),
            wrap_t: Cell::new(i32::MIN),
        }
    }
}

fn as_registry_data(handle: &BitmapHandle) -> &RegistryData {
    <dyn Any>::downcast_ref(&*handle.0).expect("Bitmap handle must be RegistryData")
}

#[derive(Clone, Debug)]
struct Gradient {
    matrix: [[f32; 3]; 3],
    gradient_type: i32,
    ratios: [f32; MAX_GRADIENT_COLORS],
    colors: [[f32; 4]; MAX_GRADIENT_COLORS],
    repeat_mode: i32,
    focal_point: f32,
    interpolation: GradientInterpolation,
}

impl Gradient {
    fn new(gradient: TessGradient, matrix: [[f32; 3]; 3]) -> Self {
        let num_colors = gradient.records.len().min(MAX_GRADIENT_COLORS);
        let mut ratios = [0.0; MAX_GRADIENT_COLORS];
        let mut colors = [[0.0; 4]; MAX_GRADIENT_COLORS];
        for i in 0..num_colors {
            let record = &gradient.records[i];
            let mut color = [
                f32::from(record.color.r) / 255.0,
                f32::from(record.color.g) / 255.0,
                f32::from(record.color.b) / 255.0,
                f32::from(record.color.a) / 255.0,
            ];
            match gradient.interpolation {
                GradientInterpolation::Rgb => {}
                GradientInterpolation::LinearRgb => srgb_to_linear(&mut color),
            }

            colors[i] = color;
            ratios[i] = f32::from(record.ratio) / 255.0;
        }

        for i in num_colors..MAX_GRADIENT_COLORS {
            ratios[i] = ratios[i - 1];
            colors[i] = colors[i - 1];
        }

        Self {
            matrix,
            gradient_type: match gradient.gradient_type {
                GradientType::Linear => 0,
                GradientType::Radial => 1,
                GradientType::Focal => 2,
            },
            ratios,
            colors,
            repeat_mode: match gradient.repeat_mode {
                GradientSpread::Pad => 0,
                GradientSpread::Repeat => 1,
                GradientSpread::Reflect => 2,
            },
            focal_point: gradient.focal_point.to_f32().clamp(-0.98, 0.98),
            interpolation: gradient.interpolation,
        }
    }
}

#[derive(Clone, Debug)]
struct BitmapDraw {
    matrix: [[f32; 3]; 3],
    handle: Option<BitmapHandle>,
    is_repeating: bool,
    is_smoothed: bool,
}

struct Mesh {
    gl: Arc<glow::Context>,
    draws: Vec<Draw>,
}

impl std::fmt::Debug for Mesh {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Mesh")
            .field("draws", &self.draws)
            .finish()
    }
}

impl Drop for Mesh {
    fn drop(&mut self) {
        unsafe {
            for draw in &self.draws {
                self.gl.delete_vertex_array(draw.vao);
                if let Some(vb) = draw.vertex_buffer {
                    self.gl.delete_buffer(vb);
                }
                self.gl.delete_buffer(draw.index_buffer);
            }
        }
    }
}

impl ShapeHandleImpl for Mesh {}

fn as_mesh(handle: &ShapeHandle) -> &Mesh {
    <dyn Any>::downcast_ref(&*handle.0).expect("Shape handle must be a Mesh")
}

#[derive(Debug)]
struct Draw {
    draw_type: DrawType,
    vao: glow::VertexArray,
    vertex_buffer: Option<glow::Buffer>,
    index_buffer: glow::Buffer,
    num_indices: i32,
    num_mask_indices: i32,
}

#[derive(Debug)]
enum DrawType {
    Color,
    Gradient(Box<Gradient>),
    Bitmap(BitmapDraw),
}

struct MsaaBuffers {
    color_renderbuffer: glow::Renderbuffer,
    stencil_renderbuffer: glow::Renderbuffer,
    render_framebuffer: glow::Framebuffer,
    color_framebuffer: glow::Framebuffer,
    framebuffer_texture: glow::Texture,
}

#[derive(Debug)]
struct ReadbackPbo {
    buffer: glow::Buffer,
    size_bytes: usize,
}

#[derive(Debug)]
struct TextureResource {
    weak: Weak<RegistryData>,
    texture: glow::Texture,
}

#[derive(Debug)]
struct PendingReadback {
    pbo: glow::Buffer,
    size_bytes: usize,
    fence: glow::Fence,
    width: u32,
    height: u32,
    token: Arc<()>,
}

#[derive(Clone, Copy, Debug)]
enum GlTarget {
    DefaultFramebuffer,
    Texture(glow::Texture),
}

#[derive(Clone, Copy, Debug)]
struct GlSurface {
    target: GlTarget,
    width: i32,
    height: i32,
    clear_color: Option<[f32; 4]>,
    y_flip: bool,
}

#[derive(Debug)]
struct GlSurfaceState {
    renderbuffer_width: i32,
    renderbuffer_height: i32,
    view_matrix: [[f32; 4]; 4],
    blend_modes: Vec<RenderBlendMode>,
}

#[derive(Debug)]
struct ScratchTexture {
    texture: glow::Texture,
    width: u32,
    height: u32,
}

#[derive(Debug)]
struct GlSyncHandle {
    readback_id: Option<u64>,
    width: u32,
    _token: Option<Arc<()>>,
    fallback_pixel_data: Option<Vec<u8>>,
}

impl SyncHandle for GlSyncHandle {}

impl GlRenderBackend {
    pub fn shared_gl(&self) -> Arc<glow::Context> { self.gl.clone() }

    pub unsafe fn new(gl: glow::Context, width: u32, height: u32) -> Result<Self, BitmapError> {
        let gl = Arc::new(gl);
        unsafe {
            gl.enable(glow::BLEND);
            gl.pixel_store_i32(glow::UNPACK_ALIGNMENT, 1);
        }

        let max_texture_size = unsafe { gl.get_parameter_i32(glow::MAX_TEXTURE_SIZE) }.max(1) as u32;

        let color_vertex = Self::compile_shader(&gl, glow::VERTEX_SHADER, COLOR_VERTEX_GLSL)?;
        let texture_vertex = Self::compile_shader(&gl, glow::VERTEX_SHADER, TEXTURE_VERTEX_GLSL)?;
        let color_fragment = Self::compile_shader(&gl, glow::FRAGMENT_SHADER, COLOR_FRAGMENT_GLSL)?;
        let bitmap_fragment =
            Self::compile_shader(&gl, glow::FRAGMENT_SHADER, BITMAP_FRAGMENT_GLSL)?;
        let gradient_fragment =
            Self::compile_shader(&gl, glow::FRAGMENT_SHADER, GRADIENT_FRAGMENT_GLSL)?;
        let filter_common_vertex =
            Self::compile_shader(&gl, glow::VERTEX_SHADER, FILTER_COMMON_VERTEX_GLSL)?;
        let filter_blur_vertex =
            Self::compile_shader(&gl, glow::VERTEX_SHADER, FILTER_BLUR_VERTEX_GLSL)?;
        let filter_glow_vertex =
            Self::compile_shader(&gl, glow::VERTEX_SHADER, FILTER_GLOW_VERTEX_GLSL)?;
        let filter_bevel_vertex =
            Self::compile_shader(&gl, glow::VERTEX_SHADER, FILTER_BEVEL_VERTEX_GLSL)?;
        let filter_displacement_vertex = Self::compile_shader(
            &gl,
            glow::VERTEX_SHADER,
            FILTER_DISPLACEMENT_MAP_VERTEX_GLSL,
        )?;
        let filter_blur_fragment =
            Self::compile_shader(&gl, glow::FRAGMENT_SHADER, FILTER_BLUR_FRAGMENT_GLSL)?;
        let filter_glow_fragment =
            Self::compile_shader(&gl, glow::FRAGMENT_SHADER, FILTER_GLOW_FRAGMENT_GLSL)?;
        let filter_color_matrix_fragment = Self::compile_shader(
            &gl,
            glow::FRAGMENT_SHADER,
            FILTER_COLOR_MATRIX_FRAGMENT_GLSL,
        )?;
        let filter_bevel_fragment =
            Self::compile_shader(&gl, glow::FRAGMENT_SHADER, FILTER_BEVEL_FRAGMENT_GLSL)?;
        let filter_displacement_fragment = Self::compile_shader(
            &gl,
            glow::FRAGMENT_SHADER,
            FILTER_DISPLACEMENT_MAP_FRAGMENT_GLSL,
        )?;

        let color_program = ShaderProgram::new(&gl, &color_vertex, &color_fragment)?;
        let bitmap_program = ShaderProgram::new(&gl, &texture_vertex, &bitmap_fragment)?;
        let gradient_program = ShaderProgram::new(&gl, &texture_vertex, &gradient_fragment)?;
        let filter_blur_program = FilterProgram::new(
            &gl,
            &filter_blur_vertex,
            &filter_blur_fragment,
            &[
                "texture_sampler",
                "dir_x",
                "dir_y",
                "full_size",
                "m",
                "m2",
                "first_weight",
                "last_offset",
                "last_weight",
            ],
            &["position", "uv"],
        )?;
        let filter_glow_program = FilterProgram::new(
            &gl,
            &filter_glow_vertex,
            &filter_glow_fragment,
            &[
                "source_texture",
                "blur_texture",
                "glow_color",
                "strength",
                "inner",
                "knockout",
                "composite_source",
            ],
            &["position", "source_uv", "blur_uv"],
        )?;
        let filter_color_matrix_program = FilterProgram::new(
            &gl,
            &filter_common_vertex,
            &filter_color_matrix_fragment,
            &["texture_sampler", "color_matrix"],
            &["position", "uv"],
        )?;
        let filter_bevel_program = FilterProgram::new(
            &gl,
            &filter_bevel_vertex,
            &filter_bevel_fragment,
            &[
                "source_texture",
                "blur_texture",
                "highlight_color",
                "shadow_color",
                "strength",
                "bevel_type",
                "knockout",
                "composite_source",
            ],
            &["position", "source_uv", "blur_uv_left", "blur_uv_right"],
        )?;
        let filter_displacement_map_program = FilterProgram::new(
            &gl,
            &filter_displacement_vertex,
            &filter_displacement_fragment,
            &[
                "source_texture",
                "map_texture",
                "filter_color",
                "components_packed",
                "mode",
                "scale_x",
                "scale_y",
                "source_width",
                "source_height",
                "map_width",
                "map_height",
                "offset_x",
                "offset_y",
                "viewscale_x",
                "viewscale_y",
            ],
            &["position", "uv"],
        )?;

        unsafe {
            gl.delete_shader(color_vertex);
            gl.delete_shader(texture_vertex);
            gl.delete_shader(color_fragment);
            gl.delete_shader(bitmap_fragment);
            gl.delete_shader(gradient_fragment);
            gl.delete_shader(filter_common_vertex);
            gl.delete_shader(filter_blur_vertex);
            gl.delete_shader(filter_glow_vertex);
            gl.delete_shader(filter_bevel_vertex);
            gl.delete_shader(filter_displacement_vertex);
            gl.delete_shader(filter_blur_fragment);
            gl.delete_shader(filter_glow_fragment);
            gl.delete_shader(filter_color_matrix_fragment);
            gl.delete_shader(filter_bevel_fragment);
            gl.delete_shader(filter_displacement_fragment);
        }

        let filter_common_quad = Self::build_filter_quad(
            &gl,
            &filter_color_matrix_program,
            &[("position", 2, 16, 0), ("uv", 2, 16, 8)],
        )?;
        let filter_blur_quad = Self::build_filter_quad(
            &gl,
            &filter_blur_program,
            &[("position", 2, 16, 0), ("uv", 2, 16, 8)],
        )?;
        let filter_glow_quad = Self::build_filter_quad(
            &gl,
            &filter_glow_program,
            &[
                ("position", 2, 24, 0),
                ("source_uv", 2, 24, 8),
                ("blur_uv", 2, 24, 16),
            ],
        )?;
        let filter_bevel_quad = Self::build_filter_quad(
            &gl,
            &filter_bevel_program,
            &[
                ("position", 2, 32, 0),
                ("source_uv", 2, 32, 8),
                ("blur_uv_left", 2, 32, 16),
                ("blur_uv_right", 2, 32, 24),
            ],
        )?;
        let filter_displacement_quad = Self::build_filter_quad(
            &gl,
            &filter_displacement_map_program,
            &[("position", 2, 16, 0), ("uv", 2, 16, 8)],
        )?;

        let offscreen_fbo = unsafe {
            gl.create_framebuffer()
                .map_err(|_| GlError::UnableToCreateFrameBuffer)?
        };
        let offscreen_stencil_rbo = unsafe {
            gl.create_renderbuffer()
                .map_err(|_| BitmapError::Unimplemented("Unable to create offscreen stencil buffer".into()))?
        };

        let mut renderer = Self {
            gl,
            msaa_buffers: None,
            msaa_sample_count: 1,
            color_program,
            gradient_program,
            bitmap_program,
            filter_blur_program,
            filter_glow_program,
            filter_color_matrix_program,
            filter_bevel_program,
            filter_displacement_map_program,
            shape_tessellator: ShapeTessellator::new(),
            color_quad_draws: vec![],
            bitmap_quad_draws: vec![],
            filter_common_quad,
            filter_blur_quad,
            filter_glow_quad,
            filter_bevel_quad,
            filter_displacement_quad,
            renderbuffer_width: 1,
            renderbuffer_height: 1,
            view_matrix: [[0.0; 4]; 4],
            mask_state: MaskState::NoMask,
            num_masks: 0,
            mask_state_dirty: true,
            is_transparent: false,
            active_program: std::ptr::null(),
            blend_modes: vec![],
            mult_color: None,
            add_color: None,
            viewport_scale_factor: 1.0,
            max_texture_size,
            offscreen_fbo,
            offscreen_stencil_rbo,
            offscreen_stencil_size: (0, 0),
            active_texture_2d: Cell::new(None),
            pending_readbacks: HashMap::new(),
            reusable_readback_pbos: Vec::new(),
            texture_resources: Vec::new(),
            scratch_texture_pool: Vec::new(),
            next_readback_id: 1,
        };

        renderer.push_blend_mode(RenderBlendMode::Builtin(BlendMode::Normal));

        let mut color_quad_draws = renderer.build_quad_mesh(&renderer.color_program)?;
        let mut bitmap_quad_draws = renderer.build_quad_mesh(&renderer.bitmap_program)?;
        renderer.color_quad_draws.append(&mut color_quad_draws);
        renderer.bitmap_quad_draws.append(&mut bitmap_quad_draws);

        // // Build quad meshes after renderer is created
        // let color_quad_draws = vec![Draw {
        //     draw_type: DrawType::Color,
        //     vao: renderer.create_vertex_array()?,
        //     vertex_buffer: Buffer {
        //         buffer: unsafe {
        //             renderer
        //                 .gl
        //                 .create_buffer()
        //                 .map_err(|_| GlError::UnableToCreateBuffer)?
        //         },
        //     },
        //     index_buffer: Buffer {
        //         buffer: unsafe {
        //             renderer
        //                 .gl
        //                 .create_buffer()
        //                 .map_err(|_| GlError::UnableToCreateBuffer)?
        //         },
        //     },
        //     num_indices: 4,
        //     num_mask_indices: 4,
        // }];

        // let bitmap_quad_draws = vec![Draw {
        //     draw_type: DrawType::Bitmap(BitmapDraw {
        //         matrix: [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]],
        //         handle: None,
        //         is_smoothed: true,
        //         is_repeating: false,
        //     }),
        //     vao: renderer.create_vertex_array()?,
        //     vertex_buffer: Buffer {
        //         buffer: unsafe {
        //             renderer
        //                 .gl
        //                 .create_buffer()
        //                 .map_err(|_| GlError::UnableToCreateBuffer)?
        //         },
        //     },
        //     index_buffer: Buffer {
        //         buffer: unsafe {
        //             renderer
        //                 .gl
        //                 .create_buffer()
        //                 .map_err(|_| GlError::UnableToCreateBuffer)?
        //         },
        //     },
        //     num_indices: 4,
        //     num_mask_indices: 4,
        // }];

        // renderer.color_quad_draws = color_quad_draws;
        // renderer.bitmap_quad_draws = bitmap_quad_draws;

        renderer.set_viewport_dimensions(ViewportDimensions {
            width: width.max(1),
            height: height.max(1),
            scale_factor: 1.0,
        });

        Ok(renderer)
    }

    fn supports_filter(filter: &Filter) -> bool {
        matches!(
            filter,
            Filter::BlurFilter(_)
                | Filter::GlowFilter(_)
                | Filter::DropShadowFilter(_)
                | Filter::ColorMatrixFilter(_)
                | Filter::BevelFilter(_)
                | Filter::DisplacementMapFilter(_)
        )
    }

    fn ensure_texture_size_supported(&self, width: u32, height: u32) -> Result<(), BitmapError> {
        if width > self.max_texture_size || height > self.max_texture_size {
            Err(BitmapError::TooLarge)
        } else {
            Ok(())
        }
    }

    // Clone helper for ShaderProgram
    fn clone_program(program: &ShaderProgram) -> ShaderProgram {
        ShaderProgram {
            program: program.program,
            uniforms: std::array::from_fn(|i| program.uniforms[i].clone()),
            vertex_position_location: program.vertex_position_location,
            vertex_color_location: program.vertex_color_location,
            num_vertex_attributes: program.num_vertex_attributes,
        }
    }

    fn build_quad_mesh(&self, program: &ShaderProgram) -> Result<Vec<Draw>, GlError> {
        let vao = self.create_vertex_array()?;

        let vertex_buffer = unsafe {
            self.gl
                .create_buffer()
                .map_err(|_| GlError::UnableToCreateBuffer)?
        };
        unsafe {
            self.gl.bind_buffer(glow::ARRAY_BUFFER, Some(vertex_buffer));
            self.gl.buffer_data_u8_slice(
                glow::ARRAY_BUFFER,
                bytemuck::cast_slice(&[
                    Vertex {
                        position: [0.0, 0.0],
                        color: 0xffff_ffff,
                    },
                    Vertex {
                        position: [1.0, 0.0],
                        color: 0xffff_ffff,
                    },
                    Vertex {
                        position: [1.0, 1.0],
                        color: 0xffff_ffff,
                    },
                    Vertex {
                        position: [0.0, 1.0],
                        color: 0xffff_ffff,
                    },
                ]),
                glow::STATIC_DRAW,
            );
        }

        let index_buffer = unsafe {
            self.gl
                .create_buffer()
                .map_err(|_| GlError::UnableToCreateBuffer)?
        };
        unsafe {
            self.gl
                .bind_buffer(glow::ELEMENT_ARRAY_BUFFER, Some(index_buffer));
            self.gl.buffer_data_u8_slice(
                glow::ELEMENT_ARRAY_BUFFER,
                bytemuck::cast_slice(&[0u32, 1, 2, 3]),
                glow::STATIC_DRAW,
            );
        }

        if program.vertex_position_location != 0xffff_ffff {
            unsafe {
                self.gl.vertex_attrib_pointer_f32(
                    program.vertex_position_location,
                    2,
                    glow::FLOAT,
                    false,
                    12,
                    0,
                );
                self.gl
                    .enable_vertex_attrib_array(program.vertex_position_location);
            }
        }

        if program.vertex_color_location != 0xffff_ffff {
            unsafe {
                self.gl.vertex_attrib_pointer_f32(
                    program.vertex_color_location,
                    4,
                    glow::UNSIGNED_BYTE,
                    true,
                    12,
                    8,
                );
                self.gl
                    .enable_vertex_attrib_array(program.vertex_color_location);
            }
        }

        self.bind_vertex_array(None);
        for i in program.num_vertex_attributes..NUM_VERTEX_ATTRIBUTES {
            unsafe {
                self.gl.disable_vertex_attrib_array(i);
            }
        }

        let draws = vec![Draw {
            draw_type: DrawType::Bitmap(BitmapDraw {
                matrix: [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]],
                handle: None,
                is_smoothed: true,
                is_repeating: false,
            }),
            vao,
            vertex_buffer: Some(vertex_buffer),
            index_buffer,
            num_indices: 4,
            num_mask_indices: 4,
        }];

        Ok(draws)
    }

    fn compile_shader(
        gl: &glow::Context,
        shader_type: u32,
        glsl_src: &str,
    ) -> Result<glow::Shader, GlError> {
        unsafe {
            let shader = gl
                .create_shader(shader_type)
                .map_err(|_| GlError::UnableToCreateShader)?;
            gl.shader_source(shader, glsl_src);
            gl.compile_shader(shader);
            if !gl.get_shader_compile_status(shader) {
                let log = gl.get_shader_info_log(shader);
                log::error!("{log}");
            }
            Ok(shader)
        }
    }


    fn build_filter_quad(
        gl: &glow::Context,
        program: &FilterProgram,
        attributes: &[(&'static str, i32, i32, i32)],
    ) -> Result<FilterQuad, GlError> {
        unsafe {
            let vao = gl
                .create_vertex_array()
                .map_err(|_| GlError::UnableToCreateVAO)?;
            let vertex_buffer = gl
                .create_buffer()
                .map_err(|_| GlError::UnableToCreateBuffer)?;

            gl.bind_vertex_array(Some(vao));
            gl.bind_buffer(glow::ARRAY_BUFFER, Some(vertex_buffer));
            for (name, size, stride, offset) in attributes {
                if let Some(location) = program.attribute(name) {
                    gl.vertex_attrib_pointer_f32(
                        location,
                        *size,
                        glow::FLOAT,
                        false,
                        *stride,
                        *offset,
                    );
                    gl.enable_vertex_attrib_array(location);
                }
            }
            gl.bind_vertex_array(None);
            gl.bind_buffer(glow::ARRAY_BUFFER, None);

            Ok(FilterQuad { vao, vertex_buffer })
        }
    }

    fn bind_texture_unit(&self, unit: u32, texture: glow::Texture) {
        unsafe {
            self.gl.active_texture(glow::TEXTURE0 + unit);
            self.gl.bind_texture(glow::TEXTURE_2D, Some(texture));
        }
        if unit == 0 {
            self.active_texture_2d.set(Some(texture));
        }
    }

    fn set_filter_texture_parameters(
        &self,
        unit: u32,
        texture: glow::Texture,
        filter: i32,
        wrap: i32,
    ) {
        self.bind_texture_unit(unit, texture);
        unsafe {
            self.gl
                .tex_parameter_i32(glow::TEXTURE_2D, glow::TEXTURE_MAG_FILTER, filter);
            self.gl
                .tex_parameter_i32(glow::TEXTURE_2D, glow::TEXTURE_MIN_FILTER, filter);
            self.gl
                .tex_parameter_i32(glow::TEXTURE_2D, glow::TEXTURE_WRAP_S, wrap);
            self.gl
                .tex_parameter_i32(glow::TEXTURE_2D, glow::TEXTURE_WRAP_T, wrap);
        }
    }

    fn create_filter_texture(&self, width: u32, height: u32) -> Result<glow::Texture, BitmapError> {
        self.ensure_texture_size_supported(width, height)?;

        let texture = unsafe {
            self.gl
                .create_texture()
                .map_err(|_| BitmapError::Unimplemented("Unable to create filter texture".into()))?
        };
        self.bind_texture_unit(0, texture);
        unsafe {
            self.gl.tex_image_2d(
                glow::TEXTURE_2D,
                0,
                glow::RGBA8 as i32,
                width as i32,
                height as i32,
                0,
                glow::RGBA,
                glow::UNSIGNED_BYTE,
                None,
            );
            self.gl
                .tex_parameter_i32(glow::TEXTURE_2D, glow::TEXTURE_MAG_FILTER, glow::LINEAR as i32);
            self.gl
                .tex_parameter_i32(glow::TEXTURE_2D, glow::TEXTURE_MIN_FILTER, glow::LINEAR as i32);
            self.gl.tex_parameter_i32(
                glow::TEXTURE_2D,
                glow::TEXTURE_WRAP_S,
                glow::CLAMP_TO_EDGE as i32,
            );
            self.gl.tex_parameter_i32(
                glow::TEXTURE_2D,
                glow::TEXTURE_WRAP_T,
                glow::CLAMP_TO_EDGE as i32,
            );
        }
        Ok(texture)
    }

    fn acquire_scratch_texture(&mut self, width: u32, height: u32) -> Result<ScratchTexture, BitmapError> {
        if let Some(index) = self
            .scratch_texture_pool
            .iter()
            .position(|texture| texture.width == width && texture.height == height)
        {
            return Ok(self.scratch_texture_pool.swap_remove(index));
        }

        Ok(ScratchTexture {
            texture: self.create_filter_texture(width, height)?,
            width,
            height,
        })
    }

    fn release_scratch_texture(&mut self, texture: ScratchTexture) {
        self.scratch_texture_pool.push(texture);
    }

    fn common_filter_vertices() -> [f32; 16] {
        [
            0.0, 0.0, 0.0, 0.0,
            1.0, 0.0, 1.0, 0.0,
            1.0, 1.0, 1.0, 1.0,
            0.0, 1.0, 0.0, 1.0,
        ]
    }

    fn common_filter_vertices_for_source(
        source_dimensions: (u32, u32),
        source_point: (u32, u32),
        source_size: (u32, u32),
    ) -> [f32; 16] {
        let source_width = source_dimensions.0.max(1) as f32;
        let source_height = source_dimensions.1.max(1) as f32;
        let left = source_point.0 as f32 / source_width;
        let top = source_point.1 as f32 / source_height;
        let right = (source_point.0 + source_size.0) as f32 / source_width;
        let bottom = (source_point.1 + source_size.1) as f32 / source_height;

        [
            0.0, 0.0, left, top,
            1.0, 0.0, right, top,
            1.0, 1.0, right, bottom,
            0.0, 1.0, left, bottom,
        ]
    }

    fn glow_filter_vertices(offset: (f32, f32), width: u32, height: u32) -> [f32; 24] {
        let dx = if width == 0 { 0.0 } else { offset.0 / width as f32 };
        let dy = if height == 0 { 0.0 } else { offset.1 / height as f32 };
        [
            0.0, 0.0, 0.0, 0.0, 0.0 + dx, 0.0 + dy,
            1.0, 0.0, 1.0, 0.0, 1.0 + dx, 0.0 + dy,
            1.0, 1.0, 1.0, 1.0, 1.0 + dx, 1.0 + dy,
            0.0, 1.0, 0.0, 1.0, 0.0 + dx, 1.0 + dy,
        ]
    }

    fn bevel_filter_vertices(offset: (f32, f32), width: u32, height: u32) -> [f32; 32] {
        let dx = if width == 0 { 0.0 } else { offset.0 / width as f32 };
        let dy = if height == 0 { 0.0 } else { offset.1 / height as f32 };
        [
            0.0, 0.0, 0.0, 0.0, 0.0 + dx, 0.0 + dy, 0.0 - dx, 0.0 - dy,
            1.0, 0.0, 1.0, 0.0, 1.0 + dx, 0.0 + dy, 1.0 - dx, 0.0 - dy,
            1.0, 1.0, 1.0, 1.0, 1.0 + dx, 1.0 + dy, 1.0 - dx, 1.0 - dy,
            0.0, 1.0, 0.0, 1.0, 0.0 + dx, 1.0 + dy, 0.0 - dx, 1.0 - dy,
        ]
    }

    fn normalized_rgba(color: Color) -> [f32; 4] {
        [
            f32::from(color.r) / 255.0,
            f32::from(color.g) / 255.0,
            f32::from(color.b) / 255.0,
            f32::from(color.a) / 255.0,
        ]
    }

    fn premultiplied_rgba(color: Color) -> [f32; 4] {
        let mut color = Self::normalized_rgba(color);
        color[0] *= color[3];
        color[1] *= color[3];
        color[2] *= color[3];
        color
    }

    fn identity_color_matrix() -> [f32; 20] {
        [
            1.0, 0.0, 0.0, 0.0, 0.0,
            0.0, 1.0, 0.0, 0.0, 0.0,
            0.0, 0.0, 1.0, 0.0, 0.0,
            0.0, 0.0, 0.0, 1.0, 0.0,
        ]
    }

    fn pick_two_textures_excluding(
        textures: &[glow::Texture; 3],
        excluded: &[glow::Texture],
    ) -> (glow::Texture, glow::Texture) {
        let mut picked = textures
            .iter()
            .copied()
            .filter(|texture| !excluded.iter().any(|excluded| excluded == texture));
        (
            picked.next().expect("missing first scratch texture"),
            picked.next().expect("missing second scratch texture"),
        )
    }

    fn pick_output_texture(
        destination: glow::Texture,
        textures: &[glow::Texture; 3],
        excluded: &[glow::Texture],
    ) -> glow::Texture {
        if !excluded.iter().any(|excluded_texture| *excluded_texture == destination) {
            return destination;
        }

        textures
            .iter()
            .copied()
            .find(|texture| !excluded.iter().any(|excluded_texture| excluded_texture == texture))
            .expect("missing output texture")
    }

    fn render_filter_to_texture_region<F>(
        &mut self,
        target_texture: glow::Texture,
        viewport_origin: (u32, u32),
        viewport_size: (u32, u32),
        program: glow::Program,
        quad_vao: glow::VertexArray,
        quad_vertex_buffer: glow::Buffer,
        vertex_data: &[u8],
        configure: F,
    ) -> Result<(), BitmapError>
    where
        F: FnOnce(&mut Self) -> Result<(), BitmapError>,
    {
        let saved_width = self.renderbuffer_width;
        let saved_height = self.renderbuffer_height;

        unsafe {
            self.gl
                .bind_framebuffer(glow::FRAMEBUFFER, Some(self.offscreen_fbo));
            self.gl.framebuffer_texture_2d(
                glow::FRAMEBUFFER,
                glow::COLOR_ATTACHMENT0,
                glow::TEXTURE_2D,
                Some(target_texture),
                0,
            );
            self.gl.framebuffer_renderbuffer(
                glow::FRAMEBUFFER,
                glow::STENCIL_ATTACHMENT,
                glow::RENDERBUFFER,
                None,
            );
            self.gl.viewport(
                viewport_origin.0 as i32,
                viewport_origin.1 as i32,
                viewport_size.0 as i32,
                viewport_size.1 as i32,
            );
            self.gl.disable(glow::STENCIL_TEST);
            self.gl.disable(glow::BLEND);
            self.gl.color_mask(true, true, true, true);
            self.gl.use_program(Some(program));
            self.gl.bind_vertex_array(Some(quad_vao));
            self.gl.bind_buffer(glow::ARRAY_BUFFER, Some(quad_vertex_buffer));
            self.gl
                .buffer_data_u8_slice(glow::ARRAY_BUFFER, vertex_data, glow::DYNAMIC_DRAW);
        }

        let result = (|| {
            let status = unsafe { self.gl.check_framebuffer_status(glow::FRAMEBUFFER) };
            if status != glow::FRAMEBUFFER_COMPLETE {
                return Err(BitmapError::Unimplemented(
                    format!("Filter framebuffer not complete: {status:#x}").into(),
                ));
            }

            configure(self)?;

            unsafe {
                self.gl.draw_arrays(glow::TRIANGLE_FAN, 0, 4);
            }

            Ok(())
        })();

        unsafe {
            self.gl.bind_vertex_array(None);
            self.gl.framebuffer_texture_2d(
                glow::FRAMEBUFFER,
                glow::COLOR_ATTACHMENT0,
                glow::TEXTURE_2D,
                None,
                0,
            );
            self.gl.bind_framebuffer(glow::FRAMEBUFFER, None);
            self.gl.viewport(0, 0, saved_width, saved_height);
            self.gl.use_program(None);
            self.gl.enable(glow::BLEND);
            self.gl.active_texture(glow::TEXTURE0);
        }

        if let Some(blend_mode) = self.blend_modes.last().cloned() {
            self.apply_blend_mode(blend_mode);
        }
        self.active_program = std::ptr::null();
        self.active_texture_2d.set(None);
        self.mask_state_dirty = true;

        result
    }

    fn with_filter_fbo<F>(&mut self, f: F) -> Result<(), BitmapError>
    where
        F: FnOnce(&mut Self) -> Result<(), BitmapError>,
    {
        let saved_width = self.renderbuffer_width;
        let saved_height = self.renderbuffer_height;

        unsafe {
            self.gl
                .bind_framebuffer(glow::FRAMEBUFFER, Some(self.offscreen_fbo));
            self.gl.framebuffer_renderbuffer(
                glow::FRAMEBUFFER,
                glow::STENCIL_ATTACHMENT,
                glow::RENDERBUFFER,
                None,
            );
            self.gl.disable(glow::STENCIL_TEST);
            self.gl.disable(glow::BLEND);
            self.gl.color_mask(true, true, true, true);
        }

        let result = f(self);

        unsafe {
            self.gl.bind_vertex_array(None);
            self.gl.framebuffer_texture_2d(
                glow::FRAMEBUFFER,
                glow::COLOR_ATTACHMENT0,
                glow::TEXTURE_2D,
                None,
                0,
            );
            self.gl.bind_framebuffer(glow::FRAMEBUFFER, None);
            self.gl.viewport(0, 0, saved_width, saved_height);
            self.gl.use_program(None);
            self.gl.enable(glow::BLEND);
            self.gl.active_texture(glow::TEXTURE0);
        }

        if let Some(blend_mode) = self.blend_modes.last().cloned() {
            self.apply_blend_mode(blend_mode);
        }
        self.active_program = std::ptr::null();
        self.active_texture_2d.set(None);
        self.mask_state_dirty = true;

        result
    }

    fn render_filter_pass<F>(
        &mut self,
        target_texture: glow::Texture,
        viewport_origin: (u32, u32),
        viewport_size: (u32, u32),
        program: glow::Program,
        quad_vao: glow::VertexArray,
        quad_vertex_buffer: glow::Buffer,
        vertex_data: &[u8],
        configure: F,
    ) -> Result<(), BitmapError>
    where
        F: FnOnce(&mut Self) -> Result<(), BitmapError>,
    {
        unsafe {
            self.gl.framebuffer_texture_2d(
                glow::FRAMEBUFFER,
                glow::COLOR_ATTACHMENT0,
                glow::TEXTURE_2D,
                Some(target_texture),
                0,
            );
            self.gl.viewport(
                viewport_origin.0 as i32,
                viewport_origin.1 as i32,
                viewport_size.0 as i32,
                viewport_size.1 as i32,
            );
            self.gl.use_program(Some(program));
            self.gl.bind_vertex_array(Some(quad_vao));
            self.gl.bind_buffer(glow::ARRAY_BUFFER, Some(quad_vertex_buffer));
            self.gl
                .buffer_data_u8_slice(glow::ARRAY_BUFFER, vertex_data, glow::DYNAMIC_DRAW);
        }

        configure(self)?;

        unsafe {
            self.gl.draw_arrays(glow::TRIANGLE_FAN, 0, 4);
        }

        Ok(())
    }

    fn render_filter_to_texture<F>(
        &mut self,
        target_texture: glow::Texture,
        width: u32,
        height: u32,
        program: glow::Program,
        quad_vao: glow::VertexArray,
        quad_vertex_buffer: glow::Buffer,
        vertex_data: &[u8],
        configure: F,
    ) -> Result<(), BitmapError>
    where
        F: FnOnce(&mut Self) -> Result<(), BitmapError>,
    {
        self.render_filter_to_texture_region(
            target_texture,
            (0, 0),
            (width, height),
            program,
            quad_vao,
            quad_vertex_buffer,
            vertex_data,
            configure,
        )
    }

    fn blit_texture_region(
        &mut self,
        source_texture: glow::Texture,
        source_dimensions: (u32, u32),
        source_point: (u32, u32),
        source_size: (u32, u32),
        destination_texture: glow::Texture,
        destination_dimensions: (u32, u32),
        dest_point: (i32, i32),
    ) -> Result<bool, BitmapError> {
        let mut src_x = source_point.0;
        let mut src_y = source_point.1;
        let mut copy_width = source_size
            .0
            .min(source_dimensions.0.saturating_sub(source_point.0));
        let mut copy_height = source_size
            .1
            .min(source_dimensions.1.saturating_sub(source_point.1));

        if copy_width == 0 || copy_height == 0 {
            return Ok(false);
        }

        let mut dst_x = dest_point.0;
        let mut dst_y = dest_point.1;

        if dst_x < 0 {
            let offset = dst_x.unsigned_abs();
            if offset >= copy_width {
                return Ok(false);
            }
            src_x += offset;
            copy_width -= offset;
            dst_x = 0;
        }

        if dst_y < 0 {
            let offset = dst_y.unsigned_abs();
            if offset >= copy_height {
                return Ok(false);
            }
            src_y += offset;
            copy_height -= offset;
            dst_y = 0;
        }

        let dst_x = dst_x as u32;
        let dst_y = dst_y as u32;

        copy_width = copy_width.min(destination_dimensions.0.saturating_sub(dst_x));
        copy_height = copy_height.min(destination_dimensions.1.saturating_sub(dst_y));

        if copy_width == 0 || copy_height == 0 {
            return Ok(false);
        }

        let texture_sampler = self
            .filter_color_matrix_program
            .uniform("texture_sampler")
            .cloned();
        let color_matrix = self.filter_color_matrix_program.uniform("color_matrix").cloned();
        let program = self.filter_color_matrix_program.program;
        let quad = (self.filter_common_quad.vao, self.filter_common_quad.vertex_buffer);
        let vertices = Self::common_filter_vertices_for_source(
            source_dimensions,
            (src_x, src_y),
            (copy_width, copy_height),
        );

        self.render_filter_to_texture_region(
            destination_texture,
            (dst_x, dst_y),
            (copy_width, copy_height),
            program,
            quad.0,
            quad.1,
            bytemuck::cast_slice(&vertices),
            |backend| {
                backend.set_filter_texture_parameters(
                    0,
                    source_texture,
                    glow::LINEAR as i32,
                    glow::CLAMP_TO_EDGE as i32,
                );
                unsafe {
                    backend.gl.uniform_1_i32(texture_sampler.as_ref(), 0);
                    backend
                        .gl
                        .uniform_1_f32_slice(color_matrix.as_ref(), &Self::identity_color_matrix());
                }
                Ok(())
            },
        )?;

        Ok(true)
    }

    fn blur_texture(
        &mut self,
        source_texture: glow::Texture,
        width: u32,
        height: u32,
        scratch_a: glow::Texture,
        scratch_b: glow::Texture,
        filter: &swf::BlurFilter,
    ) -> Result<glow::Texture, BitmapError> {
        let texture_sampler = self.filter_blur_program.uniform("texture_sampler").cloned();
        let dir_x = self.filter_blur_program.uniform("dir_x").cloned();
        let dir_y = self.filter_blur_program.uniform("dir_y").cloned();
        let full_size = self.filter_blur_program.uniform("full_size").cloned();
        let m = self.filter_blur_program.uniform("m").cloned();
        let m2 = self.filter_blur_program.uniform("m2").cloned();
        let first_weight = self.filter_blur_program.uniform("first_weight").cloned();
        let last_offset = self.filter_blur_program.uniform("last_offset").cloned();
        let last_weight = self.filter_blur_program.uniform("last_weight").cloned();
        let program = self.filter_blur_program.program;
        let quad = (self.filter_blur_quad.vao, self.filter_blur_quad.vertex_buffer);
        let vertices = Self::common_filter_vertices();
        let mut current = source_texture;
        let mut destination = scratch_a;
        let mut alternate = scratch_b;
        let mut changed = false;

        self.with_filter_fbo(|backend| {
            for _ in 0..(filter.num_passes() as usize) {
                for horizontal in [true, false] {
                    let strength = if horizontal {
                        filter.blur_x.to_f32()
                    } else {
                        filter.blur_y.to_f32()
                    };
                    let kernel_size = strength.min(255.0);
                    if kernel_size <= 1.0 {
                        continue;
                    }

                    let radius = (kernel_size - 1.0) / 2.0;
                    let m_value = radius.ceil() - 1.0;
                    let alpha = ((radius - m_value) * 255.0).floor() / 255.0;
                    let last_offset_value = 1.0 / ((1.0 / alpha) + 1.0);
                    let last_weight_value = alpha + 1.0;
                    let direction = if horizontal {
                        (1.0 / width as f32, 0.0)
                    } else {
                        (0.0, 1.0 / height as f32)
                    };

                    backend.render_filter_pass(
                        destination,
                        (0, 0),
                        (width, height),
                        program,
                        quad.0,
                        quad.1,
                        bytemuck::cast_slice(&vertices),
                        |b| {
                            b.set_filter_texture_parameters(
                                0,
                                current,
                                glow::LINEAR as i32,
                                glow::CLAMP_TO_EDGE as i32,
                            );
                            unsafe {
                                b.gl.uniform_1_i32(texture_sampler.as_ref(), 0);
                                b.gl.uniform_1_f32(dir_x.as_ref(), direction.0);
                                b.gl.uniform_1_f32(dir_y.as_ref(), direction.1);
                                b.gl.uniform_1_f32(full_size.as_ref(), kernel_size);
                                b.gl.uniform_1_f32(m.as_ref(), m_value);
                                b.gl.uniform_1_f32(m2.as_ref(), m_value * 2.0);
                                b.gl.uniform_1_f32(first_weight.as_ref(), alpha);
                                b.gl.uniform_1_f32(last_offset.as_ref(), last_offset_value);
                                b.gl.uniform_1_f32(last_weight.as_ref(), last_weight_value);
                            }
                            Ok(())
                        },
                    )?;

                    changed = true;
                    current = destination;
                    std::mem::swap(&mut destination, &mut alternate);
                }
            }
            Ok(())
        })?;

        Ok(if changed { current } else { source_texture })
    }

    fn run_color_matrix_filter(
        &mut self,
        source_texture: glow::Texture,
        destination_texture: glow::Texture,
        width: u32,
        height: u32,
        filter: &swf::ColorMatrixFilter,
    ) -> Result<(), BitmapError> {
        let texture_sampler = self
            .filter_color_matrix_program
            .uniform("texture_sampler")
            .cloned();
        let color_matrix = self.filter_color_matrix_program.uniform("color_matrix").cloned();
        let program = self.filter_color_matrix_program.program;
        let quad = (self.filter_common_quad.vao, self.filter_common_quad.vertex_buffer);
        let vertices = Self::common_filter_vertices();

        self.render_filter_to_texture(
            destination_texture,
            width,
            height,
            program,
            quad.0,
            quad.1,
            bytemuck::cast_slice(&vertices),
            |backend| {
                backend.set_filter_texture_parameters(
                    0,
                    source_texture,
                    glow::LINEAR as i32,
                    glow::CLAMP_TO_EDGE as i32,
                );
                unsafe {
                    backend
                        .gl
                        .uniform_1_i32(texture_sampler.as_ref(), 0);
                    backend
                        .gl
                        .uniform_1_f32_slice(color_matrix.as_ref(), &filter.matrix);
                }
                Ok(())
            },
        )
    }

    fn run_glow_filter(
        &mut self,
        source_texture: glow::Texture,
        destination_texture: glow::Texture,
        width: u32,
        height: u32,
        scratch_a: glow::Texture,
        scratch_b: glow::Texture,
        filter: &swf::GlowFilter,
        offset: (f32, f32),
    ) -> Result<(), BitmapError> {
        let blurred_texture = self.blur_texture(
            source_texture,
            width,
            height,
            scratch_a,
            scratch_b,
            &filter.inner_blur_filter(),
        )?;
        let source_uniform = self.filter_glow_program.uniform("source_texture").cloned();
        let blur_uniform = self.filter_glow_program.uniform("blur_texture").cloned();
        let glow_color = self.filter_glow_program.uniform("glow_color").cloned();
        let strength = self.filter_glow_program.uniform("strength").cloned();
        let inner = self.filter_glow_program.uniform("inner").cloned();
        let knockout = self.filter_glow_program.uniform("knockout").cloned();
        let composite_source = self
            .filter_glow_program
            .uniform("composite_source")
            .cloned();
        let program = self.filter_glow_program.program;
        let quad = (self.filter_glow_quad.vao, self.filter_glow_quad.vertex_buffer);
        let vertices = Self::glow_filter_vertices(offset, width, height);
        let color = Self::normalized_rgba(filter.color);

        self.render_filter_to_texture(
            destination_texture,
            width,
            height,
            program,
            quad.0,
            quad.1,
            bytemuck::cast_slice(&vertices),
            |backend| {
                backend.set_filter_texture_parameters(
                    0,
                    source_texture,
                    glow::NEAREST as i32,
                    glow::CLAMP_TO_EDGE as i32,
                );
                backend.set_filter_texture_parameters(
                    1,
                    blurred_texture,
                    glow::NEAREST as i32,
                    glow::CLAMP_TO_EDGE as i32,
                );
                unsafe {
                    backend.gl.uniform_1_i32(source_uniform.as_ref(), 0);
                    backend.gl.uniform_1_i32(blur_uniform.as_ref(), 1);
                    backend.gl.uniform_4_f32_slice(glow_color.as_ref(), &color);
                    backend.gl.uniform_1_f32(strength.as_ref(), filter.strength.to_f32());
                    backend
                        .gl
                        .uniform_1_i32(inner.as_ref(), if filter.is_inner() { 1 } else { 0 });
                    backend.gl.uniform_1_i32(
                        knockout.as_ref(),
                        if filter.is_knockout() { 1 } else { 0 },
                    );
                    backend.gl.uniform_1_i32(
                        composite_source.as_ref(),
                        if filter.composite_source() { 1 } else { 0 },
                    );
                }
                Ok(())
            },
        )
    }

    fn run_bevel_filter(
        &mut self,
        source_texture: glow::Texture,
        destination_texture: glow::Texture,
        width: u32,
        height: u32,
        scratch_a: glow::Texture,
        scratch_b: glow::Texture,
        filter: &swf::BevelFilter,
    ) -> Result<(), BitmapError> {
        let blurred_texture = self.blur_texture(
            source_texture,
            width,
            height,
            scratch_a,
            scratch_b,
            &filter.inner_blur_filter(),
        )?;
        let source_uniform = self.filter_bevel_program.uniform("source_texture").cloned();
        let blur_uniform = self.filter_bevel_program.uniform("blur_texture").cloned();
        let highlight_color = self.filter_bevel_program.uniform("highlight_color").cloned();
        let shadow_color = self.filter_bevel_program.uniform("shadow_color").cloned();
        let strength = self.filter_bevel_program.uniform("strength").cloned();
        let bevel_type = self.filter_bevel_program.uniform("bevel_type").cloned();
        let knockout = self.filter_bevel_program.uniform("knockout").cloned();
        let composite_source = self
            .filter_bevel_program
            .uniform("composite_source")
            .cloned();
        let program = self.filter_bevel_program.program;
        let quad = (self.filter_bevel_quad.vao, self.filter_bevel_quad.vertex_buffer);
        let distance = filter.distance.to_f32();
        let angle = filter.angle.to_f32();
        let vertices = Self::bevel_filter_vertices(
            (angle.cos() * distance, angle.sin() * distance),
            width,
            height,
        );
        let highlight = Self::premultiplied_rgba(filter.highlight_color);
        let shadow = Self::premultiplied_rgba(filter.shadow_color);

        self.render_filter_to_texture(
            destination_texture,
            width,
            height,
            program,
            quad.0,
            quad.1,
            bytemuck::cast_slice(&vertices),
            |backend| {
                backend.set_filter_texture_parameters(
                    0,
                    source_texture,
                    glow::NEAREST as i32,
                    glow::CLAMP_TO_EDGE as i32,
                );
                backend.set_filter_texture_parameters(
                    1,
                    blurred_texture,
                    glow::NEAREST as i32,
                    glow::CLAMP_TO_EDGE as i32,
                );
                unsafe {
                    backend.gl.uniform_1_i32(source_uniform.as_ref(), 0);
                    backend.gl.uniform_1_i32(blur_uniform.as_ref(), 1);
                    backend
                        .gl
                        .uniform_4_f32_slice(highlight_color.as_ref(), &highlight);
                    backend
                        .gl
                        .uniform_4_f32_slice(shadow_color.as_ref(), &shadow);
                    backend.gl.uniform_1_f32(strength.as_ref(), filter.strength.to_f32());
                    backend.gl.uniform_1_i32(
                        bevel_type.as_ref(),
                        if filter.is_on_top() {
                            2
                        } else if filter.is_inner() {
                            1
                        } else {
                            0
                        },
                    );
                    backend.gl.uniform_1_i32(
                        knockout.as_ref(),
                        if filter.is_knockout() { 1 } else { 0 },
                    );
                    backend.gl.uniform_1_i32(composite_source.as_ref(), 1);
                }
                Ok(())
            },
        )
    }

    fn run_displacement_map_filter(
        &mut self,
        source_texture: glow::Texture,
        destination_texture: glow::Texture,
        width: u32,
        height: u32,
        filter: &ruffle_render::filters::DisplacementMapFilter,
    ) -> Result<bool, BitmapError> {
        let map_handle = match &filter.map_bitmap {
            Some(handle) => handle,
            None => return Ok(false),
        };
        let map = as_registry_data(map_handle);
        let source_uniform = self
            .filter_displacement_map_program
            .uniform("source_texture")
            .cloned();
        let map_uniform = self
            .filter_displacement_map_program
            .uniform("map_texture")
            .cloned();
        let filter_color = self
            .filter_displacement_map_program
            .uniform("filter_color")
            .cloned();
        let components_packed = self
            .filter_displacement_map_program
            .uniform("components_packed")
            .cloned();
        let mode = self.filter_displacement_map_program.uniform("mode").cloned();
        let scale_x = self
            .filter_displacement_map_program
            .uniform("scale_x")
            .cloned();
        let scale_y = self
            .filter_displacement_map_program
            .uniform("scale_y")
            .cloned();
        let source_width = self
            .filter_displacement_map_program
            .uniform("source_width")
            .cloned();
        let source_height = self
            .filter_displacement_map_program
            .uniform("source_height")
            .cloned();
        let map_width = self
            .filter_displacement_map_program
            .uniform("map_width")
            .cloned();
        let map_height = self
            .filter_displacement_map_program
            .uniform("map_height")
            .cloned();
        let offset_x = self
            .filter_displacement_map_program
            .uniform("offset_x")
            .cloned();
        let offset_y = self
            .filter_displacement_map_program
            .uniform("offset_y")
            .cloned();
        let viewscale_x = self
            .filter_displacement_map_program
            .uniform("viewscale_x")
            .cloned();
        let viewscale_y = self
            .filter_displacement_map_program
            .uniform("viewscale_y")
            .cloned();
        let program = self.filter_displacement_map_program.program;
        let quad = (
            self.filter_displacement_quad.vao,
            self.filter_displacement_quad.vertex_buffer,
        );
        let vertices = Self::common_filter_vertices();
        let color = Self::normalized_rgba(filter.color);

        self.render_filter_to_texture(
            destination_texture,
            width,
            height,
            program,
            quad.0,
            quad.1,
            bytemuck::cast_slice(&vertices),
            |backend| {
                backend.set_filter_texture_parameters(
                    0,
                    source_texture,
                    glow::LINEAR as i32,
                    glow::CLAMP_TO_EDGE as i32,
                );
                backend.set_filter_texture_parameters(
                    1,
                    map.texture,
                    glow::NEAREST as i32,
                    glow::CLAMP_TO_EDGE as i32,
                );
                unsafe {
                    backend.gl.uniform_1_i32(source_uniform.as_ref(), 0);
                    backend.gl.uniform_1_i32(map_uniform.as_ref(), 1);
                    backend.gl.uniform_4_f32_slice(filter_color.as_ref(), &color);
                    backend.gl.uniform_1_u32(
                        components_packed.as_ref(),
                        ((filter.component_x as u32) << 8) | (filter.component_y as u32),
                    );
                    backend.gl.uniform_1_u32(
                        mode.as_ref(),
                        match filter.mode {
                            DisplacementMapFilterMode::Wrap => 0,
                            DisplacementMapFilterMode::Clamp => 1,
                            DisplacementMapFilterMode::Ignore => 2,
                            DisplacementMapFilterMode::Color => 3,
                        },
                    );
                    backend.gl.uniform_1_f32(scale_x.as_ref(), filter.scale_x);
                    backend.gl.uniform_1_f32(scale_y.as_ref(), filter.scale_y);
                    backend.gl.uniform_1_f32(source_width.as_ref(), width as f32);
                    backend.gl.uniform_1_f32(source_height.as_ref(), height as f32);
                    backend.gl.uniform_1_f32(map_width.as_ref(), map.width.get() as f32);
                    backend.gl.uniform_1_f32(map_height.as_ref(), map.height.get() as f32);
                    backend.gl.uniform_1_f32(offset_x.as_ref(), filter.map_point.0 as f32);
                    backend.gl.uniform_1_f32(offset_y.as_ref(), filter.map_point.1 as f32);
                    backend.gl.uniform_1_f32(viewscale_x.as_ref(), filter.viewscale_x);
                    backend.gl.uniform_1_f32(viewscale_y.as_ref(), filter.viewscale_y);
                }
                Ok(())
            },
        )?;

        Ok(true)
    }

    fn blit_texture(
        &mut self,
        source_texture: glow::Texture,
        destination_texture: glow::Texture,
        width: u32,
        height: u32,
    ) -> Result<(), BitmapError> {
        self.run_color_matrix_filter(
            source_texture,
            destination_texture,
            width,
            height,
            &swf::ColorMatrixFilter {
                matrix: Self::identity_color_matrix(),
            },
        )
    }

    fn apply_cache_entry_filters(
        &mut self,
        destination_texture: glow::Texture,
        width: u32,
        height: u32,
        filters: &[Filter],
    ) -> Result<(), BitmapError> {
        let temp_textures = [
            self.acquire_scratch_texture(width, height)?,
            self.acquire_scratch_texture(width, height)?,
            self.acquire_scratch_texture(width, height)?,
        ];
        let temp_handles = [
            temp_textures[0].texture,
            temp_textures[1].texture,
            temp_textures[2].texture,
        ];
        let mut current = destination_texture;
        let mut result = Ok(());

        for filter in filters {
            if filter.impotent() {
                continue;
            }

            let step_result = match filter {
                Filter::BlurFilter(filter) => {
                    let (scratch_a, scratch_b) =
                        Self::pick_two_textures_excluding(&temp_handles, &[current]);
                    self.blur_texture(current, width, height, scratch_a, scratch_b, filter)
                        .map(|next| {
                            current = next;
                        })
                }
                Filter::GlowFilter(filter) => {
                    let (scratch_a, scratch_b) =
                        Self::pick_two_textures_excluding(&temp_handles, &[current]);
                    let output = Self::pick_output_texture(
                        destination_texture,
                        &temp_handles,
                        &[current, scratch_a, scratch_b],
                    );
                    self.run_glow_filter(
                        current,
                        output,
                        width,
                        height,
                        scratch_a,
                        scratch_b,
                        filter,
                        (0.0, 0.0),
                    )
                    .map(|_| {
                        current = output;
                    })
                }
                Filter::DropShadowFilter(filter) => {
                    let (scratch_a, scratch_b) =
                        Self::pick_two_textures_excluding(&temp_handles, &[current]);
                    let output = Self::pick_output_texture(
                        destination_texture,
                        &temp_handles,
                        &[current, scratch_a, scratch_b],
                    );
                    let distance = filter.distance.to_f32();
                    let angle = filter.angle.to_f32();
                    let glow_filter = filter.inner_glow_filter();
                    self.run_glow_filter(
                        current,
                        output,
                        width,
                        height,
                        scratch_a,
                        scratch_b,
                        &glow_filter,
                        (-angle.cos() * distance, -angle.sin() * distance),
                    )
                    .map(|_| {
                        current = output;
                    })
                }
                Filter::ColorMatrixFilter(filter) => {
                    let output = Self::pick_output_texture(
                        destination_texture,
                        &temp_handles,
                        &[current],
                    );
                    self.run_color_matrix_filter(current, output, width, height, filter)
                        .map(|_| {
                            current = output;
                        })
                }
                Filter::BevelFilter(filter) => {
                    let (scratch_a, scratch_b) =
                        Self::pick_two_textures_excluding(&temp_handles, &[current]);
                    let output = Self::pick_output_texture(
                        destination_texture,
                        &temp_handles,
                        &[current, scratch_a, scratch_b],
                    );
                    self.run_bevel_filter(
                        current,
                        output,
                        width,
                        height,
                        scratch_a,
                        scratch_b,
                        filter,
                    )
                    .map(|_| {
                        current = output;
                    })
                }
                Filter::DisplacementMapFilter(filter) => {
                    let output = Self::pick_output_texture(
                        destination_texture,
                        &temp_handles,
                        &[current],
                    );
                    self.run_displacement_map_filter(current, output, width, height, filter)
                        .map(|applied| {
                            if applied {
                                current = output;
                            }
                        })
                }
                unsupported => {
                    log::warn!("Unsupported cache filter on glow backend: {unsupported:?}");
                    Ok(())
                }
            };

            if let Err(error) = step_result {
                result = Err(error);
                break;
            }
        }

        if result.is_ok() && current != destination_texture {
            result = self.blit_texture(current, destination_texture, width, height);
        }

        for texture in temp_textures {
            self.release_scratch_texture(texture);
        }
        self.active_texture_2d.set(None);

        result
    }

    fn register_shape_internal(
        &mut self,
        shape: DistilledShape,
        bitmap_source: &dyn BitmapSource,
    ) -> Result<Mesh, GlError> {
        use ruffle_render::tessellator::DrawType as TessDrawType;

        let lyon_mesh = self
            .shape_tessellator
            .tessellate_shape(shape, bitmap_source);

        let mut draws = Vec::with_capacity(lyon_mesh.draws.len());
        for draw in lyon_mesh.draws {
            let num_indices = draw.indices.len() as i32;
            let num_mask_indices = draw.mask_index_count as i32;

            let vao = self.create_vertex_array()?;
            let vertex_buffer = unsafe {
                self.gl
                    .create_buffer()
                    .map_err(|_| GlError::UnableToCreateBuffer)?
            };
            unsafe {
                self.gl.bind_buffer(glow::ARRAY_BUFFER, Some(vertex_buffer));
            }

            let vertices: Vec<_> = draw.vertices.into_iter().map(Vertex::from).collect();
            unsafe {
                self.gl.buffer_data_u8_slice(
                    glow::ARRAY_BUFFER,
                    bytemuck::cast_slice(&vertices),
                    glow::STATIC_DRAW,
                );
            }

            let index_buffer = unsafe {
                self.gl
                    .create_buffer()
                    .map_err(|_| GlError::UnableToCreateBuffer)?
            };
            unsafe {
                self.gl
                    .bind_buffer(glow::ELEMENT_ARRAY_BUFFER, Some(index_buffer));
                self.gl.buffer_data_u8_slice(
                    glow::ELEMENT_ARRAY_BUFFER,
                    bytemuck::cast_slice(&draw.indices),
                    glow::STATIC_DRAW,
                );
            }

            let program = match draw.draw_type {
                TessDrawType::Color => &self.color_program,
                TessDrawType::Gradient { .. } => &self.gradient_program,
                TessDrawType::Bitmap(_) => &self.bitmap_program,
            };

            if program.vertex_position_location != 0xffff_ffff {
                unsafe {
                    self.gl.vertex_attrib_pointer_f32(
                        program.vertex_position_location,
                        2,
                        glow::FLOAT,
                        false,
                        12,
                        0,
                    );
                    self.gl
                        .enable_vertex_attrib_array(program.vertex_position_location);
                }
            }

            if program.vertex_color_location != 0xffff_ffff {
                unsafe {
                    self.gl.vertex_attrib_pointer_f32(
                        program.vertex_color_location,
                        4,
                        glow::UNSIGNED_BYTE,
                        true,
                        12,
                        8,
                    );
                    self.gl
                        .enable_vertex_attrib_array(program.vertex_color_location);
                }
            }

            let num_vertex_attributes = program.num_vertex_attributes;

            draws.push(match draw.draw_type {
                TessDrawType::Color => Draw {
                    draw_type: DrawType::Color,
                    vao,
                    vertex_buffer: Some(vertex_buffer),
                    index_buffer,
                    num_indices,
                    num_mask_indices,
                },
                TessDrawType::Gradient { matrix, gradient } => Draw {
                    draw_type: DrawType::Gradient(Box::new(Gradient::new(
                        lyon_mesh.gradients[gradient].clone(),
                        matrix,
                    ))),
                    vao,
                    vertex_buffer: Some(vertex_buffer),
                    index_buffer,
                    num_indices,
                    num_mask_indices,
                },
                TessDrawType::Bitmap(bitmap) => Draw {
                    draw_type: DrawType::Bitmap(BitmapDraw {
                        matrix: bitmap.matrix,
                        handle: bitmap_source.bitmap_handle(bitmap.bitmap_id, self),
                        is_smoothed: bitmap.is_smoothed,
                        is_repeating: bitmap.is_repeating,
                    }),
                    vao,
                    vertex_buffer: Some(vertex_buffer),
                    index_buffer,
                    num_indices,
                    num_mask_indices,
                },
            });

            self.bind_vertex_array(None);

            for i in num_vertex_attributes..NUM_VERTEX_ATTRIBUTES {
                unsafe {
                    self.gl.disable_vertex_attrib_array(i);
                }
            }
        }

        Ok(Mesh {
            gl: self.gl.clone(),
            draws,
        })
    }

    fn create_vertex_array(&self) -> Result<glow::VertexArray, GlError> {
        unsafe {
            let vao = self
                .gl
                .create_vertex_array()
                .map_err(|_| GlError::UnableToCreateVAO)?;
            self.gl.bind_vertex_array(Some(vao));
            Ok(vao)
        }
    }

    fn bind_vertex_array(&self, vao: Option<glow::VertexArray>) {
        unsafe {
            self.gl.bind_vertex_array(vao);
        }
    }

    fn bind_texture_2d(&self, texture: glow::Texture) {
        if self.active_texture_2d.get() != Some(texture) {
            unsafe {
                self.gl.active_texture(glow::TEXTURE0);
                self.gl.bind_texture(glow::TEXTURE_2D, Some(texture));
            }
            self.active_texture_2d.set(Some(texture));
        }
    }

    fn set_texture_parameters(&self, entry: &RegistryData, filter: i32, wrap: i32) {
        unsafe {
            if entry.mag_filter.get() != filter {
                self.gl
                    .tex_parameter_i32(glow::TEXTURE_2D, glow::TEXTURE_MAG_FILTER, filter);
                entry.mag_filter.set(filter);
            }
            if entry.min_filter.get() != filter {
                self.gl
                    .tex_parameter_i32(glow::TEXTURE_2D, glow::TEXTURE_MIN_FILTER, filter);
                entry.min_filter.set(filter);
            }
            if entry.wrap_s.get() != wrap {
                self.gl
                    .tex_parameter_i32(glow::TEXTURE_2D, glow::TEXTURE_WRAP_S, wrap);
                entry.wrap_s.set(wrap);
            }
            if entry.wrap_t.get() != wrap {
                self.gl
                    .tex_parameter_i32(glow::TEXTURE_2D, glow::TEXTURE_WRAP_T, wrap);
                entry.wrap_t.set(wrap);
            }
        }
    }

    fn set_stencil_state(&mut self) {
        if self.mask_state_dirty {
            unsafe {
                match self.mask_state {
                    MaskState::NoMask => {
                        self.gl.disable(glow::STENCIL_TEST);
                        self.gl.color_mask(true, true, true, true);
                    }
                    MaskState::DrawMaskStencil => {
                        self.gl.enable(glow::STENCIL_TEST);
                        self.gl
                            .stencil_func(glow::EQUAL, (self.num_masks - 1) as i32, 0xff);
                        self.gl.stencil_op(glow::KEEP, glow::KEEP, glow::INCR);
                        self.gl.color_mask(false, false, false, false);
                    }
                    MaskState::DrawMaskedContent => {
                        self.gl.enable(glow::STENCIL_TEST);
                        self.gl
                            .stencil_func(glow::EQUAL, self.num_masks as i32, 0xff);
                        self.gl.stencil_op(glow::KEEP, glow::KEEP, glow::KEEP);
                        self.gl.color_mask(true, true, true, true);
                    }
                    MaskState::ClearMaskStencil => {
                        self.gl.enable(glow::STENCIL_TEST);
                        self.gl
                            .stencil_func(glow::EQUAL, self.num_masks as i32, 0xff);
                        self.gl.stencil_op(glow::KEEP, glow::KEEP, glow::DECR);
                        self.gl.color_mask(false, false, false, false);
                    }
                }
            }
            self.mask_state_dirty = false;
        }
    }

    fn apply_blend_mode(&mut self, mode: RenderBlendMode) {
        let (blend_op, src_rgb, dst_rgb) = match mode {
            RenderBlendMode::Builtin(BlendMode::Normal) => {
                (glow::FUNC_ADD, glow::ONE, glow::ONE_MINUS_SRC_ALPHA)
            }
            RenderBlendMode::Builtin(BlendMode::Add) => (glow::FUNC_ADD, glow::ONE, glow::ONE),
            RenderBlendMode::Builtin(BlendMode::Subtract) => {
                (glow::FUNC_REVERSE_SUBTRACT, glow::ONE, glow::ONE)
            }
            _ => (glow::FUNC_ADD, glow::ONE, glow::ONE_MINUS_SRC_ALPHA),
        };
        unsafe {
            self.gl.blend_equation_separate(blend_op, glow::FUNC_ADD);
            self.gl
                .blend_func_separate(src_rgb, dst_rgb, glow::ONE, glow::ONE_MINUS_SRC_ALPHA);
        }
    }

    fn screen_surface(&self, clear: Color) -> GlSurface {
        GlSurface {
            target: GlTarget::DefaultFramebuffer,
            width: self.renderbuffer_width.max(1),
            height: self.renderbuffer_height.max(1),
            clear_color: Some(if self.is_transparent {
                [0.0, 0.0, 0.0, 0.0]
            } else {
                Self::normalized_rgba(clear)
            }),
            y_flip: true,
        }
    }

    fn texture_surface(
        &self,
        texture: glow::Texture,
        width: u32,
        height: u32,
        clear_color: Option<Color>,
    ) -> GlSurface {
        GlSurface {
            target: GlTarget::Texture(texture),
            width: width.max(1) as i32,
            height: height.max(1) as i32,
            clear_color: clear_color.map(Self::normalized_rgba),
            y_flip: false,
        }
    }

    fn surface_view_matrix(surface: GlSurface) -> [[f32; 4]; 4] {
        let width = surface.width.max(1) as f32;
        let height = surface.height.max(1) as f32;
        if surface.y_flip {
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

    fn save_surface_state(&mut self) -> GlSurfaceState {
        GlSurfaceState {
            renderbuffer_width: self.renderbuffer_width,
            renderbuffer_height: self.renderbuffer_height,
            view_matrix: self.view_matrix,
            blend_modes: std::mem::take(&mut self.blend_modes),
        }
    }

    fn restore_surface_state(&mut self, state: GlSurfaceState) {
        self.renderbuffer_width = state.renderbuffer_width;
        self.renderbuffer_height = state.renderbuffer_height;
        self.view_matrix = state.view_matrix;

        unsafe {
            self.gl
                .viewport(0, 0, self.renderbuffer_width, self.renderbuffer_height);
        }

        self.active_program = std::ptr::null();
        self.mask_state = MaskState::NoMask;
        self.num_masks = 0;
        self.mask_state_dirty = true;
        self.mult_color = None;
        self.add_color = None;
        self.blend_modes = state.blend_modes;
        if let Some(blend_mode) = self.blend_modes.last().cloned() {
            self.apply_blend_mode(blend_mode);
        } else {
            self.push_blend_mode(RenderBlendMode::Builtin(BlendMode::Normal));
        }
    }

    fn reset_render_state(&mut self) {
        self.active_program = std::ptr::null();
        self.mask_state = MaskState::NoMask;
        self.num_masks = 0;
        self.mask_state_dirty = true;
        self.mult_color = None;
        self.add_color = None;
        self.blend_modes.clear();
        self.push_blend_mode(RenderBlendMode::Builtin(BlendMode::Normal));
        self.set_stencil_state();
    }

    fn bind_surface_target(&mut self, surface: GlSurface) -> Result<(), BitmapError> {
        match surface.target {
            GlTarget::DefaultFramebuffer => unsafe {
                self.gl.bind_framebuffer(glow::FRAMEBUFFER, None);
            },
            GlTarget::Texture(texture) => {
                self.ensure_offscreen_stencil(surface.width, surface.height);
                unsafe {
                    self.gl
                        .bind_framebuffer(glow::FRAMEBUFFER, Some(self.offscreen_fbo));
                    self.gl.framebuffer_texture_2d(
                        glow::FRAMEBUFFER,
                        glow::COLOR_ATTACHMENT0,
                        glow::TEXTURE_2D,
                        Some(texture),
                        0,
                    );
                    self.gl.framebuffer_renderbuffer(
                        glow::FRAMEBUFFER,
                        glow::STENCIL_ATTACHMENT,
                        glow::RENDERBUFFER,
                        Some(self.offscreen_stencil_rbo),
                    );
                }

                let status = unsafe { self.gl.check_framebuffer_status(glow::FRAMEBUFFER) };
                if status != glow::FRAMEBUFFER_COMPLETE {
                    self.unbind_surface_target(surface);
                    return Err(BitmapError::Unimplemented(
                        format!("Framebuffer not complete: {status:#x}").into(),
                    ));
                }
            }
        }

        self.renderbuffer_width = surface.width.max(1);
        self.renderbuffer_height = surface.height.max(1);
        self.view_matrix = Self::surface_view_matrix(surface);

        unsafe {
            self.gl
                .viewport(0, 0, self.renderbuffer_width, self.renderbuffer_height);
        }

        self.reset_render_state();

        if let Some(clear) = surface.clear_color {
            unsafe {
                self.gl.clear_color(clear[0], clear[1], clear[2], clear[3]);
                self.gl.stencil_mask(0xff);
                self.gl
                    .clear(glow::COLOR_BUFFER_BIT | glow::STENCIL_BUFFER_BIT);
            }
        }

        Ok(())
    }

    fn unbind_surface_target(&mut self, surface: GlSurface) {
        match surface.target {
            GlTarget::DefaultFramebuffer => unsafe {
                self.gl.bind_framebuffer(glow::FRAMEBUFFER, None);
            },
            GlTarget::Texture(_) => unsafe {
                self.gl.framebuffer_texture_2d(
                    glow::FRAMEBUFFER,
                    glow::COLOR_ATTACHMENT0,
                    glow::TEXTURE_2D,
                    None,
                    0,
                );
                self.gl.framebuffer_renderbuffer(
                    glow::FRAMEBUFFER,
                    glow::STENCIL_ATTACHMENT,
                    glow::RENDERBUFFER,
                    None,
                );
                self.gl.bind_framebuffer(glow::FRAMEBUFFER, None);
            },
        }
    }

    fn render_commands_to_surface(
        &mut self,
        surface: GlSurface,
        commands: CommandList,
    ) -> Result<(), BitmapError> {
        let saved_state = self.save_surface_state();
        let result = (|| {
            self.bind_surface_target(surface)?;
            commands.execute(self);
            Ok(())
        })();
        self.unbind_surface_target(surface);
        self.restore_surface_state(saved_state);
        result
    }

    fn render_to_screen(&mut self, clear: Color, commands: CommandList) -> Result<(), BitmapError> {
        self.render_commands_to_surface(self.screen_surface(clear), commands)
    }

    fn render_to_texture(
        &mut self,
        texture: glow::Texture,
        width: u32,
        height: u32,
        clear_color: Option<Color>,
        commands: CommandList,
    ) -> Result<(), BitmapError> {
        self.render_commands_to_surface(
            self.texture_surface(texture, width, height, clear_color),
            commands,
        )
    }

    fn push_blend_mode(&mut self, blend: RenderBlendMode) {
        if !same_blend_mode(self.blend_modes.last(), &blend) {
            self.apply_blend_mode(blend.clone());
        }
        self.blend_modes.push(blend);
    }

    fn pop_blend_mode(&mut self) {
        let old = self.blend_modes.pop();
        let current = self
            .blend_modes
            .last()
            .unwrap_or(&RenderBlendMode::Builtin(BlendMode::Normal));
        if !same_blend_mode(old.as_ref(), current) {
            self.apply_blend_mode(current.clone());
        }
    }

    fn delete_pending_readback(&self, pending: PendingReadback) {
        unsafe {
            self.gl.delete_sync(pending.fence);
            self.gl.delete_buffer(pending.pbo);
        }
    }

    fn ensure_offscreen_stencil(&mut self, width: i32, height: i32) {
        if self.offscreen_stencil_size == (width, height) {
            return;
        }

        unsafe {
            self.gl
                .bind_renderbuffer(glow::RENDERBUFFER, Some(self.offscreen_stencil_rbo));
            self.gl.renderbuffer_storage(
                glow::RENDERBUFFER,
                glow::STENCIL_INDEX8,
                width,
                height,
            );
            self.gl.bind_renderbuffer(glow::RENDERBUFFER, None);
        }

        self.offscreen_stencil_size = (width, height);
    }

    fn acquire_readback_pbo(&mut self, size_bytes: usize) -> Result<ReadbackPbo, BitmapError> {
        let index = self
            .reusable_readback_pbos
            .iter()
            .enumerate()
            .filter(|(_, pbo)| pbo.size_bytes >= size_bytes)
            .min_by_key(|(_, pbo)| pbo.size_bytes)
            .map(|(index, _)| index);

        if let Some(index) = index {
            Ok(self.reusable_readback_pbos.swap_remove(index))
        } else {
            let buffer = unsafe {
                self.gl.create_buffer().map_err(|_| {
                    BitmapError::Unimplemented("Unable to create pixel pack buffer".into())
                })?
            };
            Ok(ReadbackPbo {
                buffer,
                size_bytes: 0,
            })
        }
    }

    fn recycle_readback_pbo(&mut self, buffer: glow::Buffer, size_bytes: usize) {
        self.reusable_readback_pbos
            .push(ReadbackPbo { buffer, size_bytes });
    }

    fn collect_released_resources(&mut self) {
        self.texture_resources.retain(|resource| resource.weak.strong_count() > 0);
    }

    fn collect_orphaned_readbacks(&mut self) {
        let stale_ids: Vec<_> = self
            .pending_readbacks
            .iter()
            .filter_map(|(id, pending)| (Arc::strong_count(&pending.token) == 1).then_some(*id))
            .collect();

        for id in stale_ids {
            if let Some(pending) = self.pending_readbacks.remove(&id) {
                self.delete_pending_readback(pending);
            }
        }
    }

    fn enqueue_async_readback(
        &mut self,
        texture: glow::Texture,
        bounds: PixelRegion,
    ) -> Result<(u64, u32, Arc<()>), BitmapError> {
        let byte_len = (bounds.width() * bounds.height() * 4) as usize;
        let mut readback_pbo = self.acquire_readback_pbo(byte_len)?;

        unsafe {
            self.gl
                .bind_buffer(glow::PIXEL_PACK_BUFFER, Some(readback_pbo.buffer));
            if readback_pbo.size_bytes < byte_len {
                self.gl.buffer_data_size(
                    glow::PIXEL_PACK_BUFFER,
                    byte_len as i32,
                    glow::STREAM_READ,
                );
                readback_pbo.size_bytes = byte_len;
            }

            self.gl
                .bind_framebuffer(glow::FRAMEBUFFER, Some(self.offscreen_fbo));
            self.gl.framebuffer_texture_2d(
                glow::FRAMEBUFFER,
                glow::COLOR_ATTACHMENT0,
                glow::TEXTURE_2D,
                Some(texture),
                0,
            );

            let status = self.gl.check_framebuffer_status(glow::FRAMEBUFFER);
            if status != glow::FRAMEBUFFER_COMPLETE {
                self.gl.framebuffer_texture_2d(
                    glow::FRAMEBUFFER,
                    glow::COLOR_ATTACHMENT0,
                    glow::TEXTURE_2D,
                    None,
                    0,
                );
                self.gl.bind_framebuffer(glow::FRAMEBUFFER, None);
                self.gl.bind_buffer(glow::PIXEL_PACK_BUFFER, None);
                self.gl.delete_buffer(readback_pbo.buffer);
                return Err(BitmapError::Unimplemented(
                    format!("Framebuffer not complete for async pixel readback: {status:#x}").into(),
                ));
            }

            self.gl.read_pixels(
                bounds.x_min as i32,
                bounds.y_min as i32,
                bounds.width() as i32,
                bounds.height() as i32,
                glow::RGBA,
                glow::UNSIGNED_BYTE,
                glow::PixelPackData::BufferOffset(0),
            );

            let fence = match self.gl.fence_sync(glow::SYNC_GPU_COMMANDS_COMPLETE, 0) {
                Ok(fence) => fence,
                Err(e) => {
                    self.gl.framebuffer_texture_2d(
                        glow::FRAMEBUFFER,
                        glow::COLOR_ATTACHMENT0,
                        glow::TEXTURE_2D,
                        None,
                        0,
                    );
                    self.gl.bind_framebuffer(glow::FRAMEBUFFER, None);
                    self.gl.bind_buffer(glow::PIXEL_PACK_BUFFER, None);
                    self.gl.delete_buffer(readback_pbo.buffer);
                    return Err(BitmapError::Unimplemented(e.into()));
                }
            };

            self.gl.framebuffer_texture_2d(
                glow::FRAMEBUFFER,
                glow::COLOR_ATTACHMENT0,
                glow::TEXTURE_2D,
                None,
                0,
            );
            self.gl.bind_framebuffer(glow::FRAMEBUFFER, None);
            self.gl.bind_buffer(glow::PIXEL_PACK_BUFFER, None);

            let readback_id = self.next_readback_id;
            self.next_readback_id += 1;
            let token = Arc::new(());
            self.pending_readbacks.insert(
                readback_id,
                PendingReadback {
                    pbo: readback_pbo.buffer,
                    size_bytes: readback_pbo.size_bytes,
                    fence,
                    width: bounds.width(),
                    height: bounds.height(),
                    token: token.clone(),
                },
            );
            Ok((readback_id, bounds.width(), token))
        }
    }

    fn resolve_async_readback(
        &mut self,
        readback_id: u64,
        with_rgba: RgbaBufRead,
    ) -> Result<(), BitmapError> {
        let pending = self
            .pending_readbacks
            .remove(&readback_id)
            .ok_or_else(|| BitmapError::Unimplemented("Missing async readback".into()))?;

        let byte_len = (pending.width * pending.height * 4) as usize;
        unsafe {
            let wait_result = self.gl.client_wait_sync(
                pending.fence,
                glow::SYNC_FLUSH_COMMANDS_BIT,
                i32::MAX,
            );

            if wait_result == glow::WAIT_FAILED {
                self.delete_pending_readback(pending);
                return Err(BitmapError::Unimplemented("Async readback wait failed".into()));
            }

            self.gl.bind_buffer(glow::PIXEL_PACK_BUFFER, Some(pending.pbo));
            let ptr = self.gl.map_buffer_range(
                glow::PIXEL_PACK_BUFFER,
                0,
                byte_len.max(1) as i32,
                glow::MAP_READ_BIT,
            );
            if ptr.is_null() {
                self.gl.bind_buffer(glow::PIXEL_PACK_BUFFER, None);
                self.delete_pending_readback(pending);
                return Err(BitmapError::Unimplemented("Failed to map async readback buffer".into()));
            }

            let data = std::slice::from_raw_parts(ptr, byte_len);
            if !data.is_empty() {
                with_rgba(data, pending.width * 4);
            }
            self.gl.unmap_buffer(glow::PIXEL_PACK_BUFFER);
            self.gl.bind_buffer(glow::PIXEL_PACK_BUFFER, None);
            self.gl.delete_sync(pending.fence);
            self.recycle_readback_pbo(pending.pbo, pending.size_bytes);
        }

        Ok(())
    }

    /// Read pixels from a texture region by attaching it to the offscreen FBO.
    fn read_texture_pixels(&self, handle: &BitmapHandle, mut bounds: PixelRegion) -> Vec<u8> {
        let entry = as_registry_data(handle);
        bounds.clamp(entry.width.get(), entry.height.get());
        if bounds.width() == 0 || bounds.height() == 0 {
            return Vec::new();
        }

        let pixel_count = (bounds.width() * bounds.height()) as usize;
        let mut pixel_data = vec![0u8; pixel_count * 4];

        unsafe {
            self.gl
                .bind_framebuffer(glow::FRAMEBUFFER, Some(self.offscreen_fbo));
            self.gl.framebuffer_texture_2d(
                glow::FRAMEBUFFER,
                glow::COLOR_ATTACHMENT0,
                glow::TEXTURE_2D,
                Some(entry.texture),
                0,
            );

            let status = self.gl.check_framebuffer_status(glow::FRAMEBUFFER);
            if status == glow::FRAMEBUFFER_COMPLETE {
                self.gl.read_pixels(
                    bounds.x_min as i32,
                    bounds.y_min as i32,
                    bounds.width() as i32,
                    bounds.height() as i32,
                    glow::RGBA,
                    glow::UNSIGNED_BYTE,
                    glow::PixelPackData::Slice(&mut pixel_data),
                );
            } else {
                log::error!("Framebuffer not complete for pixel readback: {:#x}", status);
            }

            self.gl.framebuffer_texture_2d(
                glow::FRAMEBUFFER,
                glow::COLOR_ATTACHMENT0,
                glow::TEXTURE_2D,
                None,
                0,
            );
            self.gl.bind_framebuffer(glow::FRAMEBUFFER, None);
        }

        pixel_data
    }

    fn draw_quad<const MODE: u32, const COUNT: i32>(&mut self, color: Color, matrix: Matrix) {
        let world_matrix = [
            [matrix.a, matrix.b, 0.0, 0.0],
            [matrix.c, matrix.d, 0.0, 0.0],
            [0.0, 0.0, 1.0, 0.0],
            [
                matrix.tx.to_pixels() as f32,
                matrix.ty.to_pixels() as f32,
                0.0,
                1.0,
            ],
        ];

        let mult_color = [
            color.r as f32 / 255.0,
            color.g as f32 / 255.0,
            color.b as f32 / 255.0,
            color.a as f32 / 255.0,
        ];
        let add_color = [0.0; 4];

        self.set_stencil_state();

        let program = &self.color_program;

        if !std::ptr::eq(program, self.active_program) {
            unsafe {
                self.gl.use_program(Some(program.program));
            }
            self.active_program = program as *const ShaderProgram;
            program.uniform_matrix4fv(&self.gl, ShaderUniform::ViewMatrix, &self.view_matrix);
            self.mult_color = None;
            self.add_color = None;
        }

        self.color_program
            .uniform_matrix4fv(&self.gl, ShaderUniform::WorldMatrix, &world_matrix);
        if Some(mult_color) != self.mult_color {
            self.color_program
                .uniform4fv(&self.gl, ShaderUniform::MultColor, &mult_color);
            self.mult_color = Some(mult_color);
        }
        if Some(add_color) != self.add_color {
            self.color_program
                .uniform4fv(&self.gl, ShaderUniform::AddColor, &add_color);
            self.add_color = Some(add_color);
        }

        let quad = &self.color_quad_draws;
        self.bind_vertex_array(Some(quad[0].vao));

        let count = if COUNT < 0 {
            quad[0].num_indices
        } else {
            COUNT
        };
        unsafe {
            self.gl.draw_elements(MODE, count, glow::UNSIGNED_INT, 0);
        }
    }
}

impl Drop for GlRenderBackend {
    fn drop(&mut self) {
        let pending = std::mem::take(&mut self.pending_readbacks);
        for (_, pending) in pending {
            self.delete_pending_readback(pending);
        }
        for pbo in std::mem::take(&mut self.reusable_readback_pbos) {
            unsafe {
                self.gl.delete_buffer(pbo.buffer);
            }
        }

        for scratch in std::mem::take(&mut self.scratch_texture_pool) {
            unsafe {
                self.gl.delete_texture(scratch.texture);
            }
        }

        unsafe {
            self.gl.delete_vertex_array(self.color_quad_draws[0].vao);
            if let Some(vb) = self.color_quad_draws[0].vertex_buffer {
                self.gl.delete_buffer(vb);
            }
            self.gl.delete_buffer(self.color_quad_draws[0].index_buffer);
            self.gl.delete_vertex_array(self.bitmap_quad_draws[0].vao);
            if let Some(vb) = self.bitmap_quad_draws[0].vertex_buffer {
                self.gl.delete_buffer(vb);
            }
            self.gl.delete_buffer(self.bitmap_quad_draws[0].index_buffer);
            self.gl.delete_program(self.color_program.program);
            self.gl.delete_program(self.bitmap_program.program);
            self.gl.delete_program(self.gradient_program.program);
            self.gl.delete_program(self.filter_blur_program.program);
            self.gl.delete_program(self.filter_glow_program.program);
            self.gl.delete_program(self.filter_color_matrix_program.program);
            self.gl.delete_program(self.filter_bevel_program.program);
            self.gl.delete_program(self.filter_displacement_map_program.program);
            self.gl.delete_vertex_array(self.filter_common_quad.vao);
            self.gl.delete_buffer(self.filter_common_quad.vertex_buffer);
            self.gl.delete_vertex_array(self.filter_blur_quad.vao);
            self.gl.delete_buffer(self.filter_blur_quad.vertex_buffer);
            self.gl.delete_vertex_array(self.filter_glow_quad.vao);
            self.gl.delete_buffer(self.filter_glow_quad.vertex_buffer);
            self.gl.delete_vertex_array(self.filter_bevel_quad.vao);
            self.gl.delete_buffer(self.filter_bevel_quad.vertex_buffer);
            self.gl.delete_vertex_array(self.filter_displacement_quad.vao);
            self.gl.delete_buffer(self.filter_displacement_quad.vertex_buffer);
            self.gl.delete_renderbuffer(self.offscreen_stencil_rbo);
            self.gl.delete_framebuffer(self.offscreen_fbo);
        }
    }
}

fn same_blend_mode(first: Option<&RenderBlendMode>, second: &RenderBlendMode) -> bool {
    match (first, second) {
        (Some(RenderBlendMode::Builtin(old)), RenderBlendMode::Builtin(new)) => old == new,
        _ => false,
    }
}

fn srgb_to_linear(color: &mut [f32; 4]) {
    for n in &mut color[..3] {
        *n = if *n <= 0.04045 {
            *n / 12.92
        } else {
            f32::powf((*n + 0.055) / 1.055, 2.4)
        };
    }
}

impl RenderBackend for GlRenderBackend {
    fn render_offscreen(
        &mut self,
        handle: BitmapHandle,
        commands: CommandList,
        _quality: StageQuality,
        bounds: PixelRegion,
    ) -> Option<Box<dyn SyncHandle>> {
        self.collect_orphaned_readbacks();
        self.collect_released_resources();

        let entry = as_registry_data(&handle);
        let texture = entry.texture;
        let tex_width = entry.width.get();
        let tex_height = entry.height.get();

        if let Err(error) = self.render_to_texture(texture, tex_width, tex_height, None, commands) {
            log::warn!("Failed to render offscreen texture on glow backend: {error}");
            return None;
        }

        let mut clamped_bounds = bounds;
        clamped_bounds.clamp(entry.width.get(), entry.height.get());
        if clamped_bounds.width() == 0 || clamped_bounds.height() == 0 {
            return Some(Box::new(GlSyncHandle {
                readback_id: None,
                width: 0,
                _token: None,
                fallback_pixel_data: Some(Vec::new()),
            }));
        }

        match self.enqueue_async_readback(texture, clamped_bounds) {
            Ok((readback_id, width, token)) => Some(Box::new(GlSyncHandle {
                readback_id: Some(readback_id),
                width,
                _token: Some(token),
                fallback_pixel_data: None,
            })),
            Err(e) => {
                log::warn!("Falling back to synchronous readback: {e}");
                let pixel_data = self.read_texture_pixels(&handle, clamped_bounds);
                Some(Box::new(GlSyncHandle {
                    readback_id: None,
                    width: clamped_bounds.width(),
                    _token: None,
                    fallback_pixel_data: Some(pixel_data),
                }))
            }
        }
    }

    fn is_offscreen_supported(&self) -> bool {
        true
    }

    fn is_filter_supported(&self, filter: &Filter) -> bool {
        Self::supports_filter(filter)
    }

    fn apply_filter(
        &mut self,
        source: BitmapHandle,
        source_point: (u32, u32),
        source_size: (u32, u32),
        destination: BitmapHandle,
        dest_point: (i32, i32),
        filter: Filter,
    ) -> Option<Box<dyn SyncHandle>> {
        if !Self::supports_filter(&filter) {
            return None;
        }

        self.collect_orphaned_readbacks();
        self.collect_released_resources();

        let source_entry = as_registry_data(&source);
        let destination_entry = as_registry_data(&destination);
        let source_dimensions = (source_entry.width.get(), source_entry.height.get());
        let destination_dimensions = (destination_entry.width.get(), destination_entry.height.get());
        let filtered_width = source_size
            .0
            .min(source_dimensions.0.saturating_sub(source_point.0));
        let filtered_height = source_size
            .1
            .min(source_dimensions.1.saturating_sub(source_point.1));

        if filtered_width == 0 || filtered_height == 0 {
            return None;
        }

        let working_texture = match self.acquire_scratch_texture(filtered_width, filtered_height) {
            Ok(texture) => texture,
            Err(error) => {
                log::warn!("Failed to acquire temporary texture for filter application: {error}");
                return None;
            }
        };

        let copy_result = (|| -> Result<bool, BitmapError> {
            self.blit_texture_region(
                source_entry.texture,
                source_dimensions,
                source_point,
                (filtered_width, filtered_height),
                working_texture.texture,
                (filtered_width, filtered_height),
                (0, 0),
            )?;

            if !filter.impotent() {
                self.apply_cache_entry_filters(
                    working_texture.texture,
                    filtered_width,
                    filtered_height,
                    std::slice::from_ref(&filter),
                )?;
            }

            self.blit_texture_region(
                working_texture.texture,
                (filtered_width, filtered_height),
                (0, 0),
                (filtered_width, filtered_height),
                destination_entry.texture,
                destination_dimensions,
                dest_point,
            )
        })();

        self.release_scratch_texture(working_texture);
        self.active_texture_2d.set(None);

        let copied = match copy_result {
            Ok(copied) => copied,
            Err(error) => {
                log::warn!("Failed to apply filter on glow backend: {error}");
                return None;
            }
        };

        if !copied {
            return None;
        }

        let whole_destination = PixelRegion::for_whole_size(
            destination_dimensions.0,
            destination_dimensions.1,
        );
        match self.enqueue_async_readback(destination_entry.texture, whole_destination) {
            Ok((readback_id, width, token)) => Some(Box::new(GlSyncHandle {
                readback_id: Some(readback_id),
                width,
                _token: Some(token),
                fallback_pixel_data: None,
            })),
            Err(error) => {
                log::warn!(
                    "Falling back to synchronous readback after filter application: {error}"
                );
                let pixel_data = self.read_texture_pixels(&destination, whole_destination);
                Some(Box::new(GlSyncHandle {
                    readback_id: None,
                    width: destination_dimensions.0,
                    _token: None,
                    fallback_pixel_data: Some(pixel_data),
                }))
            }
        }
    }

    fn viewport_dimensions(&self) -> ViewportDimensions {
        ViewportDimensions {
            width: self.renderbuffer_width as u32,
            height: self.renderbuffer_height as u32,
            scale_factor: self.viewport_scale_factor,
        }
    }

    fn set_viewport_dimensions(&mut self, dimensions: ViewportDimensions) {
        self.view_matrix = [
            [1.0 / (dimensions.width as f32 / 2.0), 0.0, 0.0, 0.0],
            [0.0, -1.0 / (dimensions.height as f32 / 2.0), 0.0, 0.0],
            [0.0, 0.0, 1.0, 0.0],
            [-1.0, 1.0, 0.0, 1.0],
        ];

        self.renderbuffer_width = dimensions.width.max(1) as i32;
        self.renderbuffer_height = dimensions.height.max(1) as i32;

        unsafe {
            self.gl
                .viewport(0, 0, self.renderbuffer_width, self.renderbuffer_height);
        }
        self.viewport_scale_factor = dimensions.scale_factor;
    }

    fn register_shape(
        &mut self,
        shape: DistilledShape,
        bitmap_source: &dyn BitmapSource,
    ) -> ShapeHandle {
        self.collect_released_resources();
        let mesh = Arc::new(match self.register_shape_internal(shape, bitmap_source) {
            Ok(mesh) => mesh,
            Err(e) => {
                log::error!("Couldn't register shape: {e:?}");
                Mesh {
                    gl: self.gl.clone(),
                    draws: vec![],
                }
            }
        });

        ShapeHandle(mesh)
    }

    fn submit_frame(
        &mut self,
        clear: Color,
        commands: CommandList,
        cache_entries: Vec<BitmapCacheEntry>,
    ) {
        self.collect_orphaned_readbacks();
        self.collect_released_resources();

        for entry in cache_entries {
            let cache = as_registry_data(&entry.handle);
            let cache_texture = cache.texture;
            let cache_width = cache.width.get();
            let cache_height = cache.height.get();
            if let Err(error) = self.render_to_texture(
                cache_texture,
                cache_width,
                cache_height,
                Some(entry.clear),
                entry.commands,
            ) {
                log::warn!("Failed to render cache entry on glow backend: {error}");
                continue;
            }
            if !entry.filters.is_empty() {
                if let Err(error) = self.apply_cache_entry_filters(
                    cache_texture,
                    cache_width,
                    cache_height,
                    &entry.filters,
                ) {
                    log::warn!("Failed to apply cache-entry filters on glow backend: {error}");
                }
            }
        }

        if let Err(error) = self.render_to_screen(clear, commands) {
            log::warn!("Failed to render frame on glow backend: {error}");
        }
    }

    fn register_bitmap(&mut self, bitmap: Bitmap<'_>) -> Result<BitmapHandle, BitmapError> {
        self.collect_released_resources();
        let bitmap = match bitmap.format() {
            BitmapFormat::Rgb | BitmapFormat::Yuv420p | BitmapFormat::Rgba | BitmapFormat::Yuva420p => {
                bitmap.to_rgba()
            }
        };
        let format = glow::RGBA;

        self.ensure_texture_size_supported(bitmap.width(), bitmap.height())?;

        let texture = unsafe {
            self.gl
                .create_texture()
                .map_err(|_| BitmapError::Unimplemented("Unable to create texture".into()))?
        };
        self.bind_texture_2d(texture);
        unsafe {
            self.gl.pixel_store_i32(glow::UNPACK_ALIGNMENT, 1);
            self.gl.tex_image_2d(
                glow::TEXTURE_2D,
                0,
                glow::RGBA8 as i32,
                bitmap.width() as i32,
                bitmap.height() as i32,
                0,
                format,
                glow::UNSIGNED_BYTE,
                Some(bitmap.data()),
            );
        }

        let entry = Arc::new(RegistryData::new(self.gl.clone(), texture, bitmap.width(), bitmap.height()));
        self.set_texture_parameters(entry.as_ref(), glow::LINEAR as i32, glow::CLAMP_TO_EDGE as i32);
        self.texture_resources.push(TextureResource {
            weak: Arc::downgrade(&entry),
            texture,
        });

        Ok(BitmapHandle(entry))
    }

    fn update_texture(
        &mut self,
        handle: &BitmapHandle,
        bitmap: Bitmap<'_>,
        region: PixelRegion,
    ) -> Result<(), BitmapError> {
        let entry = as_registry_data(handle);
        self.bind_texture_2d(entry.texture);

        let bitmap = match bitmap.format() {
            BitmapFormat::Rgb | BitmapFormat::Yuv420p | BitmapFormat::Rgba | BitmapFormat::Yuva420p => {
                bitmap.to_rgba()
            }
        };
        let format = glow::RGBA;

        self.ensure_texture_size_supported(bitmap.width(), bitmap.height())?;

        if bitmap.width() != entry.width.get() || bitmap.height() != entry.height.get() {
            unsafe {
                self.gl.pixel_store_i32(glow::UNPACK_ALIGNMENT, 1);
                self.gl.tex_image_2d(
                    glow::TEXTURE_2D,
                    0,
                    glow::RGBA8 as i32,
                    bitmap.width() as i32,
                    bitmap.height() as i32,
                    0,
                    format,
                    glow::UNSIGNED_BYTE,
                    Some(bitmap.data()),
                );
            }
            entry.width.set(bitmap.width());
            entry.height.set(bitmap.height());
            return Ok(());
        }

        let mut upload_region = region;
        upload_region.clamp(bitmap.width(), bitmap.height());
        if upload_region.width() == 0 || upload_region.height() == 0 {
            return Ok(());
        }

        unsafe {
            self.gl.pixel_store_i32(glow::UNPACK_ALIGNMENT, 1);
            self.gl.pixel_store_i32(glow::UNPACK_ROW_LENGTH, bitmap.width() as i32);
            self.gl
                .pixel_store_i32(glow::UNPACK_SKIP_PIXELS, upload_region.x_min as i32);
            self.gl
                .pixel_store_i32(glow::UNPACK_SKIP_ROWS, upload_region.y_min as i32);
            self.gl.tex_sub_image_2d(
                glow::TEXTURE_2D,
                0,
                upload_region.x_min as i32,
                upload_region.y_min as i32,
                upload_region.width() as i32,
                upload_region.height() as i32,
                format,
                glow::UNSIGNED_BYTE,
                glow::PixelUnpackData::Slice(bitmap.data()),
            );
            self.gl.pixel_store_i32(glow::UNPACK_ROW_LENGTH, 0);
            self.gl.pixel_store_i32(glow::UNPACK_SKIP_PIXELS, 0);
            self.gl.pixel_store_i32(glow::UNPACK_SKIP_ROWS, 0);
        }

        Ok(())
    }

    fn create_context3d(
        &mut self,
        _profile: Context3DProfile,
    ) -> Result<Box<dyn Context3D>, BitmapError> {
        Err(BitmapError::Unimplemented("createContext3D".into()))
    }

    fn debug_info(&self) -> Cow<'static, str> {
        Cow::Borrowed("Renderer: OpenGL ES (Glow)")
    }

    fn name(&self) -> &'static str {
        "glow-gles"
    }

    fn set_quality(&mut self, _quality: StageQuality) {}

    fn compile_pixelbender_shader(
        &mut self,
        _shader: ruffle_render::pixel_bender::PixelBenderShader,
    ) -> Result<ruffle_render::pixel_bender::PixelBenderShaderHandle, BitmapError> {
        Err(BitmapError::Unimplemented(
            "compile_pixelbender_shader".into(),
        ))
    }

    fn resolve_sync_handle(
        &mut self,
        handle: Box<dyn SyncHandle>,
        with_rgba: RgbaBufRead,
    ) -> Result<(), BitmapError> {
        let handle = (handle as Box<dyn Any>)
            .downcast::<GlSyncHandle>()
            .map_err(|_| BitmapError::Unimplemented("Invalid sync handle type".into()))?;

        if let Some(readback_id) = handle.readback_id {
            self.resolve_async_readback(readback_id, with_rgba)
        } else {
            let pixel_data = handle.fallback_pixel_data.unwrap_or_default();
            if !pixel_data.is_empty() {
                with_rgba(&pixel_data, handle.width);
            }
            Ok(())
        }
    }

    fn run_pixelbender_shader(
        &mut self,
        _handle: ruffle_render::pixel_bender::PixelBenderShaderHandle,
        _arguments: &[ruffle_render::pixel_bender_support::PixelBenderShaderArgument],
        _target: &PixelBenderTarget,
    ) -> Result<PixelBenderOutput, BitmapError> {
        Err(BitmapError::Unimplemented("run_pixelbender_shader".into()))
    }

    fn create_empty_texture(
        &mut self,
        width: NonZeroU32,
        height: NonZeroU32,
    ) -> Result<BitmapHandle, BitmapError> {
        self.collect_released_resources();
        self.ensure_texture_size_supported(width.get(), height.get())?;

        let texture = unsafe {
            self.gl
                .create_texture()
                .map_err(|_| BitmapError::Unimplemented("Unable to create texture".into()))?
        };
        self.bind_texture_2d(texture);
        unsafe {
            self.gl.tex_image_2d(
                glow::TEXTURE_2D,
                0,
                glow::RGBA8 as i32,
                width.get() as i32,
                height.get() as i32,
                0,
                glow::RGBA,
                glow::UNSIGNED_BYTE,
                None,
            );
        }

        let entry = Arc::new(RegistryData::new(self.gl.clone(), texture, width.get(), height.get()));
        self.set_texture_parameters(entry.as_ref(), glow::LINEAR as i32, glow::CLAMP_TO_EDGE as i32);
        self.texture_resources.push(TextureResource {
            weak: Arc::downgrade(&entry),
            texture,
        });

        Ok(BitmapHandle(entry))
    }
}

impl CommandHandler for GlRenderBackend {
    fn render_bitmap(
        &mut self,
        bitmap: BitmapHandle,
        transform: Transform,
        smoothing: bool,
        pixel_snapping: PixelSnapping,
    ) {
        self.set_stencil_state();
        let entry = as_registry_data(&bitmap);
        let quad = &self.bitmap_quad_draws;
        let draw = &quad[0];
        let bitmap_matrix = if let DrawType::Bitmap(BitmapDraw { matrix, .. }) = &draw.draw_type {
            matrix
        } else {
            unreachable!()
        };

        let mut matrix = transform.matrix;
        pixel_snapping.apply(&mut matrix);
        matrix *= Matrix::scale(entry.width.get() as f32, entry.height.get() as f32);

        let world_matrix = [
            [matrix.a, matrix.b, 0.0, 0.0],
            [matrix.c, matrix.d, 0.0, 0.0],
            [0.0, 0.0, 1.0, 0.0],
            [
                matrix.tx.to_pixels() as f32,
                matrix.ty.to_pixels() as f32,
                0.0,
                1.0,
            ],
        ];

        let mult_color = transform.color_transform.mult_rgba_normalized();
        let add_color = transform.color_transform.add_rgba_normalized();

        self.bind_vertex_array(Some(draw.vao));

        let program = &self.bitmap_program;

        if !std::ptr::eq(program, self.active_program) {
            unsafe {
                self.gl.use_program(Some(program.program));
            }
            self.active_program = program as *const ShaderProgram;
            program.uniform_matrix4fv(&self.gl, ShaderUniform::ViewMatrix, &self.view_matrix);
            self.mult_color = None;
            self.add_color = None;
        }

        program.uniform_matrix4fv(&self.gl, ShaderUniform::WorldMatrix, &world_matrix);
        if Some(mult_color) != self.mult_color {
            program.uniform4fv(&self.gl, ShaderUniform::MultColor, &mult_color);
            self.mult_color = Some(mult_color);
        }
        if Some(add_color) != self.add_color {
            program.uniform4fv(&self.gl, ShaderUniform::AddColor, &add_color);
            self.add_color = Some(add_color);
        }

        program.uniform_matrix3fv(&self.gl, ShaderUniform::TextureMatrix, bitmap_matrix);

        self.bind_texture_2d(entry.texture);
        program.uniform1i(&self.gl, ShaderUniform::BitmapTexture, 0);

        let filter = if smoothing {
            glow::LINEAR as i32
        } else {
            glow::NEAREST as i32
        };
        self.set_texture_parameters(entry, filter, glow::CLAMP_TO_EDGE as i32);
        unsafe {
            self.gl
                .draw_elements(glow::TRIANGLE_FAN, draw.num_indices, glow::UNSIGNED_INT, 0);
        }
    }

    fn render_shape(&mut self, shape: ShapeHandle, transform: Transform) {
        let world_matrix = [
            [transform.matrix.a, transform.matrix.b, 0.0, 0.0],
            [transform.matrix.c, transform.matrix.d, 0.0, 0.0],
            [0.0, 0.0, 1.0, 0.0],
            [
                transform.matrix.tx.to_pixels() as f32,
                transform.matrix.ty.to_pixels() as f32,
                0.0,
                1.0,
            ],
        ];

        let mult_color = transform.color_transform.mult_rgba_normalized();
        let add_color = transform.color_transform.add_rgba_normalized();

        self.set_stencil_state();

        let mesh = as_mesh(&shape);
        for draw in &mesh.draws {
            let num_indices = if self.mask_state != MaskState::DrawMaskStencil
                && self.mask_state != MaskState::ClearMaskStencil
            {
                draw.num_indices
            } else {
                draw.num_mask_indices
            };
            if num_indices == 0 {
                continue;
            }

            self.bind_vertex_array(Some(draw.vao));

            let program = match &draw.draw_type {
                DrawType::Color => &self.color_program,
                DrawType::Gradient(_) => &self.gradient_program,
                DrawType::Bitmap(_) => &self.bitmap_program,
            };

            if !std::ptr::eq(program, self.active_program) {
                unsafe {
                    self.gl.use_program(Some(program.program));
                }
                self.active_program = program as *const ShaderProgram;
                program.uniform_matrix4fv(&self.gl, ShaderUniform::ViewMatrix, &self.view_matrix);
                self.mult_color = None;
                self.add_color = None;
            }

            program.uniform_matrix4fv(&self.gl, ShaderUniform::WorldMatrix, &world_matrix);
            if Some(mult_color) != self.mult_color {
                program.uniform4fv(&self.gl, ShaderUniform::MultColor, &mult_color);
                self.mult_color = Some(mult_color);
            }
            if Some(add_color) != self.add_color {
                program.uniform4fv(&self.gl, ShaderUniform::AddColor, &add_color);
                self.add_color = Some(add_color);
            }

            match &draw.draw_type {
                DrawType::Color => (),
                DrawType::Gradient(gradient) => {
                    program.uniform_matrix3fv(
                        &self.gl,
                        ShaderUniform::TextureMatrix,
                        &gradient.matrix,
                    );
                    program.uniform1i(
                        &self.gl,
                        ShaderUniform::GradientType,
                        gradient.gradient_type,
                    );
                    program.uniform1fv(&self.gl, ShaderUniform::GradientRatios, &gradient.ratios);
                    program.uniform4fv(
                        &self.gl,
                        ShaderUniform::GradientColors,
                        bytemuck::cast_slice(&gradient.colors),
                    );
                    program.uniform1i(
                        &self.gl,
                        ShaderUniform::GradientRepeatMode,
                        gradient.repeat_mode,
                    );
                    program.uniform1f(
                        &self.gl,
                        ShaderUniform::GradientFocalPoint,
                        gradient.focal_point,
                    );
                    program.uniform1i(
                        &self.gl,
                        ShaderUniform::GradientInterpolation,
                        (gradient.interpolation == GradientInterpolation::LinearRgb) as i32,
                    );
                }
                DrawType::Bitmap(bitmap) => {
                    let entry = match &bitmap.handle {
                        Some(handle) => as_registry_data(handle),
                        None => {
                            log::warn!("Tried to render a handleless bitmap");
                            continue;
                        }
                    };

                    program.uniform_matrix3fv(
                        &self.gl,
                        ShaderUniform::TextureMatrix,
                        &bitmap.matrix,
                    );

                    self.bind_texture_2d(entry.texture);
                    program.uniform1i(&self.gl, ShaderUniform::BitmapTexture, 0);

                    let filter = if bitmap.is_smoothed {
                        glow::LINEAR as i32
                    } else {
                        glow::NEAREST as i32
                    };
                    let wrap = if bitmap.is_repeating {
                        glow::REPEAT as i32
                    } else {
                        glow::CLAMP_TO_EDGE as i32
                    };
                    self.set_texture_parameters(entry, filter, wrap);
                }
            }

            unsafe {
                self.gl
                    .draw_elements(glow::TRIANGLES, num_indices, glow::UNSIGNED_INT, 0);
            }
        }
    }

    fn render_stage3d(&mut self, _bitmap: BitmapHandle, _transform: Transform) {
        panic!("Stage3D should not have been created on Glow backend")
    }

    fn draw_rect(&mut self, color: Color, matrix: Matrix) {
        self.draw_quad::<{ glow::TRIANGLE_FAN }, -1>(color, matrix)
    }

    fn draw_line(&mut self, color: Color, mut matrix: Matrix) {
        matrix.tx += Twips::HALF_PX;
        matrix.ty += Twips::HALF_PX;
        self.draw_quad::<{ glow::LINE_STRIP }, 2>(color, matrix)
    }

    fn draw_line_rect(&mut self, color: Color, mut matrix: Matrix) {
        matrix.tx += Twips::HALF_PX;
        matrix.ty += Twips::HALF_PX;
        self.draw_quad::<{ glow::LINE_LOOP }, -1>(color, matrix)
    }

    fn push_mask(&mut self) {
        debug_assert!(
            self.mask_state == MaskState::NoMask || self.mask_state == MaskState::DrawMaskedContent
        );
        self.num_masks += 1;
        self.mask_state = MaskState::DrawMaskStencil;
        self.mask_state_dirty = true;
    }

    fn activate_mask(&mut self) {
        debug_assert!(self.num_masks > 0 && self.mask_state == MaskState::DrawMaskStencil);
        self.mask_state = MaskState::DrawMaskedContent;
        self.mask_state_dirty = true;
    }

    fn deactivate_mask(&mut self) {
        debug_assert!(self.num_masks > 0 && self.mask_state == MaskState::DrawMaskedContent);
        self.mask_state = MaskState::ClearMaskStencil;
        self.mask_state_dirty = true;
    }

    fn pop_mask(&mut self) {
        debug_assert!(self.num_masks > 0 && self.mask_state == MaskState::ClearMaskStencil);
        self.num_masks -= 1;
        self.mask_state = if self.num_masks == 0 {
            MaskState::NoMask
        } else {
            MaskState::DrawMaskedContent
        };
        self.mask_state_dirty = true;
    }

    fn blend(&mut self, commands: CommandList, blend: RenderBlendMode) {
        self.push_blend_mode(blend);
        commands.execute(self);
        self.pop_blend_mode();
    }

    fn render_alpha_mask(&mut self, maskee_commands: CommandList, _mask_commands: CommandList) {
        maskee_commands.execute(self);
    }
}

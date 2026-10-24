use std::cell::Cell;
use std::collections::HashMap;
use std::sync::{Arc, Weak};

use glow::HasContext;
use ruffle_render::bitmap::{BitmapHandleImpl, SyncHandle};

use super::Mesh;

#[derive(Clone, Debug)]
pub(super) struct RegistryData {
    pub(super) texture: glow::Texture,
    pub(super) width: Cell<u32>,
    pub(super) height: Cell<u32>,
    pub(super) min_filter: Cell<i32>,
    pub(super) mag_filter: Cell<i32>,
    pub(super) wrap_s: Cell<i32>,
    pub(super) wrap_t: Cell<i32>,
}

impl Drop for RegistryData {
    fn drop(&mut self) {
        // Texture cleanup handled by GlRenderBackend.
    }
}

impl BitmapHandleImpl for RegistryData {}

impl RegistryData {
    pub(super) fn new(texture: glow::Texture, width: u32, height: u32) -> Self {
        Self {
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

#[derive(Debug)]
pub(super) struct ReadbackPbo {
    pub(super) buffer: glow::Buffer,
    pub(super) size_bytes: usize,
}

#[derive(Debug)]
pub(super) struct TextureResource {
    pub(super) weak: Weak<RegistryData>,
    pub(super) texture: glow::Texture,
}

#[derive(Debug)]
pub(super) struct DrawResources {
    pub(super) vao: glow::VertexArray,
    pub(super) vertex_buffer: glow::Buffer,
    pub(super) index_buffer: glow::Buffer,
}

#[derive(Debug)]
pub(super) struct MeshResource {
    pub(super) weak: Weak<Mesh>,
    pub(super) draws: Vec<DrawResources>,
}

#[derive(Debug)]
pub(super) struct PendingReadback {
    pub(super) pbo: glow::Buffer,
    pub(super) size_bytes: usize,
    pub(super) fence: glow::Fence,
    pub(super) width: u32,
    pub(super) height: u32,
    pub(super) token: Arc<()>,
}

#[derive(Debug)]
pub(super) struct ScratchTexture {
    pub(super) texture: glow::Texture,
    pub(super) width: u32,
    pub(super) height: u32,
}

#[derive(Debug)]
pub(super) struct GlSyncHandle {
    pub(super) readback_id: Option<u64>,
    pub(super) width: u32,
    pub(super) _token: Option<Arc<()>>,
    pub(super) fallback_pixel_data: Option<Vec<u8>>,
}

impl SyncHandle for GlSyncHandle {}

pub(super) fn delete_pending_readback(gl: &glow::Context, pending: PendingReadback) {
    unsafe {
        gl.delete_sync(pending.fence);
        gl.delete_buffer(pending.pbo);
    }
}

pub(super) fn delete_draw_resources(gl: &glow::Context, resources: &[DrawResources]) {
    unsafe {
        for draw in resources {
            gl.delete_vertex_array(draw.vao);
            gl.delete_buffer(draw.vertex_buffer);
            gl.delete_buffer(draw.index_buffer);
        }
    }
}

pub(super) fn collect_released_resources(
    gl: &glow::Context,
    texture_resources: &mut Vec<TextureResource>,
    mesh_resources: &mut Vec<MeshResource>,
) {
    let mut texture_index = 0;
    while texture_index < texture_resources.len() {
        if texture_resources[texture_index].weak.upgrade().is_none() {
            let resource = texture_resources.swap_remove(texture_index);
            unsafe {
                gl.delete_texture(resource.texture);
            }
        } else {
            texture_index += 1;
        }
    }

    let mut mesh_index = 0;
    while mesh_index < mesh_resources.len() {
        if mesh_resources[mesh_index].weak.upgrade().is_none() {
            let resource = mesh_resources.swap_remove(mesh_index);
            delete_draw_resources(gl, &resource.draws);
        } else {
            mesh_index += 1;
        }
    }
}

pub(super) fn collect_orphaned_readbacks(
    gl: &glow::Context,
    pending_readbacks: &mut HashMap<u64, PendingReadback>,
) {
    pending_readbacks.retain(|_, pending| {
        let keep = Arc::strong_count(&pending.token) > 1;
        if !keep {
            unsafe {
                gl.delete_sync(pending.fence);
                gl.delete_buffer(pending.pbo);
            }
        }
        keep
    });
}

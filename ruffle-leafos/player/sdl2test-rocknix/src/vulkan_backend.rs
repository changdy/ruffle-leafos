//! Upstream Ruffle/wgpu Vulkan rendering with a DMA-BUF -> EGL presentation bridge.
//! Flash renders on Vulkan; GLES only samples the shared final image and draws the UI.
//! No per-frame CPU pixel readback and no system driver/WSI replacement are required.
use ash::vk;
use glow::HasContext;
use ruffle_render::backend::RenderBackend;
use ruffle_render_wgpu::{
    backend::{create_wgpu_instance, WgpuRenderBackend},
    descriptors::Descriptors,
    target::{RenderTarget, TextureTarget, TextureTargetFrame},
};
use std::{
    cell::Cell,
    ffi::{c_char, c_void, CStr},
    fmt,
    os::fd::{AsRawFd, FromRawFd, IntoRawFd, OwnedFd},
    sync::{Arc, Mutex},
};

pub type Failure = Arc<Mutex<Option<String>>>;

pub async unsafe fn create(
    gl: Arc<glow::Context>,
    width: u32,
    height: u32,
    failure: Failure,
) -> Result<Box<dyn RenderBackend>, String> {
    let instance = create_wgpu_instance(wgpu::Backends::VULKAN, Default::default());
    let adapter = instance
        .request_adapter(&wgpu::RequestAdapterOptions {
            power_preference: wgpu::PowerPreference::HighPerformance,
            compatible_surface: None,
            force_fallback_adapter: false,
        })
        .await
        .map_err(|e| e.to_string())?;
    let info = adapter.get_info();
    if info.backend != wgpu::Backend::Vulkan || info.device_type == wgpu::DeviceType::Cpu {
        return Err(format!("A hardware Vulkan adapter is required: {info:?}"));
    }
    println!(
        "leaf-vulkan adapter={:?} driver={:?} backend={:?}",
        info.name, info.driver_info, info.backend
    );
    let (device, queue) = create_device(&adapter)?;
    let errors = failure.clone();
    device.on_uncaptured_error(Arc::new(move |error: wgpu::Error| {
        *errors.lock().unwrap() = Some(format!("Vulkan: {error}"));
    }));
    let errors = failure.clone();
    device.set_device_lost_callback(move |reason, message| {
        if reason != wgpu::DeviceLostReason::Destroyed {
            *errors.lock().unwrap() = Some(format!("Vulkan device lost: {reason:?}: {message}"));
        }
    });
    let target = SharedTarget::new(&device, gl, width, height, failure.clone())?;
    let descriptors = Arc::new(Descriptors::new(instance, adapter, device, queue));
    let renderer = WgpuRenderBackend::new(descriptors, target).map_err(|e| e.to_string())?;
    if let Some(error) = failure.lock().unwrap().take() {
        return Err(error);
    }
    println!("leaf-vulkan presentation=dma-buf-egl cpu_pixel_readback=false");
    Ok(Box::new(renderer))
}

// Ruffle's device limits/features, with the additional image-modifier extension
// needed by Rockchip's DMA-BUF allocator. Import through wgpu's documented HAL API.
unsafe fn create_device(adapter: &wgpu::Adapter) -> Result<(wgpu::Device, wgpu::Queue), String> {
    let mut limits = wgpu::Limits::downlevel_webgl2_defaults()
        .using_resolution(adapter.limits())
        .using_alignment(adapter.limits());
    limits.max_uniform_buffer_binding_size = adapter.limits().max_uniform_buffer_binding_size;
    limits.max_inter_stage_shader_components = adapter.limits().max_inter_stage_shader_components;
    limits.max_color_attachments = 4;
    let features = adapter.features()
        & (wgpu::Features::TEXTURE_ADAPTER_SPECIFIC_FORMAT_FEATURES
            | wgpu::Features::TEXTURE_COMPRESSION_BC
            | wgpu::Features::FLOAT32_FILTERABLE);
    let desc = wgpu::DeviceDescriptor {
        label: Some("Leaf Vulkan"),
        required_features: features,
        required_limits: limits,
        memory_hints: Default::default(),
        trace: wgpu::Trace::Off,
        experimental_features: wgpu::ExperimentalFeatures::disabled(),
    };
    let hal = adapter
        .as_hal::<wgpu::hal::api::Vulkan>()
        .ok_or("Not a Vulkan adapter")?;
    let instance = hal.shared_instance().raw_instance();
    let physical = hal.raw_physical_device();
    let supported = instance
        .enumerate_device_extension_properties(physical)
        .map_err(|e| format!("Vulkan extensions: {e:?}"))?;
    let mut extensions = hal.required_device_extensions(features);
    for extension in [
        ash::ext::image_drm_format_modifier::NAME,
        ash::khr::external_memory_fd::NAME,
        ash::ext::external_memory_dma_buf::NAME,
    ] {
        if !supported
            .iter()
            .any(|p| CStr::from_ptr(p.extension_name.as_ptr()) == extension)
        {
            return Err(format!("Required Vulkan extension missing: {extension:?}"));
        }
        if !extensions.contains(&extension) {
            extensions.push(extension);
        }
    }
    let family = instance
        .get_physical_device_queue_family_properties(physical)
        .iter()
        .position(|p| {
            p.queue_flags
                .contains(vk::QueueFlags::GRAPHICS | vk::QueueFlags::COMPUTE)
        })
        .ok_or("No Vulkan graphics/compute queue")? as u32;
    let priorities = [1.0];
    let queues = [vk::DeviceQueueCreateInfo::default()
        .queue_family_index(family)
        .queue_priorities(&priorities)];
    let names = extensions.iter().map(|e| e.as_ptr()).collect::<Vec<_>>();
    let mut physical_features = hal.physical_device_features(&extensions, features);
    let info = physical_features.add_to_device_create(
        vk::DeviceCreateInfo::default()
            .queue_create_infos(&queues)
            .enabled_extension_names(&names),
    );
    let raw = instance
        .create_device(physical, &info, None)
        .map_err(|e| format!("Vulkan device: {e:?}"))?;
    let open = hal
        .device_from_raw(
            raw,
            None,
            &extensions,
            features,
            &desc.memory_hints,
            family,
            0,
        )
        .map_err(|e| format!("Vulkan HAL device: {e:?}"))?;
    drop(hal);
    adapter
        .create_device_from_hal::<wgpu::hal::api::Vulkan>(open, &desc)
        .map_err(|e| e.to_string())
}

// Keep native allocations alive through both wgpu and EGL use. The callback owns
// another Arc and runs only after wgpu has retired all views/commands for the image.
struct NativeImage {
    device: ash::Device,
    _owner: wgpu::Device,
    image: vk::Image,
    memory: vk::DeviceMemory,
    _gbm: GbmBuffer,
}
impl Drop for NativeImage {
    fn drop(&mut self) {
        unsafe {
            self.device.destroy_image(self.image, None);
            if self.memory != vk::DeviceMemory::null() {
                self.device.free_memory(self.memory, None);
            }
        }
    }
}

// Allocate with the same GBM allocator used by EGL. Rockchip's Vulkan-exported
// allocation could not be imported by EGL; importing a GBM DMA-BUF into both APIs
// avoids that allocator mismatch. These GBM objects are not GL-context-bound.
struct GbmBuffer {
    _library: libloading::Library,
    _drm: OwnedFd,
    device: usize,
    bo: usize,
    fd: OwnedFd,
    stride: u32,
    offset: u32,
    modifier: u64,
    destroy_bo: unsafe extern "C" fn(*mut c_void),
    destroy_device: unsafe extern "C" fn(*mut c_void),
}
impl GbmBuffer {
    unsafe fn new(width: u32, height: u32) -> Result<Self, String> {
        let drm: OwnedFd = std::fs::OpenOptions::new()
            .read(true)
            .write(true)
            .open("/dev/dri/renderD128")
            .map_err(|e| format!("DRM render node: {e}"))?
            .into();
        let library = libloading::Library::new("libgbm.so.1").map_err(|e| e.to_string())?;
        let create_device = *library
            .get::<unsafe extern "C" fn(i32) -> *mut c_void>(b"gbm_create_device\0")
            .map_err(|e| e.to_string())?;
        let create_bo = *library
            .get::<unsafe extern "C" fn(*mut c_void, u32, u32, u32, u32) -> *mut c_void>(
                b"gbm_bo_create\0",
            )
            .map_err(|e| e.to_string())?;
        let get_fd = *library
            .get::<unsafe extern "C" fn(*mut c_void) -> i32>(b"gbm_bo_get_fd\0")
            .map_err(|e| e.to_string())?;
        let stride = *library
            .get::<unsafe extern "C" fn(*mut c_void) -> u32>(b"gbm_bo_get_stride\0")
            .map_err(|e| e.to_string())?;
        let modifier = *library
            .get::<unsafe extern "C" fn(*mut c_void) -> u64>(b"gbm_bo_get_modifier\0")
            .map_err(|e| e.to_string())?;
        let destroy_bo = *library
            .get::<unsafe extern "C" fn(*mut c_void)>(b"gbm_bo_destroy\0")
            .map_err(|e| e.to_string())?;
        let destroy_device = *library
            .get::<unsafe extern "C" fn(*mut c_void)>(b"gbm_device_destroy\0")
            .map_err(|e| e.to_string())?;
        let device = create_device(drm.as_raw_fd());
        if device.is_null() {
            return Err("gbm_create_device failed".into());
        }
        let bo = create_bo(device, width, height, 0x34324241, 4 | 16); // ABGR8888, rendering + linear
        if bo.is_null() {
            destroy_device(device);
            return Err("GBM linear render allocation failed".into());
        }
        let raw_fd = get_fd(bo);
        if raw_fd < 0 {
            destroy_bo(bo);
            destroy_device(device);
            return Err("GBM DMA-BUF export failed".into());
        }
        let fd = OwnedFd::from_raw_fd(raw_fd);
        let result = Self {
            _drm: drm,
            device: device as usize,
            bo: bo as usize,
            fd,
            stride: stride(bo),
            offset: 0,
            modifier: modifier(bo),
            destroy_bo,
            destroy_device,
            _library: library,
        };
        if result.modifier != 0 || result.stride < width * 4 {
            return Err("GBM did not provide a linear RGBA buffer".into());
        }
        Ok(result)
    }
}
impl Drop for GbmBuffer {
    fn drop(&mut self) {
        unsafe {
            (self.destroy_bo)(self.bo as *mut c_void);
            (self.destroy_device)(self.device as *mut c_void);
        }
    }
}

type EglCreateImage =
    unsafe extern "C" fn(*mut c_void, *mut c_void, u32, *mut c_void, *const i32) -> *mut c_void;
type EglDestroyImage = unsafe extern "C" fn(*mut c_void, *mut c_void) -> u32;
type EglImageTarget = unsafe extern "C" fn(u32, *mut c_void);
struct Presenter {
    gl: Arc<glow::Context>,
    _egl: libloading::Library,
    display: *mut c_void,
    image: *mut c_void,
    destroy_image: EglDestroyImage,
    texture: glow::Texture,
    program: glow::Program,
    vao: glow::VertexArray,
}
impl Presenter {
    unsafe fn new(
        gl: Arc<glow::Context>,
        fd: &OwnedFd,
        width: u32,
        height: u32,
        offset: u64,
        stride: u64,
    ) -> Result<Self, String> {
        let egl = libloading::Library::new("libEGL.so.1").map_err(|e| e.to_string())?;
        let current = egl
            .get::<unsafe extern "C" fn() -> *mut c_void>(b"eglGetCurrentDisplay\0")
            .map_err(|e| e.to_string())?;
        let proc = egl
            .get::<unsafe extern "C" fn(*const c_char) -> *const c_void>(b"eglGetProcAddress\0")
            .map_err(|e| e.to_string())?;
        let display = current();
        if display.is_null() {
            return Err("DMA-BUF presentation requires a current EGL display".into());
        }
        let query = egl
            .get::<unsafe extern "C" fn(*mut c_void, i32) -> *const c_char>(b"eglQueryString\0")
            .map_err(|e| e.to_string())?;
        let extensions = query(display, 0x3055);
        if extensions.is_null()
            || !CStr::from_ptr(extensions)
                .to_string_lossy()
                .split_whitespace()
                .any(|e| e == "EGL_EXT_image_dma_buf_import")
        {
            return Err("EGL_EXT_image_dma_buf_import is unavailable".into());
        }
        let has_modifiers = CStr::from_ptr(extensions)
            .to_string_lossy()
            .split_whitespace()
            .any(|e| e == "EGL_EXT_image_dma_buf_import_modifiers");
        let create = proc(c"eglCreateImageKHR".as_ptr());
        let destroy = proc(c"eglDestroyImageKHR".as_ptr());
        let target = proc(c"glEGLImageTargetTexture2DOES".as_ptr());
        if create.is_null() || destroy.is_null() || target.is_null() {
            return Err("EGL image entrypoints are unavailable".into());
        }
        let create: EglCreateImage = std::mem::transmute(create);
        let destroy_image: EglDestroyImage = std::mem::transmute(destroy);
        let target: EglImageTarget = std::mem::transmute(target);
        let mut attrs = vec![
            0x3057,
            width as i32,
            0x3056,
            height as i32, // EGL_WIDTH / HEIGHT
            0x3271,
            0x34324241, // DRM_FORMAT_ABGR8888: R,G,B,A bytes
            0x3272,
            fd.as_raw_fd(),
            0x3273,
            i32::try_from(offset).map_err(|_| "DMA-BUF offset overflow")?,
            0x3274,
            i32::try_from(stride).map_err(|_| "DMA-BUF stride overflow")?,
        ];
        if has_modifiers {
            attrs.extend([0x3443, 0, 0x3444, 0]);
        } // DRM_FORMAT_MOD_LINEAR
        attrs.push(0x3038);
        println!("leaf-vulkan egl_import width={width} height={height} stride={stride} offset={offset} modifiers={has_modifiers}");
        let image = create(
            display,
            std::ptr::null_mut(),
            0x3270,
            std::ptr::null_mut(),
            attrs.as_ptr(),
        );
        if image.is_null() {
            let error = egl
                .get::<unsafe extern "C" fn() -> u32>(b"eglGetError\0")
                .map_err(|e| e.to_string())?();
            return Err(format!(
                "eglCreateImageKHR could not import the Vulkan DMA-BUF: {error:#x}"
            ));
        }
        let texture = match gl.create_texture() {
            Ok(t) => t,
            Err(e) => {
                destroy_image(display, image);
                return Err(e);
            }
        };
        gl.bind_texture(glow::TEXTURE_2D, Some(texture));
        for _ in 0..16 {
            if gl.get_error() == glow::NO_ERROR {
                break;
            }
        }
        target(glow::TEXTURE_2D, image);
        let error = gl.get_error();
        if error != glow::NO_ERROR {
            gl.delete_texture(texture);
            destroy_image(display, image);
            return Err(format!("DMA-BUF GL import failed: {error:#x}"));
        }
        gl.tex_parameter_i32(
            glow::TEXTURE_2D,
            glow::TEXTURE_MIN_FILTER,
            glow::NEAREST as i32,
        );
        gl.tex_parameter_i32(
            glow::TEXTURE_2D,
            glow::TEXTURE_MAG_FILTER,
            glow::NEAREST as i32,
        );
        let program = match gl.create_program() {
            Ok(p) => p,
            Err(e) => {
                gl.delete_texture(texture);
                destroy_image(display, image);
                return Err(e);
            }
        };
        for (kind, source) in [
            (glow::VERTEX_SHADER, "#version 300 es\nconst vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));out vec2 uv;void main(){vec2 v=p[gl_VertexID];gl_Position=vec4(v,0,1);uv=vec2(v.x+1.0,1.0-v.y)*0.5;}"),
            (glow::FRAGMENT_SHADER, "#version 300 es\nprecision highp float;in vec2 uv;uniform sampler2D image;out vec4 color;void main(){color=texture(image,uv);}")
        ] {
            let shader = match gl.create_shader(kind){Ok(s)=>s,Err(e)=>{gl.delete_program(program);gl.delete_texture(texture);destroy_image(display,image);return Err(e);}};
            gl.shader_source(shader, source);gl.compile_shader(shader);
            if !gl.get_shader_compile_status(shader) {let error=gl.get_shader_info_log(shader);gl.delete_shader(shader);gl.delete_program(program);gl.delete_texture(texture);destroy_image(display,image);return Err(error);}
            gl.attach_shader(program, shader);gl.delete_shader(shader);
        }
        gl.link_program(program);
        if !gl.get_program_link_status(program) {
            let error = gl.get_program_info_log(program);
            gl.delete_program(program);
            gl.delete_texture(texture);
            destroy_image(display, image);
            return Err(error);
        }
        let vao = match gl.create_vertex_array() {
            Ok(v) => v,
            Err(e) => {
                gl.delete_program(program);
                gl.delete_texture(texture);
                destroy_image(display, image);
                return Err(e);
            }
        };
        Ok(Self {
            gl,
            _egl: egl,
            display,
            image,
            destroy_image,
            texture,
            program,
            vao,
        })
    }
    unsafe fn draw(&self, width: u32, height: u32) {
        let gl = &self.gl;
        gl.bind_framebuffer(glow::FRAMEBUFFER, None);
        gl.viewport(0, 0, width as i32, height as i32);
        for cap in [
            glow::BLEND,
            glow::DEPTH_TEST,
            glow::STENCIL_TEST,
            glow::SCISSOR_TEST,
            glow::CULL_FACE,
        ] {
            gl.disable(cap);
        }
        gl.color_mask(true, true, true, true);
        gl.use_program(Some(self.program));
        gl.bind_vertex_array(Some(self.vao));
        gl.active_texture(glow::TEXTURE0);
        gl.bind_texture(glow::TEXTURE_2D, Some(self.texture));
        gl.uniform_1_i32(gl.get_uniform_location(self.program, "image").as_ref(), 0);
        gl.draw_arrays(glow::TRIANGLES, 0, 3);
        // Finish sampling before Vulkan can write the shared image again. UI and
        // compositor work subsequently uses the SDL backbuffer, not this image.
        gl.finish();
        gl.bind_vertex_array(None);
        gl.use_program(None);
    }
}
impl Drop for Presenter {
    fn drop(&mut self) {
        unsafe {
            self.gl.finish();
            self.gl.delete_vertex_array(self.vao);
            self.gl.delete_program(self.program);
            self.gl.delete_texture(self.texture);
            (self.destroy_image)(self.display, self.image);
        }
    }
}

struct SharedTarget {
    inner: TextureTarget,
    presenter: Presenter,
    native: Arc<NativeImage>,
    pool: vk::CommandPool,
    acquire: vk::CommandBuffer,
    release: vk::CommandBuffer,
    fence: vk::Fence,
    queue: vk::Queue,
    released: Cell<bool>,
    frames: Cell<u64>,
    failure: Failure,
}
impl fmt::Debug for SharedTarget {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("Vulkan DMA-BUF/EGL target")
    }
}
impl SharedTarget {
    unsafe fn new(
        device: &wgpu::Device,
        gl: Arc<glow::Context>,
        width: u32,
        height: u32,
        failure: Failure,
    ) -> Result<Self, String> {
        let hal = device
            .as_hal::<wgpu::hal::api::Vulkan>()
            .ok_or("Not a Vulkan device")?;
        for extension in [
            ash::khr::external_memory_fd::NAME,
            ash::ext::external_memory_dma_buf::NAME,
        ] {
            if !hal.enabled_device_extensions().contains(&extension) {
                return Err(format!("Missing Vulkan extension: {extension:?}"));
            }
        }
        let raw = hal.raw_device();
        let instance = hal.shared_instance().raw_instance();
        let gbm = GbmBuffer::new(width, height)?;
        let usage = vk::ImageUsageFlags::COLOR_ATTACHMENT
            | vk::ImageUsageFlags::SAMPLED
            | vk::ImageUsageFlags::TRANSFER_SRC;
        let mut external = vk::ExternalMemoryImageCreateInfo::default()
            .handle_types(vk::ExternalMemoryHandleTypeFlags::DMA_BUF_EXT);
        let planes = [vk::SubresourceLayout::default()
            .offset(gbm.offset as u64)
            .row_pitch(gbm.stride as u64)];
        let mut modifier = vk::ImageDrmFormatModifierExplicitCreateInfoEXT::default()
            .drm_format_modifier(gbm.modifier)
            .plane_layouts(&planes);
        let image = raw
            .create_image(
                &vk::ImageCreateInfo::default()
                    .push_next(&mut external)
                    .push_next(&mut modifier)
                    .image_type(vk::ImageType::TYPE_2D)
                    .format(vk::Format::R8G8B8A8_UNORM)
                    .extent(vk::Extent3D {
                        width,
                        height,
                        depth: 1,
                    })
                    .mip_levels(1)
                    .array_layers(1)
                    .samples(vk::SampleCountFlags::TYPE_1)
                    .tiling(vk::ImageTiling::DRM_FORMAT_MODIFIER_EXT)
                    .usage(usage)
                    .sharing_mode(vk::SharingMode::EXCLUSIVE)
                    .initial_layout(vk::ImageLayout::UNDEFINED),
                None,
            )
            .map_err(|e| format!("Vulkan shared image: {e:?}"))?;
        let mut native = NativeImage {
            device: raw.clone(),
            _owner: device.clone(),
            image,
            memory: vk::DeviceMemory::null(),
            _gbm: gbm,
        };
        let requirements = raw.get_image_memory_requirements(image);
        let fd_api = ash::khr::external_memory_fd::Device::new(instance, raw);
        let mut fd_types = vk::MemoryFdPropertiesKHR::default();
        fd_api
            .get_memory_fd_properties(
                vk::ExternalMemoryHandleTypeFlags::DMA_BUF_EXT,
                native._gbm.fd.as_raw_fd(),
                &mut fd_types,
            )
            .map_err(|e| format!("Vulkan import memory types: {e:?}"))?;
        let fd_size = libc::lseek(native._gbm.fd.as_raw_fd(), 0, libc::SEEK_END);
        if fd_size >= 0 && requirements.size > fd_size as u64 {
            return Err(format!(
                "GBM allocation too small: {} < {}",
                fd_size, requirements.size
            ));
        }
        let properties = instance.get_physical_device_memory_properties(hal.raw_physical_device());
        let memory_type = (0..properties.memory_type_count)
            .find(|i| {
                (requirements.memory_type_bits & fd_types.memory_type_bits) & (1 << i) != 0
                    && properties.memory_types[*i as usize]
                        .property_flags
                        .contains(vk::MemoryPropertyFlags::DEVICE_LOCAL)
            })
            .ok_or("No device-local DMA-BUF memory type")?;
        let mut dedicated = vk::MemoryDedicatedAllocateInfo::default().image(image);
        let vk_fd = native._gbm.fd.try_clone().map_err(|e| e.to_string())?;
        let mut import = vk::ImportMemoryFdInfoKHR::default()
            .handle_type(vk::ExternalMemoryHandleTypeFlags::DMA_BUF_EXT)
            .fd(vk_fd.as_raw_fd());
        native.memory = raw
            .allocate_memory(
                &vk::MemoryAllocateInfo::default()
                    .allocation_size(requirements.size)
                    .memory_type_index(memory_type)
                    .push_next(&mut import)
                    .push_next(&mut dedicated),
                None,
            )
            .map_err(|e| format!("Vulkan import memory: {e:?}"))?;
        let _ = vk_fd.into_raw_fd(); // vkAllocateMemory consumed this duplicate on success.
        raw.bind_image_memory(image, native.memory, 0)
            .map_err(|e| format!("Vulkan bind image: {e:?}"))?;
        let layout = raw.get_image_subresource_layout(
            image,
            vk::ImageSubresource::default().aspect_mask(vk::ImageAspectFlags::MEMORY_PLANE_0_EXT),
        );
        if layout.row_pitch < width as u64 * 4 {
            return Err(format!("Invalid DMA-BUF row pitch: {}", layout.row_pitch));
        }
        let presenter = Presenter::new(
            gl,
            &native._gbm.fd,
            width,
            height,
            layout.offset,
            layout.row_pitch,
        )?;
        let native = Arc::new(native);
        let keep = native.clone();
        let size = wgpu::Extent3d {
            width,
            height,
            depth_or_array_layers: 1,
        };
        let hal_texture = hal.texture_from_raw(
            image,
            &wgpu::hal::TextureDescriptor {
                label: Some("Leaf shared Vulkan image"),
                size,
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::Rgba8Unorm,
                usage: wgpu::TextureUses::COLOR_TARGET
                    | wgpu::TextureUses::RESOURCE
                    | wgpu::TextureUses::COPY_SRC,
                memory_flags: wgpu::hal::MemoryFlags::empty(),
                view_formats: vec![],
            },
            Some(Box::new(move || drop(keep))),
        );
        let texture = device.create_texture_from_hal::<wgpu::hal::api::Vulkan>(
            hal_texture,
            &wgpu::TextureDescriptor {
                label: Some("Leaf shared Vulkan image"),
                size,
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::Rgba8Unorm,
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT
                    | wgpu::TextureUsages::TEXTURE_BINDING
                    | wgpu::TextureUsages::COPY_SRC,
                view_formats: &[],
            },
        );
        let pool = raw
            .create_command_pool(
                &vk::CommandPoolCreateInfo::default().queue_family_index(hal.queue_family_index()),
                None,
            )
            .map_err(|e| format!("Vulkan sync pool: {e:?}"))?;
        let commands = raw
            .allocate_command_buffers(
                &vk::CommandBufferAllocateInfo::default()
                    .command_pool(pool)
                    .level(vk::CommandBufferLevel::PRIMARY)
                    .command_buffer_count(2),
            )
            .map_err(|e| {
                raw.destroy_command_pool(pool, None);
                format!("Vulkan sync commands: {e:?}")
            })?;
        for (command, acquire) in [(commands[0], true), (commands[1], false)] {
            raw.begin_command_buffer(command, &vk::CommandBufferBeginInfo::default())
                .map_err(|e| {
                    raw.destroy_command_pool(pool, None);
                    format!("Vulkan sync begin: {e:?}")
                })?;
            let barrier = vk::ImageMemoryBarrier::default()
                .image(image)
                .subresource_range(
                    vk::ImageSubresourceRange::default()
                        .aspect_mask(vk::ImageAspectFlags::COLOR)
                        .level_count(1)
                        .layer_count(1),
                )
                .old_layout(if acquire {
                    vk::ImageLayout::GENERAL
                } else {
                    vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL
                })
                .new_layout(if acquire {
                    vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL
                } else {
                    vk::ImageLayout::GENERAL
                })
                .src_queue_family_index(if acquire {
                    vk::QUEUE_FAMILY_EXTERNAL
                } else {
                    hal.queue_family_index()
                })
                .dst_queue_family_index(if acquire {
                    hal.queue_family_index()
                } else {
                    vk::QUEUE_FAMILY_EXTERNAL
                })
                .src_access_mask(if acquire {
                    vk::AccessFlags::empty()
                } else {
                    vk::AccessFlags::COLOR_ATTACHMENT_WRITE
                })
                .dst_access_mask(if acquire {
                    vk::AccessFlags::COLOR_ATTACHMENT_WRITE
                } else {
                    vk::AccessFlags::empty()
                });
            raw.cmd_pipeline_barrier(
                command,
                if acquire {
                    vk::PipelineStageFlags::TOP_OF_PIPE
                } else {
                    vk::PipelineStageFlags::COLOR_ATTACHMENT_OUTPUT
                },
                if acquire {
                    vk::PipelineStageFlags::COLOR_ATTACHMENT_OUTPUT
                } else {
                    vk::PipelineStageFlags::BOTTOM_OF_PIPE
                },
                vk::DependencyFlags::empty(),
                &[],
                &[],
                &[barrier],
            );
            raw.end_command_buffer(command).map_err(|e| {
                raw.destroy_command_pool(pool, None);
                format!("Vulkan sync end: {e:?}")
            })?;
        }
        let fence = raw
            .create_fence(&vk::FenceCreateInfo::default(), None)
            .map_err(|e| {
                raw.destroy_command_pool(pool, None);
                format!("Vulkan sync fence: {e:?}")
            })?;
        let queue = hal.raw_queue();
        drop(hal);
        println!(
            "leaf-vulkan shared_image={}x{} stride={} offset={}",
            width, height, layout.row_pitch, layout.offset
        );
        Ok(Self {
            inner: TextureTarget {
                size,
                texture,
                format: wgpu::TextureFormat::Rgba8Unorm,
                buffer: None,
            },
            presenter,
            native,
            pool,
            acquire: commands[0],
            release: commands[1],
            fence,
            queue,
            released: Cell::new(false),
            frames: Cell::new(0),
            failure,
        })
    }
    fn fail(&self, error: String) {
        *self.failure.lock().unwrap() = Some(error);
    }
}
impl RenderTarget for SharedTarget {
    type Frame = TextureTargetFrame;
    fn format(&self) -> wgpu::TextureFormat {
        self.inner.format()
    }
    fn width(&self) -> u32 {
        self.inner.width()
    }
    fn height(&self) -> u32 {
        self.inner.height()
    }
    fn resize(&mut self, device: &wgpu::Device, width: u32, height: u32) {
        if (width, height) == (self.width(), self.height()) {
            return;
        }
        match unsafe {
            Self::new(
                device,
                self.presenter.gl.clone(),
                width,
                height,
                self.failure.clone(),
            )
        } {
            Ok(target) => *self = target,
            Err(e) => self.fail(e),
        }
    }
    fn get_next_texture(&mut self) -> Result<Self::Frame, wgpu::SurfaceError> {
        if self.failure.lock().unwrap().is_some() {
            return Err(wgpu::SurfaceError::Lost);
        }
        if self.released.get() {
            // All native/wgpu queue submissions run on the Flash thread. The
            // previous release fence and glFinish guarantee safe buffer reuse.
            if let Err(e) = unsafe {
                self.native.device.queue_submit(
                    self.queue,
                    &[vk::SubmitInfo::default().command_buffers(&[self.acquire])],
                    vk::Fence::null(),
                )
            } {
                self.fail(format!("Vulkan acquire: {e:?}"));
                return Err(wgpu::SurfaceError::Lost);
            }
        }
        self.inner.get_next_texture()
    }
    fn submit<I: IntoIterator<Item = wgpu::CommandBuffer>>(
        &self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        commands: I,
        frame: Self::Frame,
    ) -> wgpu::SubmissionIndex {
        let submit_at = crate::performance::vulkan_start();
        let index = self.inner.submit(device, queue, commands, frame);
        let result = (|| unsafe {
            self.native.device.reset_fences(&[self.fence])?;
            self.native.device.queue_submit(
                self.queue,
                &[vk::SubmitInfo::default().command_buffers(&[self.release])],
                self.fence,
            )?;
            self.native
                .device
                .wait_for_fences(&[self.fence], true, 2_000_000_000)?;
            Ok::<(), vk::Result>(())
        })();
        crate::performance::vulkan_end(0, submit_at);
        if let Err(error) = result {
            self.fail(format!("Vulkan presentation sync: {error:?}"));
            return index;
        }
        self.released.set(true);
        let poll_at = crate::performance::vulkan_start();
        let _ = device.poll(wgpu::PollType::Poll);
        crate::performance::vulkan_end(1, poll_at);
        let present_at = crate::performance::vulkan_start();
        unsafe {
            self.presenter.draw(self.width(), self.height());
        }
        crate::performance::vulkan_end(2, present_at);
        if self.frames.get() == 0 {
            println!("leaf-vulkan first_frame=presented");
        }
        self.frames.set(self.frames.get() + 1);
        index
    }
}
impl Drop for SharedTarget {
    fn drop(&mut self) {
        unsafe {
            let _ = self.native.device.device_wait_idle();
            self.native.device.destroy_fence(self.fence, None);
            self.native.device.destroy_command_pool(self.pool, None);
            println!("leaf-vulkan presented_frames={}", self.frames.get());
        }
    }
}

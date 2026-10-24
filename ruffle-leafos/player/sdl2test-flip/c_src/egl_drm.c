// c_src/egl_drm.c
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
struct gbm_device *gbm_device;
struct gbm_surface *gbm_surface;
int g_device_fd;
drmModeConnectorPtr connector = NULL;
drmModeCrtcPtr crtc = NULL;
drmModeEncoderPtr encoder = NULL;
drmModeResPtr res = NULL;

// uint32_t my_fb;
// uint32_t last_fb;
EGLDisplay display;
EGLContext egl_context;
EGLSurface egl_surface;

// 全局或上下文结构体中的状态变量
static struct gbm_bo *s_previous_bo = NULL;
static int s_page_flip_pending = 0;

/**
 * DRM Page Flip 事件回调
 * @param fd        DRM 设备文件描述符
 * @param frame     触发事件的帧序号 (vblank count)
 * @param sec       事件发生时的秒级时间戳
 * @param usec      事件发生时的微秒级时间戳
 * @param user_data drmModePageFlip 调用时传入的自定义指针
 */
static void page_flip_handler(int fd, unsigned int frame,
                              unsigned int sec, unsigned int usec,
                              void *user_data)
{
    // 1.在buffer上屏之后，此时 GPU 已经不再读取它，可以安全归还给 GBM 缓冲池。
    if (s_previous_bo != NULL) {
        gbm_surface_release_buffer(gbm_surface, s_previous_bo);
        s_previous_bo = NULL;
    }

    // 2. 清除 pending 标志，允许主循环提交新帧
    s_page_flip_pending = 0;

    // (可选) 计算精确帧间隔用于性能监控
    // double ms = sec * 1000.0 + usec / 1000.0;
}

drmEventContext evctx = {
    .version = DRM_EVENT_CONTEXT_VERSION,
    .page_flip_handler = page_flip_handler,
};

void gen_gbm_surface()
{
    // my_fb=0;
    // last_fb=0;

	gbm_surface=NULL;
	gbm_device=NULL;
	g_device_fd=0;

	g_device_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	fprintf(stdout, "g_device_fd:0x%x\n", g_device_fd);

    drmSetMaster(g_device_fd);

	res = drmModeGetResources(g_device_fd);
    // connector = drmModeGetConnector(g_device_fd, res->connectors[1]);
    // encoder = drmModeGetEncoder(g_device_fd, res->encoders[2]);
    // crtc = drmModeGetCrtc(g_device_fd, res->crtcs[1]);
    
    for (int i = 0; i < res->count_connectors; i++) {
		connector = drmModeGetConnector(g_device_fd, res->connectors[i]);
		
		if (connector->connection == DRM_MODE_CONNECTED) {  
            printf("Connector %d (type %d, status %d) supports:\n",  
                   i, connector->connector_type, connector->connection);  
            for (int j = 0; j < connector->count_modes; j++) {  
                printf("  Mode %d: %dx%d, flag: 0x%08x\n",  
                       j, connector->modes[j].hdisplay, connector->modes[j].vdisplay,  
                       connector->modes[j].flags);  
            }  
        }  

		// find a connected connection
		if (connector->connection == DRM_MODE_CONNECTED)
			break;

		drmFree(connector);
		connector = NULL;
	}

	encoder = drmModeGetEncoder(g_device_fd, connector->encoder_id);
	crtc = drmModeGetCrtc(g_device_fd, encoder->crtc_id);

	
	gbm_device = gbm_create_device(g_device_fd);
	uint32_t surface_fmt = GBM_FORMAT_ARGB8888;//flip
	if (!gbm_device_is_format_supported(gbm_device, surface_fmt, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING)) {
        fprintf(stderr, "GBM surface format not supported. Trying anyway.\n");
    }
	
	gbm_surface=gbm_surface_create(gbm_device, crtc->mode.hdisplay, crtc->mode.vdisplay, surface_fmt, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
		
	fprintf(stdout, "gbm_surface:%p surface_fmt:0x%x\n", gbm_surface,surface_fmt);
}

EGLDisplay get_egl_display_from_drm() {

    gen_gbm_surface();

    // 获取 EGLDisplay（通过 GBM）
	PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplay = NULL;
        eglGetPlatformDisplay = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");

    if (!eglGetPlatformDisplay) {
        fprintf(stderr, "eglGetPlatformDisplay not supported\n");
        gbm_device_destroy(gbm_device);
        return EGL_NO_DISPLAY;
    }


    display = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR,gbm_device,NULL);
    if (display == EGL_NO_DISPLAY) {
        fprintf(stderr, "Failed to get EGLDisplay\n");
        gbm_device_destroy(gbm_device);
        return EGL_NO_DISPLAY;
    }

    // 注意：gbm_device 设备生命周期必须 >= EGLDisplay 使用期！
    // 通常我们把 gbm_device 指针存起来，或由调用者管理
    // 这里简化：返回 display，但 gbm_device 不能立即释放！

    return display;
}


EGLConfig get_config()
{
    EGLint egl_config_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_STENCIL_SIZE, 8,
        EGL_RED_SIZE,   8,  
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE,  8,  
        EGL_ALPHA_SIZE, 8,
        EGL_NATIVE_VISUAL_ID, GBM_FORMAT_ARGB8888,
        EGL_NONE
    };

    EGLint num_configs = 0;
    EGLConfig configs = {0};
    if(eglChooseConfig(display, egl_config_attribs, &configs, 1, &num_configs) != EGL_TRUE)
    {
        fprintf(stdout, "get_config failed with code :%d\n", eglGetError());
    }
    return configs;
}


void make_current()
{
    get_egl_display_from_drm();
    // 2. 初始化 EGL
    EGLint majorVersion, minorVersion;
    eglInitialize(display, &majorVersion, &minorVersion);
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint egl_context_attribs_es[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLConfig egl_config = get_config();
    egl_context = eglCreateContext(display, egl_config, EGL_NO_CONTEXT, egl_context_attribs_es);
    if(egl_context == EGL_NO_CONTEXT)
    {
        fprintf(stdout, "eglCreateContext failed with code:%d\n", eglGetError());
    }

    egl_surface = eglCreateWindowSurface(display, egl_config, (EGLNativeWindowType)(gbm_surface), 0);

    if (egl_surface == EGL_NO_SURFACE)
    {
        fprintf(stdout, "Create window surface failed with code: %d\n", eglGetError());
    }

    eglMakeCurrent(display, egl_surface, egl_surface, egl_context);

}

// 声明函数（供 Rust 调用）
void * get_gbm_surface()
{
    return (void *)gbm_surface;
}

// 销毁回调：当 GBM BO 被释放时，自动清理关联的 FB
static void fb_destroy_callback(struct gbm_bo *bo, void *data)
{
    uint32_t fb_id = (uint32_t)(uintptr_t)data;

    if (fb_id > 0) {
        //fprintf(stdout,  "drmModeRmFB\n");
        drmModeRmFB(g_device_fd, fb_id);
    }
}

/**
 * 获取或创建与 GBM BO 关联的 DRM Framebuffer
 * @param bo       GBM Buffer Object
 * @return         DRM FB ID，失败返回 0
 */
uint32_t get_or_create_fb(struct gbm_bo *bo)
{
    // ⭐ 第一步：检查 BO 是否已有关联的 FB
    uint32_t fb_id = (uint32_t)(uintptr_t)gbm_bo_get_user_data(bo);
    if (fb_id != 0) {
        //fprintf(stdout, "命中缓存\n");
        return fb_id;
    }

    // ⭐ 第二步：首次遇到此 BO，创建新的 FB
    if(drmModeAddFB(g_device_fd, gbm_bo_get_width(bo),
                gbm_bo_get_height(bo), 24,
                gbm_bo_get_bpp(bo),
                gbm_bo_get_stride(bo),
                gbm_bo_get_handle(bo).u32,
                &fb_id)!=0)
    {
        fprintf(stderr,  "drmModeAddFB failed\n");
    }

    // ⭐ 第三步：将 FB ID 绑定到 BO，并注册销毁回调
    // 当最终回收此 BO 时，fb_destroy_callback 会自动被调用，移除对应的 FB
    gbm_bo_set_user_data(bo, (void *)(uintptr_t)fb_id, fb_destroy_callback);

    return fb_id;
}
int firstFrame=1;
void display_output_flip()
{
    if(firstFrame)
    {
        eglSwapBuffers(display, egl_surface);
        struct gbm_bo *bo = gbm_surface_lock_front_buffer(gbm_surface);
        uint32_t fb = get_or_create_fb(bo);

        if(drmModeSetCrtc(g_device_fd, crtc->crtc_id, fb, 0, 0,
                    &connector->connector_id, 1, &crtc->mode)!=0)
        {
            fprintf(stderr,  "first drmModeSetCrtc failed\n");
        }

        s_page_flip_pending = 1;
        s_previous_bo = bo;
        int ret = drmModePageFlip(g_device_fd, crtc->crtc_id, fb,
                              DRM_MODE_PAGE_FLIP_EVENT, NULL);
        if (ret != 0) {
            // 翻页失败（如队列满），立即释放避免泄漏
            fprintf(stderr, "PageFlip failed1:%d\n", errno);
            s_previous_bo = NULL;
            s_page_flip_pending = 0;
            gbm_surface_release_buffer(gbm_surface, bo);
        }
        
        firstFrame = 0;
        return;
    }

    // ⭐ 关键：如果上一帧还没上屏，不要提交新帧
    if (s_page_flip_pending) {
        drmHandleEvent(g_device_fd, &evctx);
    }

    eglSwapBuffers(display, egl_surface);

    // ---- 获取新渲染的 buffer ----
    struct gbm_bo *bo = gbm_surface_lock_front_buffer(gbm_surface);
    if (!bo) {
        fprintf(stderr, "lock_front_buffer failed\n");
        return;
    }

    // ---- 复用 FB (首次为 bo 创建，后续从 user_data 取回) ----
    uint32_t fb_id = get_or_create_fb(bo);

    s_previous_bo = bo;
    s_page_flip_pending = 1;
    // ---- 异步翻页 ----
    int ret = drmModePageFlip(g_device_fd, crtc->crtc_id, fb_id,
                              DRM_MODE_PAGE_FLIP_EVENT, NULL);
    if (ret != 0) {
        // 翻页失败（如队列满），立即释放避免泄漏
        fprintf(stderr, "PageFlip failed:%d\n", errno);
        s_previous_bo = NULL;
        s_page_flip_pending = 0;
        gbm_surface_release_buffer(gbm_surface, bo);
    }
}

void clean()
{
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, egl_context);
    eglDestroySurface(display, egl_surface);
    gbm_surface_destroy(gbm_surface);

	drmModeFreeCrtc(crtc);
	drmModeFreeEncoder(encoder);
    drmModeFreeConnector(connector);
	drmModeFreeResources(res);
    gbm_device_destroy(gbm_device);
    close(g_device_fd);
}

// void display_output()
// {
//     eglSwapBuffers(display, egl_surface);
//     //锁定 GBM 表面的前端缓冲区，从而允许应用程序访问和读取当前显示在屏幕上的像素数据。这在需要从屏幕缓冲区中读取内容（例如截图或调试）时非常有用。
//     struct gbm_bo *bo = gbm_surface_lock_front_buffer(gbm_surface);
//     if(bo==NULL)
//     {
//         fprintf(stderr, "gbm_surface_lock_front_buffer failed\n");
//     }

//     //在 DRM 驱动程序中注册一个新的帧缓冲区，并将其与指定的 GBM 缓冲区(bo)关联
//     if(drmModeAddFB(g_device_fd, gbm_bo_get_width(bo),
//                 gbm_bo_get_height(bo), 24,
//                 gbm_bo_get_bpp(bo),
//                 gbm_bo_get_stride(bo),
//                 gbm_bo_get_handle(bo).u32,
//                 &my_fb)!=0)
//     {
//         fprintf(stderr,  "drmModeAddFB failed\n");
//     }

//     // show my_fb
//     if(drmModeSetCrtc(g_device_fd, crtc->crtc_id, my_fb, 0, 0,
//                 &connector->connector_id, 1, &crtc->mode)!=0)
//     {
//         fprintf(stderr,  "drmModeSetCrtc failed\n");
//     }

//     //删除之前的缓冲区
//     if(last_fb>0)
//     {
//         drmModeRmFB(g_device_fd, last_fb);
//         last_fb=my_fb;
//     }
    
//     // hold on for a moment
//     // sleep(2);

//     // restore previous fb
//     // assert(!drmModeSetCrtc(g_device_fd, crtc->crtc_id, fb->fb_id, 0, 0,
//     //             &connector->connector_id, 1, &crtc->mode));

//     gbm_surface_release_buffer(gbm_surface, bo);

// }


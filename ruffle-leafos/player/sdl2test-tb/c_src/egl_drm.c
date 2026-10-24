// c_src/egl_drm.c
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
struct gbm_device *gbm_device;
struct gbm_surface *gbm_surface;
int g_device_fd;
drmModeConnectorPtr connector = NULL;
drmModeCrtcPtr crtc = NULL;
drmModeEncoderPtr encoder = NULL;
drmModeResPtr res = NULL;

uint32_t my_fb;
uint32_t last_fb;
EGLDisplay display;
EGLSurface egl_surface;

void gen_gbm_surface()
{
    my_fb=0;
    last_fb=0;

	gbm_surface=NULL;
	gbm_device=NULL;
	g_device_fd=0;

	g_device_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	fprintf(stdout, "g_device_fd:0x%x\n", g_device_fd);

    //drmSetMaster(g_device_fd);

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
    EGLContext egl_context = eglCreateContext(display, egl_config, EGL_NO_CONTEXT, egl_context_attribs_es);
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

void display_output()
{
    eglSwapBuffers(display, egl_surface);
    //锁定 GBM 表面的前端缓冲区，从而允许应用程序访问和读取当前显示在屏幕上的像素数据。这在需要从屏幕缓冲区中读取内容（例如截图或调试）时非常有用。
    struct gbm_bo *bo = gbm_surface_lock_front_buffer(gbm_surface);
    if(bo==NULL)
    {
        fprintf(stderr, "gbm_surface_lock_front_buffer failed\n");
    }

    //在 DRM 驱动程序中注册一个新的帧缓冲区，并将其与指定的 GBM 缓冲区(bo)关联
    if(drmModeAddFB(g_device_fd, gbm_bo_get_width(bo),
                gbm_bo_get_height(bo), 24,
                gbm_bo_get_bpp(bo),
                gbm_bo_get_stride(bo),
                gbm_bo_get_handle(bo).u32,
                &my_fb)!=0)
    {
        fprintf(stderr,  "drmModeAddFB failed\n");
    }

    // show my_fb
    if(drmModeSetCrtc(g_device_fd, crtc->crtc_id, my_fb, 0, 0,
                &connector->connector_id, 1, &crtc->mode)!=0)
    {
        fprintf(stderr,  "drmModeSetCrtc failed\n");
    }

    //删除之前的缓冲区
    if(last_fb>0)
    {
        drmModeRmFB(g_device_fd, last_fb);
        last_fb=my_fb;
    }
    
    // hold on for a moment
    // sleep(2);

    // restore previous fb
    // assert(!drmModeSetCrtc(g_device_fd, crtc->crtc_id, fb->fb_id, 0, 0,
    //             &connector->connector_id, 1, &crtc->mode));

    gbm_surface_release_buffer(gbm_surface, bo);

}


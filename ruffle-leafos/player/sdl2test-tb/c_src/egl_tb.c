// c_src/egl_drm.c
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>

EGLDisplay display;
EGLSurface egl_surface;


EGLDisplay get_egl_display_from_gl() {


    // 获取 EGLDisplay
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) {
        fprintf(stderr, "Failed to get EGLDisplay\n");
        return EGL_NO_DISPLAY;
    }
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


void make_current_tb()
{
    get_egl_display_from_gl();
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

    egl_surface = eglCreateWindowSurface(display, egl_config, (EGLNativeWindowType)(0), 0);

    if (egl_surface == EGL_NO_SURFACE)
    {
        fprintf(stdout, "Create window surface failed with code: %d\n", eglGetError());
    }

    eglMakeCurrent(display, egl_surface, egl_surface, egl_context);

}

void display_output_tb()
{
    eglSwapBuffers(display, egl_surface);
}


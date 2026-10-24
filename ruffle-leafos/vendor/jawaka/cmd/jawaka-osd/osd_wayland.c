#include "cmd/jawaka-osd/osd_backend.h"
#include "cmd/jawaka-osd/osd_text.h"
#include "cmd/jawaka-osd/osd_view.h"

#include "xdg-shell-client-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include "internal/core/log.h"
#include "internal/platform/paths.h"

/* PNG only: the daemon's copy also builds the JPEG decoder, which is what uses
   these two helpers, so a PNG-only build leaves them unreferenced. Vendored
   code, so silence it here rather than editing the header or dragging in a
   decoder the OSD has no use for. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"
#pragma GCC diagnostic pop

#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

/* Volume and brightness: a rounded pill inset from the top-left corner. */
#define JW_OSD_PILL_PCT        50   /* share of screen width */
#define JW_OSD_PILL_H          60
#define JW_OSD_INSET           28
#define JW_OSD_PILL_ALPHA     168
#define JW_OSD_GLYPH_PAD       12
#define JW_OSD_LINE_ON         10   /* filled portion */
#define JW_OSD_LINE_OFF        10   /* remainder: same weight as the fill */
#define JW_OSD_LINE_OFF_ALPHA  70
#define JW_OSD_LINE_TAIL       26   /* gap between the line and the pill edge */

typedef struct {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;

    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wl_buffer *buffer;
    void *pixels;
    size_t buffer_size;
    int width;
    int height;
    jw_osd_view view;
    bool visible;
    bool configured;
    bool display_lost;            /* logged once; the fd is no longer watched */
} jw_wayland_osd;

static jw_wayland_osd s_osd;

static int jw__env_int(const char *name, int fallback) {
    const char *value = getenv(name);
    if (!value || !value[0]) {
        return fallback;
    }
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || (end && *end != '\0') || parsed <= 0 || parsed > 4096) {
        return fallback;
    }
    return (int)parsed;
}

#define JW_OSD_SHM_TEMPLATE "/tmp/jawaka-osd-shm-XXXXXX"

static int jw__create_shm_file(size_t size) {
    char template[] = JW_OSD_SHM_TEMPLATE;
    int fd = mkstemp(template);
    if (fd < 0) {
        return -1;
    }
    unlink(template);
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* WL_SHM_FORMAT_ARGB8888 is premultiplied: the compositor expects the colour
   already scaled by its own alpha. Writing straight colour is close enough to
   invisible for near-black, which is why the old toast got away with it, but a
   translucent white blows out badly. Everything new goes through here. */
static uint32_t jw__premul(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)a << 24) |
           ((uint32_t)((r * a + 127) / 255) << 16) |
           ((uint32_t)((g * a + 127) / 255) << 8) |
           ((uint32_t)((b * a + 127) / 255));
}

/* Source-over, both sides premultiplied, with an extra coverage term so edges
   can be anti-aliased by weight rather than by a second pass. */
static void jw__blend(uint32_t *dst, uint32_t src, int coverage) {
    if (coverage <= 0) return;
    uint32_t sa = (src >> 24) & 0xFF;
    if (coverage < 255) {
        sa = (sa * (uint32_t)coverage + 127) / 255;
        uint32_t sr = (((src >> 16) & 0xFF) * (uint32_t)coverage + 127) / 255;
        uint32_t sg = (((src >> 8) & 0xFF) * (uint32_t)coverage + 127) / 255;
        uint32_t sb = ((src & 0xFF) * (uint32_t)coverage + 127) / 255;
        src = (sa << 24) | (sr << 16) | (sg << 8) | sb;
    }
    if (sa == 255) { *dst = src; return; }

    uint32_t inv = 255 - sa;
    uint32_t d = *dst;
    uint32_t a = sa + (((d >> 24) & 0xFF) * inv + 127) / 255;
    uint32_t r = ((src >> 16) & 0xFF) + ((((d >> 16) & 0xFF) * inv + 127) / 255);
    uint32_t g = ((src >> 8) & 0xFF) + ((((d >> 8) & 0xFF) * inv + 127) / 255);
    uint32_t b = (src & 0xFF) + (((d & 0xFF) * inv + 127) / 255);
    *dst = (a << 24) | (r << 16) | (g << 8) | b;
}

/* Rounded rect with anti-aliased corners. Coverage comes from the distance to
   the corner centre, so a fully rounded pill (radius == height/2) reads smooth
   at the size the OSD draws it. */
static void jw__fill_round_rect(uint32_t *pixels, int width, int height,
                                int x, int y, int w, int h, int radius,
                                uint32_t color) {
    if (!pixels || w <= 0 || h <= 0) return;
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;
    if (radius < 0) radius = 0;

    for (int py = y; py < y + h; py++) {
        if (py < 0 || py >= height) continue;
        for (int px = x; px < x + w; px++) {
            if (px < 0 || px >= width) continue;
            int cov = 255;
            if (radius > 0) {
                double cx = 0.0, cy = 0.0;
                int corner = 0;
                if (px < x + radius)          { cx = x + radius - 0.5;         corner = 1; }
                else if (px >= x + w - radius){ cx = x + w - radius - 0.5;     corner = 1; }
                if (py < y + radius)          { cy = y + radius - 0.5;         corner += 1; }
                else if (py >= y + h - radius){ cy = y + h - radius - 0.5;     corner += 1; }
                if (corner == 2) {
                    double dx = px + 0.5 - cx, dy = py + 0.5 - cy;
                    double dist = sqrt(dx * dx + dy * dy);
                    double edge = radius - dist + 0.5;
                    if (edge <= 0.0) continue;
                    if (edge < 1.0) cov = (int)(edge * 255.0);
                }
            }
            jw__blend(&pixels[py * width + px], color, cov);
        }
    }
}

static void jw__fill_rect(uint32_t *pixels, int width, int height,
                          int x, int y, int w, int h, uint32_t color) {
    if (!pixels || w <= 0 || h <= 0) {
        return;
    }
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > width) w = width - x;
    if (y + h > height) h = height - y;
    if (w <= 0 || h <= 0) {
        return;
    }

    for (int row = y; row < y + h; row++) {
        uint32_t *dst = pixels + row * width + x;
        for (int col = 0; col < w; col++) {
            dst[col] = color;
        }
    }
}

/* ---- glyph art -------------------------------------------------------------
   The speaker and sun were drawn rectangle by rectangle, which shows at this
   size. They are PNGs in the launcher's res/ui now, decoded once and scaled on
   blit. If either is missing the OSD still draws -- it falls back to the old
   primitives rather than showing a bar with a hole where the icon goes. */
typedef struct {
    unsigned char *pixels;   /* RGBA8, stb order */
    int            w, h;
    bool           tried;
} jw_osd_glyph;

static jw_osd_glyph s_glyph_volume;
static jw_osd_glyph s_glyph_brightness;

static void jw__glyph_load(jw_osd_glyph *g, const char *name) {
    if (g->tried) return;
    g->tried = true;
    char *res = jw_launcher_res_dir();
    if (!res) return;
    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/ui/%s.png", res, name);
    free(res);
    if (n <= 0 || (size_t)n >= sizeof(path)) return;
    FILE *f = fopen(path, "rb");
    if (!f) {
        jw_log_warn("osd: no glyph at %s; falling back to drawn art", path);
        return;
    }
    unsigned char *buf = NULL;
    long len = 0;
    if (fseek(f, 0, SEEK_END) == 0 && (len = ftell(f)) > 0 &&
        fseek(f, 0, SEEK_SET) == 0) {
        buf = (unsigned char *)malloc((size_t)len);
        if (buf && fread(buf, 1, (size_t)len, f) != (size_t)len) {
            free(buf);
            buf = NULL;
        }
    }
    fclose(f);
    if (!buf) return;

    int comp = 0;
    g->pixels = stbi_load_from_memory(buf, (int)len, &g->w, &g->h, &comp, 4);
    free(buf);
    if (!g->pixels) {
        jw_log_warn("osd: glyph %s did not decode; falling back to drawn art", path);
    }
}

/* Nearest-neighbour is enough going down from 256px to ~34, and it keeps the
   OSD free of a resampler. The source is white on transparency, so the tint is
   just the glyph alpha scaled into the requested colour. */
static void jw__draw_glyph(uint32_t *pixels, int width, int height,
                           const jw_osd_glyph *g, int x, int y, int box,
                           uint8_t r, uint8_t gr, uint8_t b) {
    if (!g->pixels || g->w <= 0 || g->h <= 0 || box <= 0) return;
    for (int row = 0; row < box; row++) {
        int py = y + row;
        if (py < 0 || py >= height) continue;
        int sy = row * g->h / box;
        for (int col = 0; col < box; col++) {
            int px = x + col;
            if (px < 0 || px >= width) continue;
            int sx = col * g->w / box;
            unsigned char a = g->pixels[(sy * g->w + sx) * 4 + 3];
            if (!a) continue;
            jw__blend(&pixels[py * width + px], jw__premul(a, r, gr, b), 255);
        }
    }
}

/* Composite SDL_ttf output into the shm buffer. The rendered surface is
   straight alpha; the buffer is premultiplied. Clipped to the banner box so
   text can never land outside the surface it belongs to. */
static void jw__blit_text(uint32_t *pixels, int width, int height,
                          SDL_Surface *text, int x, int y, const jw_osd_rect *clip) {
    if (!pixels || !text || !clip || SDL_LockSurface(text) != 0) return;
    for (int row = 0; row < text->h; row++) {
        int py = y + row;
        if (py < 0 || py >= height || py < clip->y || py >= clip->y + clip->h) continue;
        const uint8_t *line = (const uint8_t *)text->pixels + row * text->pitch;
        for (int col = 0; col < text->w; col++) {
            int px = x + col;
            if (px < 0 || px >= width || px < clip->x || px >= clip->x + clip->w) continue;
            uint32_t p;
            memcpy(&p, line + col * 4, sizeof(p));
            uint8_t a = (uint8_t)(p >> 24);
            if (!a) continue;
            jw__blend(&pixels[py * width + px],
                      jw__premul(a, (uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p), 255);
        }
    }
    SDL_UnlockSurface(text);
}

static uint32_t jw__opaque(uint32_t rgb) {
    return jw__premul(255, (uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb);
}

static void jw__draw_sun(uint32_t *pixels, int width, int height,
                         int cx, int cy, uint32_t color) {
    jw__fill_rect(pixels, width, height, cx - 8, cy - 8, 16, 16, color);
    jw__fill_rect(pixels, width, height, cx - 2, cy - 20, 4, 8, color);
    jw__fill_rect(pixels, width, height, cx - 2, cy + 12, 4, 8, color);
    jw__fill_rect(pixels, width, height, cx - 20, cy - 2, 8, 4, color);
    jw__fill_rect(pixels, width, height, cx + 12, cy - 2, 8, 4, color);
}

static void jw__draw_speaker(uint32_t *pixels, int width, int height,
                             int cx, int cy, uint32_t color) {
    /* Speaker body */
    jw__fill_rect(pixels, width, height, cx - 12, cy - 6, 8, 12, color);
    /* Speaker cone */
    jw__fill_rect(pixels, width, height, cx - 4, cy - 12, 4, 24, color);
    jw__fill_rect(pixels, width, height, cx,     cy - 16, 4, 32, color);
    /* Sound waves */
    jw__fill_rect(pixels, width, height, cx + 8,  cy - 4, 3, 8, color);
    jw__fill_rect(pixels, width, height, cx + 14, cy - 8, 3, 16, color);
}

static void jw__toast_rect(int *out_x, int *out_y, int *out_w, int *out_h);

static int jw__draw_osd(jw_osd_rect *out_rect) {
    uint32_t *pixels = (uint32_t *)s_osd.pixels;
    if (!pixels || !out_rect) return -1;

    /* Start from nothing every time. The buffer is only zeroed when it is
       created, and these draws composite rather than overwrite, so a surface
       that stays up across repeated presses would stack one translucent pill on
       the last until it read as opaque. Whole buffer, not the damaged rect: the
       modes do not share a region, so clearing only the current one would leave
       the other mode's pixels behind. */
    memset(pixels, 0, s_osd.buffer_size);

    if (s_osd.view.kind == JW_OSD_VIEW_STAGE) {
        /* The launcher's launch-notice pill: opaque, a thin border and small
           text, bottom-centred against the full output. */
        jw_osd_banner banner;
        if (jw_osd_banner_render(s_osd.view.stage, s_osd.view.pending_items,
                                 s_osd.width, s_osd.height, &banner) != 0) {
            return -1;
        }
        const jw_osd_rect box = banner.layout.box;
        int border = banner.metrics.border;
        jw__fill_round_rect(pixels, s_osd.width, s_osd.height,
                            box.x, box.y, box.w, box.h, banner.metrics.radius,
                            jw__opaque(JW_OSD_BANNER_MUTED));
        jw__fill_round_rect(pixels, s_osd.width, s_osd.height,
                            box.x + border, box.y + border,
                            box.w - 2 * border, box.h - 2 * border,
                            banner.metrics.inner_radius,
                            jw__opaque(JW_OSD_BANNER_BACKGROUND));
        jw__blit_text(pixels, s_osd.width, s_osd.height, banner.title,
                      banner.layout.title_x, banner.layout.title_y, &box);
        if (banner.action) {
            jw__blit_text(pixels, s_osd.width, s_osd.height, banner.action,
                          banner.layout.action_x, banner.layout.action_y, &box);
        }
        jw_osd_banner_free(&banner);
        *out_rect = box;
        return 0;
    }

    /* ---- volume and brightness ----------------------------------------------
       A pill in the top-left corner: the glyph for what is changing, and one
       line for where it now sits. No number -- the length is the readout, and a
       digit is something to read rather than glance at. */
    int x, y, pill_w, pill_h;
    jw__toast_rect(&x, &y, &pill_w, &pill_h);
    *out_rect = (jw_osd_rect){ x, y, pill_w, pill_h };
    int radius = pill_h / 2;

    jw__fill_round_rect(pixels, s_osd.width, s_osd.height,
                        x, y, pill_w, pill_h, radius,
                        jw__premul(JW_OSD_PILL_ALPHA, 16, 18, 22));

    int box = pill_h - JW_OSD_GLYPH_PAD * 2;
    int gx  = x + JW_OSD_GLYPH_PAD + JW_OSD_GLYPH_PAD / 2;
    int gy  = y + (pill_h - box) / 2;

    bool volume = s_osd.view.kind == JW_OSD_VIEW_VOLUME;
    jw_osd_glyph *glyph = volume ? &s_glyph_volume : &s_glyph_brightness;
    jw__glyph_load(glyph, volume ? "osd-volume" : "osd-brightness");
    if (glyph->pixels) {
        jw__draw_glyph(pixels, s_osd.width, s_osd.height, glyph, gx, gy, box,
                       255, 255, 255);
    } else if (volume) {
        jw__draw_speaker(pixels, s_osd.width, s_osd.height,
                         gx + box / 2, gy + box / 2, jw__premul(255, 255, 255, 255));
    } else {
        jw__draw_sun(pixels, s_osd.width, s_osd.height,
                     gx + box / 2, gy + box / 2, jw__premul(255, 255, 255, 255));
    }

    /* The line stops short of the pill's right edge rather than running to it,
       so the pill reads as holding the line instead of being cropped by it. */
    int line_x = gx + box + JW_OSD_GLYPH_PAD + JW_OSD_GLYPH_PAD / 2;
    int line_w = x + pill_w - JW_OSD_LINE_TAIL - line_x;
    if (line_w < JW_OSD_LINE_ON) line_w = JW_OSD_LINE_ON;

    int on_h  = JW_OSD_LINE_ON;
    int off_h = JW_OSD_LINE_OFF;
    int on_w  = (line_w * s_osd.view.percent) / 100;
    if (on_w < on_h) on_w = on_h;          /* never shorter than its own cap */

    /* Rest of the line first, full width, so the filled part caps over it. */
    jw__fill_round_rect(pixels, s_osd.width, s_osd.height,
                        line_x, y + (pill_h - off_h) / 2, line_w, off_h, off_h / 2,
                        jw__premul(JW_OSD_LINE_OFF_ALPHA, 255, 255, 255));
    jw__fill_round_rect(pixels, s_osd.width, s_osd.height,
                        line_x, y + (pill_h - on_h) / 2, on_w, on_h, on_h / 2,
                        jw__premul(255, 255, 255, 255));
    return 0;
}

/* The level pill's rectangle. The draw reports it back as the damaged region,
   so the two cannot drift apart again the way they did when the overlay moved
   to the top-left and the damage rect kept pointing at the bottom. */
static void jw__toast_rect(int *out_x, int *out_y, int *out_w, int *out_h) {
    int h = JW_OSD_PILL_H;
    int w = s_osd.width * JW_OSD_PILL_PCT / 100;
    if (w > s_osd.width - JW_OSD_INSET * 2) w = s_osd.width - JW_OSD_INSET * 2;
    int x = JW_OSD_INSET;
    int y = JW_OSD_INSET;
    if (out_x) *out_x = x;
    if (out_y) *out_y = y;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

/* Which mode the last damage rectangle described. -1 means "nothing on screen
   yet", which forces a full-surface damage on the next show. */
static int s_damaged_mode = -1;
static jw_osd_rect s_damaged_rect;

static void jw__destroy_surface(void) {
    if (s_osd.buffer) {
        wl_buffer_destroy(s_osd.buffer);
        s_osd.buffer = NULL;
    }
    if (s_osd.pixels) {
        munmap(s_osd.pixels, s_osd.buffer_size);
        s_osd.pixels = NULL;
        s_osd.buffer_size = 0;
    }
    if (s_osd.toplevel) {
        xdg_toplevel_destroy(s_osd.toplevel);
        s_osd.toplevel = NULL;
    }
    if (s_osd.xdg_surface) {
        xdg_surface_destroy(s_osd.xdg_surface);
        s_osd.xdg_surface = NULL;
    }
    if (s_osd.surface) {
        wl_surface_destroy(s_osd.surface);
        s_osd.surface = NULL;
    }
    s_osd.visible = false;
    s_osd.configured = false;
    s_damaged_mode = -1;      /* a new surface shows nothing yet */
}

static void jw__hide_surface(void) {
    if (!s_osd.surface) {
        return;
    }

    jw__destroy_surface();
    if (s_osd.display) {
        wl_display_flush(s_osd.display);
    }
}

static void jw__xdg_wm_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener s_wm_base_listener = {
    .ping = jw__xdg_wm_ping,
};

static void jw__xdg_surface_configure(void *data, struct xdg_surface *surface, uint32_t serial) {
    (void)surface;
    jw_wayland_osd *osd = (jw_wayland_osd *)data;
    xdg_surface_ack_configure(osd->xdg_surface, serial);
    osd->configured = true;
}

static const struct xdg_surface_listener s_xdg_surface_listener = {
    .configure = jw__xdg_surface_configure,
};

static void jw__toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                                   int32_t width, int32_t height,
                                   struct wl_array *states) {
    (void)data;
    (void)toplevel;
    (void)width;
    (void)height;
    (void)states;
}

static void jw__toplevel_close(void *data, struct xdg_toplevel *toplevel) {
    (void)data;
    (void)toplevel;
    jw__destroy_surface();
}

static const struct xdg_toplevel_listener s_toplevel_listener = {
    .configure = jw__toplevel_configure,
    .close = jw__toplevel_close,
};

static void jw__registry_global(void *data, struct wl_registry *registry,
                                uint32_t name, const char *interface,
                                uint32_t version) {
    jw_wayland_osd *osd = (jw_wayland_osd *)data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        osd->compositor = wl_registry_bind(registry, name, &wl_compositor_interface,
                                           version < 4 ? version : 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        osd->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        osd->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(osd->wm_base, &s_wm_base_listener, osd);
    }
}

static void jw__registry_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener s_registry_listener = {
    .global = jw__registry_global,
    .global_remove = jw__registry_remove,
};

static int jw__create_buffer(void) {
    s_osd.buffer_size = (size_t)s_osd.width * (size_t)s_osd.height * 4u;
    int fd = jw__create_shm_file(s_osd.buffer_size);
    if (fd < 0) {
        return -1;
    }

    s_osd.pixels = mmap(NULL, s_osd.buffer_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (s_osd.pixels == MAP_FAILED) {
        s_osd.pixels = NULL;
        close(fd);
        return -1;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(s_osd.shm, fd, (int32_t)s_osd.buffer_size);
    close(fd);
    if (!pool) {
        munmap(s_osd.pixels, s_osd.buffer_size);
        s_osd.pixels = NULL;
        return -1;
    }

    s_osd.buffer = wl_shm_pool_create_buffer(pool, 0, s_osd.width, s_osd.height,
                                            s_osd.width * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    if (s_osd.buffer) {
        memset(s_osd.pixels, 0, s_osd.buffer_size);
    }
    return s_osd.buffer ? 0 : -1;
}

static int jw__ensure_surface(void) {
    if (s_osd.surface && s_osd.buffer && s_osd.pixels) {
        return 0;
    }

    s_osd.surface = wl_compositor_create_surface(s_osd.compositor);
    if (!s_osd.surface) {
        return -1;
    }

    struct wl_region *empty = wl_compositor_create_region(s_osd.compositor);
    if (empty) {
        wl_surface_set_input_region(s_osd.surface, empty);
        wl_region_destroy(empty);
    }

    s_osd.xdg_surface = xdg_wm_base_get_xdg_surface(s_osd.wm_base, s_osd.surface);
    s_osd.toplevel = xdg_surface_get_toplevel(s_osd.xdg_surface);
    xdg_surface_add_listener(s_osd.xdg_surface, &s_xdg_surface_listener, &s_osd);
    xdg_toplevel_add_listener(s_osd.toplevel, &s_toplevel_listener, &s_osd);
    xdg_toplevel_set_title(s_osd.toplevel, "Jawaka OSD");
    xdg_toplevel_set_fullscreen(s_osd.toplevel, NULL);
    wl_surface_commit(s_osd.surface);
    wl_display_roundtrip(s_osd.display);

    if (!s_osd.configured) {
        jw__destroy_surface();
        return -1;
    }

    if (jw__create_buffer() != 0) {
        jw__destroy_surface();
        return -1;
    }

    return 0;
}

static int jw__show_surface(void) {
    if (jw__ensure_surface() != 0) {
        return -1;
    }

    jw_osd_rect rect;
    if (jw__draw_osd(&rect) != 0) {
        return -1;
    }
    wl_surface_attach(s_osd.surface, s_osd.buffer, 0, 0);

    /* Damage is the region where the new buffer differs from what the surface
       already shows -- clearing shared memory does not tell the compositor
       anything. The level pill sits top-left and the banner bottom-centre, and
       a banner changes size with its text, so any change of mode or bounds
       (a width that only shrinks included) reports the whole surface. Only a
       redraw into the same rectangle keeps the tight region. */
    jw_osd_rect damage = jw_osd_damage_rect(s_damaged_mode == (int)s_osd.view.kind,
                                            s_damaged_rect, rect,
                                            s_osd.width, s_osd.height);
    wl_surface_damage_buffer(s_osd.surface, damage.x, damage.y, damage.w, damage.h);
    s_damaged_mode = (int)s_osd.view.kind;
    s_damaged_rect = rect;
    wl_surface_commit(s_osd.surface);
    wl_display_flush(s_osd.display);
    s_osd.visible = true;
    return 0;
}

int jw_osd_backend_init(void) {
    if (!getenv("XDG_RUNTIME_DIR")) {
        setenv("XDG_RUNTIME_DIR", "/var/run", 0);
    }
    if (!getenv("WAYLAND_DISPLAY")) {
        setenv("WAYLAND_DISPLAY", "wayland-0", 0);
    }

    memset(&s_osd, 0, sizeof(s_osd));
    s_osd.width = jw__env_int("CAT_WINDOW_WIDTH", 960);
    s_osd.height = jw__env_int("CAT_WINDOW_HEIGHT", 720);
    jw_osd_view_reset(&s_osd.view);
    /* Fonts open once, before the OSD reports ready. Without one the level
       toasts still work and every banner show fails visibly to the daemon. */
    (void)jw_osd_text_init(s_osd.width);

    s_osd.display = wl_display_connect(NULL);
    if (!s_osd.display) {
        return -1;
    }
    s_osd.registry = wl_display_get_registry(s_osd.display);
    wl_registry_add_listener(s_osd.registry, &s_registry_listener, &s_osd);
    wl_display_roundtrip(s_osd.display);

    if (!s_osd.compositor || !s_osd.shm || !s_osd.wm_base) {
        jw_osd_backend_shutdown();
        return -1;
    }
    return 0;
}

/* A show the backend could not submit leaves nothing to restore later, and
   the OSD reports the failure instead of replying ok. */
static int jw__apply(jw_osd_view_effect effect) {
    switch (effect) {
        case JW_OSD_VIEW_KEEP:
            return 0;
        case JW_OSD_VIEW_HIDE:
            jw__hide_surface();
            return 0;
        case JW_OSD_VIEW_DRAW:
            if (jw__show_surface() == 0) return 0;
            break;
    }
    jw_log_warn("osd: could not show the surface");
    jw_osd_view_reset(&s_osd.view);
    jw__hide_surface();
    return -1;
}

int jw_osd_backend_show_brightness(int percent, uint64_t now_ms) {
    return jw__apply(jw_osd_view_level(&s_osd.view, JW_OSD_VIEW_BRIGHTNESS,
                                       percent, now_ms));
}

int jw_osd_backend_show_volume(int percent, uint64_t now_ms) {
    return jw__apply(jw_osd_view_level(&s_osd.view, JW_OSD_VIEW_VOLUME,
                                       percent, now_ms));
}

int jw_osd_backend_show_game_launch(jw_osd_game_stage stage,
                                    int pending_items, uint64_t now_ms) {
    return jw__apply(jw_osd_view_stage(&s_osd.view, stage, pending_items, now_ms));
}

void jw_osd_backend_hide_game_launch(void) {
    (void)jw__apply(jw_osd_view_hide_stage(&s_osd.view));
}

/* The main loop sleeps on the display fd, so compositor events (a ping, a
   configure, a buffer release) are read here as they arrive instead of piling
   up in the socket until the next roundtrip. Never blocks: when the fd has
   nothing, the read is cancelled. */
static void jw__read_events(void) {
    struct wl_display *display = s_osd.display;
    if (!display || wl_display_get_error(display) != 0) return;
    while (wl_display_prepare_read(display) != 0) {
        if (wl_display_dispatch_pending(display) < 0) return;
    }
    struct pollfd pfd = { .fd = wl_display_get_fd(display), .events = POLLIN };
    if (poll(&pfd, 1, 0) > 0) {
        (void)wl_display_read_events(display);
    } else {
        wl_display_cancel_read(display);
    }
}

int jw_osd_backend_event_fd(void) {
    if (!s_osd.display) return -1;
    int error = wl_display_get_error(s_osd.display);
    if (error != 0) {
        /* A dead connection polls readable forever; stop watching it. */
        if (!s_osd.display_lost) {
            jw_log_warn("osd: lost the compositor connection: %s", strerror(error));
            s_osd.display_lost = true;
        }
        return -1;
    }
    return wl_display_get_fd(s_osd.display);
}

int jw_osd_backend_timeout_ms(uint64_t now_ms) {
    return jw_osd_view_timeout_ms(&s_osd.view, now_ms);
}

void jw_osd_backend_tick(uint64_t now_ms) {
    if (s_osd.display) {
        jw__read_events();
        wl_display_dispatch_pending(s_osd.display);
        wl_display_flush(s_osd.display);
    }
    (void)jw__apply(jw_osd_view_tick(&s_osd.view, now_ms));
}

void jw_osd_backend_shutdown(void) {
    jw_osd_view_reset(&s_osd.view);
    jw__destroy_surface();
    jw_osd_text_shutdown();
    if (s_osd.wm_base) {
        xdg_wm_base_destroy(s_osd.wm_base);
        s_osd.wm_base = NULL;
    }
    if (s_osd.shm) {
        wl_shm_destroy(s_osd.shm);
        s_osd.shm = NULL;
    }
    if (s_osd.compositor) {
        wl_compositor_destroy(s_osd.compositor);
        s_osd.compositor = NULL;
    }
    if (s_osd.registry) {
        wl_registry_destroy(s_osd.registry);
        s_osd.registry = NULL;
    }
    if (s_osd.display) {
        wl_display_disconnect(s_osd.display);
        s_osd.display = NULL;
    }
}

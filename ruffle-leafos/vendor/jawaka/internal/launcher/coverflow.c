#include "internal/launcher/coverflow.h"

#include "catastrophe.h"

#include <SDL2/SDL.h>
#include <math.h>

/* ── CF-local math ──────────────────────────────────────────────────────── */

float jw_cf_clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

float jw_cf_ease_in_out_cubic(float t) {
    if (t < 0.5f) return 4.f * t * t * t;
    float u = -2.f * t + 2.f;
    return 1.f - (u * u * u) * 0.5f;
}

uint32_t jw_cf_anim_ms(void) {
    const cat_stylesheet *ss = cat_get_stylesheet();
    uint32_t ms = ss ? ss->launcher.coverflow_anim_ms : 180u;
    if (ms == 0) ms = 180u;
    if (ms < 80u) ms = 80u;
    if (ms > 600u) ms = 600u;
    return ms;
}

/* ── Carousel tween ─────────────────────────────────────────────────────── */

void jw_games_cf_renormalize(jw_games_cf *cf, int count) {
    if (!cf || count <= 0) return;
    while (cf->vpos >= (float)count) {
        cf->vpos -= (float)count;
        cf->target -= (float)count;
    }
    while (cf->vpos < 0.f) {
        cf->vpos += (float)count;
        cf->target += (float)count;
    }
}

void jw_games_cf_step(jw_games_cf *cf, uint32_t now) {
    if (!cf || !cf->active) return;

    uint32_t dt = now - cf->last_ms;
    cf->last_ms = now;
    if (dt == 0) dt = 1;
    if (dt > 100) dt = 100;

    float tau = jw_cf_clampf((float)jw_cf_anim_ms() * 0.25f, 25.f, 150.f);
    float k = 1.0f - expf(-(float)dt / tau);
    cf->vpos += (cf->target - cf->vpos) * k;
}

/* ── Per-section card metrics + the shared carousel driver ──────────────── */

const jw_cf_layout jw_cf_layouts[JW_CF_SECTION_COUNT] = {
    [JW_CF_SECTION_SYSTEMS]   = { 0.90f, 0.45f, 0.70f, 0.66f },
    [JW_CF_SECTION_FAVORITES] = { 0.72f, 0.45f, 0.70f, 0.66f },
    [JW_CF_SECTION_RECENTS]   = { 0.72f, 0.45f, 0.70f, 0.66f },
    [JW_CF_SECTION_APPS]      = { 0.80f, 0.45f, 0.70f, 0.66f },
    [JW_CF_SECTION_GAMES]     = { 0.72f, 0.45f, 0.70f, 0.66f },
};

bool jw_cf_draw_cards(jw_games_cf *cf, int count, int cursor,
                      jw_cf_icon_fn icon_fn, void *ctx, jw_cf_layout layout) {
    if (count <= 0) return false;
    int sw = cat_get_screen_width();
    int sh = cat_get_screen_height();
    uint32_t now = SDL_GetTicks();

    /* Window geometry: W slots each side of centre, so up to VISIBLE cards are on
       screen at once. The carousel only wraps into an endless ring when there are
       at least that many distinct items; with fewer, it clamps at the ends so a
       card is never drawn twice — equivalently, it never shows more cards than
       there are items (a 1-item list is a single centred card). */
    const int  W     = JW_CF_WINDOW;
    const bool loops = jw_cf_list_loops(count);

    /* Continuous carousel position; re-init on entry / list change. */
    if (!cf->inited || cf->count_seen != count) {
        cf->inited = true;
        cf->count_seen = count;
        cf->vpos = cf->target = (float)cursor;
        cf->last_cursor = cursor;
        cf->last_ms = now;
        cf->active = false;
    }
    if (cursor != cf->last_cursor) {
        int raw = cursor - cf->last_cursor;
        if (loops) {                                  /* take the shortest way round the ring */
            while (raw >  count / 2) raw -= count;
            while (raw < -count / 2) raw += count;
        }
        cf->last_cursor = cursor;
        if (raw > 6 || raw < -6) {                    /* letter jump / big -> snap */
            cf->target += (float)raw;
            cf->vpos = cf->target;
            cf->last_ms = now;
            cf->active = false;
            if (loops) jw_games_cf_renormalize(cf, count);
        } else {
            cf->target += (float)raw;
            if (!cf->active) cf->last_ms = (now > 16u) ? now - 16u : 0u;
            cf->active = true;
        }
    }
    if (cf->active) {
        jw_games_cf_step(cf, now);
        if (fabsf(cf->target - cf->vpos) < 0.003f) {
            cf->active = false;
            cf->vpos = cf->target;
            if (loops) jw_games_cf_renormalize(cf, count);
        } else {
            cat_request_frame();
        }
    }
    float vc = cf->vpos;

    /* Card geometry as fractions of the screen (resolution-independent). */
    float CH = sh * layout.size;
    float CW = CH * 0.70f;
    float oy = sh * layout.oy;                    /* lower, so the title clears the art */
    float cx = sw * 0.5f;
    float STEP = CW * layout.step;
    const float   SIDE_SCALE = layout.side_scale;
    const uint8_t SIDE_ALPHA = 160;
    const float   TILT       = 0.80f;            /* ~46 deg */

    int base = (int)floorf(vc + 0.5f);

    /* Draw far -> near so nearer cards overlap. When looping, indices wrap the
       ends into an endless ring (last card slides into the first); when clamped
       (fewer items than the window), slots past either end are skipped so no card
       is ever drawn twice. */
    for (int ring = W; ring >= 0; ring--) {
        for (int o = -W; o <= W; o++) {
            int slot = base + o;
            float d = (float)slot - vc;
            if (fabsf(d) > (float)W + 0.5f) continue;
            if ((int)(fabsf(d) + 0.5f) != ring) continue;

            float ad = fabsf(d);
            float c  = 1.0f - jw_cf_clampf(ad, 0.f, 1.f);      /* 1 centre .. 0 side */
            float scale = SIDE_SCALE + (1.0f - SIDE_SCALE) * c;
            float hw  = CW * 0.5f * scale;
            float hh  = CH * 0.5f * scale;
            /* Tilt so each side card's OUTER edge faces the viewer. */
            float ang = -TILT * jw_cf_clampf(d, -1.f, 1.f);
            uint8_t alpha = (uint8_t)(SIDE_ALPHA + (int)((255 - SIDE_ALPHA) * c));
            float ox = cx + d * STEP;

            int idx;
            if (loops) {
                idx = ((slot % count) + count) % count;
            } else {
                if (slot < 0 || slot >= count) continue;   /* clamp: no card past the ends */
                idx = slot;
            }
            int tw = 0, th = 0;
            SDL_Texture *tex = icon_fn(ctx, idx, &tw, &th);
            jw_cf_draw_card(tex, tw, th, ox, oy, hw, hh, ang, alpha);
        }
    }
    return cf->active;
}

/* ── Pure render primitives ─────────────────────────────────────────────── */

/* Draw one album card (perspective-rotated about its vertical axis) plus a
 * short fading reflection beneath it. (ox,oy) is the card centre on screen;
 * hw/hh are the uniform card half-extents; ang is the tilt in radians; alpha
 * dims sides. The cover is CONTAIN-fit (never cropped) inside a uniform card
 * backing, so mismatched box-art aspect ratios keep the same outer shape while
 * still showing every edge of the art. */
void jw_cf_draw_card(SDL_Texture *tex, int tw, int th,
                     float ox, float oy, float hw, float hh,
                     float ang, uint8_t alpha) {
    float ca = cosf(ang), sa = sinf(ang);
    const float F = hw * 6.0f + 1.0f;            /* perspective focal length */

    /* Project a local (lx,ly) through the shared rotation+perspective. */
    #define CF_PROJ(LX, LY, OUT) do {                              \
        float _xr = (LX) * ca, _zr = (LX) * sa;                    \
        float _pp = F / (F + _zr);                                 \
        (OUT).x = ox + _xr * _pp; (OUT).y = oy + (LY) * _pp;       \
    } while (0)

    SDL_FPoint fulluv[4] = { {0,0}, {1,0}, {1,1}, {0,1} };

    /* Missing art -> a dark placeholder card so the slot still reads. Real covers
       draw at their NATIVE aspect (below) with no case: within a system all box
       art shares a shape, so the flow stays uniform without forcing one frame. */
    if (!tex || tw <= 0 || th <= 0) {
        SDL_FPoint bp[4];
        CF_PROJ(-hw, -hh, bp[0]); CF_PROJ(+hw, -hh, bp[1]);
        CF_PROJ(+hw, +hh, bp[2]); CF_PROJ(-hw, +hh, bp[3]);
        SDL_Color bc[4] = { {26,28,35,alpha}, {26,28,35,alpha},
                            {16,17,22,alpha}, {16,17,22,alpha} };
        cat_draw_textured_quad(NULL, bp, fulluv, bc);
        goto done;
    }

    /* Contain the cover inside the frame: fit the tighter axis, centre the rest. */
    float frameAR = hw / hh;
    float texAR   = (float)tw / (float)th;
    float ahw = hw, ahh = hh;
    if (texAR > frameAR) ahh = hw / texAR;       /* wider -> letterbox top/bottom */
    else                 ahw = hh * texAR;       /* taller -> pillarbox left/right */

    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);

    /* Draw the cover (and its reflection) as N vertical strips instead of one
       quad. SDL_RenderGeometry maps textures affinely per triangle, so a single
       perspective-foreshortened card shears along its diagonal as it turns (the
       "skew" artifact). CF_PROJ foreshortens horizontally (rotation about the
       vertical axis), so slicing the card into thin vertical columns keeps each
       strip near-planar and the warp disappears — the same trick the channel cube
       uses with horizontal strips. */
    const int   N  = 16;
    const float f  = 0.55f;                       /* reflection drop fraction */
    const float vb = 1.f - f;
    uint8_t ra = (uint8_t)((int)alpha * 90 / 255);
    for (int i = 0; i < N; i++) {
        float u0 = (float)i / (float)N, u1 = (float)(i + 1) / (float)N;
        float lx0 = -ahw + u0 * (2.f * ahw);
        float lx1 = -ahw + u1 * (2.f * ahw);
        SDL_FPoint tl, tr, br, bl;
        CF_PROJ(lx0, -ahh, tl); CF_PROJ(lx1, -ahh, tr);
        CF_PROJ(lx1, +ahh, br); CF_PROJ(lx0, +ahh, bl);

        /* Reflection strip: mirror the bottom edge downward, fading out. */
        SDL_FPoint rp[4] = {
            bl, br,
            { br.x + f * (br.x - tr.x), br.y + f * (br.y - tr.y) },
            { bl.x + f * (bl.x - tl.x), bl.y + f * (bl.y - tl.y) },
        };
        SDL_FPoint ruv[4]  = { {u0,1}, {u1,1}, {u1,vb}, {u0,vb} };
        SDL_Color  rcol[4] = { {255,255,255,ra}, {255,255,255,ra},
                               {255,255,255,0},  {255,255,255,0} };
        cat_draw_textured_quad(tex, rp, ruv, rcol);

        /* Cover strip. */
        SDL_FPoint cp[4]  = { tl, tr, br, bl };
        SDL_FPoint cuv[4] = { {u0,0}, {u1,0}, {u1,1}, {u0,1} };
        SDL_Color  col[4] = { {255,255,255,alpha}, {255,255,255,alpha},
                              {255,255,255,alpha}, {255,255,255,alpha} };
        cat_draw_textured_quad(tex, cp, cuv, col);
    }

done:
    #undef CF_PROJ
    return;
}

/* Draw one cube face, subdivided into horizontal strips. SDL_RenderGeometry maps
   textures affinely per triangle, so a single strongly-foreshortened quad shears
   along its diagonal; thin strips keep each triangle near-planar and the warp
   disappears. kind 0 = front (outgoing); kind 1 = incoming (hinged below when
   dir>0, above when dir<0). */
void jw_cf_draw_cube_face(SDL_Texture *tex, int kind, int dir,
                          float W, float H, float cx, float cy,
                          float C, float S, float cphi, float sphi) {
    const int N = 16;
    for (int r = 0; r < N; r++) {
        float ev[2] = { (float)r / N, (float)(r + 1) / N };
        SDL_FPoint p[4], uv[4];
        for (int e = 0; e < 2; e++) {
            float vv = ev[e];
            float Y, Z, texv;
            if (kind == 0)    { Y = -H*0.5f + vv*H; Z = +H*0.5f;        texv = vv; }
            else if (dir > 0) { Y = -H*0.5f;        Z = +H*0.5f - vv*H; texv = 1.f - vv; }
            else              { Y = +H*0.5f;        Z = +H*0.5f - vv*H; texv = vv; }
            float _y = Y*cphi - Z*sphi;
            float _z = Y*sphi + Z*cphi;
            float _p = C / (C - _z);
            float sxL = cx + (-W*0.5f) * _p * S;
            float sxR = cx + (+W*0.5f) * _p * S;
            float sy  = cy + _y * _p * S;
            int a = (e == 0) ? 0 : 3;   /* top edge -> TL,TR (0,1); bottom -> BL,BR (3,2) */
            int b = (e == 0) ? 1 : 2;
            p[a].x = sxL; p[a].y = sy; uv[a].x = 0.f; uv[a].y = texv;
            p[b].x = sxR; p[b].y = sy; uv[b].x = 1.f; uv[b].y = texv;
        }
        SDL_Color c4[4] = { {255,255,255,255}, {255,255,255,255},
                            {255,255,255,255}, {255,255,255,255} };
        cat_draw_textured_quad(tex, p, uv, c4);
    }
}

/* Rotate the outgoing + incoming channel snapshots as two adjacent cube faces
   about the horizontal axis. */
void jw_cf_draw_cube(const jw_cf_cube *cube, uint32_t elapsed) {
    if (!cube || !cube->out_tex || !cube->in_tex) return;
    SDL_Texture *out_tex = cube->out_tex;
    SDL_Texture *in_tex  = cube->in_tex;

    float sw = (float)cat_get_screen_width();
    float sh = (float)cat_get_screen_height();
    float t  = (float)elapsed / (float)JW_CF_CUBE_MS;
    if (t > 1.f) t = 1.f;
    t = jw_cf_ease_in_out_cubic(t);

    int   dir = cube->dir;                             /* +1 down, -1 up */
    float phi = -(float)dir * 1.5707963f * t;         /* rotate about X axis */
    float W = sw, H = sh, cx = sw * 0.5f, cy = sh * 0.5f;
    float C = 1.35f * H;                               /* camera distance (close = strong
                                                          perspective; strips below keep
                                                          the warp from shearing) */
    float S = (C - H * 0.5f) / C;                      /* front face fills at t=0 */
    float cphi = cosf(phi), sphi = sinf(phi);

    SDL_SetTextureBlendMode(out_tex, SDL_BLENDMODE_NONE);
    SDL_SetTextureBlendMode(in_tex,  SDL_BLENDMODE_NONE);

    /* Draw the farther face first (front centre depth vs incoming centre depth). */
    float zf = (H * 0.5f) * cphi;
    float zg = (dir > 0) ? -(H * 0.5f) * sphi : (H * 0.5f) * sphi;
    if (zf >= zg) {
        jw_cf_draw_cube_face(in_tex,  1, dir, W, H, cx, cy, C, S, cphi, sphi);
        jw_cf_draw_cube_face(out_tex, 0, dir, W, H, cx, cy, C, S, cphi, sphi);
    } else {
        jw_cf_draw_cube_face(out_tex, 0, dir, W, H, cx, cy, C, S, cphi, sphi);
        jw_cf_draw_cube_face(in_tex,  1, dir, W, H, cx, cy, C, S, cphi, sphi);
    }
}

/* Small ▲▼ (side by side) in the bottom-right corner cueing the channel switch. */
void jw_cf_draw_channel_hint(void) {
    SDL_Renderer *ren = cat_get_renderer();
    int sw = cat_get_screen_width();
    int sh = cat_get_screen_height();
    float s   = (float)cat_scale(9);       /* half-width */
    float h   = s * 0.7f;                   /* half-height */
    float gap = (float)cat_scale(10);
    float cyc = (float)sh - cat_scale(34);  /* vertical centre of the pair */
    float dx  = (float)sw - cat_scale(26) - s; /* down triangle (rightmost) centre */
    float ux  = dx - (2.f * s + gap);       /* up triangle centre, to its left */
    SDL_Color w = { 236, 238, 240, 255 };
    SDL_Vertex up[3] = {                                  /* pointing up */
        { { ux,     cyc - h }, w, {0,0} },
        { { ux - s, cyc + h }, w, {0,0} },
        { { ux + s, cyc + h }, w, {0,0} },
    };
    SDL_RenderGeometry(ren, NULL, up, 3, NULL, 0);
    SDL_Vertex dn[3] = {                                  /* pointing down */
        { { dx - s, cyc - h }, w, {0,0} },
        { { dx + s, cyc - h }, w, {0,0} },
        { { dx,     cyc + h }, w, {0,0} },
    };
    SDL_RenderGeometry(ren, NULL, dn, 3, NULL, 0);
}

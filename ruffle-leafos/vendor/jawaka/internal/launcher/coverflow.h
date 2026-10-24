#ifndef JW_LAUNCHER_COVERFLOW_H
#define JW_LAUNCHER_COVERFLOW_H

#include <SDL2/SDL.h>

#include <stdbool.h>
#include <stdint.h>

/* Cover Flow — the launcher's album-card carousel (systems/favorites/recents/
   apps/games channels) plus the rotating-cube channel transition. Extracted from
   the launcher main.c in stages; this first slice owns the animation state types
   and the pure render primitives (geometry only, no launcher state), so both the
   module and the launcher share one definition.

   Naming: public API is `jw_cf_*`; module-private statics stay `jw__cf_*`. */

/* Continuous, wrapping carousel position for a coverflow channel. vpos/target are
   CONTINUOUS (can drift outside [0,count)) so a wrap from the last cover to the
   first is a single-card slide, not a full-list rewind. The visible cursor is the
   list cursor; these just drive the smooth motion. */
typedef struct {
    bool      inited;
    int       count_seen;    /* list size when last initialised (re-init on change) */
    int       last_cursor;   /* last observed list cursor */
    bool      active;
    float     vpos;          /* current visual centre (continuous) */
    float     target;        /* tween target (stays ≡ cursor mod count) */
    uint32_t  last_ms;
} jw_games_cf;

/* Vertical channel switch, animated as a rotating cube: the outgoing + incoming
   channels are snapshot to textures and drawn as two adjacent cube faces. The
   launcher owns the snapshots (it knows how to render a channel); the module owns
   the type and the compositing. */
typedef struct {
    bool         active;
    int          dir;        /* +1 = down/next (in from below), -1 = up/prev */
    uint32_t     start_ms;
    SDL_Texture *out_tex;    /* outgoing channel snapshot (render target) */
    SDL_Texture *in_tex;     /* incoming channel snapshot */
} jw_cf_cube;

/* Duration of the channel-switch cube rotation. */
#define JW_CF_CUBE_MS 360u

/* Cover Flow window: JW_CF_WINDOW cover slots each side of centre, so up to
   JW_CF_VISIBLE covers are on screen at once. The carousel only wraps into an
   endless ring when there are at least this many items; with fewer it clamps at
   the ends (a cover is never drawn twice, and navigation never snaps across the
   row). Render (jw_cf_draw_cards) and input (the launcher's carousel stepping)
   must agree on this, so it lives in the header. */
#define JW_CF_WINDOW  3
#define JW_CF_VISIBLE (2 * JW_CF_WINDOW + 1)

static inline bool jw_cf_list_loops(int count) { return count >= JW_CF_VISIBLE; }

/* The launcher's Cover Flow runtime state, bundled so the launcher embeds one
   handle rather than three loose fields. systems_cf = the systems/favorites/
   recents/apps channel carousel; games_cf = the drilled-in games carousel;
   cube = the vertical channel-switch cube. */
typedef struct {
    jw_games_cf       games_cf;
    jw_games_cf       systems_cf;
    jw_cf_cube        cube;
} jw_coverflow;

/* ── CF-local math ──────────────────────────────────────────────────────── */
float jw_cf_clampf(float v, float lo, float hi);
float jw_cf_ease_in_out_cubic(float t);

/* Slide duration (ms) from the active stylesheet, clamped to a sane range. */
uint32_t jw_cf_anim_ms(void);

/* ── Carousel tween (operates on jw_games_cf) ───────────────────────────── */
/* Fold vpos/target back into [0,count) after a wrap so the floats never drift. */
void jw_games_cf_renormalize(jw_games_cf *cf, int count);
/* Advance the exponential approach of vpos -> target for one frame at `now`. */
void jw_games_cf_step(jw_games_cf *cf, uint32_t now);

/* ── Per-section card metrics + the shared carousel driver ──────────────── */

/* Per-section Cover Flow card metrics (data, not code). size = card height as a
   fraction of screen height; oy = vertical centre fraction; step = neighbour
   spacing as a fraction of card width; side_scale = how big side cards stay
   toward the edges. One profile per carousel section so each tunes separately. */
typedef struct {
    float size, oy, step, side_scale;
} jw_cf_layout;

typedef enum {
    JW_CF_SECTION_SYSTEMS = 0,
    JW_CF_SECTION_FAVORITES,
    JW_CF_SECTION_RECENTS,
    JW_CF_SECTION_APPS,
    JW_CF_SECTION_GAMES,
    JW_CF_SECTION_COUNT
} jw_cf_section;

/* The per-section metrics table, indexed by jw_cf_section. */
extern const jw_cf_layout jw_cf_layouts[JW_CF_SECTION_COUNT];

/* Resolve one item's card texture by index. `ctx` is the caller's context
   (opaque to the module — the launcher passes its state). The tw/th out-params
   receive the texture's native size; returns NULL (with tw=th=0) while art
   streams in or when there is none, which the card renderer draws as a
   placeholder. */
typedef SDL_Texture *(*jw_cf_icon_fn)(void *ctx, int idx, int *tw, int *th);

/* Advance the continuous wrap tween on `cursor`, then draw the window of angled
   album cards (one texture per item via icon_fn) with floor reflections. The
   caller clears the stage, draws labels/overlays, and presents. Returns true
   while a slide is still in flight (the caller uses this to gate its inline
   cover-decode throttle and request another frame). */
bool jw_cf_draw_cards(jw_games_cf *cf, int count, int cursor,
                      jw_cf_icon_fn icon_fn, void *ctx, jw_cf_layout layout);

/* ── Pure render primitives (geometry in, pixels out) ───────────────────── */

/* Draw one album card (perspective-rotated about its vertical axis) plus a short
   fading reflection beneath it. (ox,oy) is the card centre on screen; hw/hh are
   the uniform card half-extents; ang is the tilt in radians; alpha dims sides.
   The cover is CONTAIN-fit inside a uniform card backing; NULL/zero-size tex draws
   a dark placeholder. Rendered as vertical strips so the affine texture warp along
   a single quad's diagonal never shears the turning card. */
void jw_cf_draw_card(SDL_Texture *tex, int tw, int th,
                     float ox, float oy, float hw, float hh,
                     float ang, uint8_t alpha);

/* Draw one channel-cube face, subdivided into horizontal strips (same anti-shear
   trick as the card, foreshortening vertically). kind 0 = front (outgoing);
   kind 1 = incoming (hinged below when dir>0, above when dir<0). C/S/cphi/sphi are
   the caller's camera + rotation terms. */
void jw_cf_draw_cube_face(SDL_Texture *tex, int kind, int dir,
                          float W, float H, float cx, float cy,
                          float C, float S, float cphi, float sphi);

/* Composite the cube's two channel snapshots as adjacent faces rotating about the
   horizontal axis, `elapsed` ms into the JW_CF_CUBE_MS transition. No-op until
   both snapshots exist (the launcher fills them via its channel capture). */
void jw_cf_draw_cube(const jw_cf_cube *cube, uint32_t elapsed);

/* Small ▲▼ pair in the bottom-right corner cueing the Up/Down channel switch. */
void jw_cf_draw_channel_hint(void);

#endif /* JW_LAUNCHER_COVERFLOW_H */

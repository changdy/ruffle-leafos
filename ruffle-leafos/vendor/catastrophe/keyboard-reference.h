/* ═══════════════════════════════════════════════════════════════════════════
 * KEYBOARD Implementation — matches Gabagool 5-row layout with special keys
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Special key identifiers used internally for cursor tracking.
 * These are negative to distinguish them from character-key grid indices. */
#define CAT__KB_KEY_BACKSPACE  (-1)
#define CAT__KB_KEY_ENTER      (-2)
#define CAT__KB_KEY_SHIFT      (-3)
#define CAT__KB_KEY_SYMBOL     (-4)
#define CAT__KB_KEY_SPACE      (-5)

#define CAT__KB_SHIFT_LABEL "\xe2\x87\xa7" /* ⇧ */
#define CAT__KB_FOCUS_ANIM_MS 80.0f

typedef struct {
    int id;
    int x, y, w, h;
    int radius;
    const char *label;
    TTF_Font *font;
    bool space_indicator;
} cat__kb_key_visual;

typedef struct {
    int target_id;
    float from_x, from_y, from_w, from_h;
    float target_x, target_y, target_w, target_h;
    float current_x, current_y, current_w, current_h;
    uint32_t start_ms;
    bool initialized;
    bool active;
} cat__kb_focus_motion;

static void cat__kb_focus_position(cat__kb_focus_motion *motion, uint32_t now,
                                   float *x, float *y, float *w, float *h) {
    float t = 1.0f;
    if (motion->active) {
        t = cat__clampf((float)(now - motion->start_ms) /
                        CAT__KB_FOCUS_ANIM_MS, 0.0f, 1.0f);
        float inv = 1.0f - t;
        t = 1.0f - inv * inv * inv;
    }
    *x = cat__lerpf(motion->from_x, motion->target_x, t);
    *y = cat__lerpf(motion->from_y, motion->target_y, t);
    *w = cat__lerpf(motion->from_w, motion->target_w, t);
    *h = cat__lerpf(motion->from_h, motion->target_h, t);
    if (t >= 1.0f) motion->active = false;
}

static void cat__kb_draw_focus(cat__kb_focus_motion *motion,
                               const cat__kb_key_visual *target,
                               cat_draw_color bg) {
    if (!motion || !target) return;

    uint32_t now = SDL_GetTicks();
    if (!motion->initialized) {
        motion->target_id = target->id;
        motion->from_x = motion->target_x = (float)target->x;
        motion->from_y = motion->target_y = (float)target->y;
        motion->from_w = motion->target_w = (float)target->w;
        motion->from_h = motion->target_h = (float)target->h;
        motion->initialized = true;
    } else if (motion->target_id != target->id ||
               motion->target_x != (float)target->x ||
               motion->target_y != (float)target->y ||
               motion->target_w != (float)target->w ||
               motion->target_h != (float)target->h) {
        float x, y, w, h;
        cat__kb_focus_position(motion, now, &x, &y, &w, &h);
        motion->from_x = x;
        motion->from_y = y;
        motion->from_w = w;
        motion->from_h = h;
        motion->target_x = (float)target->x;
        motion->target_y = (float)target->y;
        motion->target_w = (float)target->w;
        motion->target_h = (float)target->h;
        motion->target_id = target->id;
        motion->start_ms = now;
        motion->active = true;
    }

    float fx, fy, fw, fh;
    cat__kb_focus_position(motion, now, &fx, &fy, &fw, &fh);
    motion->current_x = fx;
    motion->current_y = fy;
    motion->current_w = fw;
    motion->current_h = fh;
    if (motion->active) cat_request_frame();

    int x = (int)(fx + 0.5f);
    int y = (int)(fy + 0.5f);
    int w = (int)(fw + 0.5f);
    int h = (int)(fh + 0.5f);
    cat_draw_rounded_rect(x, y, w, h, target->radius, bg);
}

static void cat__kb_draw_keys(const cat__kb_key_visual *keys, int count,
                              int selected_id, cat__kb_focus_motion *motion,
                              cat_draw_color normal_bg, cat_draw_color focus_bg,
                              cat_draw_color normal_fg, cat_draw_color focus_fg) {
    const cat__kb_key_visual *target = NULL;
    for (int i = 0; i < count; i++) {
        const cat__kb_key_visual *key = &keys[i];
        cat_draw_rounded_rect(key->x, key->y, key->w, key->h,
                              key->radius, normal_bg);
        if (key->id == selected_id) target = key;
    }

    /* The moving layer contains only the background. Labels remain attached to
       their keys and are painted afterward, so focus motion can never make two
       keys appear to exchange content. */
    if (target) cat__kb_draw_focus(motion, target, focus_bg);

    for (int i = 0; i < count; i++) {
        const cat__kb_key_visual *key = &keys[i];
        float focus = 0.0f;
        if (target) {
            float focus_cx = motion->current_x + motion->current_w * 0.5f;
            float focus_cy = motion->current_y + motion->current_h * 0.5f;
            float key_cx = (float)key->x + key->w * 0.5f;
            float key_cy = (float)key->y + key->h * 0.5f;
            float span_x = (motion->current_w + key->w) * 0.5f;
            float span_y = (motion->current_h + key->h) * 0.5f;
            if (span_x < 1.0f) span_x = 1.0f;
            if (span_y < 1.0f) span_y = 1.0f;
            float dx = (focus_cx - key_cx) / span_x;
            float dy = (focus_cy - key_cy) / span_y;
            focus = cat__clampf(1.0f - sqrtf(dx * dx + dy * dy), 0.0f, 1.0f);
            focus = focus * focus * (3.0f - 2.0f * focus);
        }
        cat_draw_color fg = cat_draw_color_lerp(normal_fg, focus_fg, focus);
        if (key->space_indicator) {
            int line_w = key->w / 3;
            int line_h = CAT_S(3);
            cat_draw_rect(key->x + (key->w - line_w) / 2,
                          key->y + (key->h - line_h) / 2,
                          line_w, line_h, fg);
        } else if (key->label && key->font) {
            int tw = cat_measure_text(key->font, key->label);
            int th = TTF_FontHeight(key->font);
            cat_draw_text(key->font, key->label,
                          key->x + (key->w - tw) / 2,
                          key->y + (key->h - th) / 2, fg);
        }
    }
}

/* 5-row general layout — 12-column virtual grid.
 * Row 0: 1-0 keys (10) + backspace (2 cols)
 * Row 1: qwertyuiop (10 cols, centered)
 * Row 2: asdfghjkl (9 cols) + enter (1.5 cols)
 * Row 3: shift (2 cols) + zxcvbnm (7 cols) + symbol (2 cols)
 * Row 4: space (8 cols, centered)
 */

/* Lower-case keys per row (NULL-terminated) */
static const char *cat__kb5_row0_lower[] = {"1","2","3","4","5","6","7","8","9","0", NULL};
static const char *cat__kb5_row1_lower[] = {"q","w","e","r","t","y","u","i","o","p", NULL};
static const char *cat__kb5_row2_lower[] = {"a","s","d","f","g","h","j","k","l", NULL};
static const char *cat__kb5_row3_lower[] = {"z","x","c","v","b","n","m", NULL};

/* Upper-case */
static const char *cat__kb5_row0_upper[] = {"1","2","3","4","5","6","7","8","9","0", NULL};
static const char *cat__kb5_row1_upper[] = {"Q","W","E","R","T","Y","U","I","O","P", NULL};
static const char *cat__kb5_row2_upper[] = {"A","S","D","F","G","H","J","K","L", NULL};
static const char *cat__kb5_row3_upper[] = {"Z","X","C","V","B","N","M", NULL};

/* Symbols mode */
static const char *cat__kb5_row0_sym[] = {"!","@","#","$","%","^","&","*","(",")", NULL};
static const char *cat__kb5_row1_sym[] = {"`","~","[","]","\\","|","{","}",";",":", NULL};
static const char *cat__kb5_row2_sym[] = {"'","\"","<",">","?","/","+","=","_", NULL};
static const char *cat__kb5_row3_sym[] = {",",".","-","\xe2\x82\xac","\xc2\xa3","\xc2\xa5","\xc2\xa2", NULL};

/* Numeric layout */
static const char *cat__kb_numeric[] = {
    "1", "2", "3",
    "4", "5", "6",
    "7", "8", "9",
    NULL, "0", NULL,
};
#define CAT__KB_COLS_NUMERIC  3
#define CAT__KB_ROWS_NUMERIC  4

/* Keyboard row metadata for 5-row general layout */
typedef struct {
    const char **chars;       /* pointer to char array for this row */
    int          char_count;  /* number of character keys */
    int          special;     /* special key at end: CAT__KB_KEY_* or 0 = none */
    int          special_pre; /* special key at start: CAT__KB_KEY_* or 0 = none */
} cat__kb_row_info;

static int cat__kb5_count(const char **row) {
    int n = 0;
    while (row[n]) n++;
    return n;
}

static bool cat__kb_utf8_is_continuation_byte(unsigned char c) {
    return (c & 0xC0u) == 0x80u;
}

static int cat__kb_utf8_clamp_boundary(const char *text, int idx) {
    int len;

    if (!text) return 0;
    len = (int)strlen(text);
    if (idx < 0) idx = 0;
    if (idx > len) idx = len;

    while (idx > 0 && idx < len &&
           cat__kb_utf8_is_continuation_byte((unsigned char)text[idx])) {
        idx--;
    }
    return idx;
}

static int cat__kb_utf8_prev_boundary(const char *text, int idx) {
    int len;

    if (!text) return 0;
    len = (int)strlen(text);
    if (idx < 0) idx = 0;
    if (idx > len) idx = len;
    if (idx > 0 &&
        idx < len &&
        cat__kb_utf8_is_continuation_byte((unsigned char)text[idx])) {
        return cat__kb_utf8_clamp_boundary(text, idx);
    }

    idx = cat__kb_utf8_clamp_boundary(text, idx);
    if (idx <= 0) return 0;
    idx--;
    while (idx > 0 && cat__kb_utf8_is_continuation_byte((unsigned char)text[idx])) {
        idx--;
    }
    return idx;
}

static int cat__kb_utf8_next_boundary(const char *text, int idx) {
    int len;

    if (!text) return 0;
    len = (int)strlen(text);
    if (idx < 0) idx = 0;
    if (idx > len) idx = len;
    if (idx >= len) return len;

    if (cat__kb_utf8_is_continuation_byte((unsigned char)text[idx])) {
        while (idx < len &&
               cat__kb_utf8_is_continuation_byte((unsigned char)text[idx])) {
            idx++;
        }
        return idx;
    }

    idx++;
    while (idx < len && cat__kb_utf8_is_continuation_byte((unsigned char)text[idx])) {
        idx++;
    }
    return idx;
}

/* Helper: insert a string at text_cursor in result->text */
static int cat__keyboard_ex_impl(const char *initial_text, const char *help_text,
                cat_keyboard_layout layout, SDL_Texture *backdrop,
                cat_keyboard_result *result);

static void cat__kb_insert(cat_keyboard_result *result, int *text_cursor, const char *str) {
    int cursor;
    int len;
    int slen;

    if (!result || !text_cursor || !str) return;
    cursor = cat__kb_utf8_clamp_boundary(result->text, *text_cursor);
    len = (int)strlen(result->text);
    slen = (int)strlen(str);
    if (len + slen < (int)sizeof(result->text) - 1) {
        memmove(result->text + cursor + slen,
                result->text + cursor,
                len - cursor + 1);
        memcpy(result->text + cursor, str, slen);
        *text_cursor = cursor + slen;
        cat_ui_feedback_emit(CAT_UI_ENTERED);
    } else {
        cat_ui_feedback_emit(CAT_UI_EDGE);   /* the field is full */
    }
}

/* Helper: backspace at text_cursor */
static void cat__kb_backspace(cat_keyboard_result *result, int *text_cursor) {
    int cursor;
    int prev;
    int len;

    if (!result || !text_cursor) return;
    cursor = cat__kb_utf8_clamp_boundary(result->text, *text_cursor);
    if (cursor <= 0) {
        *text_cursor = 0;
        cat_ui_feedback_emit(CAT_UI_EDGE);   /* nothing left to delete */
        return;
    }

    prev = cat__kb_utf8_prev_boundary(result->text, cursor);
    len = (int)strlen(result->text);
    memmove(result->text + prev,
            result->text + cursor,
            len - cursor + 1);
    *text_cursor = prev;
    cat_ui_feedback_emit(CAT_UI_ENTERED);
}

/* The caret clamps at both ends of the text rather than wrapping, so pushing
   past either end is a boundary. */
static void cat__kb_move_cursor_left(const char *text, int *text_cursor) {
    if (!text_cursor) return;
    int before = *text_cursor;
    *text_cursor = cat__kb_utf8_prev_boundary(text, *text_cursor);
    cat_ui_feedback_emit(*text_cursor != before ? CAT_UI_MOVED : CAT_UI_EDGE);
}

static void cat__kb_move_cursor_right(const char *text, int *text_cursor) {
    if (!text_cursor) return;
    int before = *text_cursor;
    *text_cursor = cat__kb_utf8_next_boundary(text, *text_cursor);
    cat_ui_feedback_emit(*text_cursor != before ? CAT_UI_MOVED : CAT_UI_EDGE);
}

static void cat__kb_draw_footer(void) {
    cat_footer_item kb_footer[] = {
        { .button = CAT_BTN_Y,     .label = "CANCEL",  .is_confirm = false },
        { .button = CAT_BTN_MENU,  .label = "HELP",    .is_confirm = false },
        { .button = CAT_BTN_START, .label = "CONFIRM", .is_confirm = true  },
    };
    cat_draw_footer(kb_footer, (int)(sizeof(kb_footer) / sizeof(kb_footer[0])));
}

/* Draw keyboard input text field content with horizontal scrolling.
   Keeps the caret visible by adjusting *text_scroll as the cursor moves. */
static void cat__kb_draw_input_text(TTF_Font *font, cat_keyboard_result *result,
                                   int text_cursor, int *text_scroll,
                                   int input_x, int input_y, int input_w, int input_h,
                                   bool caret_visible, cat_draw_color text_color) {
    int ty = input_y + (input_h - TTF_FontHeight(font)) / 2;
    int tx = input_x + CAT_S(16);
    int field_w = input_w - CAT_S(32);
    int safe_cursor;

    if (field_w <= 0) return;
    int caret_w = CAT_S(2);
    safe_cursor = cat__kb_utf8_clamp_boundary(result->text, text_cursor);

    /* Measure cursor pixel position */
    char saved = result->text[safe_cursor];
    result->text[safe_cursor] = '\0';
    int cursor_px = cat_measure_text(font, result->text);
    result->text[safe_cursor] = saved;

    /* Adjust scroll to keep caret visible, centering when it drifts */
    if (cursor_px - *text_scroll > field_w - caret_w)
        *text_scroll = cursor_px - field_w / 2;
    if (*text_scroll > 0 && cursor_px - *text_scroll < field_w / 3)
        *text_scroll = cursor_px - field_w / 2;
    if (*text_scroll < 0)
        *text_scroll = 0;
    int full_w = cat_measure_text(font, result->text);
    int max_scroll = full_w - field_w;
    if (max_scroll < 0) max_scroll = 0;
    if (*text_scroll > max_scroll)
        *text_scroll = max_scroll;

    /* Draw text with scroll offset, clipped to field */
    if (result->text[0]) {
        SDL_Rect clip = {tx, ty, field_w, TTF_FontHeight(font)};
        SDL_RenderSetClipRect(cat_get_renderer(), &clip);
        cat_draw_text(font, result->text, tx - *text_scroll, ty, text_color);
        SDL_RenderSetClipRect(cat_get_renderer(), NULL);
    }

    /* Draw caret */
    if (caret_visible) {
        int cx = tx + cursor_px - *text_scroll;
        cat_draw_rect(cx, ty, caret_w, TTF_FontHeight(font), text_color);
    }
}

/* Built-in keyboard help text matching Gabagool's instructions */
static const char *cat__kb_help_default =
    "D-Pad: Navigate between keys\n"
    "A: Type the selected key\n"
    "B: Backspace\n"
    "X: Space\n"
    "L1 / R1: Move cursor within text\n"
    "Select: Toggle Shift (uppercase/symbols)\n"
    "Y: Cancel without saving\n"
    "Start: Confirm input";

static const char *cat__kb_help_numeric =
    "D-Pad: Navigate between keys\n"
    "A: Type the selected digit\n"
    "B: Backspace\n"
    "L1 / R1: Move cursor within text\n"
    "Y: Cancel without saving\n"
    "Start: Confirm input";

static const char *cat__kb_help_url =
    "D-Pad: Navigate between keys\n"
    "A: Type the selected key\n"
    "B: Backspace\n"
    "X: Toggle shortcut alternates\n"
    "L1 / R1: Move cursor within text\n"
    "Select: Toggle Shift (uppercase)\n"
    "Y: Cancel without saving\n"
    "Start: Confirm input\n"
    "123/abc: Toggle number/symbol grid";

/* Extended keyboard: identical to cat_keyboard() but accepts an optional
   full-screen backdrop texture. When `backdrop` is non-NULL it is blitted to
   fill the screen each frame in place of the themed background — letting a
   caller (e.g. a Cover Flow launcher) show its own snapshot behind the keys.
   A supplied backdrop also suppresses the control-hint footer and maps X to
   cancel/back-out (the caller owns the chrome, and its users are expected to
   know the controls — matching the host's overlay). cat_keyboard()
   below is a thin wrapper that passes backdrop = NULL, so every existing caller
   is byte-for-byte unchanged. */
/* A blocking widget runs its own event loop, so the host's input dispatch never
   sees it open or close -- it has to say so itself, at both ends, including the
   cancelled and failed exits. */
int cat_keyboard_ex(const char *initial_text, const char *help_text,
                cat_keyboard_layout layout, SDL_Texture *backdrop,
                cat_keyboard_result *result) {
    cat_ui_feedback_emit(CAT_UI_SURFACE);
    int rc = cat__keyboard_ex_impl(initial_text, help_text, layout, backdrop, result);
    cat_ui_feedback_emit(CAT_UI_SURFACE);
    return rc;
}

static int cat__keyboard_ex_impl(const char *initial_text, const char *help_text,
                cat_keyboard_layout layout, SDL_Texture *backdrop,
                cat_keyboard_result *result) {
    if (!result) return CAT_ERROR;

    memset(result, 0, sizeof(*result));
    if (initial_text) {
        strncpy(result->text, initial_text, sizeof(result->text) - 1);
    }

    cat_theme *theme = cat_get_theme();
    int screen_w = cat_get_screen_width();
    int screen_h = cat_get_screen_height();

    TTF_Font *text_font    = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *key_font     = cat_get_font(CAT_FONT_MEDIUM);   /* Gabagool: MediumFont for keys */
    /* Special keys (⇧ ← ↵) use the glyph-complete symbol font, not the themed
       family — lean fonts (Inter, Nunito, Lexend…) lack those symbols. */
    TTF_Font *special_font = cat_get_symbol_font();
    if (!text_font || !key_font) return CAT_ERROR;

    bool is_numeric = (layout == CAT_KB_NUMERIC);

    /* ── Numeric keyboard (simple 3×4 grid) ── */
    if (is_numeric) {
        int kb_cols = CAT__KB_COLS_NUMERIC;
        int kb_rows = CAT__KB_ROWS_NUMERIC;
        int key_w = CAT_S(80);
        int key_h = CAT_S(52);
        int key_gap = CAT_S(6);
        int key_r = CAT_S(8);
        int grid_w = kb_cols * (key_w + key_gap) - key_gap;
        int grid_x = (screen_w - grid_w) / 2;
        int grid_y = screen_h - (kb_rows * (key_h + key_gap)) - cat_get_footer_height();
        int input_y = CAT_S(40), input_h = CAT_S(60), input_x = CAT_S(40);
        int input_w = screen_w - CAT_S(80);
        int cursor_x = 0, cursor_y = 0;
        int text_cursor = (int)strlen(result->text);
        int text_scroll = 0;
        bool running = true;
        uint32_t caret_blink = SDL_GetTicks();
        bool caret_visible = true;
        cat__kb_focus_motion focus_motion = {0};

        while (running) {
            uint32_t now = SDL_GetTicks();
            if (now - caret_blink > 500) { caret_visible = !caret_visible; caret_blink = now; }
            cat_request_frame_in(500);

            cat_input_event ev;
            while (cat_poll_input(&ev)) {
                if (!ev.pressed) continue;
                switch (ev.button) {
                    /* The grid wraps in both axes, so key-to-key is always a move. */
                    case CAT_BTN_UP:    cursor_y = (cursor_y - 1 + kb_rows) % kb_rows;
                                        cat_ui_feedback_emit(CAT_UI_MOVED); break;
                    case CAT_BTN_DOWN:  cursor_y = (cursor_y + 1) % kb_rows;
                                        cat_ui_feedback_emit(CAT_UI_MOVED); break;
                    case CAT_BTN_LEFT:  cursor_x = (cursor_x - 1 + kb_cols) % kb_cols;
                                        cat_ui_feedback_emit(CAT_UI_MOVED); break;
                    case CAT_BTN_RIGHT: cursor_x = (cursor_x + 1) % kb_cols;
                                        cat_ui_feedback_emit(CAT_UI_MOVED); break;
                    case CAT_BTN_A: {
                        int ki = cursor_y * kb_cols + cursor_x;
                        if (ki < kb_rows * kb_cols && cat__kb_numeric[ki])
                            cat__kb_insert(result, &text_cursor, cat__kb_numeric[ki]);
                        break;
                    }
                    case CAT_BTN_B: cat__kb_backspace(result, &text_cursor); break;
                    case CAT_BTN_X: if (backdrop) return CAT_CANCELLED; break;
                    case CAT_BTN_Y: return CAT_CANCELLED;
                    case CAT_BTN_START: return CAT_OK;
                    case CAT_BTN_L1: cat__kb_move_cursor_left(result->text, &text_cursor); break;
                    case CAT_BTN_R1: cat__kb_move_cursor_right(result->text, &text_cursor); break;
                    case CAT_BTN_MENU:
                        cat_show_help_overlay(help_text ? help_text : cat__kb_help_numeric);
                        break;
                    default: break;
                }
            }

            if (backdrop) SDL_RenderCopy(cat_get_renderer(), backdrop, NULL, NULL);
            else          cat_draw_background();
            cat_draw_pill(input_x, input_y, input_w, input_h, theme->highlight);
            cat__kb_draw_input_text(text_font, result, text_cursor, &text_scroll,
                                   input_x, input_y, input_w, input_h,
                                   caret_visible, theme->highlighted_text);
            cat__kb_key_visual keys[CAT__KB_COLS_NUMERIC * CAT__KB_ROWS_NUMERIC];
            int key_count = 0;
            for (int row = 0; row < kb_rows; row++) {
                for (int col = 0; col < kb_cols; col++) {
                    int ki = row * kb_cols + col;
                    const char *key = cat__kb_numeric[ki];
                    if (!key) continue;
                    int kx = grid_x + col * (key_w + key_gap);
                    int ky = grid_y + row * (key_h + key_gap);
                    keys[key_count++] = (cat__kb_key_visual){
                        .id = ki, .x = kx, .y = ky, .w = key_w, .h = key_h,
                        .radius = key_r, .label = key, .font = key_font,
                    };
                }
            }
            cat__kb_draw_keys(keys, key_count,
                              cursor_y * kb_cols + cursor_x, &focus_motion,
                              theme->accent, theme->highlight,
                              theme->hint, theme->highlighted_text);
            if (!backdrop) cat__kb_draw_footer();
            cat_present();

        }
        return CAT_CANCELLED;
    }

    /* ═══════════════════════════════════════════════════════════════════════
     * 5-Row General Keyboard (matches Gabagool)
     * ═══════════════════════════════════════════════════════════════════════ */

    /* Keyboard occupies available space minus footer, text input = screen_h/10 */
    int footer_h = cat_get_footer_height();
    int kb_area_h  = screen_h * 85 / 100 - footer_h;
    int input_h    = screen_h / 10;
    int input_y    = (screen_h - kb_area_h - footer_h - input_h) / 2;
    int input_x    = CAT_S(40);
    int input_w    = screen_w - CAT_S(80);

    /* 12-column grid, 5 key rows (footer handled separately) */
    int grid_cols = 12;
    int grid_rows_total = 5;
    int key_spacing = CAT_S(4);
    int key_w = (screen_w - CAT_S(40) - key_spacing * (grid_cols - 1)) / grid_cols;
    int key_h = (kb_area_h - key_spacing * (grid_rows_total - 1)) / grid_rows_total;
    int key_r = CAT_S(6);

    /* Grid top-left */
    int grid_x = (screen_w - (grid_cols * (key_w + key_spacing) - key_spacing)) / 2;
    int grid_y = input_y + input_h + CAT_S(10);

    /* State */
    int cursor_row = 1;  /* Start on qwerty row */
    int cursor_col = 0;
    bool shift = false;
    bool symbols = false;
    int text_cursor = (int)strlen(result->text);
    int text_scroll = 0;
    bool running = true;
    cat__kb_focus_motion focus_motion = {0};

    uint32_t caret_blink = SDL_GetTicks();
    bool caret_visible = true;

    /* Current key arrays per row */
    #define CAT__KB5_ROW_COUNT 5

    while (running) {
        uint32_t now = SDL_GetTicks();
        if (now - caret_blink > 500) { caret_visible = !caret_visible; caret_blink = now; }
        cat_request_frame_in(500);

        /* Build current row info from mode */
        const char **r0 = symbols ? cat__kb5_row0_sym : (shift ? cat__kb5_row0_upper : cat__kb5_row0_lower);
        const char **r1 = symbols ? cat__kb5_row1_sym : (shift ? cat__kb5_row1_upper : cat__kb5_row1_lower);
        const char **r2 = symbols ? cat__kb5_row2_sym : (shift ? cat__kb5_row2_upper : cat__kb5_row2_lower);
        const char **r3 = symbols ? cat__kb5_row3_sym : (shift ? cat__kb5_row3_upper : cat__kb5_row3_lower);

        int r0n = cat__kb5_count(r0);  /* 10 */
        int r1n = cat__kb5_count(r1);  /* 10 */
        int r2n = cat__kb5_count(r2);  /* 9 */
        int r3n = cat__kb5_count(r3);  /* 7 */

        /* Effective navigation column limits per row:
         * Row 0: 0..10 (10 chars + backspace at col 10)
         * Row 1: 0..9  (10 chars)
         * Row 2: 0..9  (9 chars + enter at col 9)
         * Row 3: 0..8  (shift at 0, then 7 chars at 1..7, symbol at 8)
         * Row 4: 0 only (space bar)  */
        int row_max_col[CAT__KB5_ROW_COUNT] = {10, 9, 9, 8, 0};

        /* Input */
        cat_input_event iev;
        while (cat_poll_input(&iev)) {
            if (!iev.pressed) continue;

            switch (iev.button) {
                /* Rows and columns both wrap, so key-to-key is always a move. */
                case CAT_BTN_UP:
                    cursor_row = (cursor_row - 1 + CAT__KB5_ROW_COUNT) % CAT__KB5_ROW_COUNT;
                    if (cursor_col > row_max_col[cursor_row])
                        cursor_col = row_max_col[cursor_row];
                    cat_ui_feedback_emit(CAT_UI_MOVED);
                    break;
                case CAT_BTN_DOWN:
                    cursor_row = (cursor_row + 1) % CAT__KB5_ROW_COUNT;
                    if (cursor_col > row_max_col[cursor_row])
                        cursor_col = row_max_col[cursor_row];
                    cat_ui_feedback_emit(CAT_UI_MOVED);
                    break;
                case CAT_BTN_LEFT:
                    cursor_col--;
                    if (cursor_col < 0) cursor_col = row_max_col[cursor_row];
                    cat_ui_feedback_emit(CAT_UI_MOVED);
                    break;
                case CAT_BTN_RIGHT:
                    cursor_col++;
                    if (cursor_col > row_max_col[cursor_row]) cursor_col = 0;
                    cat_ui_feedback_emit(CAT_UI_MOVED);
                    break;

                case CAT_BTN_A: {
                    /* Type the selected character or activate special key */
                    if (cursor_row == 0) {
                        if (cursor_col < r0n)
                            cat__kb_insert(result, &text_cursor, r0[cursor_col]);
                        else /* backspace */
                            cat__kb_backspace(result, &text_cursor);
                    } else if (cursor_row == 1) {
                        if (cursor_col < r1n)
                            cat__kb_insert(result, &text_cursor, r1[cursor_col]);
                    } else if (cursor_row == 2) {
                        if (cursor_col < r2n)
                            cat__kb_insert(result, &text_cursor, r2[cursor_col]);
                        else /* enter = confirm */
                            return CAT_OK;
                    } else if (cursor_row == 3) {
                        if (cursor_col == 0) {
                            /* shift */
                            shift = !shift; symbols = false;
                            cat_ui_feedback_emit(CAT_UI_ENTERED);
                        } else if (cursor_col <= r3n) {
                            cat__kb_insert(result, &text_cursor, r3[cursor_col - 1]);
                        } else {
                            /* symbol */
                            symbols = !symbols; shift = false;
                            cat_ui_feedback_emit(CAT_UI_ENTERED);
                        }
                    } else if (cursor_row == 4) {
                        /* space bar */
                        cat__kb_insert(result, &text_cursor, " ");
                    }
                    break;
                }

                case CAT_BTN_B:
                    /* Backspace (Gabagool mapping) */
                    cat__kb_backspace(result, &text_cursor);
                    break;

                case CAT_BTN_X:
                    /* Immersive (backdrop) mode: X backs out, matching the
                       host's overlay. Otherwise it's Space (Gabagool mapping). */
                    if (backdrop) return CAT_CANCELLED;
                    cat__kb_insert(result, &text_cursor, " ");
                    break;

                case CAT_BTN_Y:
                    /* Exit without saving (Gabagool mapping) */
                    return CAT_CANCELLED;

                case CAT_BTN_SELECT:
                    /* Toggle shift (Gabagool mapping) */
                    shift = !shift; symbols = false;
                    cat_ui_feedback_emit(CAT_UI_ENTERED);
                    break;

                case CAT_BTN_START:
                    /* Enter / confirm */
                    return CAT_OK;

                case CAT_BTN_L1:
                    /* Move text cursor left */
                    cat__kb_move_cursor_left(result->text, &text_cursor);
                    break;

                case CAT_BTN_R1:
                    /* Move text cursor right */
                    cat__kb_move_cursor_right(result->text, &text_cursor);
                    break;

                case CAT_BTN_MENU:
                    /* Show keyboard help overlay.
                     *
                     * NOTE: Unlike earlier releases, this uses the caller-provided
                     * `help_text` (when non-NULL) as the Menu help overlay content,
                     * falling back to `cat__kb_help_default` only when `help_text`
                     * is NULL. Callers should therefore pass concise help/usage
                     * strings here, not prompt-like text, since it will be shown
                     * verbatim in the overlay.
                     */
                    cat_show_help_overlay(help_text ? help_text : cat__kb_help_default);
                    break;

                default: break;
            }
        }

        /* ── Render ── */
        if (backdrop) SDL_RenderCopy(cat_get_renderer(), backdrop, NULL, NULL);
        else          cat_draw_background();

        /* Text input field */
        cat_draw_rounded_rect(input_x, input_y, input_w, input_h, CAT_S(8), theme->highlight);
        cat__kb_draw_input_text(text_font, result, text_cursor, &text_scroll,
                               input_x, input_y, input_w, input_h,
                               caret_visible, theme->highlighted_text);

        /* ── Key rendering ── */
        cat_draw_color key_bg_normal  = theme->accent;
        cat_draw_color key_bg_sel     = theme->highlight;
        cat_draw_color key_fg_normal  = theme->hint;
        cat_draw_color key_fg_sel     = theme->highlighted_text;
        cat__kb_key_visual keys[48];
        int key_count = 0;

        /* Collect the layout so backgrounds, moving focus, and fixed labels can
           be painted in separate layers. */
        #define CAT__KB_DRAW_KEY(px, py, pw, ph, plabel, is_sel, font_to_use) do { \
            int _id = (is_sel) ? cursor_row * 32 + cursor_col : -key_count - 1; \
            keys[key_count++] = (cat__kb_key_visual){ \
                .id = _id, \
                .x = (px), .y = (py), .w = (pw), .h = (ph), \
                .radius = key_r, .label = (plabel), .font = (font_to_use), \
            }; \
        } while(0)

        /* Row 0: numbers + backspace */
        {
            int row_y = grid_y;
            int cx = grid_x;
            for (int i = 0; i < r0n; i++) {
                bool sel = (cursor_row == 0 && cursor_col == i);
                CAT__KB_DRAW_KEY(cx, row_y, key_w, key_h, r0[i], sel, key_font);
                cx += key_w + key_spacing;
            }
            /* Backspace — 2× width */
            {
                int bw = key_w * 2 + key_spacing;
                bool sel = (cursor_row == 0 && cursor_col == r0n);
                CAT__KB_DRAW_KEY(cx, row_y, bw, key_h, "\xe2\x86\x90", sel, special_font ? special_font : key_font); /* ← */
            }
        }

        /* Row 1: qwertyuiop (centered) */
        {
            int row_y = grid_y + (key_h + key_spacing);
            int row_w = r1n * (key_w + key_spacing) - key_spacing;
            int cx = (screen_w - row_w) / 2;
            for (int i = 0; i < r1n; i++) {
                bool sel = (cursor_row == 1 && cursor_col == i);
                CAT__KB_DRAW_KEY(cx, row_y, key_w, key_h, r1[i], sel, key_font);
                cx += key_w + key_spacing;
            }
        }

        /* Row 2: asdfghjkl + enter */
        {
            int row_y = grid_y + 2 * (key_h + key_spacing);
            /* 9 keys + enter (1.5× width) */
            int enter_w = key_w * 3 / 2 + key_spacing / 2;
            int row_w = r2n * (key_w + key_spacing) + enter_w;
            int cx = (screen_w - row_w) / 2;
            for (int i = 0; i < r2n; i++) {
                bool sel = (cursor_row == 2 && cursor_col == i);
                CAT__KB_DRAW_KEY(cx, row_y, key_w, key_h, r2[i], sel, key_font);
                cx += key_w + key_spacing;
            }
            /* Enter */
            {
                bool sel = (cursor_row == 2 && cursor_col == r2n);
                CAT__KB_DRAW_KEY(cx, row_y, enter_w, key_h, "\xe2\x86\xb5", sel, special_font ? special_font : key_font); /* ↵ */
            }
        }

        /* Row 3: shift + zxcvbnm + symbol */
        {
            int row_y = grid_y + 3 * (key_h + key_spacing);
            int shift_w = key_w * 2 + key_spacing;
            int sym_w   = key_w * 2 + key_spacing;
            int row_w = shift_w + key_spacing + r3n * (key_w + key_spacing) - key_spacing + key_spacing + sym_w;
            int cx = (screen_w - row_w) / 2;
            /* Shift */
            {
                bool sel = (cursor_row == 3 && cursor_col == 0);
                CAT__KB_DRAW_KEY(cx, row_y, shift_w, key_h, CAT__KB_SHIFT_LABEL, sel, special_font ? special_font : key_font);
                cx += shift_w + key_spacing;
            }
            /* Character keys */
            for (int i = 0; i < r3n; i++) {
                bool sel = (cursor_row == 3 && cursor_col == i + 1);
                CAT__KB_DRAW_KEY(cx, row_y, key_w, key_h, r3[i], sel, key_font);
                cx += key_w + key_spacing;
            }
            /* Symbol toggle */
            {
                bool sel = (cursor_row == 3 && cursor_col == r3n + 1);
                CAT__KB_DRAW_KEY(cx, row_y, sym_w, key_h, symbols ? "ABC" : "#+=", sel, key_font);
            }
        }

        /* Row 4: space bar (8× width, centered) */
        {
            int row_y = grid_y + 4 * (key_h + key_spacing);
            int space_w = key_w * 8 + key_spacing * 7;
            int sx = (screen_w - space_w) / 2;
            bool sel = (cursor_row == 4);
            int id = sel ? cursor_row * 32 + cursor_col : -key_count - 1;
            keys[key_count++] = (cat__kb_key_visual){
                .id = id, .x = sx, .y = row_y, .w = space_w, .h = key_h,
                .radius = key_r, .space_indicator = true,
            };
        }

        #undef CAT__KB_DRAW_KEY

        cat__kb_draw_keys(keys, key_count, cursor_row * 32 + cursor_col,
                          &focus_motion, key_bg_normal, key_bg_sel,
                          key_fg_normal, key_fg_sel);

        if (!backdrop) cat__kb_draw_footer();

        cat_present();

    }

    return CAT_CANCELLED;
}

/* Backdrop-free keyboard — the original entry point. Unchanged behavior. */
int cat_keyboard(const char *initial_text, const char *help_text,
                cat_keyboard_layout layout, cat_keyboard_result *result) {
    return cat_keyboard_ex(initial_text, help_text, layout, NULL, result);
}

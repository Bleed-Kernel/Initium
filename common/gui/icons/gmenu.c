#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <gui/icons/gmenu.h>
#include <menu.h>
#include <lib/config.h>
#include <lib/term.h>
#include <lib/gterm.h>
#include <lib/getchar.h>
#include <lib/uri.h>
#include <lib/libc.h>
#include <lib/misc.h>
#include <lib/print.h>
#include <lib/rand.h>
#include <fs/file.h>
#include <lib/stb_image.h>
#include <mm/pmm.h>
#include <sys/cpu.h>
#include <drivers/mouse.h>
#include <flanterm.h>
#include <flanterm_backends/fb.h>

// shortcut: a level shows at most this many entries, raise it if a config ever needs more.
#define GMENU_MAX 128

struct gmenu_image {
    uint32_t *px; // 0xAARRGGBB, A 255 = opaque
    size_t w, h;
};

struct gmenu_icon {
    struct gmenu_image rest;
};

static struct flanterm_fb_context *ctx;
static uint32_t *back;
static size_t back_size;
static int64_t W, H, U;
// Rows outside [clip_y0, clip_y1) keep last frame's pixels, so nothing may draw there.
static int64_t clip_y0, clip_y1;

// Tile layout, in pixels: icon box, tile and gap between tiles.
#define ICON (96 * U)
#define TILE (ICON + 32 * U)
#define GAP (20 * U)

bool gmenu_usable(void) {
    if (quiet || serial || term_backend != GTERM || terms_i == 0) {
        return false;
    }
    char *gui = config_get_value(NULL, 0, "GUI");
    if (gui != NULL && strcmp(gui, "icons") != 0) {
        return false;
    }
    struct flanterm_fb_context *c = (void *)terms[0];
    return c->rotation == FLANTERM_FB_ROTATE_0 && c->width >= 640 && c->height >= 400;
}

static inline void blend_px(uint32_t *d, uint32_t rgb, unsigned a) {
    uint32_t s = *d;
    unsigned ia = 255 - a;
    uint32_t r = (((rgb >> 16) & 0xff) * a + ((s >> 16) & 0xff) * ia + 127) / 255;
    uint32_t g = (((rgb >> 8) & 0xff) * a + ((s >> 8) & 0xff) * ia + 127) / 255;
    uint32_t b = ((rgb & 0xff) * a + (s & 0xff) * ia + 127) / 255;
    *d = (r << 16) | (g << 8) | b;
}

static void fill_rect(int64_t x, int64_t y, int64_t w, int64_t h, uint32_t rgb, unsigned a) {
    int64_t x0 = x < 0 ? 0 : x, y0 = y < clip_y0 ? clip_y0 : y;
    int64_t x1 = x + w > W ? W : x + w, y1 = y + h > clip_y1 ? clip_y1 : y + h;
    if (a == 0) return;
    for (int64_t py = y0; py < y1; py++) {
        uint32_t *row = &back[py * W];
        if (a >= 255) {
            for (int64_t px = x0; px < x1; px++) row[px] = rgb;
        } else {
            for (int64_t px = x0; px < x1; px++) blend_px(&row[px], rgb, a);
        }
    }
}

static void draw_glyph(int64_t x, int64_t y, uint8_t c, int64_t scale, uint32_t rgb, unsigned a) {
    bool *glyph = &ctx->font_bool[(size_t)c * ctx->font_height * ctx->font_width];
    for (size_t gy = 0; gy < ctx->font_height; gy++) {
        for (size_t gx = 0; gx < ctx->font_width; gx++) {
            if (glyph[gy * ctx->font_width + gx]) {
                fill_rect(x + gx * scale, y + gy * scale, scale, scale, rgb, a);
            }
        }
    }
}

// shortcut: UTF-8 beyond ASCII draws as '?', map through cp437 if names need it.
static size_t text_chars(const char *s) {
    size_t n = 0;
    for (; *s; s++) {
        if (((uint8_t)*s & 0xc0) != 0x80) n++;
    }
    return n;
}

static int64_t text_width(const char *s, int64_t scale) {
    return (int64_t)text_chars(s) * ctx->font_width * scale;
}

// Draws at most max_chars characters, with a soft shadow so text reads on any wallpaper.
static void draw_text_a(int64_t x, int64_t y, const char *s, size_t max_chars, int64_t scale, uint32_t rgb, unsigned a) {
    int64_t off = scale > 1 ? scale / 2 : 1;
    for (int pass = 0; pass < 2; pass++) {
        int64_t cx = x;
        size_t n = 0;
        for (const char *p = s; *p && n < max_chars; p++) {
            uint8_t c = (uint8_t)*p;
            if ((c & 0xc0) == 0x80) continue;
            if (c & 0x80) c = '?';
            if (pass == 0) draw_glyph(cx + off, y + off, c, scale, 0, 140 * a / 255);
            else draw_glyph(cx, y, c, scale, rgb, a);
            cx += ctx->font_width * scale;
            n++;
        }
    }
}

static void draw_text_n(int64_t x, int64_t y, const char *s, size_t max_chars, int64_t scale, uint32_t rgb) {
    draw_text_a(x, y, s, max_chars, scale, rgb, 255);
}

// Centred on cx, cut down with "..." to fit max_w.
static void draw_text_centred_a(int64_t cx, int64_t y, const char *s, int64_t scale, uint32_t rgb, int64_t max_w, unsigned a) {
    int64_t cw = ctx->font_width * scale;
    size_t n = text_chars(s);
    if ((int64_t)n * cw <= max_w) {
        draw_text_a(cx - (int64_t)n * cw / 2, y, s, n, scale, rgb, a);
        return;
    }
    size_t keep = max_w / cw > 3 ? max_w / cw - 3 : 0;
    int64_t x = cx - (int64_t)(keep + 3) * cw / 2;
    draw_text_a(x, y, s, keep, scale, rgb, a);
    draw_text_a(x + (int64_t)keep * cw, y, "...", 3, scale, rgb, a);
}

static void draw_text_centred(int64_t cx, int64_t y, const char *s, int64_t scale, uint32_t rgb, int64_t max_w) {
    draw_text_centred_a(cx, y, s, scale, rgb, max_w, 255);
}

// Box filter down (nearest up) to fit size x size, premultiplied so edges don't fringe.
static void scale_image(struct gmenu_image *out, const uint8_t *img, size_t iw, size_t ih, size_t size) {
    if (iw >= ih) {
        out->w = size;
        out->h = ih * size / iw ? ih * size / iw : 1;
    } else {
        out->h = size;
        out->w = iw * size / ih ? iw * size / ih : 1;
    }
    out->px = ext_mem_alloc_counted(out->w * out->h, sizeof(uint32_t));
    for (size_t dy = 0; dy < out->h; dy++) {
        size_t sy0 = dy * ih / out->h, sy1 = (dy + 1) * ih / out->h;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (size_t dx = 0; dx < out->w; dx++) {
            size_t sx0 = dx * iw / out->w, sx1 = (dx + 1) * iw / out->w;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            uint64_t r = 0, g = 0, b = 0, a = 0, n = 0;
            for (size_t sy = sy0; sy < sy1; sy++) {
                for (size_t sx = sx0; sx < sx1; sx++) {
                    const uint8_t *p = &img[(sy * iw + sx) * 4];
                    r += (uint64_t)p[0] * p[3];
                    g += (uint64_t)p[1] * p[3];
                    b += (uint64_t)p[2] * p[3];
                    a += p[3];
                    n++;
                }
            }
            uint32_t px = 0;
            if (a != 0) {
                px = (uint32_t)(a / n) << 24 | (uint32_t)(r / a) << 16 | (uint32_t)(g / a) << 8 | (uint32_t)(b / a);
            }
            out->px[dy * out->w + dx] = px;
        }
    }
}

// RGBA bytes for stbi_image_free, or NULL.
static uint8_t *read_image(char *uri, int *iw, int *ih) {
    if (secure_boot_active && strchr(uri, '#') == NULL) {
        return NULL;
    }
    struct file_handle *f = uri_open_optional(uri, MEMMAP_BOOTLOADER_RECLAIMABLE,
#if defined(__i386__)
        false, NULL, NULL
#else
        true
#endif
    );
    if (f == NULL) {
        return NULL;
    }
    int bpp;
    uint8_t *img = stbi_load_from_memory(f->fd, f->size, iw, ih, &bpp, 4);
    fclose(f);
    if (img != NULL && (*iw <= 0 || *ih <= 0)) {
        stbi_image_free(img);
        return NULL;
    }
    return img;
}

// A bare name like bleed-kernel is a built in icon, <config dir>/icons/<name>.png
// on the boot volume; anything with a ':' or '/' is a path.
static bool icon_uri(const char *value, char *uri, size_t size) {
    size_t len = strlen(value);
    if (strchr(value, ':') != NULL || strchr(value, '/') != NULL) {
        if (len >= size) return false;
        memcpy(uri, value, len + 1);
        return true;
    }
    for (const char *p = value; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) {
            return false;
        }
    }
    if (len == 0 || 7 + strlen(config_dir) + 7 + len + 5 >= size) {
        return false;
    }
    char *p = uri;
    const char *parts[] = { "boot():", config_dir, "/icons/", value, ".png" };
    for (size_t i = 0; i < sizeof(parts) / sizeof(*parts); i++) {
        size_t n = strlen(parts[i]);
        memcpy(p, parts[i], n);
        p += n;
    }
    *p = 0;
    return true;
}

static struct gmenu_icon *load_named_icon(const char *value) {
    char uri[512];
    int iw = 0, ih = 0;
    if (!icon_uri(value, uri, sizeof(uri))) return NULL;
    uint8_t *img = read_image(uri, &iw, &ih);
    if (img == NULL) return NULL;
    struct gmenu_icon *icon = ext_mem_alloc(sizeof(*icon));
    scale_image(&icon->rest, img, iw, ih, ICON);
    stbi_image_free(img);
    return icon;
}

// The shared fallbacks, loaded once per menu session.
static struct gmenu_icon *builtin_icon(const char *name, struct gmenu_icon **slot, bool *tried) {
    if (!*tried) {
        *tried = true;
        *slot = load_named_icon(name);
    }
    return *slot;
}

static struct gmenu_icon *generic_icon, *folder_icon, *back_icon, *fw_icon, *efi_icon;
static bool generic_tried, folder_tried, back_tried, fw_tried, efi_tried;

static struct gmenu_icon *entry_icon(struct menu_entry *entry) {
    if (!entry->icon_tried) {
        entry->icon_tried = true;
        char *value = config_get_value(entry->body, 0, "ICONS");
        if (value == NULL) {
            value = config_get_value(entry->body, 0, "ICON");
        }
        if (value != NULL) {
            // config_get_value hands back a shared buffer the open may reuse.
            char *copy = strdup(value);
            entry->icon = load_named_icon(copy);
            pmm_free(copy, strlen(copy) + 1);
        }
    }
    if (entry->icon != NULL) {
        return entry->icon;
    }
    return entry->sub != NULL ? builtin_icon("folder", &folder_icon, &folder_tried)
                              : builtin_icon("generic", &generic_icon, &generic_tried);
}

static void draw_image(const struct gmenu_image *img, int64_t src_box, int64_t cx, int64_t cy, int64_t box, unsigned alpha) {
    int64_t w = (int64_t)img->w * box / src_box, h = (int64_t)img->h * box / src_box;
    if (w <= 0 || h <= 0 || alpha == 0) return;
    int64_t x0 = cx - w / 2, y0 = cy - h / 2;
    for (int64_t y = 0; y < h; y++) {
        int64_t py = y0 + y;
        if (py < clip_y0 || py >= clip_y1) continue;
        const uint32_t *row = &img->px[(w == (int64_t)img->w ? y : y * (int64_t)img->h / h) * img->w];
        for (int64_t x = 0; x < w; x++) {
            int64_t px = x0 + x;
            if (px < 0 || px >= W) continue;
            uint32_t p = row[w == (int64_t)img->w ? x : x * (int64_t)img->w / w];
            unsigned a = (p >> 24) * alpha / 255;
            if (a) blend_px(&back[py * W + px], p & 0xffffff, a);
        }
    }
}

static void draw_icon(const struct gmenu_icon *icon, int64_t cx, int64_t cy, unsigned alpha) {
    draw_image(&icon->rest, ICON, cx, cy, ICON, alpha);
}

struct level {
    struct menu_entry *parent;
    struct menu_entry *items[GMENU_MAX]; // NULL is the Back tile
    size_t n;
};

// Firmware Setup and the UEFI Shell sit at the end of the top row as tiles of their own.
static struct menu_entry fw_tile = { .name = "Firmware Setup" };
static struct menu_entry shell_tile = { .name = "UEFI Shell" };
static const struct gmenu_opts *opts;

static bool is_tool(struct menu_entry *e) {
    return e == &fw_tile || e == &shell_tile;
}

static struct gmenu_icon *tile_icon(struct menu_entry *e) {
    return e == NULL ? builtin_icon("back", &back_icon, &back_tried)
         : e == &fw_tile ? builtin_icon("firmware", &fw_icon, &fw_tried)
         : e == &shell_tile ? builtin_icon("efi", &efi_icon, &efi_tried)
         : entry_icon(e);
}

static void build_level(struct level *lv, struct menu_entry *parent) {
    lv->parent = parent;
    lv->n = 0;
    if (parent != NULL) {
        lv->items[lv->n++] = NULL;
    }
    for (struct menu_entry *e = parent ? parent->sub : menu_tree; e != NULL && lv->n < GMENU_MAX; e = e->next) {
        if (!should_skip_entry(e)) {
            lv->items[lv->n++] = e;
        }
    }
    if (parent == NULL && opts->firmware && lv->n < GMENU_MAX) lv->items[lv->n++] = &fw_tile;
    if (parent == NULL && opts->shell && lv->n < GMENU_MAX) lv->items[lv->n++] = &shell_tile;
}

static size_t level_index(struct level *lv, struct menu_entry *e) {
    for (size_t i = 0; e != NULL && i < lv->n; i++) {
        if (lv->items[i] == e) return i;
    }
    return lv->parent != NULL && lv->n > 1 ? 1 : 0;
}

// Everything that moves eases towards a target, in 1/256 steps (tile positions in 1/256 tiles).
struct anim {
    int64_t cam;
    int64_t zoom[GMENU_MAX];
    int64_t label;
    int64_t row;
    int64_t fade;
    int64_t pick;
    bool picking, leaving;
};

static struct anim an;
static size_t an_last_sel = (size_t)-1;

static int64_t row_y(void) {
    return H * 9 / 20 - TILE / 2;
}

static int64_t visible_tiles(void) {
    int64_t vis = (W - 96 * U + GAP) / (TILE + GAP);
    return vis < 1 ? 1 : vis;
}

static int64_t cam_target(struct level *lv, size_t sel) {
    int64_t n = lv->n, vis = visible_tiles();
    if (n <= vis) return (n - 1) * 128;
    int64_t c = (int64_t)sel * 256, lo = (vis - 1) * 128, hi = (n - 1) * 256 - (vis - 1) * 128;
    return c < lo ? lo : c > hi ? hi : c;
}

static int64_t tile_cx(int64_t pos) {
    return W / 2 + (pos - an.cam) * (TILE + GAP) / 256;
}

// One easing step: covers dt/tau of the way, at least one unit, snapping once close.
static bool approach(int64_t *v, int64_t target, uint64_t dt_us, uint64_t tau_us) {
    int64_t d = target - *v;
    if (d == 0) return false;
    if (d > -3 && d < 3) { *v = target; return false; }
    int64_t k = (int64_t)(dt_us * 256 / tau_us);
    if (k > 256) k = 256;
    int64_t step = d * k / 256;
    if (step == 0) step = d > 0 ? 1 : -1;
    *v += step;
    return true;
}

static bool anim_step(struct level *lv, size_t sel, uint64_t dt) {
    bool moving = false;
    moving |= approach(&an.cam, cam_target(lv, sel), dt, 70000);
    for (size_t i = 0; i < lv->n; i++) {
        moving |= approach(&an.zoom[i], i == sel ? 256 : 0, dt, i == sel ? 50000 : 160000);
    }
    moving |= approach(&an.label, 256, dt, 90000);
    moving |= approach(&an.pick, an.picking ? 256 : 0, dt, 60000);
    moving |= approach(&an.row, an.leaving ? 0 : 256, dt, an.leaving ? 70000 : 110000);
    moving |= approach(&an.fade, an.leaving ? 0 : 256, dt, an.leaving ? 90000 : 140000);
    return moving;
}

static void anim_level(struct level *lv, size_t sel) {
    an.cam = cam_target(lv, sel);
    for (size_t i = 0; i < GMENU_MAX; i++) an.zoom[i] = i == sel ? 256 : 0;
    an.row = 0;
    an.label = 0;
    an_last_sel = sel;
}

static bool hit_tile(struct level *lv, int64_t px, int64_t py, size_t *out) {
    int64_t ry = row_y();
    if (py < ry || py >= ry + TILE) return false;
    for (size_t i = 0; i < lv->n; i++) {
        int64_t cx = tile_cx((int64_t)i * 256);
        if (px >= cx - TILE / 2 && px < cx + TILE / 2) {
            *out = i;
            return true;
        }
    }
    return false;
}

// Copies rows [y0, y1) of the back buffer out, dimmed by fade (0..256).
static void present(int64_t y0, int64_t y1, int64_t fade) {
    mouse_erase_pointer();
    size_t stride = ctx->pitch / 4;
    bool native = ctx->red_mask_shift == 16 && ctx->green_mask_shift == 8 && ctx->blue_mask_shift == 0
               && ctx->red_mask_size == 8 && ctx->green_mask_size == 8 && ctx->blue_mask_size == 8;
    for (int64_t y = y0; y < y1; y++) {
        volatile uint32_t *row = ctx->framebuffer + y * stride;
        uint32_t *src = &back[y * W];
        if (native && fade >= 256) {
            memcpy((void *)row, src, (size_t)W * sizeof(uint32_t));
            continue;
        }
        for (int64_t x = 0; x < W; x++) {
            uint32_t p = src[x];
            if (fade < 256) {
                p = ((((p >> 16) & 0xff) * fade >> 8) << 16) | ((((p >> 8) & 0xff) * fade >> 8) << 8)
                  | ((p & 0xff) * fade >> 8);
            }
            if (!native) {
                uint32_t r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, b = p & 0xff;
                p = (r >> (8 - ctx->red_mask_size)) << ctx->red_mask_shift
                  | (g >> (8 - ctx->green_mask_size)) << ctx->green_mask_shift
                  | (b >> (8 - ctx->blue_mask_size)) << ctx->blue_mask_shift;
            }
            row[x] = p;
        }
    }
    if (ctx->flush_callback != NULL) {
        ctx->flush_callback(ctx->framebuffer + y0 * stride, ctx->pitch * (y1 - y0));
    }
    mouse_render_pointer_overlay();
}

static void draw_backdrop(const uint32_t *src) {
    if (src != NULL) {
        memcpy(&back[clip_y0 * W], &src[clip_y0 * W], (size_t)((clip_y1 - clip_y0) * W) * sizeof(uint32_t));
        return;
    }
    for (int64_t y = clip_y0; y < clip_y1; y++) {
        uint32_t t = (uint32_t)(y * 255 / H);
        uint32_t rgb = ((0x1e - (0x1e - 0x0b) * t / 255) << 16)
                     | ((0x24 - (0x24 - 0x0d) * t / 255) << 8)
                     | (0x30 - (0x30 - 0x12) * t / 255);
        for (int64_t x = 0; x < W; x++) back[y * W + x] = rgb;
    }
}

struct hint { const char *key, *label; };

static void draw_hints(int64_t y, const struct hint *hints, size_t n, uint32_t key_rgb) {
    int64_t cw = ctx->font_width * U;
    int64_t total = 0;
    for (size_t i = 0; i < n; i++) {
        total += text_width(hints[i].key, U) + cw + text_width(hints[i].label, U) + (i ? 3 * cw : 0);
    }
    int64_t x = (W - total) / 2;
    for (size_t i = 0; i < n; i++) {
        if (i) x += 3 * cw;
        draw_text_n(x, y, hints[i].key, (size_t)-1, U, key_rgb);
        x += text_width(hints[i].key, U) + cw;
        draw_text_n(x, y, hints[i].label, (size_t)-1, U, 0xc8c8c8);
        x += text_width(hints[i].label, U);
    }
}

static void draw(struct level *lv, size_t sel, const struct gmenu_opts *o, uint64_t timeout_ms, bool full) {
    int64_t fh = ctx->font_height;
    int64_t ry = row_y();

    static int64_t shown_fade;
    if (full) shown_fade = 0;
    bool whole = an.fade < 256 || shown_fade < 256;
    clip_y0 = whole ? 0 : ry - fh * U - 40 * U;
    if (clip_y0 < 0) clip_y0 = 0;
    clip_y1 = H;
    draw_backdrop(gterm_backdrop);

    if (o->branding[0] != '\0') {
        draw_text_centred(W / 2, 28 * U, o->branding, U, o->branding_rgb, W - 32 * U);
    }

    if (lv->n == 0) {
        const char *msg = o->config_ready ? "The config file contains no valid entries" : "Config file not found";
        draw_text_centred(W / 2, H / 2 - fh * U, msg, U, 0xffffff, W - 32 * U);
    } else {
        unsigned row_a = (unsigned)(an.row * 255 / 256);

        if (lv->parent != NULL) {
            draw_text_centred_a(W / 2, ry - fh * U - 24 * U, lv->parent->name, U, 0xc8c8c8, W - 64 * U, row_a);
        }

        int64_t cy = ry + TILE / 2;
        bool left_hidden = false, right_hidden = false;
        for (size_t i = 0; i < lv->n; i++) {
            int64_t cx = tile_cx((int64_t)i * 256);
            if (cx + TILE / 2 <= 0) { left_hidden = true; continue; }
            if (cx - TILE / 2 >= W) { right_hidden = true; continue; }
            int64_t pk = an.pick;
            uint32_t box_rgb = 0;
            for (int sh = 0; sh <= 16; sh += 8) {
                int64_t from = 0xff, to = (o->branding_rgb >> sh) & 0xff;
                box_rgb |= (uint32_t)(from + (to - from) * pk / 256) << sh;
            }
            unsigned box_a = (unsigned)(52 + (110 - 52) * pk / 256);
            if (i == sel) {
                fill_rect(cx - TILE / 2, ry, TILE, TILE, box_rgb, box_a * row_a / 255);
            }
            struct gmenu_icon *icon = tile_icon(lv->items[i]);
            if (icon == NULL) continue;
            draw_icon(icon, cx, cy, (unsigned)(150 + 105 * an.zoom[i] / 256) * row_a / 255);
        }

        int64_t arrow_y = ry + (TILE - fh * U * 2) / 2;
        if (left_hidden) {
            draw_glyph(16 * U, arrow_y, 0x11, 2 * U, 0xffffff, 200 * row_a / 255);
        }
        if (right_hidden) {
            draw_glyph(W - 16 * U - (int64_t)ctx->font_width * 2 * U, arrow_y, 0x10, 2 * U, 0xffffff, 200 * row_a / 255);
        }

        struct menu_entry *e = lv->items[sel];
        unsigned label_a = (unsigned)(an.label * row_a / 256);
        int64_t label_y = ry + TILE + 56 * U;
        draw_text_centred_a(W / 2, label_y, e ? e->name : "Back", U, 0xffffff, W - 64 * U, label_a);
        if (e != NULL && !is_tool(e)) {
            const char *comment = config_get_value(e->body, 0, "COMMENT");
            if (comment != NULL) {
                draw_text_centred_a(W / 2, label_y + fh * U + 8 * U, comment, U, 0xc8c8c8, W - 64 * U, label_a);
            }
        }
    }

    if (timeout_ms != 0 && lv->n != 0 && lv->items[sel] != NULL) {
        char buf[32];
        char *p = buf;
        const char *pre = "Booting in ";
        while (*pre) *p++ = *pre++;
        uint64_t secs = (timeout_ms + 999) / 1000;
        char digits[8];
        size_t nd = 0;
        do { digits[nd++] = '0' + secs % 10; secs /= 10; } while (secs && nd < sizeof(digits));
        while (nd) *p++ = digits[--nd];
        *p++ = 's';
        *p = 0;
        draw_text_centred(W / 2, H - 40 * U - fh * U * 2, buf, U, o->help_rgb, W);
    }

    if (!help_hidden && !an.leaving) {
        struct hint hints[6];
        size_t n = 0;
        if (lv->n != 0) hints[n++] = (struct hint){ "ENTER", "Boot" };
        if (o->editor && lv->n != 0 && lv->items[sel] != NULL && !is_tool(lv->items[sel])) hints[n++] = (struct hint){ "E", "Edit" };
        if (o->editor) hints[n++] = (struct hint){ "B", "Blank" };
        if (lv->parent != NULL) hints[n++] = (struct hint){ "ESC", "Back" };
        draw_hints(H - 24 * U - fh * U, hints, n, o->help_rgb);
    }

    present(clip_y0, clip_y1, an.fade);
    if (whole) shown_fade = an.fade;
}

// Microseconds, or a guess from the time asked to sleep when the TSC is not calibrated.
static uint64_t now_us(uint64_t slept_ms) {
    static uint64_t fake;
    if (tsc_freq != 0) return rdtsc_usec();
    fake += slept_ms * 1000;
    return fake;
}

#define FRAME_MS 16

static void game(void);

static int finish(int action) {
    pmm_free(back, back_size);
    back = NULL;
    // flanterm still thinks its old cells are on screen, so repaint them all.
    print("\e[2J\e[H");
    FOR_TERM(flanterm_full_refresh(TERM));
    return action;
}

// Runs the animations until they settle (or about a second has gone).
static void settle(struct level *lv, size_t sel) {
    uint64_t last = now_us(0);
    for (int frames = 0; frames < 60; frames++) {
        uint64_t t = now_us(0);
        bool moving = anim_step(lv, sel, t - last);
        last = t;
        draw(lv, sel, opts, 0, true);
        if (!moving) break;
        pit_sleep_ms_and_quit_on_input(FRAME_MS);
        if (tsc_freq == 0) (void)now_us(FRAME_MS);
    }
}

static int leave(struct level *lv, size_t sel, int action) {
    an.picking = true;
    settle(lv, sel);
    an.picking = false;
    if (!terse || action != '\n') {
        return finish(action);
    }
    an.leaving = true;
    an.pick = 256;
    an.picking = true; // keeps the box coloured while it fades
    settle(lv, sel);
    an.leaving = false;
    an.picking = false;
    pmm_free(back, back_size);
    back = NULL;
    quiet = true;
    return action;
}

int gmenu_run(struct menu_entry **selected, uint64_t *timeout_ms, bool *skip_timeout,
              const struct gmenu_opts *o) {
    ctx = (void *)terms[0];
    W = ctx->width;
    H = ctx->height;
    U = ctx->font_scale_x ? ctx->font_scale_x : 1;
    back_size = W * H * sizeof(uint32_t);
    back = ext_mem_alloc(back_size);

    opts = o;
    static struct level lv;
    build_level(&lv, *selected ? (*selected)->parent : NULL);
    size_t sel = level_index(&lv, *selected);

    // The menu fades in from black, the row a moment later.
    anim_level(&lv, sel);
    an.fade = 0;
    an.leaving = false;
    an.picking = false;
    an.pick = 0;

    char typed[3] = { 0 };
    bool full = true;
    uint64_t last = now_us(0);

    for (;;) {
        struct menu_entry *cur = lv.n ? lv.items[sel] : NULL;
        if (cur == NULL || cur->sub != NULL || is_tool(cur)) {
            *skip_timeout = true;
        }
        if (sel != an_last_sel) {
            an.label = 0;
            an_last_sel = sel;
        }

        uint64_t t = now_us(0);
        uint64_t dt = t - last;
        last = t;
        bool moving = anim_step(&lv, sel, dt);
        if (!*skip_timeout) {
            uint64_t ms = dt / 1000;
            if (ms >= *timeout_ms) {
                *selected = cur;
                return leave(&lv, sel, '\n');
            }
            *timeout_ms -= ms;
            last -= dt % 1000;
        }
        draw(&lv, sel, o, *skip_timeout ? 0 : *timeout_ms, full);
        full = false;

        uint64_t wait = moving ? FRAME_MS : (uint64_t)65535 * 1000;
        if (!*skip_timeout) {
            uint64_t to_tick = *timeout_ms % 1000 ? *timeout_ms % 1000 : 1000;
            if (to_tick < wait) wait = to_tick;
        }
        int c = pit_sleep_ms_and_quit_on_input(wait);
        if (tsc_freq == 0) (void)now_us(wait);
        if (c == 0) continue;
        if (c != GETCHAR_MOUSE) *skip_timeout = true;

        // Typing "cat" in the menu opens the game.
        typed[0] = typed[1];
        typed[1] = typed[2];
        typed[2] = c >= 'A' && c <= 'Z' ? (char)(c + 32) : c > 0 && c < 128 ? (char)c : 0;
        if (typed[0] == 'c' && typed[1] == 'a' && typed[2] == 't') {
            typed[2] = 0;
            *skip_timeout = true;
            game();
            anim_level(&lv, sel);
            an.fade = 0;
            full = true;
            last = now_us(0);
            continue;
        }

        size_t activate = (size_t)-1;
        switch (c) {
            case GETCHAR_CURSOR_LEFT:
            case GETCHAR_CURSOR_UP:
                if (lv.n) sel = sel ? sel - 1 : lv.n - 1;
                break;
            case GETCHAR_CURSOR_RIGHT:
            case GETCHAR_CURSOR_DOWN:
                if (lv.n) sel = sel + 1 < lv.n ? sel + 1 : 0;
                break;
            case GETCHAR_HOME:
                sel = 0;
                break;
            case GETCHAR_END:
                if (lv.n) sel = lv.n - 1;
                break;
            case '\n':
            case ' ':
                activate = sel;
                break;
            case '1': case '2': case '3': case '4': case '5':
            case '6': case '7': case '8': case '9': {
                size_t i = (size_t)(c - '1') + (lv.parent != NULL ? 1 : 0);
                if (i < lv.n) activate = i;
                break;
            }
            case GETCHAR_ESCAPE:
            case '\e':
            case '\b':
            case 127:
                if (lv.parent != NULL) activate = 0;
                break;
            case 'e': case 'E':
                if (o->editor && cur != NULL && cur->sub == NULL && !is_tool(cur)) {
                    *selected = cur;
                    return finish('e');
                }
                break;
            case 'b': case 'B':
                if (o->editor) return finish('b');
                break;
            case 's': case 'S':
                if (o->firmware) return leave(&lv, sel, 's');
                break;
            case 'u': case 'U':
                if (o->shell) return leave(&lv, sel, 'u');
                break;
            case GETCHAR_MOUSE: {
                struct mouse_state m;
                mouse_get_state(&m);
                int64_t gw = ctx->glyph_width, gh = ctx->glyph_height;
                size_t hit;
                if (m.wheel != 0 && lv.n) {
                    int64_t s = (int64_t)sel + m.wheel;
                    sel = s < 0 ? 0 : (s >= (int64_t)lv.n ? lv.n - 1 : (size_t)s);
                    *skip_timeout = true;
                } else if (m.click
                        && hit_tile(&lv, ctx->offset_x + m.click_x * gw + gw / 2,
                                    ctx->offset_y + m.click_y * gh + gh / 2, &hit)) {
                    activate = hit;
                    *skip_timeout = true;
                } else if (m.moved
                        && hit_tile(&lv, ctx->offset_x + m.x * gw + gw / 2,
                                    ctx->offset_y + m.y * gh + gh / 2, &hit)
                        && hit != sel) {
                    sel = hit;
                    *skip_timeout = true;
                } else {
                    mouse_render_pointer_overlay();
                }
                break;
            }
            default:
                break;
        }

        if (activate == (size_t)-1 || activate >= lv.n) {
            continue;
        }
        struct menu_entry *e = lv.items[activate];
        if (e == NULL) {
            struct menu_entry *from = lv.parent;
            build_level(&lv, from->parent);
            sel = level_index(&lv, from);
            anim_level(&lv, sel);
            an.row = an.label = 256;
        } else if (e == &fw_tile) {
            return leave(&lv, activate, 's');
        } else if (e == &shell_tile) {
            return leave(&lv, activate, 'u');
        } else if (e->sub != NULL) {
            build_level(&lv, e);
            sel = level_index(&lv, NULL);
            anim_level(&lv, sel);
            an.row = an.label = 256;
        } else {
            sel = activate;
            *selected = e;
            return leave(&lv, sel, '\n');
        }
    }
}

#define G_OBSTACLES 24

struct obstacle {
    bool used, byte, taken;
    int64_t x, y, w, h;
};

static void game(void) {
    static uint64_t best;
    int64_t ground = H * 2 / 3;
    int64_t size = 64 * U;
    int64_t px = W / 5;
    int64_t band0 = ground - 280 * U, band1 = ground + 24 * U;
    if (band0 < 0) band0 = 0;
    if (band1 > H) band1 = H;

    uint32_t *bg = ext_mem_alloc(back_size);
    clip_y0 = 0;
    clip_y1 = H;
    draw_backdrop(gterm_backdrop);
    fill_rect(0, 0, W, H, 0x000000, 170);
    memcpy(bg, back, back_size);
    present(0, H, 256);

    struct gmenu_image cat = { 0 };
    {
        char uri[512];
        int iw = 0, ih = 0;
        uint8_t *img = icon_uri("bleed-kernel", uri, sizeof(uri)) ? read_image(uri, &iw, &ih) : NULL;
        if (img != NULL) {
            scale_image(&cat, img, iw, ih, size);
            stbi_image_free(img);
        }
    }

    struct obstacle ob[G_OBSTACLES];
    int64_t y, vy, speed, dist, spawn;
    int jumps;
    uint64_t bytes, score = 0;
    bool reset = true;
    uint64_t last = now_us(0);

    srand((uint32_t)rdtsc());

    for (;;) {
        if (reset) {
            reset = false;
            memset(ob, 0, sizeof(ob));
            y = vy = dist = 0;
            jumps = 2;
            bytes = 0;
            speed = 330 * U * 256;
            spawn = 200 * U * 256;
        }

        uint64_t t = now_us(0);
        uint64_t dt = t - last;
        last = t;
        if (dt > 100000) dt = 100000;

        for (uint64_t left = dt; left > 0 && !reset;) {
            uint64_t s = left > 2000 ? 2000 : left;
            left -= s;
            vy -= (int64_t)(2600 * U * 256) * (int64_t)s / 1000000;
            y += vy * (int64_t)s / 1000000;
            if (y <= 0) { y = 0; vy = 0; jumps = 2; }

            int64_t move = speed * (int64_t)s / 1000000;
            dist += move;
            spawn -= move;
            speed += (int64_t)(10 * U * 256) * (int64_t)s / 1000000;
            if (speed > 820 * U * 256) speed = 820 * U * 256;

            for (int i = 0; i < G_OBSTACLES; i++) {
                if (!ob[i].used) continue;
                ob[i].x -= move;
                if (ob[i].x + ob[i].w < 0) ob[i].used = false;
            }

            if (spawn <= 0) {
                uint32_t r = rand32();
                int kind = r % 10;
                int64_t x = W * 256 + 40 * U * 256;
                for (int i = 0; i < G_OBSTACLES; i++) {
                    if (ob[i].used) continue;
                    ob[i] = (struct obstacle){ .used = true, .x = x };
                    if (kind < 6) {          // a bug to hop
                        ob[i].w = (int64_t)(26 + r / 16 % 30) * U * 256;
                        ob[i].h = (int64_t)(26 + r / 512 % 26) * U * 256;
                    } else if (kind < 8) {   // a tall one, needs both jumps
                        ob[i].w = 24 * U * 256;
                        ob[i].h = (int64_t)(118 + r / 16 % 22) * U * 256;
                    } else {                 // a byte to grab
                        ob[i].byte = true;
                        ob[i].w = ob[i].h = 16 * U * 256;
                        ob[i].y = (int64_t)(70 + r / 16 % 90) * U * 256;
                    }
                    break;
                }
                spawn = (int64_t)(300 + rand32() % 300) * U * 256 + speed / 3;
            }

            int64_t cx0 = (px - size / 2 + size / 5) * 256, cx1 = (px + size / 2 - size / 5) * 256;
            int64_t cy0 = y + size * 256 / 8, cy1 = y + size * 256 * 3 / 4;
            for (int i = 0; i < G_OBSTACLES; i++) {
                if (!ob[i].used || ob[i].taken) continue;
                if (ob[i].x >= cx1 || ob[i].x + ob[i].w <= cx0) continue;
                if (ob[i].y >= cy1 || ob[i].y + ob[i].h <= cy0) continue;
                if (ob[i].byte) {
                    ob[i].taken = true;
                    bytes++;
                } else {
                    reset = true;
                }
            }
            score = (uint64_t)(dist / (8 * U * 256)) + bytes * 25;
            if (score > best) best = score;
        }

        clip_y0 = band0;
        clip_y1 = band1;
        memcpy(&back[band0 * W], &bg[band0 * W], (size_t)((band1 - band0) * W) * sizeof(uint32_t));

        fill_rect(0, ground, W, 2 * U, 0xaa2020, 255);
        int64_t step = 48 * U, off = (dist / 256) % step;
        for (int64_t x = -off; x < W; x += step) {
            fill_rect(x, ground + 8 * U, 12 * U, 2 * U, 0x6a1a1a, 255);
        }

        for (int i = 0; i < G_OBSTACLES; i++) {
            if (!ob[i].used || ob[i].taken) continue;
            int64_t h = ob[i].h / 256;
            fill_rect(ob[i].x / 256, ground - ob[i].y / 256 - h, ob[i].w / 256, h,
                      ob[i].byte ? 0xffd166 : 0xd03030, 255);
        }

        int64_t cat_cy = ground - y / 256 - size / 2;
        if (cat.px != NULL) {
            draw_image(&cat, size, px, cat_cy, size, 255);
        } else {
            fill_rect(px - size / 2, cat_cy - size / 2, size, size, 0xaa2020, 255);
        }

        char line[64];
        {
            char *p = line;
            const char *parts[] = { "SCORE ", NULL, "   BEST ", NULL };
            uint64_t nums[] = { 0, score, 0, best };
            for (int i = 0; i < 4; i++) {
                if (parts[i] != NULL) {
                    for (const char *q = parts[i]; *q; q++) *p++ = *q;
                    continue;
                }
                char d[24];
                int nd = 0;
                uint64_t v = nums[i];
                do { d[nd++] = '0' + v % 10; v /= 10; } while (v);
                while (nd) *p++ = d[--nd];
            }
            *p = 0;
        }
        draw_text_n(W - 32 * U - text_width(line, U), band0 + 16 * U, line, (size_t)-1, U, 0xffffff);

        present(band0, band1, 256);

        // only wait out what is left of the frame, drawing it already took some
        uint64_t spent = (now_us(0) - t) / 1000;
        uint64_t wait = spent + 1 < FRAME_MS ? FRAME_MS - spent : 1;
        int c = pit_sleep_ms_and_quit_on_input(wait);
        if (tsc_freq == 0) (void)now_us(wait);
        if (c == GETCHAR_MOUSE) {
            struct mouse_state m;
            mouse_get_state(&m);
            continue;
        }
        if (c == GETCHAR_ESCAPE || c == '\e') {
            break;
        }
        if ((c == ' ' || c == GETCHAR_CURSOR_UP || c == '\n') && jumps > 0) {
            vy = (int64_t)(jumps == 2 ? 720 : 600) * U * 256;
            jumps--;
        }
    }

    if (cat.px != NULL) pmm_free(cat.px, cat.w * cat.h * sizeof(uint32_t));
    pmm_free(bg, back_size);
}

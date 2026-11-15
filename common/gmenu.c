#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <gmenu.h>
#include <menu.h>
#include <lib/config.h>
#include <lib/term.h>
#include <lib/gterm.h>
#include <lib/getchar.h>
#include <lib/uri.h>
#include <lib/libc.h>
#include <lib/misc.h>
#include <lib/print.h>
#include <fs/file.h>
#include <lib/stb_image.h>
#include <mm/pmm.h>
#include <drivers/mouse.h>
#include <flanterm.h>
#include <flanterm_backends/fb.h>

#define GMENU_MAX 128

struct gmenu_icon {
    uint32_t *px;
    size_t w, h;
};

static struct flanterm_fb_context *ctx;
static uint32_t *back;
static size_t back_size;
static int64_t W, H, U;

// Tile layout, in pixels: icon box, tile and gap between tiles.
#define ICON (96 * U)
#define TILE (ICON + 32 * U)
#define GAP (20 * U)

bool gmenu_usable(void) {
    if (quiet || serial || term_backend != GTERM || terms_i == 0) {
        return false;
    }
    char *style = config_get_value(NULL, 0, "INTERFACE_STYLE");
    if (style != NULL && strcmp(style, "classic") == 0) {
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
    int64_t x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int64_t x1 = x + w > W ? W : x + w, y1 = y + h > H ? H : y + h;
    for (int64_t py = y0; py < y1; py++) {
        for (int64_t px = x0; px < x1; px++) {
            blend_px(&back[py * W + px], rgb, a);
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
static void draw_text_n(int64_t x, int64_t y, const char *s, size_t max_chars, int64_t scale, uint32_t rgb) {
    int64_t off = scale > 1 ? scale / 2 : 1;
    for (int pass = 0; pass < 2; pass++) {
        int64_t cx = x;
        size_t n = 0;
        for (const char *p = s; *p && n < max_chars; p++) {
            uint8_t c = (uint8_t)*p;
            if ((c & 0xc0) == 0x80) continue;
            if (c & 0x80) c = '?';
            if (pass == 0) draw_glyph(cx + off, y + off, c, scale, 0, 140);
            else draw_glyph(cx, y, c, scale, rgb, 255);
            cx += ctx->font_width * scale;
            n++;
        }
    }
}

// Centred on cx, cut down with "..." to fit max_w.
static void draw_text_centred(int64_t cx, int64_t y, const char *s, int64_t scale, uint32_t rgb, int64_t max_w) {
    int64_t cw = ctx->font_width * scale;
    size_t n = text_chars(s);
    if ((int64_t)n * cw <= max_w) {
        draw_text_n(cx - (int64_t)n * cw / 2, y, s, n, scale, rgb);
        return;
    }
    size_t keep = max_w / cw > 3 ? max_w / cw - 3 : 0;
    int64_t x = cx - (int64_t)(keep + 3) * cw / 2;
    draw_text_n(x, y, s, keep, scale, rgb);
    draw_text_n(x + (int64_t)keep * cw, y, "...", 3, scale, rgb);
}

// Box filter down (nearest up) to fit size x size, premultiplied so edges don't fringe.
static struct gmenu_icon *scale_icon(const uint8_t *img, size_t iw, size_t ih, size_t size) {
    struct gmenu_icon *icon = ext_mem_alloc(sizeof(*icon));
    if (iw >= ih) {
        icon->w = size;
        icon->h = ih * size / iw ? ih * size / iw : 1;
    } else {
        icon->h = size;
        icon->w = iw * size / ih ? iw * size / ih : 1;
    }
    icon->px = ext_mem_alloc_counted(icon->w * icon->h, sizeof(uint32_t));
    for (size_t dy = 0; dy < icon->h; dy++) {
        size_t sy0 = dy * ih / icon->h, sy1 = (dy + 1) * ih / icon->h;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (size_t dx = 0; dx < icon->w; dx++) {
            size_t sx0 = dx * iw / icon->w, sx1 = (dx + 1) * iw / icon->w;
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
            uint32_t out = 0;
            if (a != 0) {
                out = (uint32_t)(a / n) << 24 | (uint32_t)(r / a) << 16 | (uint32_t)(g / a) << 8 | (uint32_t)(b / a);
            }
            icon->px[dy * icon->w + dx] = out;
        }
    }
    return icon;
}

static struct gmenu_icon *load_icon(char *uri) {
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

    int iw = 0, ih = 0, bpp;
    uint8_t *img = stbi_load_from_memory(f->fd, f->size, &iw, &ih, &bpp, 4);
    fclose(f);
    if (img == NULL || iw <= 0 || ih <= 0) {
        if (img != NULL) stbi_image_free(img);
        return NULL;
    }
    struct gmenu_icon *icon = scale_icon(img, iw, ih, ICON);
    stbi_image_free(img);
    return icon;
}

// A bare name like bleed-kernel is a built in icon, <config dir>/icons/<name>.png
// on the boot volume; anything with a ':' or '/' is a path.
static struct gmenu_icon *load_named_icon(const char *value) {
    char uri[512];
    size_t len = strlen(value);
    if (strchr(value, ':') != NULL || strchr(value, '/') != NULL) {
        if (len >= sizeof(uri)) return NULL;
        memcpy(uri, value, len + 1);
        return load_icon(uri);
    }
    for (const char *p = value; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) {
            return NULL;
        }
    }
    if (len == 0 || 7 + strlen(config_dir) + 7 + len + 5 >= sizeof(uri)) {
        return NULL;
    }
    char *p = uri;
    const char *parts[] = { "boot():", config_dir, "/icons/", value, ".png" };
    for (size_t i = 0; i < sizeof(parts) / sizeof(*parts); i++) {
        size_t n = strlen(parts[i]);
        memcpy(p, parts[i], n);
        p += n;
    }
    *p = 0;
    return load_icon(uri);
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

static void draw_icon(struct gmenu_icon *icon, int64_t x, int64_t y) {
    x += (ICON - (int64_t)icon->w) / 2;
    y += (ICON - (int64_t)icon->h) / 2;
    for (size_t iy = 0; iy < icon->h; iy++) {
        int64_t py = y + iy;
        if (py < 0 || py >= H) continue;
        for (size_t ix = 0; ix < icon->w; ix++) {
            int64_t px = x + ix;
            if (px < 0 || px >= W) continue;
            uint32_t p = icon->px[iy * icon->w + ix];
            if (p >> 24) blend_px(&back[py * W + px], p & 0xffffff, p >> 24);
        }
    }
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

struct layout {
    size_t first, vis;
    int64_t row_x, row_y;
};

static void get_layout(struct level *lv, size_t sel, struct layout *l) {
    l->vis = (W - 96 * U + GAP) / (TILE + GAP);
    if (l->vis == 0) l->vis = 1;
    if (l->vis > lv->n) l->vis = lv->n;
    l->first = sel > l->vis / 2 ? sel - l->vis / 2 : 0;
    if (l->first + l->vis > lv->n) l->first = lv->n - l->vis;
    l->row_x = (W - ((int64_t)l->vis * TILE + ((int64_t)l->vis - 1) * GAP)) / 2;
    l->row_y = H * 9 / 20 - TILE / 2;
}

static bool hit_tile(struct level *lv, size_t sel, int64_t px, int64_t py, size_t *out) {
    struct layout l;
    get_layout(lv, sel, &l);
    if (py < l.row_y || py >= l.row_y + TILE || px < l.row_x) return false;
    int64_t i = (px - l.row_x) / (TILE + GAP);
    if (i >= (int64_t)l.vis || (px - l.row_x) % (TILE + GAP) >= TILE) return false;
    *out = l.first + i;
    return true;
}

static void present(void) {
    mouse_erase_pointer();
    size_t stride = ctx->pitch / 4;
    bool native = ctx->red_mask_shift == 16 && ctx->green_mask_shift == 8 && ctx->blue_mask_shift == 0
               && ctx->red_mask_size == 8 && ctx->green_mask_size == 8 && ctx->blue_mask_size == 8;
    for (int64_t y = 0; y < H; y++) {
        volatile uint32_t *row = ctx->framebuffer + y * stride;
        uint32_t *src = &back[y * W];
        for (int64_t x = 0; x < W; x++) {
            uint32_t p = src[x];
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
        ctx->flush_callback(ctx->framebuffer, ctx->pitch * H);
    }
    mouse_render_pointer_overlay();
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

static void draw(struct level *lv, size_t sel, const struct gmenu_opts *o, uint64_t timeout_ms) {
    if (gterm_backdrop != NULL) {
        memcpy(back, gterm_backdrop, back_size);
    } else {
        for (int64_t y = 0; y < H; y++) {
            uint32_t t = (uint32_t)(y * 255 / H);
            uint32_t rgb = ((0x1e - (0x1e - 0x0b) * t / 255) << 16)
                         | ((0x24 - (0x24 - 0x0d) * t / 255) << 8)
                         | (0x30 - (0x30 - 0x12) * t / 255);
            for (int64_t x = 0; x < W; x++) back[y * W + x] = rgb;
        }
    }

    int64_t fh = ctx->font_height;

    if (o->branding[0] != '\0') {
        draw_text_centred(W / 2, 28 * U, o->branding, U, o->branding_rgb, W - 32 * U);
    }

    if (lv->n == 0) {
        const char *msg = o->config_ready ? "The config file contains no valid entries" : "Config file not found";
        draw_text_centred(W / 2, H / 2 - fh * U, msg, U, 0xffffff, W - 32 * U);
    } else {
        struct layout l;
        get_layout(lv, sel, &l);

        if (lv->parent != NULL) {
            draw_text_centred(W / 2, l.row_y - fh * U - 24 * U, lv->parent->name, U, 0xc8c8c8, W - 64 * U);
        }

        for (size_t i = l.first; i < l.first + l.vis; i++) {
            int64_t tx = l.row_x + (int64_t)(i - l.first) * (TILE + GAP);
            if (i == sel) {
                fill_rect(tx, l.row_y, TILE, TILE, 0xffffff, 52);
            }
            int64_t ix = tx + (TILE - ICON) / 2, iy = l.row_y + (TILE - ICON) / 2;
            struct menu_entry *e = lv->items[i];
            struct gmenu_icon *icon = e == NULL ? builtin_icon("back", &back_icon, &back_tried)
                                    : e == &fw_tile ? builtin_icon("firmware", &fw_icon, &fw_tried)
                                    : e == &shell_tile ? builtin_icon("efi", &efi_icon, &efi_tried)
                                    : entry_icon(e);
            if (icon != NULL) {
                draw_icon(icon, ix, iy);
            }
        }

        int64_t arrow_y = l.row_y + (TILE - fh * U * 2) / 2;
        if (l.first > 0) {
            draw_glyph(l.row_x - 40 * U, arrow_y, 0x11, 2 * U, 0xffffff, 200);
        }
        if (l.first + l.vis < lv->n) {
            draw_glyph(l.row_x + (int64_t)l.vis * (TILE + GAP) - GAP + 24 * U, arrow_y, 0x10, 2 * U, 0xffffff, 200);
        }

        struct menu_entry *e = lv->items[sel];
        int64_t label_y = l.row_y + TILE + 56 * U;
        draw_text_centred(W / 2, label_y, e ? e->name : "Back", U, 0xffffff, W - 64 * U);
        if (e != NULL && !is_tool(e)) {
            const char *comment = config_get_value(e->body, 0, "COMMENT");
            if (comment != NULL) {
                draw_text_centred(W / 2, label_y + fh * U + 8 * U, comment, U, 0xc8c8c8, W - 64 * U);
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

    if (!help_hidden) {
        struct hint hints[6];
        size_t n = 0;
        if (lv->n != 0) hints[n++] = (struct hint){ "ENTER", "Boot" };
        if (o->editor && lv->n != 0 && lv->items[sel] != NULL && !is_tool(lv->items[sel])) hints[n++] = (struct hint){ "E", "Edit" };
        if (o->editor) hints[n++] = (struct hint){ "B", "Blank" };
        if (lv->parent != NULL) hints[n++] = (struct hint){ "ESC", "Back" };
        draw_hints(H - 24 * U - fh * U, hints, n, o->help_rgb);
    }

    present();
}

static int finish(int action) {
    pmm_free(back, back_size);
    back = NULL;
    // flanterm still thinks its old cells are on screen, so repaint them all.
    print("\e[2J\e[H");
    FOR_TERM(flanterm_full_refresh(TERM));
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

    for (;;) {
        struct menu_entry *cur = lv.n ? lv.items[sel] : NULL;
        if (cur == NULL || cur->sub != NULL || is_tool(cur)) {
            *skip_timeout = true;
        }
        draw(&lv, sel, o, *skip_timeout ? 0 : *timeout_ms);

        int c;
        if (!*skip_timeout && *timeout_ms != 0) {
            uint64_t sleep_ms = *timeout_ms % 1000;
            if (sleep_ms == 0) sleep_ms = 1000;
            c = pit_sleep_ms_and_quit_on_input(sleep_ms);
            if (c == 0) {
                *timeout_ms -= sleep_ms;
                if (*timeout_ms == 0) {
                    *selected = cur;
                    return finish('\n');
                }
                continue;
            }
            *skip_timeout = true;
        } else {
            c = pit_sleep_ms_and_quit_on_input((uint64_t)65535 * 1000);
            if (c == 0) continue;
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
                if (o->firmware) return finish('s');
                break;
            case 'u': case 'U':
                if (o->shell) return finish('u');
                break;
            case GETCHAR_MOUSE: {
                struct mouse_state m;
                mouse_get_state(&m);
                int64_t gw = ctx->glyph_width, gh = ctx->glyph_height;
                size_t hit;
                if (m.wheel != 0 && lv.n) {
                    int64_t s = (int64_t)sel + m.wheel;
                    sel = s < 0 ? 0 : (s >= (int64_t)lv.n ? lv.n - 1 : (size_t)s);
                } else if (m.click
                        && hit_tile(&lv, sel, ctx->offset_x + m.click_x * gw + gw / 2,
                                    ctx->offset_y + m.click_y * gh + gh / 2, &hit)) {
                    activate = hit;
                } else if (m.moved
                        && hit_tile(&lv, sel, ctx->offset_x + m.x * gw + gw / 2,
                                    ctx->offset_y + m.y * gh + gh / 2, &hit)
                        && hit != sel) {
                    sel = hit;
                } else {
                    mouse_render_pointer_overlay();
                    continue;
                }
                break;
            }
            default:
                continue;
        }

        if (activate == (size_t)-1 || activate >= lv.n) {
            continue;
        }
        struct menu_entry *e = lv.items[activate];
        if (e == NULL) {
            struct menu_entry *from = lv.parent;
            build_level(&lv, from->parent);
            sel = level_index(&lv, from);
        } else if (e == &fw_tile) {
            return finish('s');
        } else if (e == &shell_tile) {
            return finish('u');
        } else if (e->sub != NULL) {
            build_level(&lv, e);
            sel = level_index(&lv, NULL);
        } else {
            *selected = e;
            return finish('\n');
        }
    }
}

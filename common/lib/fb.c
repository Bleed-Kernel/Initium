#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <lib/fb.h>
#include <lib/fb_quirks.h>
#include <lib/misc.h>
#include <drivers/vbe.h>
#include <drivers/gop.h>
#include <mm/pmm.h>
#include <mm/mtrr.h>
#include <mm/efi_pt.h>
#include <sys/cpu.h>
#include <lib/bgrt.h>

struct fb_info *fb_fbs;
size_t fb_fbs_count = 0;

void fb_init(struct fb_info **ret, size_t *_fbs_count,
             uint64_t target_width, uint64_t target_height, uint16_t target_bpp,
             bool preserve_screen, bool keep_wc, bool corrected) {
    if (quiet) {
        preserve_screen = true;
    }

#if defined(BIOS)
    *ret = ext_mem_alloc(sizeof(struct fb_info));
    if (init_vbe(*ret, target_width, target_height, target_bpp)) {
        *_fbs_count = 1;

        (*ret)->edid = get_edid_info();
        size_t mode_count;
        (*ret)->mode_list = vbe_get_mode_list(&mode_count);
        (*ret)->mode_count = mode_count;
    } else {
        *_fbs_count = 0;
        pmm_free(*ret, sizeof(struct fb_info));
        *ret = NULL;
    }
#elif defined(UEFI)
    init_gop(ret, _fbs_count, target_width, target_height, target_bpp);
#endif

    fb_fbs = *ret;
    fb_fbs_count = *_fbs_count;

    // Initium draws by a corrected copy, the caller keeping the firmware's
    // description. The copy goes without the modes, whose list it would share.
    if (!corrected && fb_fbs_count != 0) {
        fb_fbs = ext_mem_alloc_counted(fb_fbs_count, sizeof(struct fb_info));
        for (size_t i = 0; i < fb_fbs_count; i++) {
            fb_fbs[i] = (*ret)[i];
            fb_fbs[i].mode_count = 0;
            fb_fbs[i].mode_list = NULL;
        }
    }

    fb_quirks_apply(fb_fbs, fb_fbs_count);

    // Map the framebuffers as write-combining so the clear (and, when kept,
    // terminal rendering) is fast. keep_wc leaves it active for the caller.
    bool want_wc = keep_wc || !preserve_screen;

    if (want_wc) {
        for (size_t i = 0; i < *_fbs_count; i++) {
            uint64_t fb_size = CHECKED_MUL((uint64_t)fb_fbs[i].framebuffer_pitch,
                                           (uint64_t)fb_fbs[i].framebuffer_height, continue);
            if (fb_size == 0) {
                continue;
            }
#if defined(__x86_64__) && defined(UEFI)
            efi_pt_set_fb_wc(fb_fbs[i].framebuffer_addr, fb_size);
#else
            mtrr_wc_add_fb_range(fb_fbs[i].framebuffer_addr, fb_size);
#endif
        }
    }

    if (!preserve_screen) {
        for (size_t i = 0; i < *_fbs_count; i++) {
            fb_clear(&fb_fbs[i]);
        }
    }

    if (want_wc && !keep_wc) {
#if defined(__x86_64__) && defined(UEFI)
        efi_pt_restore();
#else
        mtrr_restore();
#endif
    }

#if defined(UEFI)
    if (!preserve_screen && *_fbs_count > 0) {
        bgrt_restore(fb_fbs[0].framebuffer_width, fb_fbs[0].framebuffer_height);
    }
#endif
}

void fb_clear(struct fb_info *fb) {
    for (size_t y = 0; y < fb->framebuffer_height; y++) {
        switch (fb->framebuffer_bpp) {
            case 32: {
                uint32_t *fbp = (void *)(uintptr_t)fb->framebuffer_addr;
                size_t row = (y * fb->framebuffer_pitch) / 4;
                for (size_t x = 0; x < fb->framebuffer_width; x++) {
                    fbp[row + x] = 0;
                }
                break;
            }
            case 16: {
                uint16_t *fbp = (void *)(uintptr_t)fb->framebuffer_addr;
                size_t row = (y * fb->framebuffer_pitch) / 2;
                for (size_t x = 0; x < fb->framebuffer_width; x++) {
                    fbp[row + x] = 0;
                }
                break;
            }
            default: {
                uint8_t *fbp = (void *)(uintptr_t)fb->framebuffer_addr;
                size_t row = y * fb->framebuffer_pitch;
                size_t row_bytes = fb->framebuffer_width * (fb->framebuffer_bpp / 8);
                for (size_t x = 0; x < row_bytes; x++) {
                    fbp[row + x] = 0;
                }
                break;
            }
        }
    }

    fb_flush((volatile void *)(uintptr_t)fb->framebuffer_addr,
             (size_t)fb->framebuffer_pitch * fb->framebuffer_height);
}


bool fb_flush_reliable(void) {
    static bool probed = false;
    static bool reliable = true;

    if (!probed) {
        probed = true;
    }

    return reliable;
}

bool fb_flush(volatile void *base, size_t length) {
    typedef bool (*flush_fn)(volatile void *, size_t);
    static flush_fn fn = NULL;
    static bool probed = false;

    if (!probed) {
        probed = true;
    }

    if (fn != NULL) {
        return fn(base, length);
    }

    // Coherent by construction, or with no way to get there.
    return fb_flush_reliable();
}

void fb_flush_cb(volatile void *base, size_t length) {
    (void)fb_flush(base, length);
}

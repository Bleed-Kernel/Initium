#ifndef GMENU_H
#define GMENU_H

#include <stdint.h>
#include <stdbool.h>
#include <lib/config.h>

// The graphical icon menu, drawn straight onto the first terminal's framebuffer.
struct gmenu_opts {
    const char *branding;
    uint32_t branding_rgb;
    uint32_t help_rgb;
    bool config_ready;
    bool editor;
    bool firmware;
    bool shell;
};

bool gmenu_usable(void);

int gmenu_run(struct menu_entry **selected, uint64_t *timeout_ms, bool *skip_timeout,
              const struct gmenu_opts *opts);

#endif

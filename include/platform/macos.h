#ifndef MACOS_H
#define MACOS_H

#include "config/config.h"
#include "core/bongocat.h"
#include "utils/error.h"

#include <signal.h>

BONGOCAT_NODISCARD bongocat_error_t macos_app_init(config_t *config);
BONGOCAT_NODISCARD bongocat_error_t
macos_app_run(volatile sig_atomic_t *running);
void macos_app_update_config(config_t *config);
void macos_app_toggle_visible(void);
void macos_app_set_tick_callback(void (*callback)(void));
void macos_app_cleanup(void);
BONGOCAT_NODISCARD int macos_phys_dim(int logical);
void draw_bar(void);

#endif  // MACOS_H

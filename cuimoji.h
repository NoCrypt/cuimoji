#ifndef CUIMOJI_H
#define CUIMOJI_H

#include <cui/cui.h>
#include <stdbool.h>
#include <signal.h>

#define CUIMOJI_WIN_W 360
#define CUIMOJI_WIN_H 480
#define CUIMOJI_MIN_W 240
#define CUIMOJI_MIN_H 240
#define CUIMOJI_MAX_W 770
#define CUIMOJI_MAX_H 770
#define CUIMOJI_VERSION "0.3"

/* Shared between the X11 app loop, the SDL2 software loop and the SDL2
 * Vulkan loop. */
extern bool cuimoji_quit_requested;
extern volatile sig_atomic_t cuimoji_toggle_requested;
extern void (*cuimoji_hide_window)(void);

/* Last window geometry, persisted in the state file. Width/height are 0 when
 * unset; the position is only meaningful on X11 (Wayland decides placement). */
extern int cuimoji_win_w, cuimoji_win_h, cuimoji_win_x, cuimoji_win_y;
extern bool cuimoji_win_pos_valid;
/* UI size: the logical->physical zoom used by the SDL backends. */
extern float cuimoji_ui_zoom;

void cuimoji_state_load(void);
void cuimoji_state_save(void);
void cuimoji_fonts_scan(void);
void cuimoji_font_apply(cui_ctx *ui);
cui_theme_mode cuimoji_theme(void);
void cuimoji_ui(cui_ctx *ui);

#ifdef CUIMOJI_HAVE_SDL2
/* Wayland-native window through SDL2, drawing with the software renderer. */
int cuimoji_run_sdl(void);
#endif

#ifdef CUIMOJI_HAVE_VULKAN
int cuimoji_run_vulkan(void);
#endif

#endif

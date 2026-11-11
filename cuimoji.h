#ifndef CUIMOJI_H
#define CUIMOJI_H

#include <cui/cui.h>
#include <stdbool.h>

#define CUIMOJI_WIN_W 360
#define CUIMOJI_WIN_H 480

/* Shared between the X11 app loop, the SDL2 software loop and the SDL2
 * Vulkan loop. */
extern bool cuimoji_quit_requested;
extern void (*cuimoji_hide_window)(void);

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

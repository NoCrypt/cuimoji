#ifndef CUIMOJI_H
#define CUIMOJI_H

#include <cui/cui.h>
#include <stdbool.h>

/* Shared between the CUI/X11 app loop and the SDL2/Vulkan loop. */
extern bool cuimoji_quit_requested;
extern void (*cuimoji_hide_window)(void);

void cuimoji_state_load(void);
void cuimoji_state_save(void);
void cuimoji_fonts_scan(void);
void cuimoji_font_apply(cui_ctx *ui);
void cuimoji_ui(cui_ctx *ui);

#ifdef CUIMOJI_HAVE_VULKAN
int cuimoji_run_vulkan(void);
#endif

#endif

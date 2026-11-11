#ifndef CUIMOJI_SDL_COMMON_H
#define CUIMOJI_SDL_COMMON_H

#ifdef CUIMOJI_HAVE_SDL2
#include <SDL2/SDL.h>
#include <cui/cui.h>

/* Polls SDL events into CUI. When wait_ms > 0, blocks until an event arrives
 * or the timeout passes (idle costs no CPU). Returns false once the window
 * should close; *woke is set when an event was handled. */
bool cuimoji_sdl_pump(cui_ctx *ui, int wait_ms, bool *woke);

#endif
#endif

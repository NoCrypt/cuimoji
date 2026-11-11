#ifndef CUIMOJI_SDL_COMMON_H
#define CUIMOJI_SDL_COMMON_H

#ifdef CUIMOJI_HAVE_SDL2
#include <SDL2/SDL.h>
#include <cui/cui.h>

/* Polls SDL events into CUI. Returns false once the window should close. */
bool cuimoji_sdl_pump(cui_ctx *ui);

#endif
#endif

/* SDL2 frontend in software mode: a Wayland-native window (SDL2 wayland
 * driver) drawing CUI's software renderer into a streaming texture. This is
 * the Wayland path; the X11 window in main.c is the fallback. */
#ifdef CUIMOJI_HAVE_SDL2
#include <SDL2/SDL.h>
#include <cui/cui.h>
#include <cui/cui_render.h>

#include "cuimoji.h"
#include "sdl_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SDL_Window *sdl_win;
static void sdl_hide(void) {
    if (sdl_win) {
        SDL_HideWindow(sdl_win);
        SDL_PumpEvents();
    }
}

int cuimoji_run_sdl(void) {
    if (getenv("WAYLAND_DISPLAY")) setenv("SDL_VIDEODRIVER", "wayland", 0);
    SDL_SetHint(SDL_HINT_APP_NAME, "cuimoji");
    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
    /* no decorations (borderless window) and no GPU surfaces for this UI */
    SDL_SetHint("SDL_VIDEO_WAYLAND_ALLOW_LIBDECOR", "0");
    SDL_SetHint("SDL_VIDEO_WAYLAND_PREFER_LIBDECOR", "0");
    SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "0");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "cuimoji: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    sdl_win = SDL_CreateWindow("cuimoji", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               CUIMOJI_WIN_W, CUIMOJI_WIN_H,
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!sdl_win) {
        fprintf(stderr, "cuimoji: SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    /* Software only: an accelerated SDL renderer opens an EGL context and
     * maps the whole GPU driver, which costs tens of MB for this UI. */
    SDL_SetWindowBordered(sdl_win, SDL_FALSE);
    SDL_Renderer *ren = SDL_CreateRenderer(sdl_win, -1, SDL_RENDERER_SOFTWARE | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(sdl_win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) {
        fprintf(stderr, "cuimoji: SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(sdl_win);
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderDrawColor(ren, 13, 17, 23, 255);

    cui_sw_renderer *sw = cui_sw_create();
    if (!sw) {
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(sdl_win);
        SDL_Quit();
        return 1;
    }
    cui_ctx *ui = cui_create(&(cui_ctx_desc){
        .renderer = cui_sw_iface(sw),
        .theme = cuimoji_theme(),
        .accent = CUI_ACCENT_NEUTRAL,
    });
    if (!ui) {
        cui_sw_destroy(sw);
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(sdl_win);
        SDL_Quit();
        return 1;
    }

    cuimoji_font_apply(ui);
    cuimoji_hide_window = sdl_hide;
    cuimoji_quit_requested = false;
    SDL_StartTextInput();

    unsigned char *pixels = NULL;
    int pix_w = 0, pix_h = 0;
    SDL_Texture *tex = NULL;
    int tex_w = 0, tex_h = 0;

    uint64_t freq = SDL_GetPerformanceFrequency();
    uint64_t last = SDL_GetPerformanceCounter();

    while (!cuimoji_quit_requested) {
        if (!cuimoji_sdl_pump(ui)) break;

        int ww = 0, wh = 0, pw = 0, ph = 0;
        SDL_GetWindowSize(sdl_win, &ww, &wh);
        SDL_GetRendererOutputSize(ren, &pw, &ph);
        if (ww <= 0 || wh <= 0 || pw <= 0 || ph <= 0) { SDL_Delay(8); continue; }

        if (pw != pix_w || ph != pix_h) {
            unsigned char *n = realloc(pixels, (size_t)pw * (size_t)ph * 4);
            if (!n) { SDL_Delay(8); continue; }
            pixels = n;
            pix_w = pw;
            pix_h = ph;
        }
        /* Create the new texture before dropping the old one, and keep
         * presenting the previous frame (scaled by the compositor) while
         * there is no texture for the current size: no blank frame. */
        if (tex_w != pw || tex_h != ph) {
            SDL_Texture *nt = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888,
                                                SDL_TEXTUREACCESS_STREAMING, pw, ph);
            if (!nt) { SDL_Delay(8); continue; }
            if (tex) SDL_DestroyTexture(tex);
            tex = nt;
            tex_w = pw;
            tex_h = ph;
        }
        if (!tex) { SDL_Delay(8); continue; }

        float scale = ww > 0 ? (float)pw / (float)ww : 1.0f;
        cui_set_viewport(ui, (float)ww, (float)wh, scale);

        uint64_t now = SDL_GetPerformanceCounter();
        float dt = (float)((double)(now - last) / (double)freq);
        last = now;
        if (dt <= 0 || dt > 0.25f) dt = 1.0f / 120.0f;

        cui_begin_frame(ui, dt);
        cuimoji_ui(ui);
        cui_end_frame(ui);
        if (cuimoji_quit_requested) break;

        const cui_draw_data *dd = cui_get_draw_data(ui);
        cui_sw_render(sw, dd, pixels, pw, ph, pw * 4, CUI_SW_RGBA);

        SDL_UpdateTexture(tex, NULL, pixels, pw * 4);
        /* The copy covers the whole drawable; clearing first only adds a
         * dark flash between resize frames. */
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
    }

    SDL_StopTextInput();
    free(pixels);
    if (tex) SDL_DestroyTexture(tex);
    cui_destroy(ui);
    cui_sw_destroy(sw);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(sdl_win);
    SDL_Quit();
    return 0;
}

#endif

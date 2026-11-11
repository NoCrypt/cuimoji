/* SDL2 + Vulkan frontend, using the small renderer scaffold from
 * cui-hello/vulkan. The same CUI UI and paste path as the X11 frontend. */
#ifdef CUIMOJI_HAVE_VULKAN
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <cui/cui.h>

#include "cui_vulkan.h"
#include "cuimoji.h"
#include "sdl_common.h"

#include <stdio.h>
#include <stdlib.h>

static SDL_Window *vk_win;
static void sdl_hide(void) {
    if (vk_win) {
        SDL_HideWindow(vk_win);
        SDL_PumpEvents();
    }
}

int cuimoji_run_vulkan(void) {
    if (!getenv("SDL_VIDEODRIVER") && getenv("WAYLAND_DISPLAY")) setenv("SDL_VIDEODRIVER", "wayland", 0);
    SDL_SetHint(SDL_HINT_APP_NAME, "cuimoji");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "cuimoji: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    vk_win = SDL_CreateWindow("cuimoji", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              CUIMOJI_WIN_W, CUIMOJI_WIN_H,
                              SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!vk_win) {
        fprintf(stderr, "cuimoji: SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    cui_vk *vk = cui_vk_create(vk_win);
    if (!vk) {
        SDL_DestroyWindow(vk_win);
        SDL_Quit();
        return 1;
    }
    cui_renderer_iface iface = cui_vk_iface(vk);
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = iface, .theme = cuimoji_theme(),
                                              .accent = CUI_ACCENT_NEUTRAL });
    if (!ui) {
        cui_vk_destroy(vk);
        SDL_DestroyWindow(vk_win);
        SDL_Quit();
        return 1;
    }

    cuimoji_font_apply(ui);
    cuimoji_hide_window = sdl_hide;
    cuimoji_quit_requested = false;

    int last_pw = -1, last_ph = -1;
    uint64_t freq = SDL_GetPerformanceFrequency();
    uint64_t last = SDL_GetPerformanceCounter();

    while (!cuimoji_quit_requested && !cuimoji_toggle_requested) {
        if (!cuimoji_sdl_pump(ui)) break;

        int ww = 0, wh = 0, pw = 0, ph = 0;
        SDL_GetWindowSize(vk_win, &ww, &wh);
        SDL_Vulkan_GetDrawableSize(vk_win, &pw, &ph);
        if (pw <= 0 || ph <= 0) { SDL_Delay(16); continue; }
        if (pw != last_pw || ph != last_ph) {
            last_pw = pw;
            last_ph = ph;
            cui_vk_resize(vk);
        }

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
        if (!cui_vk_render(vk, dd)) cui_vk_render(vk, dd);
    }

    cui_destroy(ui);
    cui_vk_destroy(vk);
    SDL_DestroyWindow(vk_win);
    SDL_Quit();
    return 0;
}

#endif

/* SDL2 + Vulkan frontend, using the small renderer scaffold from
 * cui-hello/vulkan. The same CUI UI and paste path as the X11 frontend. */
#ifdef CUIMOJI_HAVE_VULKAN
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <cui/cui.h>

#include "cui_vulkan.h"
#include "cuimoji.h"

#include <stdio.h>
#include <stdlib.h>

static cui_key map_key(SDL_Keycode k) {
    if (k >= SDLK_a && k <= SDLK_z) return (cui_key)(CUI_KEY_A + (k - SDLK_a));
    if (k >= SDLK_0 && k <= SDLK_9) return (cui_key)(CUI_KEY_0 + (k - SDLK_0));
    if (k >= SDLK_F1 && k <= SDLK_F12) return (cui_key)(CUI_KEY_F1 + (k - SDLK_F1));
    switch (k) {
    case SDLK_TAB: return CUI_KEY_TAB;
    case SDLK_RETURN: case SDLK_KP_ENTER: return CUI_KEY_ENTER;
    case SDLK_ESCAPE: return CUI_KEY_ESCAPE;
    case SDLK_BACKSPACE: return CUI_KEY_BACKSPACE;
    case SDLK_DELETE: return CUI_KEY_DELETE;
    case SDLK_SPACE: return CUI_KEY_SPACE;
    case SDLK_LEFT: return CUI_KEY_LEFT;
    case SDLK_RIGHT: return CUI_KEY_RIGHT;
    case SDLK_UP: return CUI_KEY_UP;
    case SDLK_DOWN: return CUI_KEY_DOWN;
    case SDLK_HOME: return CUI_KEY_HOME;
    case SDLK_END: return CUI_KEY_END;
    case SDLK_PAGEUP: return CUI_KEY_PAGE_UP;
    case SDLK_PAGEDOWN: return CUI_KEY_PAGE_DOWN;
    case SDLK_MINUS: return CUI_KEY_MINUS;
    case SDLK_EQUALS: return CUI_KEY_EQUAL;
    case SDLK_COMMA: return CUI_KEY_COMMA;
    case SDLK_PERIOD: return CUI_KEY_PERIOD;
    case SDLK_SLASH: return CUI_KEY_SLASH;
    default: return CUI_KEY_NONE;
    }
}

static int mods(void) {
    SDL_Keymod m = SDL_GetModState();
    int r = 0;
    if (m & KMOD_SHIFT) r |= CUI_MOD_SHIFT;
    if (m & KMOD_CTRL) r |= CUI_MOD_CTRL;
    if (m & KMOD_ALT) r |= CUI_MOD_ALT;
    if (m & KMOD_GUI) r |= CUI_MOD_SUPER;
    return r;
}

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

    vk_win = SDL_CreateWindow("cuimoji", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 520, 720,
                              SDL_WINDOW_VULKAN | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!vk_win) {
        fprintf(stderr, "cuimoji: SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_SetWindowBordered(vk_win, SDL_FALSE);
    SDL_SetWindowResizable(vk_win, SDL_TRUE);

    cui_vk *vk = cui_vk_create(vk_win);
    if (!vk) {
        SDL_DestroyWindow(vk_win);
        SDL_Quit();
        return 1;
    }
    cui_renderer_iface iface = cui_vk_iface(vk);
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = iface, .theme = CUI_THEME_DARK,
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

    bool run = true;
    int last_pw = -1, last_ph = -1;
    uint64_t freq = SDL_GetPerformanceFrequency();
    uint64_t last = SDL_GetPerformanceCounter();

    while (run && !cuimoji_quit_requested) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT: run = false; break;
            case SDL_WINDOWEVENT:
                switch (e.window.event) {
                case SDL_WINDOWEVENT_LEAVE: cui_input_mouse_leave(ui); break;
                case SDL_WINDOWEVENT_FOCUS_LOST: cui_input_focus(ui, false); break;
                case SDL_WINDOWEVENT_FOCUS_GAINED: cui_input_focus(ui, true); break;
                }
                break;
            case SDL_MOUSEMOTION:
                cui_input_mouse_move(ui, (float)e.motion.x, (float)e.motion.y);
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP: {
                cui_mouse_button b = e.button.button == SDL_BUTTON_RIGHT ? CUI_MOUSE_RIGHT
                                    : e.button.button == SDL_BUTTON_MIDDLE ? CUI_MOUSE_MIDDLE
                                    : CUI_MOUSE_LEFT;
                cui_input_mouse_move(ui, (float)e.button.x, (float)e.button.y);
                cui_input_mouse_button(ui, b, e.type == SDL_MOUSEBUTTONDOWN);
                break;
            }
            case SDL_MOUSEWHEEL:
                cui_input_scroll(ui, (float)e.wheel.x, (float)e.wheel.y);
                break;
            case SDL_KEYDOWN:
            case SDL_KEYUP: {
                cui_key k = map_key(e.key.keysym.sym);
                if (k != CUI_KEY_NONE) cui_input_key(ui, k, e.type == SDL_KEYDOWN, mods());
                break;
            }
            case SDL_TEXTINPUT:
                cui_input_text(ui, e.text.text);
                break;
            }
        }
        if (!run || cuimoji_quit_requested) break;

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

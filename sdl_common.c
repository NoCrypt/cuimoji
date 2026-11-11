/* Event translation shared by the SDL2 frontends (software and Vulkan). */
#ifdef CUIMOJI_HAVE_SDL2
#include <SDL2/SDL.h>
#include <cui/cui.h>

#include "cuimoji.h"
#include "sdl_common.h"

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

static bool handle_event(cui_ctx *ui, const SDL_Event *ev) {
    const SDL_Event e = *ev;
    switch (e.type) {
    case SDL_QUIT:
        cuimoji_quit_requested = true;
        return true;
    case SDL_WINDOWEVENT:
        switch (e.window.event) {
        case SDL_WINDOWEVENT_LEAVE: cui_input_mouse_leave(ui); return true;
        case SDL_WINDOWEVENT_FOCUS_LOST: cui_input_focus(ui, false); return true;
        case SDL_WINDOWEVENT_FOCUS_GAINED: cui_input_focus(ui, true); return true;
        /* SDL acks an interactive resize on the next frame callback, so an
         * exposure must produce a commit or the resize stalls. */
        case SDL_WINDOWEVENT_EXPOSED:
        case SDL_WINDOWEVENT_RESIZED:
        case SDL_WINDOWEVENT_SIZE_CHANGED: return true;
        }
        return false;
    case SDL_MOUSEMOTION:
        cui_input_mouse_move(ui, (float)e.motion.x, (float)e.motion.y);
        return true;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
        cui_mouse_button b = e.button.button == SDL_BUTTON_RIGHT ? CUI_MOUSE_RIGHT
                            : e.button.button == SDL_BUTTON_MIDDLE ? CUI_MOUSE_MIDDLE
                            : CUI_MOUSE_LEFT;
        cui_input_mouse_move(ui, (float)e.button.x, (float)e.button.y);
        cui_input_mouse_button(ui, b, e.type == SDL_MOUSEBUTTONDOWN);
        return true;
    }
    case SDL_MOUSEWHEEL:
        cui_input_scroll(ui, (float)e.wheel.x, (float)e.wheel.y);
        return true;
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        cui_key k = map_key(e.key.keysym.sym);
        if (k != CUI_KEY_NONE) cui_input_key(ui, k, e.type == SDL_KEYDOWN, mods());
        return true;
    }
    case SDL_TEXTINPUT:
        cui_input_text(ui, e.text.text);
        return true;
    }
    return false;
}

bool cuimoji_sdl_pump(cui_ctx *ui, int wait_ms, bool *woke) {
    if (woke) *woke = false;
    SDL_Event e;
    if (wait_ms > 0) {
        if (SDL_WaitEventTimeout(&e, wait_ms) && handle_event(ui, &e) && woke) *woke = true;
    }
    while (SDL_PollEvent(&e))
        if (handle_event(ui, &e) && woke) *woke = true;
    return !cuimoji_quit_requested && !cuimoji_toggle_requested;
}

#endif

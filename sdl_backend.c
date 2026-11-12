#define _GNU_SOURCE
/* SDL2 frontend: a Wayland-native window (SDL2 wayland driver) with CUI's
 * software renderer drawing into wl_shm buffers attached to SDL's own
 * wl_surface, double buffered. SDL3's software renderer would present via a
 * GPU texture framebuffer and map the whole EGL/NVIDIA stack for this
 * software-only UI; wl_shm keeps it at a few MB. */
#ifdef CUIMOJI_HAVE_SDL2
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <cui/cui.h>
#include <cui/cui_render.h>
#include <wayland-client.h>

#include "cuimoji.h"
#include "sdl_common.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

typedef struct {
    struct wl_buffer *buffer;
    void *data;
    size_t size;
    int w, h;
    bool busy, retire;
} shm_buf;

#define NBUF 2

static struct wl_shm *shm;
static struct wl_surface *surf;
static struct wl_display *wldpy;
static shm_buf bufs[NBUF];
static SDL_Window *sdl_win;

static void buf_release(void *data, struct wl_buffer *release) {
    (void)release;
    shm_buf *b = data;
    b->busy = false;
    if (b->retire) {
        wl_buffer_destroy(b->buffer);
        munmap(b->data, b->size);
        memset(b, 0, sizeof *b);
    }
}
static const struct wl_buffer_listener buf_listener = { buf_release };

static void reg_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version) {
    (void)data;
    (void)version;
    if (strcmp(iface, wl_shm_interface.name) == 0)
        shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
}
static void reg_remove(void *data, struct wl_registry *reg, uint32_t name) {
    (void)data;
    (void)reg;
    (void)name;
}
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

static void sdl_hide(void) {
    if (sdl_win) {
        SDL_HideWindow(sdl_win);
        SDL_PumpEvents();
    }
}

static void buf_drop(shm_buf *b) {
    if (!b->buffer) return;
    if (b->busy) {
        b->retire = true;
        return;
    }
    wl_buffer_destroy(b->buffer);
    munmap(b->data, b->size);
    memset(b, 0, sizeof *b);
}

/* A free buffer of exactly this size; create one when asked. Buffers of
 * other sizes are dropped (or retired until the compositor releases them). */
static shm_buf *buf_get(int w, int h) {
    for (int i = 0; i < NBUF; i++)
        if (bufs[i].buffer && bufs[i].w == w && bufs[i].h == h && !bufs[i].busy && !bufs[i].retire)
            return &bufs[i];

    for (int i = 0; i < NBUF; i++)
        if (bufs[i].buffer && (bufs[i].w != w || bufs[i].h != h)) buf_drop(&bufs[i]);

    for (int i = 0; i < NBUF; i++) {
        if (bufs[i].buffer) continue;
        size_t size = (size_t)w * 4 * (size_t)h;
        int fd = memfd_create("cuimoji", MFD_CLOEXEC);
        if (fd < 0) return NULL;
        if (ftruncate(fd, (off_t)size) != 0) {
            close(fd);
            return NULL;
        }
        void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (data == MAP_FAILED) {
            close(fd);
            return NULL;
        }
        struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int)size);
        struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_XRGB8888);
        wl_shm_pool_destroy(pool);
        close(fd);
        if (!buf) {
            munmap(data, size);
            return NULL;
        }
        bufs[i].buffer = buf;
        bufs[i].data = data;
        bufs[i].size = size;
        bufs[i].w = w;
        bufs[i].h = h;
        bufs[i].busy = false;
        bufs[i].retire = false;
        wl_buffer_add_listener(buf, &buf_listener, &bufs[i]);
        return &bufs[i];
    }
    return NULL;
}

int cuimoji_run_sdl(void) {
    if (getenv("WAYLAND_DISPLAY")) setenv("SDL_VIDEODRIVER", "wayland", 0);
    SDL_SetHint(SDL_HINT_APP_NAME, "cuimoji");
    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
    /* no decorations (borderless window) for this small popup */
    SDL_SetHint("SDL_VIDEO_WAYLAND_ALLOW_LIBDECOR", "0");
    SDL_SetHint("SDL_VIDEO_WAYLAND_PREFER_LIBDECOR", "0");
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
    SDL_SetWindowBordered(sdl_win, SDL_FALSE);
    SDL_SetWindowMinimumSize(sdl_win, CUIMOJI_MIN_W, CUIMOJI_MIN_H);
    SDL_SetWindowMaximumSize(sdl_win, CUIMOJI_MAX_W, CUIMOJI_MAX_H);
    if (cuimoji_win_w && cuimoji_win_h)
        SDL_SetWindowSize(sdl_win, cuimoji_win_w, cuimoji_win_h);
    if (cuimoji_win_pos_valid && strcmp(SDL_GetCurrentVideoDriver(), "wayland") != 0)
        SDL_SetWindowPosition(sdl_win, cuimoji_win_x, cuimoji_win_y);

    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(sdl_win, &wm) || wm.subsystem != SDL_SYSWM_WAYLAND) {
        fprintf(stderr, "cuimoji: SDL is not on Wayland, using the X11 window\n");
        SDL_DestroyWindow(sdl_win);
        sdl_win = NULL;
        SDL_Quit();
        return 1;
    }
    wldpy = wm.info.wl.display;
    surf = wm.info.wl.surface;

    struct wl_registry *reg = wl_display_get_registry(wldpy);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(wldpy);
    if (!shm) {
        fprintf(stderr, "cuimoji: no wl_shm on this Wayland compositor\n");
        wl_registry_destroy(reg);
        SDL_DestroyWindow(sdl_win);
        sdl_win = NULL;
        SDL_Quit();
        return 1;
    }

    cui_sw_renderer *sw = cui_sw_create();
    if (!sw) {
        wl_registry_destroy(reg);
        SDL_DestroyWindow(sdl_win);
        sdl_win = NULL;
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
        wl_registry_destroy(reg);
        SDL_DestroyWindow(sdl_win);
        sdl_win = NULL;
        SDL_Quit();
        return 1;
    }

    cuimoji_font_apply(ui);
    cuimoji_hide_window = sdl_hide;
    cuimoji_quit_requested = false;
    cuimoji_toggle_requested = 0;
    SDL_StartTextInput();

    uint64_t freq = SDL_GetPerformanceFrequency();
    uint64_t last = SDL_GetPerformanceCounter();
    bool have_frame = false, pending = false;
    int draw_w = 0, draw_h = 0;

    while (!cuimoji_quit_requested && !cuimoji_toggle_requested) {
        /* Render only when something moves: pending work, a CUI animation or
         * settle frame, an input event, or a CUI timer that is due. */
        bool dirty = pending || !have_frame || cui_needs_redraw(ui);
        int wake_ms = -1;
        if (!dirty) {
            float wake = cui_next_wakeup(ui);
            if (wake >= 0) {
                wake_ms = (int)(wake * 1000.0f);
                if (wake_ms < 0) wake_ms = 0;
            }
        }
        int wait_ms = dirty ? 0 : 200;
        if (wake_ms >= 0 && wake_ms < wait_ms) wait_ms = wake_ms;
        bool timer_due = wake_ms >= 0 && wake_ms <= wait_ms;

        bool woke = false;
        if (!cuimoji_sdl_pump(ui, wait_ms, &woke)) break;
        wl_display_dispatch_pending(wldpy);

        int ww = 0, wh = 0;
        SDL_GetWindowSize(sdl_win, &ww, &wh);
        if (ww <= 0 || wh <= 0) { SDL_Delay(8); continue; }
        bool size_diff = (ww != draw_w || wh != draw_h);

        /* Idle: no input, no size change, no timer, nothing moving. */
        if (have_frame && !size_diff && !woke && !timer_due && !pending && !cui_needs_redraw(ui))
            continue;

        /* Both buffers still held by the compositor: wait for a release. */
        shm_buf *b = buf_get(ww, wh);
        if (!b) { pending = true; SDL_Delay(1); continue; }
        pending = false;

        uint64_t now = SDL_GetPerformanceCounter();
        float dt = (float)((double)(now - last) / (double)freq);
        last = now;
        if (dt <= 0 || dt > 0.25f) dt = 1.0f / 120.0f;

        float zoom = cuimoji_ui_zoom > 0.1f ? cuimoji_ui_zoom : 1.0f;
        int lw = (int)((float)ww / zoom + 0.5f), lh = (int)((float)wh / zoom + 0.5f);
        if (lw < 1) lw = 1;
        if (lh < 1) lh = 1;
        cui_set_viewport(ui, (float)lw, (float)lh, zoom);
        cui_begin_frame(ui, dt);
        cuimoji_ui(ui);
        cui_end_frame(ui);
        if (cuimoji_quit_requested || cuimoji_toggle_requested) break;

        const cui_draw_data *dd = cui_get_draw_data(ui);
        cui_sw_render(sw, dd, b->data, ww, wh, ww * 4, CUI_SW_BGRA);

        wl_surface_attach(surf, b->buffer, 0, 0);
        wl_surface_damage(surf, 0, 0, ww, wh);
        wl_surface_commit(surf);
        b->busy = true;
        have_frame = true;
        draw_w = ww;
        draw_h = wh;
        wl_display_flush(wldpy);
    }

    SDL_StopTextInput();
    SDL_GetWindowSize(sdl_win, &cuimoji_win_w, &cuimoji_win_h);
    if (strcmp(SDL_GetCurrentVideoDriver(), "wayland") != 0) {
        SDL_GetWindowPosition(sdl_win, &cuimoji_win_x, &cuimoji_win_y);
        cuimoji_win_pos_valid = true;
    }
    cuimoji_state_save();
    for (int i = 0; i < NBUF; i++) buf_drop(&bufs[i]);
    wl_registry_destroy(reg);
    cui_destroy(ui);
    cui_sw_destroy(sw);
    SDL_DestroyWindow(sdl_win);
    sdl_win = NULL;
    SDL_Quit();
    return 0;
}

#endif

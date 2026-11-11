#define _GNU_SOURCE
/* SDL2 frontend in software mode: a Wayland-native window (SDL2 wayland
 * driver). CUI draws into wl_shm buffers attached to SDL's own wl_surface,
 * double buffered. During a resize nothing is committed, so the compositor
 * scales the last buffer instead of flashing through intermediate sizes. */
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

/* A free buffer of exactly this size; create one when asked. Buffer sizes
 * other than the wanted one are dropped (or retired until released). */
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

    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(sdl_win, &wm) || wm.subsystem != SDL_SYSWM_WAYLAND) {
        fprintf(stderr, "cuimoji: SDL is not on Wayland, using the X11 window\n");
        SDL_DestroyWindow(sdl_win);
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
        SDL_Quit();
        return 1;
    }

    cui_sw_renderer *sw = cui_sw_create();
    cui_ctx *ui = sw ? cui_create(&(cui_ctx_desc){
                          .renderer = cui_sw_iface(sw),
                          .theme = cuimoji_theme(),
                          .accent = CUI_ACCENT_NEUTRAL,
                      })
                     : NULL;
    if (!ui) {
        if (sw) cui_sw_destroy(sw);
        wl_registry_destroy(reg);
        SDL_DestroyWindow(sdl_win);
        SDL_Quit();
        return 1;
    }

    cuimoji_font_apply(ui);
    cuimoji_hide_window = sdl_hide;
    cuimoji_quit_requested = false;
    SDL_StartTextInput();

    int draw_w = 0, draw_h = 0;
    Uint32 last_draw_ms = 0;
    bool have_frame = false;
    uint64_t freq = SDL_GetPerformanceFrequency();
    uint64_t last = SDL_GetPerformanceCounter();

    while (!cuimoji_quit_requested && !cuimoji_toggle_requested) {
        if (!cuimoji_sdl_pump(ui)) break;
        wl_display_dispatch_pending(wldpy);

        int ww = 0, wh = 0;
        SDL_GetWindowSize(sdl_win, &ww, &wh);
        if (ww <= 0 || wh <= 0) { SDL_Delay(8); continue; }

        Uint32 now_ms = SDL_GetTicks();
        bool size_diff = (ww != draw_w || wh != draw_h);
        /* During a drag, draw at most ~30 fps: crisp enough to feel live, and
         * the compositor scales the last buffer in between. Rendering every
         * step used to be the heavy part. */
        if (have_frame && size_diff && (Uint32)(now_ms - last_draw_ms) < 33) {
            last = SDL_GetPerformanceCounter();
            SDL_Delay(2);
            continue;
        }
        /* Nothing changed: do not render. */
        if (have_frame && !size_diff && !cui_needs_redraw(ui)) {
            SDL_Delay(8);
            continue;
        }

        shm_buf *b = buf_get(ww, wh);
        if (!b) { SDL_Delay(8); continue; }

        uint64_t now = SDL_GetPerformanceCounter();
        float dt = (float)((double)(now - last) / (double)freq);
        last = now;
        if (dt <= 0 || dt > 0.25f) dt = 1.0f / 120.0f;

        cui_set_viewport(ui, (float)ww, (float)wh, 1.0f);
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
        last_draw_ms = now_ms;
        wl_display_flush(wldpy);
    }

    SDL_StopTextInput();
    for (int i = 0; i < NBUF; i++) buf_drop(&bufs[i]);
    wl_registry_destroy(reg);
    cui_destroy(ui);
    cui_sw_destroy(sw);
    SDL_DestroyWindow(sdl_win);
    SDL_Quit();
    return 0;
}

#endif

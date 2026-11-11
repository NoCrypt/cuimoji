/*
 * cuimoji - a small grid emoji picker for Linux, built on CUI.
 *
 * An Emote/Smile-like picker with a fast auto-paste path. CUI provides the
 * searchable emoji grid; this app adds the desktop glue:
 *
 *   - X11 window (runs under XWayland on Wayland sessions)
 *   - instant X11 clipboard copy, handed to wl-copy in the background so the
 *     clipboard survives after the window closes
 *   - Ctrl+V sent from a persistent /dev/uinput keyboard (no daemon, no
 *     process spawn, no portal)
 *   - waits for the compositor to give focus back to the previous window
 *     (using niri's IPC when available) so the paste lands in the right app;
 *     terminals get Ctrl+Shift+V
 *   - recents, skin tone and the emoji font are saved under
 *     $XDG_STATE_HOME/cuimoji
 *   - emoji set picker: system font (or any .ttf/.otf dropped into
 *     ~/.local/share/cuimoji/emoji)
 */
#define _GNU_SOURCE
#include <cui/cui.h>
#include <cui/cui_app.h>

/* The app needs a few small pieces from CUI's internals: the platform
 * clipboard callback, a wakeup after the window is hidden (so the X11
 * selection stays served), and ids inside the emoji picker. Pinned to CUI
 * v1.2.0. */
#include "cui_internal.h"

#include "uinput.h"
#include "cuimoji.h"

#ifdef CUIMOJI_HAVE_EMOJIBASE
#include "emojibase.h"

static unsigned char alias_lower(unsigned char c) { return c >= 'A' && c <= 'Z' ? (unsigned char)(c + 32) : c; }

/* CUI calls this for names its own matcher missed (the weak default returns
 * false). All query words must appear somewhere in the Emojibase text. */
bool cui__em_alias_match(const char *emoji, int len, const char *query) {
    const char *aliases = cuimoji_alias_lookup(emoji, len);
    if (!aliases) return false;
    const char *q = query;
    while (*q) {
        while (*q == ' ') q++;
        if (!*q) break;
        const char *w = q;
        while (*q && *q != ' ') q++;
        int wl = (int)(q - w);
        bool found = false;
        for (int i = 0; aliases[i] && !found; i++) {
            int k = 0;
            while (k < wl && aliases[i + k] &&
                   alias_lower((unsigned char)aliases[i + k]) == alias_lower((unsigned char)w[k])) k++;
            found = k == wl;
        }
        if (!found) return false;
    }
    return true;
}
#endif

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PICKER_ID "emoji"
static char picker_id[32] = PICKER_ID;
static int picker_gen;
static bool font_pending;
#define GRID_H 592
#define FOOTER_H 48   /* matches CUI's emoji preview row */
#define DEFAULT_FOCUS_DELAY_MS 20
#define ICON_OVERLAY_PX 22.0f   /* CUI draws picker icons at 18 px, too small on
                                 * this font stack; we redraw them a bit larger */
#define MAX_FONTS 32

static char recent[512];
static int tone;
static int renderer_mode;   /* 0 software, 1 OpenGL, 2 Vulkan */
static int emoji_size = 28;
static int ui_scale = 100;
static int paste_delay_ms = 20;   /* fallback when niri cannot be asked */
static int theme_mode;            /* 0 dark, 1 light */
static int platform_mode;         /* 0 auto, 1 wayland (SDL2), 2 X11 */
static bool initial_focus_done;
static bool debug_on;
static int uinput_fd = -1;
static Display *xdpy;
static Window xwin;
static char paste_target[96];
static float footer_y;
static float footer_h = 34;

bool cuimoji_quit_requested;
volatile sig_atomic_t cuimoji_toggle_requested;
void (*cuimoji_hide_window)(void);
int cuimoji_win_w, cuimoji_win_h, cuimoji_win_x, cuimoji_win_y;
bool cuimoji_win_pos_valid;

static void sig_toggle(int sig) {
    (void)sig;
    cuimoji_toggle_requested = 1;
}

/* One instance only: a second launch just asks the running one to close
 * (SIGUSR1), which makes Mod+Period a toggle instead of a window farm. */
static int single_instance(void) {
    const char *run = getenv("XDG_RUNTIME_DIR");
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/cuimoji.lock", run && run[0] ? run : "/tmp");
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        char buf[32];
        ssize_t n = pread(fd, buf, sizeof buf - 1, 0);
        if (n > 0) {
            buf[n] = 0;
            pid_t pid = (pid_t)atoi(buf);
            if (pid > 0) kill(pid, SIGUSR1);
        }
        close(fd);
        return 1;
    }
    ftruncate(fd, 0);
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%d\n", (int)getpid());
    if (n > 0) (void)!pwrite(fd, buf, (size_t)n, 0);
    return fd;
}

static void x11_hide(void) {
    if (xdpy && xwin) {
        XUnmapWindow(xdpy, xwin);
        XFlush(xdpy);
    }
}

/* the emoji font choices */
static char font_labels[MAX_FONTS][72];
static char font_paths[MAX_FONTS][PATH_MAX];
static const char *font_items[MAX_FONTS];
static int font_count;
static int font_index;
static char selected_font[PATH_MAX];

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static bool is_self_app(const char *id) {
    /* niri reports the X11 WM_CLASS; CUI's default class is "CUI", the title
     * is "cuimoji". Our own hint may or may not have reached XWayland by the
     * time the surface is mapped, so accept both. */
    return !id[0] || strcmp(id, "cuimoji") == 0 || strcmp(id, "CUI") == 0;
}

static bool is_terminal_app(const char *id) {
    static const char *const terminals[] = {
        "kitty", "foot", "alacritty", "wezterm", "org.wezfurlong.wezterm",
        "konsole", "org.kde.konsole", "ghostty", "com.mitchellh.ghostty",
        "gnome-terminal", "org.gnome.Terminal", "ptyxis", "org.gnome.Ptyxis",
        "xterm", "urxvt", "rxvt", "st", "terminator", "tilix", "com.gexperts.Tilix",
    };
    for (size_t i = 0; i < sizeof terminals / sizeof terminals[0]; i++)
        if (strcmp(id, terminals[i]) == 0) return true;
    return false;
}

static bool focused_app_id(char *out, size_t cap) {
    out[0] = 0;
    FILE *p = popen("niri msg --json focused-window 2>/dev/null", "r");
    if (!p) return false;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1, p);
    buf[n] = 0;
    pclose(p);

    const char *key = "\"app_id\":\"";
    const char *s = strstr(buf, key);
    if (!s) return false;
    s += strlen(key);
    const char *e = strchr(s, '"');
    if (!e) return false;
    size_t len = (size_t)(e - s);
    if (len >= cap) len = cap - 1;
    memcpy(out, s, len);
    out[len] = 0;
    return out[0] != 0;
}

static double wait_for_focus(char *target, size_t cap, double start) {
    int delay_ms = paste_delay_ms;
    const char *env = getenv("CUIMOJI_PASTE_DELAY_MS");
    if (env && atoi(env) >= 0) delay_ms = atoi(env);

    if (getenv("NIRI_SOCKET")) {
        for (int i = 0; i < 4; i++) {
            if (!focused_app_id(target, cap)) break;
            if (!is_self_app(target)) return now_ms() - start;
            usleep(3000);
        }
    }

    usleep((useconds_t)delay_ms * 1000);
    return now_ms() - start;
}

static pid_t start_wl_copy(const char *text) {
    int fds[2];
    if (pipe(fds) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        close(fds[1]);
        dup2(fds[0], STDIN_FILENO);
        close(fds[0]);
        execlp("wl-copy", "wl-copy", (char *)NULL);
        _exit(127);
    }

    close(fds[0]);
    size_t len = strlen(text), off = 0;
    while (off < len) {
        ssize_t n = write(fds[1], text + off, len - off);
        if (n <= 0) break;
        off += (size_t)n;
    }
    close(fds[1]);
    return pid;
}

static void finish_wl_copy(pid_t pid) {
    if (pid <= 0) return;
    int status = 0;
    waitpid(pid, &status, 0);
}

static void fallback_paste(bool shift) {
    pid_t pid = fork();
    if (pid != 0) return;
    if (shift)
        execlp("ydotool", "ydotool", "key", "29:1", "42:1", "47:1", "47:0", "42:0", "29:0", (char *)NULL);
    else
        execlp("ydotool", "ydotool", "key", "29:1", "47:1", "47:0", "29:0", (char *)NULL);
    _exit(127);
}

/* ---- state ---------------------------------------------------------- */

static void state_dir(char *out, size_t cap) {
    const char *base = getenv("XDG_STATE_HOME");
    if (!base || !base[0]) {
        const char *home = getenv("HOME");
        snprintf(out, cap, "%s/.local/state/cuimoji", home ? home : ".");
    } else {
        snprintf(out, cap, "%s/cuimoji", base);
    }
    if (mkdir(out, 0755) != 0 && errno == ENOENT) {
        char parent[1024];
        snprintf(parent, sizeof parent, "%s", out);
        char *slash = strrchr(parent, '/');
        if (slash) {
            *slash = 0;
            mkdir(parent, 0755);
            mkdir(out, 0755);
        }
    }
}

cui_theme_mode cuimoji_theme(void) { return theme_mode ? CUI_THEME_LIGHT : CUI_THEME_DARK; }

void cuimoji_state_load(void) {
    char dir[1024], path[1200];
    state_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s/state", dir);
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[2048];
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "tone=", 5) == 0) {
            tone = atoi(line + 5);
            if (tone < 0) tone = 0;
            if (tone > 5) tone = 5;
        } else if (strncmp(line, "recent=", 7) == 0) {
            line[strcspn(line, "\r\n")] = 0;
            snprintf(recent, sizeof recent, "%s", line + 7);
        } else if (strncmp(line, "emoji_font=", 11) == 0) {
            line[strcspn(line, "\r\n")] = 0;
            snprintf(selected_font, sizeof selected_font, "%s", line + 11);
        } else if (strncmp(line, "paste_delay=", 12) == 0) {
            paste_delay_ms = atoi(line + 12);
            if (paste_delay_ms < 0) paste_delay_ms = 0;
            if (paste_delay_ms > 500) paste_delay_ms = 500;
        } else if (strncmp(line, "platform=", 9) == 0) {
            line[strcspn(line, "\r\n")] = 0;
            if (strcmp(line + 9, "wayland") == 0) platform_mode = 1;
            else if (strcmp(line + 9, "x11") == 0) platform_mode = 2;
            else platform_mode = 0;
        } else if (strncmp(line, "theme=", 6) == 0) {
            line[strcspn(line, "\r\n")] = 0;
            theme_mode = strcmp(line + 6, "light") == 0 ? 1 : 0;
        } else if (strncmp(line, "emoji_size=", 11) == 0) {
            emoji_size = atoi(line + 11);
            if (emoji_size < 24) emoji_size = 24;
            if (emoji_size > 128) emoji_size = 128;
        } else if (strncmp(line, "ui_scale=", 9) == 0) {
            ui_scale = atoi(line + 9);
            if (ui_scale < 80) ui_scale = 80;
            if (ui_scale > 150) ui_scale = 150;
        } else if (strncmp(line, "renderer=", 9) == 0) {
            line[strcspn(line, "\r\n")] = 0;
            if (strcmp(line + 9, "opengl") == 0) renderer_mode = 1;
            else if (strcmp(line + 9, "vulkan") == 0) renderer_mode = 2;
            else renderer_mode = 0;
        } else if (strncmp(line, "win_w=", 6) == 0) {
            cuimoji_win_w = atoi(line + 6);
            if (cuimoji_win_w < CUIMOJI_MIN_W || cuimoji_win_w > CUIMOJI_MAX_W) cuimoji_win_w = 0;
        } else if (strncmp(line, "win_h=", 6) == 0) {
            cuimoji_win_h = atoi(line + 6);
            if (cuimoji_win_h < CUIMOJI_MIN_H || cuimoji_win_h > CUIMOJI_MAX_H) cuimoji_win_h = 0;
        } else if (strncmp(line, "win_pos=", 8) == 0) {
            int wx, wy;
            if (sscanf(line + 8, "%d,%d", &wx, &wy) == 2) {
                cuimoji_win_x = wx;
                cuimoji_win_y = wy;
                cuimoji_win_pos_valid = true;
            }
        }
    }
    if (!cuimoji_win_w || !cuimoji_win_h) cuimoji_win_w = cuimoji_win_h = 0;
    fclose(f);
}

static void capture_x11_geometry(void) {
    if (!xdpy || !xwin) return;
    XWindowAttributes wa;
    Window child;
    int rx = 0, ry = 0;
    if (!XGetWindowAttributes(xdpy, xwin, &wa)) return;
    cuimoji_win_w = wa.width;
    cuimoji_win_h = wa.height;
    if (XTranslateCoordinates(xdpy, xwin, DefaultRootWindow(xdpy), 0, 0, &rx, &ry, &child)) {
        cuimoji_win_x = rx;
        cuimoji_win_y = ry;
        cuimoji_win_pos_valid = true;
    }
}

void cuimoji_state_save(void) {
    capture_x11_geometry();
    char dir[1024], path[1200], tmp[1300];
    state_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s/state", dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f,
            "tone=%d\nrecent=%s\nemoji_font=%s\nemoji_size=%d\nui_scale=%d\npaste_delay=%d\ntheme=%s\n"
            "platform=%s\nrenderer=%s\n",
            tone, recent, selected_font, emoji_size, ui_scale, paste_delay_ms,
            theme_mode ? "light" : "dark",
            platform_mode == 1 ? "wayland" : platform_mode == 2 ? "x11" : "auto",
            renderer_mode == 1 ? "opengl" : renderer_mode == 2 ? "vulkan" : "software");
    if (cuimoji_win_w && cuimoji_win_h)
        fprintf(f, "win_w=%d\nwin_h=%d\n", cuimoji_win_w, cuimoji_win_h);
    if (cuimoji_win_pos_valid)
        fprintf(f, "win_pos=%d,%d\n", cuimoji_win_x, cuimoji_win_y);
    fclose(f);
    rename(tmp, path);
}

/* ---- emoji fonts ---------------------------------------------------- */

static void font_dir(char *out, size_t cap) {
    const char *home = getenv("HOME");
    char base[PATH_MAX];
    snprintf(base, sizeof base, "%s/.local/share/cuimoji", home ? home : ".");
    mkdir(base, 0755);
    snprintf(out, cap, "%s/emoji", base);
    mkdir(out, 0755);
}

static void add_font(const char *label, const char *path) {
    if (font_count >= MAX_FONTS) return;
    for (int i = 0; i < font_count; i++)
        if (strcmp(font_paths[i], path) == 0) return;
    snprintf(font_labels[font_count], sizeof font_labels[0], "%s", label);
    snprintf(font_paths[font_count], sizeof font_paths[0], "%s", path);
    font_items[font_count] = font_labels[font_count];
    font_count++;
}

void cuimoji_fonts_scan(void) {
    add_font("System", "");

    static const char *const known[] = {
        "/usr/share/fonts/apple-color-emoji/apple-color-emoji.ttf",
        "/usr/share/fonts/truetype/apple-color-emoji/apple-color-emoji.ttf",
        "/usr/share/fonts/noto/NotoColorEmoji.ttf",
    };
    static const char *const labels[] = {
        "Apple Color Emoji",
        "Apple Color Emoji",
        "Noto Color Emoji",
    };
    for (size_t i = 0; i < sizeof known / sizeof known[0]; i++)
        if (access(known[i], R_OK) == 0) add_font(labels[i], known[i]);

    char dir[PATH_MAX];
    font_dir(dir, sizeof dir);
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d))) {
            const char *name = ent->d_name;
            size_t len = strlen(name);
            if (len < 5) continue;
            const char *ext = strrchr(name, '.');
            if (!ext) continue;
            if (strcasecmp(ext, ".ttf") != 0 && strcasecmp(ext, ".otf") != 0 && strcasecmp(ext, ".ttc") != 0)
                continue;
            char label[72];
            snprintf(label, sizeof label, "%.*s", (int)(ext - name), name);
            char path[PATH_MAX];
            snprintf(path, sizeof path, "%s/%s", dir, name);
            add_font(label, path);
        }
        closedir(d);
    }

    font_index = 0;
    for (int i = 0; i < font_count; i++) {
        if (strcmp(font_paths[i], selected_font) == 0) {
            font_index = i;
            break;
        }
    }
}

void cuimoji_font_apply(cui_ctx *ui) {
    int idx = font_index >= 0 && font_index < font_count ? font_index : 0;
    snprintf(selected_font, sizeof selected_font, "%s", font_paths[idx]);
    /* Switching fonts at runtime: tear the emoji state (font, atlas, cached
     * slots) down first, so the new font is loaded exactly like at startup. */
    if (ui->emoji && ui->emoji_hook) ui->emoji_hook(ui, 2);
    if (selected_font[0]) {
        if (!cui_emoji_font_load_file(ui, selected_font)) cui_emoji_font_load_system(ui);
    } else {
        cui_emoji_font_load_system(ui);
    }
}

/* ---- picker ids and icon overlays ----------------------------------- */

static cui_id picker_search_id(cui_ctx *ui) {
    cui_push_id(ui, picker_id);
    cui_id id = cui_make_id(ui, "#search");
    cui_pop_id(ui);
    return id;
}

static cui_id picker_sub_id(cui_ctx *ui, const char *s) {
    cui_push_id(ui, picker_id);
    cui_id id = cui_make_id(ui, s);
    cui_pop_id(ui);
    return id;
}

static cui_id picker_tab_id(cui_ctx *ui, int kind) {
    cui_push_id(ui, picker_id);
    cui_push_id_int(ui, kind);
    cui_id id = cui_make_id(ui, "#tab");
    cui_pop_id(ui);
    cui_pop_id(ui);
    return id;
}

static cui_id picker_tone_id(cui_ctx *ui, int kind) {
    cui_push_id(ui, picker_id);
    cui_push_id_int(ui, kind);
    cui_id id = cui_make_id(ui, "#tone");
    cui_pop_id(ui);
    cui_pop_id(ui);
    return id;
}

static void overlay_emoji(cui_ctx *ui, cui_id id, const char *emoji) {
    cui_rect r = cui_get_rect(ui, id);
    if (r.w <= 0 || r.h <= 0) return;
    float side = ICON_OVERLAY_PX;
    cui_rect d = { r.x + (r.w - side) * 0.5f, r.y + (r.h - side) * 0.5f, side, side };
    cui_draw_emoji(ui, emoji, -1, d);
}
static bool picker_has_id(cui_ctx *ui, cui_id id) {
    for (int i = 0; i < ui->node_count; i++)
        if (ui->nodes[i].id == id) return true;
    return false;
}

/* CUI draws the picker tabs and the tone button at 18 px, which this emoji
 * font stack does not rasterize. Redraw them a bit larger on top. */
static void draw_picker_icons(cui_ctx *ui) {
    static const char *const tab_icons[10] = {
        NULL, "\xf0\x9f\x98\x80", "\xf0\x9f\x91\x8b", "\xf0\x9f\x90\xbb", "\xf0\x9f\x8d\x94",
        "\xe2\x9c\x88\xef\xb8\x8f", "\xe2\x9a\xbd", "\xf0\x9f\x92\xa1", "\xf0\x9f\x94\xa3", "\xf0\x9f\x8f\x81",
    };
    static const char *const tone_hands[6] = {
        "\xe2\x9c\x8b", "\xe2\x9c\x8b\xf0\x9f\x8f\xbb", "\xe2\x9c\x8b\xf0\x9f\x8f\xbc",
        "\xe2\x9c\x8b\xf0\x9f\x8f\xbd", "\xe2\x9c\x8b\xf0\x9f\x8f\xbe", "\xe2\x9c\x8b\xf0\x9f\x8f\xbf",
    };

    bool tone_open = picker_has_id(ui, picker_tone_id(ui, 0));
    if (tone_open) {
        for (int k = 0; k < 6; k++) overlay_emoji(ui, picker_tone_id(ui, k), tone_hands[k]);
    } else {
        overlay_emoji(ui, picker_sub_id(ui, "#tones"), tone_hands[tone < 0 ? 0 : tone > 5 ? 5 : tone]);
    }
    for (int kind = 1; kind <= 9; kind++) overlay_emoji(ui, picker_tab_id(ui, kind), tab_icons[kind]);
}

/* A paint-only leaf over the picker: the icons are drawn after the picker's
 * own nodes, so they end up on top of the tab buttons. */
static void paint_picker_icons(cui_ctx *ui, cui_node *n, void *data) {
    (void)n;
    (void)data;
    (void)footer_y;
    (void)footer_h;
    /* CUI's own preview row draws the divider, the hovered emoji and its
     * name (or the hint when nothing is hovered); only the picker icons are
     * overlaid here. */
    draw_picker_icons(ui);
}

static void icon_overlay_leaf(cui_ctx *ui) {
    int leaf = cui__leaf(ui, 0, CUI_FILL, CUI_FILL);
    ui->nodes[leaf].paint = paint_picker_icons;
}

/* ---- paste ---------------------------------------------------------- */

static void do_pick(cui_ctx *ui, const char *emoji) {
    double t0 = now_ms();
    char text[64];
    snprintf(text, sizeof text, "%s", emoji);

    /* 1. Clipboard now: CUI's X11 selection (instant) and wl-copy in the
     *    background, which is what Wayland apps actually read from. */
    if (ui->platform.set_clipboard) ui->platform.set_clipboard(ui->platform.user, text);
    double t_copy = now_ms() - t0;
    pid_t wl_copy = start_wl_copy(text);

    /* 2. Hide immediately so the compositor can refocus the previous window. */
    if (cuimoji_hide_window) cuimoji_hide_window();
    double t_unmap = now_ms() - t0;

    /* 3. Wait for focus to leave cuimoji; niri's IPC tells us exactly. */
    double focus_ms = wait_for_focus(paste_target, sizeof paste_target, t0);
    bool shift = is_terminal_app(paste_target);

    /* 4. Make sure wl-copy owns the clipboard before the paste. */
    finish_wl_copy(wl_copy);
    double t_wl = now_ms() - t0;

    /* Let the target app finish taking focus and reading the clipboard. */
    usleep(15000);

    /* 5. Paste from the already-open uinput device. */
    double t_before_key = now_ms();
    if (uinput_fd >= 0) uinput_paste(uinput_fd, shift);
    else fallback_paste(shift);
    double key_ms = now_ms() - t_before_key;

    cuimoji_state_save();

    if (debug_on) {
        fprintf(stderr,
                "cuimoji: target=%s shift=%d copy=%.1fms unmap=%.1fms focus=%.1fms wl-copy=%.1fms key=%.1fms total=%.1fms\n",
                paste_target[0] ? paste_target : "?", shift, t_copy, t_unmap - t_copy,
                focus_ms, t_wl - t_unmap, key_ms, now_ms() - t0);
    }

    cuimoji_quit_requested = true;
}

void cuimoji_ui(cui_ctx *ui);

static bool printable_key_pressed(cui_ctx *ui) {
    for (int k = CUI_KEY_A; k <= CUI_KEY_Z; k++) if (cui_key_pressed(ui, (cui_key)k)) return true;
    for (int k = CUI_KEY_0; k <= CUI_KEY_9; k++) if (cui_key_pressed(ui, (cui_key)k)) return true;
    return cui_key_pressed(ui, CUI_KEY_SPACE) || cui_key_pressed(ui, CUI_KEY_MINUS) ||
           cui_key_pressed(ui, CUI_KEY_EQUAL) || cui_key_pressed(ui, CUI_KEY_COMMA) ||
           cui_key_pressed(ui, CUI_KEY_PERIOD) || cui_key_pressed(ui, CUI_KEY_SLASH);
}

void cuimoji_ui(cui_ctx *ui) {

    if (font_pending) {
        picker_gen++;
        snprintf(picker_id, sizeof picker_id, "emoji-%d", picker_gen);
        cuimoji_font_apply(ui);
        font_pending = false;
        ui->settle_frames = 4;
        initial_focus_done = false;
    } else if (cui_key_pressed(ui, CUI_KEY_F2) && font_count > 1) {
        font_index = (font_index + 1) % font_count;
        snprintf(selected_font, sizeof selected_font, "%s", font_paths[font_index]);
        font_pending = true;
        cuimoji_state_save();
    }

    static int applied_theme = -1;
    if (applied_theme != theme_mode) {
        cui_set_theme(ui, theme_mode ? CUI_THEME_LIGHT : CUI_THEME_DARK);
        applied_theme = theme_mode;
    }

    if (!initial_focus_done) {
        /* Start with the keyboard cursor on the first item: the most recent
         * emoji, or the first one in the current group when there are none. */
        ui->focus = picker_search_id(ui);
        cui_input_key(ui, CUI_KEY_DOWN, true, 0);
        cui_input_key(ui, CUI_KEY_DOWN, false, 0);
        ui->focus_visible = true;
        initial_focus_done = true;
    } else if (ui->focus != picker_search_id(ui) && printable_key_pressed(ui)) {
        /* The grid has focus; typing should start a search. The platform has
         * already queued the text, and the search field picks it up when it is
         * built later in this frame. */
        ui->focus = picker_search_id(ui);
    }

    {
        /* The picker no longer has CUI's 48 px preview row. Leave room for
         * our own 34 px footer instead: "Pick an emoji" on the left, the
         * settings gear on the right, both on the same line. */
        static char hint[48];
        snprintf(hint, sizeof hint, "cuimoji %s", CUIMOJI_VERSION);
        ui->theme.font_scale = (float)ui_scale / 100.0f;
        /* The grid view starts below the search row and the group tabs; its
         * y position is the chrome height (they scale with the UI size). */
        cui_rect grid_rect = cui_get_rect(ui, picker_sub_id(ui, "#grid"));
        float chrome = grid_rect.y > 20.0f ? grid_rect.y : 94.0f;
        float grid_h = ui->vh - FOOTER_H - chrome;
        if (grid_h < 120.0f) grid_h = 120.0f;
        footer_y = ui->vh - FOOTER_H;
        footer_h = FOOTER_H;
        float pad_bottom = (footer_h - 28.0f) * 0.5f;
        if (pad_bottom < 2.0f) pad_bottom = 2.0f;

        cui_box(ui, .dir = CUI_STACK, .width = CUI_FILL, .height = CUI_FILL) {
            const char *emoji = cui_emoji_picker(
                ui, picker_id,
                .width = CUI_FILL,
                .height = grid_h,
                .size = (float)emoji_size,
                .recent = recent,
                .recent_cap = sizeof recent,
                .tone = &tone,
                .placeholder = "Search emoji",
                .hint = hint);

            if (emoji) {
                do_pick(ui, emoji);
            } else if (cui_key_pressed(ui, CUI_KEY_ESCAPE)) {
                cuimoji_state_save();
                cuimoji_quit_requested = true;
            }

            icon_overlay_leaf(ui);

            /* Settings trigger, inside the picker's bottom bar. The column
             * is only there to put the button at the bottom-right; it has
             * no background or hit area of its own. */
            cui_column(ui, .width = CUI_FILL, .height = CUI_FILL,
                       .justify = CUI_JUSTIFY_END) {
                cui_row(ui, .width = CUI_FILL, .pad_left = 0, .pad_right = 10,
                        .pad_top = 0, .pad_bottom = pad_bottom, .gap = 8,
                        .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_END) {
                    if (cui_icon_button(ui, CUI_ICON_SETTINGS, .size = CUI_SIZE_SM, .tooltip = "Settings") ||
                        cui_key_pressed(ui, CUI_KEY_F1))
                        cui_popover_toggle(ui, "settings");
                    cui_popover(ui, "settings", .placement = CUI_PLACE_TOP_END,
                                .width = 300, .pad = 12, .gap = 8) {
                        cui_column(ui, .width = CUI_FILL, .gap = 8, .scroll_y = true,
                                   .max_height = ui->vh - 140.0f) {
                        static const char *const themes[] = { "Dark", "Light" };
                        int th = theme_mode ? 1 : 0;
                        if (cui_select(ui, "theme", themes, 2, &th,
                                       .label = "Theme", .width = CUI_FILL)) {
                            theme_mode = th;
                            cuimoji_state_save();
                        }
                        if (cui_select(ui, "emoji-set", font_items, font_count, &font_index,
                                       .label = "Emoji set", .width = CUI_FILL)) {
                            snprintf(selected_font, sizeof selected_font, "%s", font_paths[font_index]);
                            font_pending = true;
                            cuimoji_state_save();
                        }
                        double sz = (double)emoji_size;
                        if (cui_number(ui, "emoji-size", &sz, 24, 128,
                                       .label = "Emoji size", .step = 2, .suffix = " px",
                                       .width = CUI_FILL)) {
                            emoji_size = (int)(sz + 0.5);
                            ui->settle_frames = 4;   /* refill the emoji atlas */
                            cuimoji_state_save();
                        }
                        double us = (double)ui_scale;
                        if (cui_number(ui, "ui-size", &us, 80, 150,
                                       .label = "UI size", .step = 5, .suffix = " %",
                                       .width = CUI_FILL)) {
                            ui_scale = (int)(us + 0.5);
                            cuimoji_state_save();
                        }
                        double dl = (double)paste_delay_ms;
                        if (cui_number(ui, "paste-delay", &dl, 0, 500,
                                       .label = "Paste delay (fallback)", .step = 10,
                                       .suffix = " ms", .width = CUI_FILL)) {
                            paste_delay_ms = (int)(dl + 0.5);
                            cuimoji_state_save();
                        }
                        static const char *const renderers[] = { "Software", "OpenGL", "Vulkan" };
                        int ridx = renderer_mode;
                        if (cui_select(ui, "renderer", renderers, 3, &ridx,
                                       .label = "Renderer", .width = CUI_FILL)) {
                            renderer_mode = ridx;
                            cuimoji_state_save();
                        }
                        static const char *const platforms[] = { "Auto", "Wayland", "X11" };
                        int pidx = platform_mode;
                        if (cui_select(ui, "platform", platforms, 3, &pidx,
                                       .label = "Window system", .width = CUI_FILL)) {
                            platform_mode = pidx;
                            cuimoji_state_save();
                        }
                        }
                    }
                }
            }
        }
    }
}

int main(void) {
    debug_on = getenv("CUIMOJI_DEBUG") != NULL;
    int lock_fd = single_instance();
    if (lock_fd == 1) return 0;   /* another instance took over */

    cuimoji_state_load();
    cuimoji_fonts_scan();
    signal(SIGUSR1, sig_toggle);
    uinput_fd = uinput_keyboard_open("cuimoji virtual keyboard");

    /* Window system: X11 (through XWayland on a Wayland session), or a
     * Wayland-native SDL2 window. "Auto" picks Wayland when it is there. */
    bool want_sdl = platform_mode == 1 || (platform_mode == 0 && getenv("WAYLAND_DISPLAY"));
#ifdef CUIMOJI_HAVE_SDL2
    if (want_sdl) {
#ifdef CUIMOJI_HAVE_VULKAN
        if (renderer_mode == 2) {
            if (cuimoji_run_vulkan() == 0) {
                uinput_keyboard_close(uinput_fd);
                return 0;
            }
            fprintf(stderr, "cuimoji: Vulkan backend failed, falling back\n");
            renderer_mode = 0;
        }
#else
        if (renderer_mode == 2) {
            fprintf(stderr, "cuimoji: this build has no Vulkan backend, using the software renderer\n");
            renderer_mode = 0;
        }
#endif
        if (cuimoji_run_sdl() == 0) {
            uinput_keyboard_close(uinput_fd);
            return 0;
        }
        fprintf(stderr, "cuimoji: SDL2/Wayland failed, using the X11 window\n");
    }
#else
    if (want_sdl)
        fprintf(stderr, "cuimoji: this build has no SDL2 backend, using the X11 window\n");
    if (renderer_mode == 2) renderer_mode = 0;
#endif

    cui_app_desc desc;
    memset(&desc, 0, sizeof desc);
    desc.title = "cuimoji";
    desc.width = cuimoji_win_w ? cuimoji_win_w : CUIMOJI_WIN_W;
    desc.height = cuimoji_win_h ? cuimoji_win_h : CUIMOJI_WIN_H;
    desc.min_width = CUIMOJI_MIN_W;
    desc.min_height = CUIMOJI_MIN_H;
    desc.fixed_size = false;
    desc.hidden = true;   /* we map it ourselves after setting WM_CLASS */
    desc.theme = theme_mode ? CUI_THEME_LIGHT : CUI_THEME_DARK;
    desc.renderer = (renderer_mode == 1 || getenv("CUIMOJI_GL")) ? CUI_RENDERER_OPENGL : CUI_RENDERER_SOFTWARE;

    cui_app *app = cui_app_create(&desc);
    if (!app) return 1;

    /* Tell XWayland/niri who we are, so the window rule and the focus check
     * have a stable app id. CUI's default is res_class "CUI". */
    xdpy = XOpenDisplay(NULL);
    if (xdpy) {
        xwin = (Window)(uintptr_t)cui_app_native_window(app);
        XClassHint ch;
        ch.res_name = (char *)"cuimoji";
        ch.res_class = (char *)"cuimoji";
        XSetClassHint(xdpy, xwin, &ch);
        XSizeHints *hints = XAllocSizeHints();
        if (hints) {
            hints->flags = PMinSize | PMaxSize;
            hints->min_width = CUIMOJI_MIN_W;
            hints->min_height = CUIMOJI_MIN_H;
            hints->max_width = CUIMOJI_MAX_W;
            hints->max_height = CUIMOJI_MAX_H;
            XSetWMNormalHints(xdpy, xwin, hints);
            XFree(hints);
        }
        if (cuimoji_win_pos_valid) XMoveWindow(xdpy, xwin, cuimoji_win_x, cuimoji_win_y);
        XMapRaised(xdpy, xwin);
        XFlush(xdpy);
    }

    cuimoji_font_apply(cui_app_ui(app));
    cuimoji_hide_window = x11_hide;

    cuimoji_quit_requested = false;
    while (cui_app_frame(app) && !cuimoji_quit_requested && !cuimoji_toggle_requested)
        cuimoji_ui(cui_app_ui(app));

    cuimoji_state_save();
    uinput_keyboard_close(uinput_fd);
    if (xdpy) XCloseDisplay(xdpy);
    cui_app_destroy(app);
    return 0;
}

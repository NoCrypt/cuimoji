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
 *     (using the compositor IPC when available) so the paste lands in the
 *     right app;
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

static void alias_norm(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    bool space = false;
    while (*src && n + 1 < cap) {
        unsigned char c = (unsigned char)*src++;
        if (c == '_' || c == '-' || c == ' ' || c == '\t') {
            if (n) space = true;
            continue;
        }
        if (space && n + 1 < cap) dst[n++] = ' ';
        space = false;
        dst[n++] = (char)alias_lower(c);
    }
    dst[n] = 0;
}

/* Levenshtein distance, capped; also treats one adjacent swap as distance 1. */
static int alias_distance(const char *a, int la, const char *b, int lb, int cap) {
    if (la > 63 || lb > 63 || abs(la - lb) > cap) return cap + 1;
    int prev[64], cur[64];
    for (int j = 0; j <= lb; j++) prev[j] = j;
    for (int i = 1; i <= la; i++) {
        cur[0] = i;
        for (int j = 1; j <= lb; j++) {
            int cost = a[i - 1] != b[j - 1];
            int v = prev[j] + 1;
            if (cur[j - 1] + 1 < v) v = cur[j - 1] + 1;
            if (prev[j - 1] + cost < v) v = prev[j - 1] + cost;
            cur[j] = v;
        }
        memcpy(prev, cur, sizeof(int) * (size_t)(lb + 1));
    }
    int d = prev[lb];
    if (d == 2 && la == lb) {
        for (int i = 0; i + 1 < la; i++)
            if (a[i] == b[i + 1] && a[i + 1] == b[i]) {
                bool same = true;
                for (int k = 0; k < la && same; k++)
                    if (k != i && k != i + 1 && a[k] != b[k]) same = false;
                if (same) return 1;
            }
    }
    return d;
}

static int alias_score_text(const char *text, const char *query, bool shortcode) {
    char full[512];
    alias_norm(full, sizeof full, text);
    int ql = (int)strlen(query);
    if (!full[0] || !ql) return 0;
    int best = 0;
    int exact = shortcode ? 1100 : 650;
    int prefix = shortcode ? 950 : 550;
    int contains = shortcode ? 800 : 400;
    int word_exact = shortcode ? 1100 : 600;
    int word_prefix = shortcode ? 900 : 500;
    int word_contains = shortcode ? 750 : 350;
    if (strcmp(full, query) == 0) best = exact;
    else if (strncmp(full, query, (size_t)ql) == 0) best = prefix;
    else if (strstr(full, query)) best = contains;

    bool fuzzy = !strchr(query, ' ') && ql >= 3;
    char work[512];
    snprintf(work, sizeof work, "%s", full);
    for (char *tok = work; tok && *tok;) {
        char *sp = strchr(tok, ' ');
        if (sp) *sp = 0;
        int tl = (int)strlen(tok);
        if (tl) {
            if (strcmp(tok, query) == 0) { if (word_exact > best) best = word_exact; }
            else if (strncmp(tok, query, (size_t)(ql < tl ? ql : tl)) == 0 && ql <= tl) {
                if (word_prefix > best) best = word_prefix;
            } else if (strstr(tok, query)) { if (word_contains > best) best = word_contains; }
            if (fuzzy && abs(tl - ql) <= 2) {
                int cap = ql >= 6 ? 2 : 1;
                int d = alias_distance(query, ql, tok, tl, cap);
                if (d == 1) {
                    int s = shortcode ? 300 : 250;
                    if (s > best) best = s;
                } else if (d == 2 && ql >= 6) {
                    int s = shortcode ? 180 : 150;
                    if (s > best) best = s;
                }
            }
        }
        tok = sp ? sp + 1 : NULL;
    }
    return best;
}

/* Strong version of CUI's weak alias-score hook. Shortcodes beat tags, and
 * single-word typos are accepted at a lower score. */
int cui__em_alias_score(const char *emoji, int len, const char *query) {
    char q[128];
    alias_norm(q, sizeof q, query);
    if (!q[0]) return 0;
    int best = 0;
    const char *shortcodes = cuimoji_shortcode_lookup(emoji, len);
    if (shortcodes) best = alias_score_text(shortcodes, q, true);
    const char *aliases = cuimoji_alias_lookup(emoji, len);
    if (aliases) {
        int s = alias_score_text(aliases, q, false);
        if (s > best) best = s;
    }
    return best;
}
#endif

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
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
static int paste_delay_ms = 20;   /* fallback when the compositor cannot be asked */
static bool paste_after_pick = true;
static int theme_mode;            /* 0 dark, 1 light */
static int platform_mode;         /* 0 auto, 1 wayland (SDL2), 2 X11 */
static bool initial_focus_done;
static bool debug_on;
static int uinput_fd = -1;
static Display *xdpy;
static Window xwin;
static cui_app *x11_app;
static int x11_ui_scale = -1;
static char paste_target[96];
static float footer_y;
static float footer_h = 34;

bool cuimoji_quit_requested;
volatile sig_atomic_t cuimoji_toggle_requested;
void (*cuimoji_hide_window)(void);
int cuimoji_win_w, cuimoji_win_h, cuimoji_win_x, cuimoji_win_y;
bool cuimoji_win_pos_valid;
float cuimoji_ui_zoom = 1.0f;

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
    /* compositors report the X11 WM_CLASS; CUI's default class is "CUI", the title
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

static bool json_string(const char *buf, const char *key, char *out, size_t cap) {
    out[0] = 0;
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *s = strstr(buf, pat);
    if (!s) return false;
    s += strlen(pat);
    while (*s == ' ' || *s == '\t') s++;
    if (*s++ != ':') return false;
    while (*s == ' ' || *s == '\t') s++;
    if (*s++ != '"') return false;
    const char *e = s;
    while (*e && *e != '"') {
        if (*e == '\\' && e[1]) e++;
        e++;
    }
    if (*e != '"') return false;
    size_t len = (size_t)(e - s);
    if (len >= cap) len = cap - 1;
    memcpy(out, s, len);
    out[len] = 0;
    return out[0] != 0;
}

/* niri, Mango and Hyprland all export a signature when they run their IPC. */
static bool have_focus_ipc(void) {
    return getenv("NIRI_SOCKET") || getenv("MANGO_INSTANCE_SIGNATURE") ||
           getenv("HYPRLAND_INSTANCE_SIGNATURE") || getenv("SWAYSOCK");
}

static bool focused_app_id(char *out, size_t cap) {
    out[0] = 0;
    if (getenv("SWAYSOCK")) {
        FILE *p = popen("swaymsg -t get_tree -r 2>/dev/null | "
                        "jq -r '.. | objects | select(.focused == true) | "
                        "(.app_id // .window_properties.class // empty)' 2>/dev/null | "
                        "head -n 1", "r");
        if (!p) return false;
        char buf[512];
        size_t n = fread(buf, 1, sizeof buf - 1, p);
        buf[n] = 0;
        pclose(p);
        buf[strcspn(buf, "\r\n")] = 0;
        if (!buf[0]) return false;
        snprintf(out, cap, "%s", buf);
        return true;
    }
    const char *cmd = NULL, *key = NULL;
    if (getenv("NIRI_SOCKET")) {
        cmd = "niri msg --json focused-window 2>/dev/null";
        key = "app_id";
    } else if (getenv("MANGO_INSTANCE_SIGNATURE")) {
        cmd = "mmsg get focusing-client 2>/dev/null";
        key = "appid";
    } else if (getenv("HYPRLAND_INSTANCE_SIGNATURE")) {
        cmd = "hyprctl activewindow -j 2>/dev/null";
        key = "class";
    }
    if (!cmd) return false;

    FILE *p = popen(cmd, "r");
    if (!p) return false;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1, p);
    buf[n] = 0;
    pclose(p);

    if (json_string(buf, key, out, cap)) return true;
    /* Hyprland uses initialClass for some XWayland clients. */
    if (strcmp(key, "class") == 0) return json_string(buf, "initialClass", out, cap);
    return false;
}

static double wait_for_focus(char *target, size_t cap, double start) {
    int delay_ms = paste_delay_ms;
    const char *env = getenv("CUIMOJI_PASTE_DELAY_MS");
    if (env && atoi(env) >= 0) delay_ms = atoi(env);

    if (have_focus_ipc()) {
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

/* ---- theme file -----------------------------------------------------
 * Optional overrides beside the state file. Every field of cui_theme is
 * exposed; the file is regenerated (fully commented) when missing, and the
 * values in it win over the active theme and the Settings. */

typedef struct {
    const char *name;
    size_t off;   /* offset of a cui_color inside cui_theme */
} theme_color_def;

#define THEME_COLOR_OFF(tok) (offsetof(cui_theme, colors) + (size_t)(tok) * sizeof(cui_color))

static const theme_color_def theme_color_defs[] = {
    { "background", THEME_COLOR_OFF(CUI_TOKEN_BACKGROUND) },
    { "surface", THEME_COLOR_OFF(CUI_TOKEN_SURFACE) },
    { "surface_raised", THEME_COLOR_OFF(CUI_TOKEN_SURFACE_RAISED) },
    { "surface_muted", THEME_COLOR_OFF(CUI_TOKEN_SURFACE_MUTED) },
    { "foreground", THEME_COLOR_OFF(CUI_TOKEN_FOREGROUND) },
    { "text_secondary", THEME_COLOR_OFF(CUI_TOKEN_TEXT_SECONDARY) },
    { "text_muted", THEME_COLOR_OFF(CUI_TOKEN_TEXT_MUTED) },
    { "border", THEME_COLOR_OFF(CUI_TOKEN_BORDER) },
    { "border_subtle", THEME_COLOR_OFF(CUI_TOKEN_BORDER_SUBTLE) },
    { "border_strong", THEME_COLOR_OFF(CUI_TOKEN_BORDER_STRONG) },
    { "accent", THEME_COLOR_OFF(CUI_TOKEN_ACCENT) },
    { "accent_strong", THEME_COLOR_OFF(CUI_TOKEN_ACCENT_STRONG) },
    { "accent_subtle", THEME_COLOR_OFF(CUI_TOKEN_ACCENT_SUBTLE) },
    { "accent_foreground", THEME_COLOR_OFF(CUI_TOKEN_ACCENT_FOREGROUND) },
    { "success", THEME_COLOR_OFF(CUI_TOKEN_SUCCESS) },
    { "warning", THEME_COLOR_OFF(CUI_TOKEN_WARNING) },
    { "danger", THEME_COLOR_OFF(CUI_TOKEN_DANGER) },
    { "info", THEME_COLOR_OFF(CUI_TOKEN_INFO) },
    { "control_on", THEME_COLOR_OFF(CUI_TOKEN_CONTROL_ON) },
    { "control_glyph", THEME_COLOR_OFF(CUI_TOKEN_CONTROL_GLYPH) },
    { "control_track", THEME_COLOR_OFF(CUI_TOKEN_CONTROL_TRACK) },
    { "control_track_hover", THEME_COLOR_OFF(CUI_TOKEN_CONTROL_TRACK_HOVER) },
    { "control_thumb", THEME_COLOR_OFF(CUI_TOKEN_CONTROL_THUMB) },
    { "control_thumb_on", THEME_COLOR_OFF(CUI_TOKEN_CONTROL_THUMB_ON) },
    { "hover", THEME_COLOR_OFF(CUI_TOKEN_HOVER) },
    { "active", THEME_COLOR_OFF(CUI_TOKEN_ACTIVE) },
    { "scrim", THEME_COLOR_OFF(CUI_TOKEN_SCRIM) },
    { "focus_ring", THEME_COLOR_OFF(CUI_TOKEN_FOCUS_RING) },
    { "series_1", THEME_COLOR_OFF(CUI_TOKEN_SERIES_1) },
    { "series_2", THEME_COLOR_OFF(CUI_TOKEN_SERIES_2) },
    { "series_3", THEME_COLOR_OFF(CUI_TOKEN_SERIES_3) },
    { "series_4", THEME_COLOR_OFF(CUI_TOKEN_SERIES_4) },
    { "series_5", THEME_COLOR_OFF(CUI_TOKEN_SERIES_5) },
    { "series_6", THEME_COLOR_OFF(CUI_TOKEN_SERIES_6) },
    { "tone_sky", offsetof(cui_theme, tone_sky) },
    { "tone_teal", offsetof(cui_theme, tone_teal) },
    { "tone_green", offsetof(cui_theme, tone_green) },
    { "tone_amber", offsetof(cui_theme, tone_amber) },
    { "tone_rose", offsetof(cui_theme, tone_rose) },
    { "tone_orchid", offsetof(cui_theme, tone_orchid) },
    { "tone_violet", offsetof(cui_theme, tone_violet) },
};
#define THEME_COLOR_COUNT (sizeof theme_color_defs / sizeof theme_color_defs[0])

typedef struct {
    const char *name;
    size_t off;   /* offset of a float inside cui_theme */
} theme_num_def;

#define THEME_SHADOW_OFF(set, idx, field) \
    (offsetof(cui_theme, set) + (size_t)(idx) * sizeof(cui_shadow) + offsetof(cui_shadow, field))

static const theme_num_def theme_num_defs[] = {
    { "radius_control", offsetof(cui_theme, radius_control) },
    { "radius_panel", offsetof(cui_theme, radius_panel) },
    { "radius_surface", offsetof(cui_theme, radius_surface) },
    { "control_sm", offsetof(cui_theme, control_sm) },
    { "control_md", offsetof(cui_theme, control_md) },
    { "control_lg", offsetof(cui_theme, control_lg) },
    { "text_xs", offsetof(cui_theme, text_xs) },
    { "text_sm", offsetof(cui_theme, text_sm) },
    { "text_body", offsetof(cui_theme, text_body) },
    { "text_lg", offsetof(cui_theme, text_lg) },
    { "heading_1", offsetof(cui_theme, heading) + 0 * sizeof(float) },
    { "heading_2", offsetof(cui_theme, heading) + 1 * sizeof(float) },
    { "heading_3", offsetof(cui_theme, heading) + 2 * sizeof(float) },
    { "heading_4", offsetof(cui_theme, heading) + 3 * sizeof(float) },
    { "motion", offsetof(cui_theme, motion) },
    { "font_scale", offsetof(cui_theme, font_scale) },
    { "shadow_resting_1_y", THEME_SHADOW_OFF(shadow_resting, 0, y) },
    { "shadow_resting_1_blur", THEME_SHADOW_OFF(shadow_resting, 0, blur) },
    { "shadow_resting_1_spread", THEME_SHADOW_OFF(shadow_resting, 0, spread) },
    { "shadow_resting_1_alpha", THEME_SHADOW_OFF(shadow_resting, 0, alpha) },
    { "shadow_resting_2_y", THEME_SHADOW_OFF(shadow_resting, 1, y) },
    { "shadow_resting_2_blur", THEME_SHADOW_OFF(shadow_resting, 1, blur) },
    { "shadow_resting_2_spread", THEME_SHADOW_OFF(shadow_resting, 1, spread) },
    { "shadow_resting_2_alpha", THEME_SHADOW_OFF(shadow_resting, 1, alpha) },
    { "shadow_raised_1_y", THEME_SHADOW_OFF(shadow_raised, 0, y) },
    { "shadow_raised_1_blur", THEME_SHADOW_OFF(shadow_raised, 0, blur) },
    { "shadow_raised_1_spread", THEME_SHADOW_OFF(shadow_raised, 0, spread) },
    { "shadow_raised_1_alpha", THEME_SHADOW_OFF(shadow_raised, 0, alpha) },
    { "shadow_raised_2_y", THEME_SHADOW_OFF(shadow_raised, 1, y) },
    { "shadow_raised_2_blur", THEME_SHADOW_OFF(shadow_raised, 1, blur) },
    { "shadow_raised_2_spread", THEME_SHADOW_OFF(shadow_raised, 1, spread) },
    { "shadow_raised_2_alpha", THEME_SHADOW_OFF(shadow_raised, 1, alpha) },
    { "shadow_floating_1_y", THEME_SHADOW_OFF(shadow_floating, 0, y) },
    { "shadow_floating_1_blur", THEME_SHADOW_OFF(shadow_floating, 0, blur) },
    { "shadow_floating_1_spread", THEME_SHADOW_OFF(shadow_floating, 0, spread) },
    { "shadow_floating_1_alpha", THEME_SHADOW_OFF(shadow_floating, 0, alpha) },
    { "shadow_floating_2_y", THEME_SHADOW_OFF(shadow_floating, 1, y) },
    { "shadow_floating_2_blur", THEME_SHADOW_OFF(shadow_floating, 1, blur) },
    { "shadow_floating_2_spread", THEME_SHADOW_OFF(shadow_floating, 1, spread) },
    { "shadow_floating_2_alpha", THEME_SHADOW_OFF(shadow_floating, 1, alpha) },
};
#define THEME_NUM_COUNT (sizeof theme_num_defs / sizeof theme_num_defs[0])

static cui_color theme_color_vals[THEME_COLOR_COUNT];
static uint64_t theme_color_mask;
static float theme_num_vals[THEME_NUM_COUNT];
static uint64_t theme_num_mask;
static bool theme_ready;

static void theme_path_for_mode(int mode, char *out, size_t cap) {
    char dir[1024];
    state_dir(dir, sizeof dir);
    snprintf(out, cap, "%s/theme-%s", dir, mode ? "light" : "dark");
}

static void theme_path(char *out, size_t cap) {
    theme_path_for_mode(theme_mode, out, cap);
}

static void theme_legacy_path(char *out, size_t cap) {
    char dir[1024];
    state_dir(dir, sizeof dir);
    snprintf(out, cap, "%s/theme", dir);
}

static void theme_write_template(const char *path, const cui_theme *t, const char *mode_name) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f,
            "# cuimoji theme overrides for the %s theme\n"
            "# Uncomment a line and edit its value. Values here win over this mode's\n"
            "# theme and over the Settings. Delete this file to regenerate the\n"
            "# commented reference values.\n"
            "#\n"
            "# Colors are #rrggbb or #rrggbbaa. On the Wayland window the alpha\n"
            "# channel is composited: a transparent background or a translucent\n"
            "# surface shows the desktop (and compositor blur) behind the picker.\n"
            "# The X11 window stays opaque. Sizes are points/pixels, motion and\n"
            "# font_scale are multipliers. font_scale is a text multiplier on\n"
            "# top of the UI size. Changes are picked up while cuimoji runs.\n\n"
            "# --- semantic colors -------------------------------------------\n", mode_name);
    for (size_t i = 0; i < THEME_COLOR_COUNT; i++) {
        const cui_color *c = (const cui_color *)((const char *)t + theme_color_defs[i].off);
        fprintf(f, "# %s = #%02x%02x%02x%02x\n", theme_color_defs[i].name,
                c->r, c->g, c->b, c->a);
    }
    fprintf(f, "\n# --- sizes and metrics -----------------------------------------\n");
    for (size_t i = 0; i < THEME_NUM_COUNT; i++) {
        float v = *(const float *)((const char *)t + theme_num_defs[i].off);
        fprintf(f, "# %s = %g\n", theme_num_defs[i].name, (double)v);
    }
    fclose(f);
}

static char *theme_trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

static bool theme_parse_color(const char *s, cui_color *out) {
    if (*s != '#') return false;
    s++;
    size_t n = strlen(s);
    unsigned r, g, b, a = 255;
    if (n == 6) {
        if (sscanf(s, "%2x%2x%2x", &r, &g, &b) != 3) return false;
    } else if (n == 8) {
        if (sscanf(s, "%2x%2x%2x%2x", &r, &g, &b, &a) != 4) return false;
    } else {
        return false;
    }
    *out = (cui_color){ (uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)a };
    return true;
}

static void theme_load(void) {
    theme_color_mask = 0;
    theme_num_mask = 0;
    char path[1200];
    theme_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *s = theme_trim(line);
        if (!*s || *s == '#') continue;
        char *eq = strchr(s, '=');
        if (!eq) {
            fprintf(stderr, "cuimoji: theme: expected 'key = value': %s\n", s);
            continue;
        }
        *eq = 0;
        char *key = theme_trim(s);
        char *val = theme_trim(eq + 1);
        bool known = false;
        for (size_t i = 0; i < THEME_COLOR_COUNT; i++) {
            if (strcmp(key, theme_color_defs[i].name) != 0) continue;
            if (theme_parse_color(val, &theme_color_vals[i]))
                theme_color_mask |= 1ull << i;
            else
                fprintf(stderr, "cuimoji: theme: bad color for %s: %s\n", key, val);
            known = true;
            break;
        }
        if (known) continue;
        for (size_t i = 0; i < THEME_NUM_COUNT; i++) {
            if (strcmp(key, theme_num_defs[i].name) != 0) continue;
            char *end;
            float v = strtof(val, &end);
            if (end != val) {
                theme_num_vals[i] = v;
                theme_num_mask |= 1ull << i;
            } else {
                fprintf(stderr, "cuimoji: theme: bad number for %s: %s\n", key, val);
            }
            known = true;
            break;
        }
        if (!known) fprintf(stderr, "cuimoji: theme: unknown key: %s\n", key);
    }
    fclose(f);
}

static time_t theme_mtime;
static long theme_nsec;
static off_t theme_size;
static double theme_checked;
static bool theme_need_template;
static double theme_template_at;

static void theme_stat_track(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        theme_mtime = st.st_mtime;
        theme_nsec = st.st_mtim.tv_nsec;
        theme_size = st.st_size;
    } else {
        theme_mtime = 0;
        theme_nsec = 0;
        theme_size = 0;
    }
    theme_checked = now_ms();
}

static bool theme_copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "r");
    if (!in) return false;
    FILE *out = fopen(dst, "w");
    if (!out) { fclose(in); return false; }
    char buf[4096];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    if (ferror(in)) ok = false;
    if (fclose(in) != 0) ok = false;
    if (fclose(out) != 0) ok = false;
    if (!ok) remove(dst);
    return ok;
}

static void cuimoji_theme_init(cui_ctx *ui) {
    char path[1200], legacy[1200];
    theme_path(path, sizeof path);
    theme_legacy_path(legacy, sizeof legacy);
    FILE *f = fopen(path, "r");
    if (f) {
        fclose(f);
    } else {
        /* Upgrade the pre-split "theme" file to the mode that is active now. */
        FILE *old = fopen(legacy, "r");
        if (old) {
            fclose(old);
            if (rename(legacy, path) != 0 && !theme_copy_file(legacy, path))
                theme_write_template(path, cui_theme_get(ui), theme_mode ? "Light" : "Dark");
        } else {
            theme_write_template(path, cui_theme_get(ui), theme_mode ? "Light" : "Dark");
        }
    }
    theme_load();
    theme_stat_track(path);
}

/* Pick up edits to the active mode's theme file without a restart. */
static void cuimoji_theme_watch(void) {
    double now = now_ms();
    if (now - theme_checked < 500.0) return;
    theme_checked = now;
    char path[1200];
    theme_path(path, sizeof path);
    struct stat st;
    if (stat(path, &st) != 0) {
        /* The file was removed: drop its overrides and recreate the template. */
        if (theme_color_mask || theme_num_mask) theme_load();
        if (!theme_need_template) {
            theme_need_template = true;
            theme_template_at = now;
        }
        return;
    }
    if (st.st_mtime == theme_mtime && st.st_mtim.tv_nsec == theme_nsec &&
        st.st_size == theme_size) return;
    theme_stat_track(path);
    theme_load();
}

/* Generate a mode's reference file once its theme animation has settled. */
static void cuimoji_theme_pending(cui_ctx *ui) {
    if (!theme_need_template) return;
    if (now_ms() - theme_template_at < 800.0) return;
    char path[1200];
    theme_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (f) fclose(f);
    else theme_write_template(path, cui_theme_get(ui), theme_mode ? "Light" : "Dark");
    theme_load();
    theme_need_template = false;
    theme_stat_track(path);
}

static void cuimoji_theme_mode_changed(void) {
    char path[1200];
    theme_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (f) {
        fclose(f);
        theme_need_template = false;
    } else {
        theme_need_template = true;
        theme_template_at = now_ms();
    }
    theme_load();
    theme_stat_track(path);
}

/* Applied after the theme/font settings each frame so file values win. */
static void cuimoji_theme_apply(cui_ctx *ui) {
    if (!theme_color_mask && !theme_num_mask) return;
    cui_theme *t = cui_theme_get(ui);
    for (size_t i = 0; i < THEME_COLOR_COUNT; i++)
        if (theme_color_mask & (1ull << i))
            *(cui_color *)((char *)t + theme_color_defs[i].off) = theme_color_vals[i];
    size_t font_scale_off = offsetof(cui_theme, font_scale);
    for (size_t i = 0; i < THEME_NUM_COUNT; i++) {
        if (!(theme_num_mask & (1ull << i))) continue;
        /* font_scale is a text multiplier over the UI size */
        if (theme_num_defs[i].off == font_scale_off)
            t->font_scale *= theme_num_vals[i];
        else
            *(float *)((char *)t + theme_num_defs[i].off) = theme_num_vals[i];
    }
}

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
        } else if (strncmp(line, "paste=", 6) == 0) {
            paste_after_pick = atoi(line + 6) != 0;
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
            if (ui_scale < 90) ui_scale = 90;
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
            "platform=%s\nrenderer=%s\npaste=%d\n",
            tone, recent, selected_font, emoji_size, ui_scale, paste_delay_ms,
            theme_mode ? "light" : "dark",
            platform_mode == 1 ? "wayland" : platform_mode == 2 ? "x11" : "auto",
            renderer_mode == 1 ? "opengl" : renderer_mode == 2 ? "vulkan" : "software",
            paste_after_pick ? 1 : 0);
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
    if (getenv("CUIMOJI_DEBUG")) fprintf(stderr, "cuimoji: font %s = %s\n", label, path);
    font_count++;
}

static bool font_label_exists(const char *label) {
    for (int i = 0; i < font_count; i++)
        if (strcmp(font_labels[i], label) == 0) return true;
    return false;
}

typedef struct { int nfont; void **fonts; } fc_fontset;

/* Optional fontconfig discovery: find the emoji families wherever the distro
 * installed them, instead of relying on the two or three known paths. The
 * library is loaded at runtime, so systems without it still work. */
static void fontconfig_scan(void) {
    void *lib = dlopen("libfontconfig.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!lib) return;
    int (*fc_init)(void) = (int (*)(void))dlsym(lib, "FcInit");
    void *(*pattern_create)(void) = (void *(*)(void))dlsym(lib, "FcPatternCreate");
    void (*pattern_destroy)(void *) = (void (*)(void *))dlsym(lib, "FcPatternDestroy");
    int (*pattern_add_string)(void *, const char *, const unsigned char *) =
        (int (*)(void *, const char *, const unsigned char *))dlsym(lib, "FcPatternAddString");
    int (*config_substitute)(void *, void *, int) =
        (int (*)(void *, void *, int))dlsym(lib, "FcConfigSubstitute");
    void (*default_substitute)(void *) = (void (*)(void *))dlsym(lib, "FcDefaultSubstitute");
    void *(*font_match)(void *, void *, int *) =
        (void *(*)(void *, void *, int *))dlsym(lib, "FcFontMatch");
    int (*pattern_get_string)(void *, const char *, int, unsigned char **) =
        (int (*)(void *, const char *, int, unsigned char **))dlsym(lib, "FcPatternGetString");
    void *(*config_get_current)(void) = (void *(*)(void))dlsym(lib, "FcConfigGetCurrent");
    void *(*font_list)(void *, void *, void *) = (void *(*)(void *, void *, void *))dlsym(lib, "FcFontList");
    void *(*object_set_create)(void) = (void *(*)(void))dlsym(lib, "FcObjectSetCreate");
    int (*object_set_add)(void *, const char *) = (int (*)(void *, const char *))dlsym(lib, "FcObjectSetAdd");
    void (*object_set_destroy)(void *) = (void (*)(void *))dlsym(lib, "FcObjectSetDestroy");
    void (*font_set_destroy)(void *) = (void (*)(void *))dlsym(lib, "FcFontSetDestroy");
    int (*pattern_get_bool)(void *, const char *, int, int *) =
        (int (*)(void *, const char *, int, int *))dlsym(lib, "FcPatternGetBool");
    int (*pattern_get_charset)(void *, const char *, int, void **) =
        (int (*)(void *, const char *, int, void **))dlsym(lib, "FcPatternGetCharSet");
    int (*charset_has_char)(void *, unsigned int) =
        (int (*)(void *, unsigned int))dlsym(lib, "FcCharSetHasChar");
    if (!fc_init || !pattern_create || !pattern_destroy || !pattern_add_string ||
        !config_substitute || !default_substitute || !font_match ||
        !pattern_get_string || !config_get_current || !fc_init()) {
        dlclose(lib);
        return;
    }
    void *config = config_get_current();
    static const char *const families[][2] = {
        { "Apple Color Emoji", "Apple Color Emoji" },
        { "Noto Color Emoji", "Noto Color Emoji" },
        { "Fluent Emoji", "Fluent Emoji" },
        { "Fluent Emoji Flat", "Fluent Emoji Flat" },
    };
    for (size_t i = 0; i < sizeof families / sizeof families[0]; i++) {
        void *pat = pattern_create();
        if (!pat) continue;
        pattern_add_string(pat, "family", (const unsigned char *)families[i][0]);
        config_substitute(config, pat, 0);   /* FcMatchPattern */
        default_substitute(pat);
        int result = 1;   /* FcResultNoMatch */
        void *match = font_match(config, pat, &result);
        if (match) {
            unsigned char *family = NULL, *file = NULL;
            if (pattern_get_string(match, "family", 0, &family) == 0 && family &&
                strcasecmp((const char *)family, families[i][0]) == 0 &&
                pattern_get_string(match, "file", 0, &file) == 0 && file)
                add_font(families[i][1], (const char *)file);
            pattern_destroy(match);
        }
        pattern_destroy(pat);
    }

    /* Also list any other installed font that advertises color glyphs and has
     * an emoji character (One UI Emoji, OpenMoji, Twemoji, JoyPixels, ...). */
    if (font_list && object_set_create && object_set_add && object_set_destroy &&
        font_set_destroy && pattern_get_bool && pattern_get_charset && charset_has_char) {
        void *pat = pattern_create();
        void *os = object_set_create();
        if (pat && os) {
            object_set_add(os, "family");
            object_set_add(os, "file");
            object_set_add(os, "color");
            object_set_add(os, "charset");
            fc_fontset *set = font_list(config, pat, os);
            if (set) {
                for (int i = 0; i < set->nfont; i++) {
                    void *font = set->fonts[i];
                    unsigned char *family = NULL, *file = NULL;
                    void *charset = NULL;
                    int color = 0;
                    if (pattern_get_string(font, "family", 0, &family) != 0 || !family || !family[0]) continue;
                    if (pattern_get_string(font, "file", 0, &file) != 0 || !file) continue;
                    pattern_get_bool(font, "color", 0, &color);
                    bool has_emoji = pattern_get_charset(font, "charset", 0, &charset) == 0 && charset &&
                        (charset_has_char(charset, 0x1F600) || charset_has_char(charset, 0x1F44D));
                    bool name_emoji = strcasestr((const char *)family, "emoji") ||
                                      strcasestr((const char *)family, "moji");
                    if (!has_emoji || (!color && !name_emoji)) continue;
                    if (font_label_exists((const char *)family)) continue;
                    add_font((const char *)family, (const char *)file);
                }
                font_set_destroy(set);
            }
        }
        if (pat) pattern_destroy(pat);
        if (os) object_set_destroy(os);
    }
    dlclose(lib);
}

void cuimoji_fonts_scan(void) {
    add_font("System", "");

    static const char *const known[] = {
        "/usr/share/fonts/apple-color-emoji/apple-color-emoji.ttf",
        "/usr/share/fonts/truetype/apple-color-emoji/apple-color-emoji.ttf",
        "/usr/share/fonts/noto/NotoColorEmoji.ttf",
        "/usr/share/fonts/fluent-emoji/FluentEmojiColor.ttf",
        "/usr/share/fonts/fluent-emoji/FluentEmojiFlat.ttf",
        "/usr/share/fonts/fluent-emoji/FluentEmojiHighContrast.ttf",
    };
    static const char *const labels[] = {
        "Apple Color Emoji",
        "Apple Color Emoji",
        "Noto Color Emoji",
        "Fluent Emoji",
        "Fluent Emoji Flat",
        "Fluent Emoji High Contrast",
    };
    for (size_t i = 0; i < sizeof known / sizeof known[0]; i++)
        if (access(known[i], R_OK) == 0) add_font(labels[i], known[i]);
    fontconfig_scan();

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
    /* The group tabs use Lucide icons beyond CUI's embedded subset. Load the
     * merged icon font next to the executable once; if it is missing, CUI's
     * built-in icons stay in place. */
    static bool icon_font_loaded;
    if (!icon_font_loaded) {
        icon_font_loaded = true;
        char exe[PATH_MAX];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n > 0) {
            exe[n] = 0;
            char *slash = strrchr(exe, '/');
            if (slash) {
                *slash = 0;
                char path[PATH_MAX];
                snprintf(path, sizeof path, "%s/lucide-cuimoji.ttf", exe);
                cui_font icons = cui_font_load_file(ui, path);
                if (icons >= 0) cui_font_set_role(ui, CUI_FONT_ICONS, icons);
            }
        }
    }

    /* Some fonts (notably Apple Color Emoji) have no color data for the
     * standalone female/male/medical symbols. Load a text symbol fallback so
     * CUI can draw them instead of leaving blank cells. */
    static bool symbol_fallback_loaded;
    if (!symbol_fallback_loaded) {
        symbol_fallback_loaded = true;
        static const char *const symbol_paths[] = {
            "/usr/share/fonts/noto/NotoSansSymbols-Regular.ttf",
            "/usr/share/fonts/gsfonts/NimbusSans-Regular.otf",
            "/usr/share/fonts/TTF/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        };
        for (size_t i = 0; i < sizeof symbol_paths / sizeof symbol_paths[0]; i++) {
            if (access(symbol_paths[i], R_OK) != 0) continue;
            cui_font sym = cui_font_load_file(ui, symbol_paths[i]);
            if (sym >= 0) {
                cui_font_add_fallback(ui, CUI_FONT_BODY, sym);
                cui_font_add_fallback(ui, CUI_FONT_MEDIUM, sym);
                break;
            }
        }
    }

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

/* CUI draws the tone button at 18 px; redraw the hands a bit larger on top.
 * The group tabs are normal Lucide icons now, so they stay with CUI. */
static void draw_picker_icons(cui_ctx *ui) {
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

    if (!paste_after_pick) {
        /* wl-copy keeps owning the Wayland clipboard after this process exits. */
        cuimoji_state_save();
        cuimoji_quit_requested = true;
        return;
    }

    /* 3. Wait for focus to leave cuimoji; the compositor IPC tells us exactly. */
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
    cuimoji_ui_zoom = (float)ui_scale / 100.0f;
    if (x11_app && x11_ui_scale != ui_scale) {
        x11_ui_scale = ui_scale;
        cui_app_set_ui_scale(x11_app, (float)ui_scale / 100.0f);
    }
    if (!theme_ready) {
        cuimoji_theme_init(ui);
        theme_ready = true;
    }
    cuimoji_theme_watch();
    cuimoji_theme_pending(ui);
    /* A visible window does not repaint on its own, so wake up now and then
     * to notice theme-file edits. */
    cui_request_wakeup(ui, 1.0f);

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
        cuimoji_theme_mode_changed();
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
        cuimoji_theme_apply(ui);
        /* The grid view starts below the search row and the group tabs; its
         * y position is the chrome height (they scale with the UI size). */
        cui_rect grid_rect = cui_get_rect(ui, picker_sub_id(ui, "#grid"));
        float chrome = grid_rect.y > 20.0f ? grid_rect.y : 94.0f;
        /* CUI centers the panel's 1 px border on its edge; leave one pixel
         * inside the viewport so the bottom outline is not clipped. */
        float grid_h = ui->vh - FOOTER_H - chrome - 1.0f;
        if (grid_h < 40.0f) grid_h = 40.0f;
        footer_y = ui->vh - FOOTER_H;
        footer_h = FOOTER_H;
        float pad_bottom = 2.0f;   /* the LG settings button is 44 px */

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
                    if (cui_icon_button(ui, CUI_ICON_SETTINGS, .size = CUI_SIZE_LG,
                                        .color = CUI_COLOR_TEXT_MUTED, .tooltip = "Settings") ||
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
                        static const char *const paste_items[] = { "On", "Off" };
                        int paste_choice = paste_after_pick ? 0 : 1;
                        if (cui_select(ui, "paste-mode", paste_items, 2, &paste_choice,
                                       .label = "Paste after picking", .width = CUI_FILL)) {
                            paste_after_pick = paste_choice == 0;
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
                        if (cui_number(ui, "ui-size", &us, 90, 150,
                                       .label = "UI size", .step = 5, .suffix = " %",
                                       .width = CUI_FILL)) {
                            ui_scale = (int)(us + 0.5);
                            ui->settle_frames = 4;   /* apply the new zoom */
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

int main(int argc, char **argv) {
    if (argc > 1) {
        if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0) {
            printf("cuimoji %s\n", CUIMOJI_VERSION);
            return 0;
        }
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            printf("cuimoji %s - emoji picker\n\nUsage: cuimoji [--version]\n"
                   "Run without arguments to open the picker.\n", CUIMOJI_VERSION);
            return 0;
        }
    }
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
    x11_app = app;
    x11_ui_scale = ui_scale;
    cui_app_set_ui_scale(app, (float)ui_scale / 100.0f);

    /* Tell XWayland/the compositor who we are, so the window rule and the focus check
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

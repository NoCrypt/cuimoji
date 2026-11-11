# cuimoji

A small Emote/Smile-like emoji picker for Linux, built on
[CUI](https://github.com/yappologistic/CUI) native UI components.

The whole UI is CUI's searchable emoji grid: search, group tabs, recents, skin
tones and a preview. On top of that, cuimoji adds the desktop glue: a settings
popover, three renderers, and a fast auto-paste path.

## Features

- **Grid**: recents, nine Unicode groups, skin tones, search by name.
- **First focus on the first recent emoji** when the picker opens, so Enter
  picks your most recent emoji. Typing switches to the search field; Down goes
  back into the grid.
- **Settings** (gear in the bottom bar, or `F1`):
  - **Emoji set** — System (Noto Color Emoji), Apple Color Emoji, or any
    `.ttf`/`.otf`/`.ttc` in `~/.local/share/cuimoji/emoji/`.
    `F2` cycles sets.
  - **Emoji size** — 20 to 40 px.
  - **UI size** — Small / Default / Large.
  - **Renderer** — Software, OpenGL, Vulkan. Applied on the next launch.
- **Resizable window**: the grid and the 34 px bottom bar follow the size.
- **Fast auto-paste**: the emoji goes to the clipboard and Ctrl+V is written
  straight to a persistent `/dev/uinput` keyboard. No daemon, no portal, no
  D-Bus round trip. Terminals get Ctrl+Shift+V.
- Recents, skin tone, emoji font, sizes and renderer are saved in
  `$XDG_STATE_HOME/cuimoji/state`.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/cuimoji
```

Dependencies: `libx11`, and for the Vulkan renderer `sdl2` + `vulkan-headers`
(the option simply falls back to Software without them). CUI v1.2.0 is fetched
by CMake.

CUI is patched by `patches/cui-fast-emoji.patch`: it draws up to 256 new emoji
per frame and keeps the 1024 texture, so the grid is complete on the first
frame instead of filling in over ~10 frames.

## Key binding (niri)

```kdl
Mod+Period hotkey-overlay-title="Open cuimoji" { spawn "/home/ncpt/Project/cuimoji/build/cuimoji"; }

window-rule {
    match app-id=r#"^cuimoji$"#
    open-floating true
    default-column-width { fixed 360; }
    default-window-height { fixed 480; }
}
```

The rule keeps it floating (it is resizable, so niri would tile it
otherwise) and opens it at 360x480, the app's default size. The minimum it
can be resized to is 240x260.

## How the paste works

1. The emoji goes to the clipboard: CUI's X11 selection immediately, plus
   `wl-copy` for the Wayland clipboard (Wayland apps read that one).
2. The window is hidden so the compositor refocuses the previous window.
3. The app asks niri (`niri msg --json focused-window`) until focus has left
   cuimoji, instead of sleeping blindly.
4. Ctrl+V is sent from the uinput keyboard that was opened at startup, with
   small delays between the key events so Qt/Electron apps (Telegram,
   SereinGram) pick it up reliably.

Measured on this machine: ~80 ms from picking to the keypress (software
renderer; `wl-copy` is most of it, the key itself is instant). `CUIMOJI_DEBUG=1`
prints the breakdown (`CUIMOJI_GL=1` forces OpenGL). Resident memory: about
8 MB PSS with the software renderer.

## Notes

- `/dev/uinput` needs the user in the `input` group (as with ydotool); if it
  cannot be opened the app falls back to running `ydotool`.
- `wl-copy` (wl-clipboard) is needed to keep the clipboard after the app exits.
- `niri` is optional: without it the app waits a short fixed delay before
  pasting.
- `vulkan/cui_vulkan.c`, `vulkan/cui_vulkan.h` and `vulkan/ui_spv.h` are the
  small Vulkan renderer scaffold from the CUI mosaic demo (`cui-hello/vulkan`),
  reused here as the Vulkan backend.

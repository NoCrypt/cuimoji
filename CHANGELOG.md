# Changelog

All notable changes to cuimoji are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions use
[Semantic Versioning](https://semver.org/).

## [0.3] - 2026-11-12

### Added
- One-line installer that downloads the latest release, installs per-user in
  `~/.local/lib/cuimoji`, adds a `cuimoji` command and an app-menu entry, and
  installs missing SDL2/Wayland/X11/Vulkan and `wl-clipboard` packages.
- Running the installer again offers **Upgrade**, **Remove**, or **Cancel**.
- Fontconfig-based emoji font discovery: Apple Color Emoji, Noto Color Emoji,
  Fluent Emoji (Color, Flat, High Contrast), One UI Emoji, OpenMoji, Twemoji,
  JoyPixels, and other installed emoji fonts.
- High-contrast and monochrome emoji fonts are accepted; monochrome fonts use
  the current text color.
- Release packages: AppImage, Flatpak bundle, and Arch Linux package.

### Changed
- New Smile app icon.
- Emoji font list no longer depends only on a few hardcoded paths.

## [0.2] - 2026-11-12

### Added
- Emojibase search aliases: names, tags, and shortcodes such as “burger”,
  “hungry”, and “rofl”.
- Hover preview and the emoji name in the bottom bar.
- Settings popover: theme, emoji set, emoji size, UI size, paste delay,
  renderer, and window system.
- Theme file at `$XDG_STATE_HOME/cuimoji/theme` for colors, tones, radii,
  control sizes, text sizes, shadows, motion, and font scale.
- UI size as a true zoom for the whole interface.
- Apple Color Emoji skin-tone support.
- Single-instance toggle and remembered window size (and X11 position).
- Lucide group-tab icons and a larger settings button.
- Resizable window with 240×240 minimum and 770×770 maximum.
- Idle-friendly rendering: no frames while nothing changes.

### Fixed
- The bottom window outline no longer clips.
- Apple Color Emoji’s blank toned variants now render.
- Text-presentation symbols ♀, ♂, and ⚕ fall back to a symbol font.
- Search-field and dropdown outlines match the app chrome.

## [0.1] - 2026-11-11

- Initial release: CUI emoji grid with recents, groups, search, and skin tones.
- Native Wayland and X11 windows.
- Fast auto-paste through `/dev/uinput`, with no daemon or portal.
- Settings and state persistence.

[0.3]: https://github.com/NoCrypt/cuimoji/releases/tag/v0.3
[0.2]: https://github.com/NoCrypt/cuimoji/releases/tag/v0.2
[0.1]: https://github.com/NoCrypt/cuimoji/releases/tag/v0.1

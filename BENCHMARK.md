# Benchmark

Measured memory and size of cuimoji against other Linux emoji pickers.

## Environment

- Arch Linux, niri 26.04 on Wayland, 1920×1080, 16 GiB RAM.
- **cuimoji** 0.5, Release build (stripped) from `build/cuimoji`, plus the Flatpak build.
- **rofi** 2.0.0 + **rofi-emoji** 4.1.0
- **fuzzel** 1.15.0
- **wofi** 1.5.3
- **rofimoji** 6.8.0
- **bemoji** 0.4.0
- **wofi-emoji** 1.2
- **bemenu** 0.6.23 (Wayland renderer)
- **Smile** 2.12.2 (Flatpak)
- **Emote** 5.2.0 (Flatpak)

## Method

Memory is the sum of `Pss` from `/proc/<pid>/smaps_rollup` over the picker's
whole process tree, sampled after the window opened and left idle; the peak over
roughly five seconds is reported. `Rss` is reported alongside it. PSS splits
shared library pages between processes, so it is the fairer number when
comparing separate applications; RSS is what `ps` and systemd show.

- The picker was open and idle; no search text was typed.
- Text selectors were fed an emoji list on stdin. List sizes: rofi-emoji
  5,042 lines; bemoji 3,963; wofi-emoji 1,870. Fuzzel was measured with
  1,870, 3,963, and 5,042-line lists.
- Flatpak numbers include the `bwrap` and `xdg-dbus-proxy` processes.
- Smile and Emote run as daemons. Their long-idle RSS can be lower than their
  working set because pages are swapped out; systemd recorded cgroup peaks of
  297.7 MiB for Smile and 284.8 MiB for Emote. The open values below are hot
  measurements.
- Sizes are the picker's executable or script plus its own data. Frontends,
  interpreters, and Flatpak runtimes are listed separately.

## Memory

| Picker | Interface | Open PSS | Open RSS | Idle PSS |
|---|---|---:|---:|---:|
| **cuimoji** (native) | grid | **14.5 MiB** | 21.0 MiB | **0** |
| **cuimoji** (Flatpak) | grid | 18.3 MiB | 23.9 MiB | 0 |
| fuzzel + 1,870-line list | text | 8.5 MiB | 18.6 MiB | 0 |
| fuzzel + 3,963-line list | text | 10.0 MiB | 20.1 MiB | 0 |
| fuzzel + 5,042-line list | text | 15.7 MiB | 26.0 MiB | 0 |
| bemenu + 5,042-line list | text | 11.4 MiB | 27.4 MiB | 0 |
| bemoji + bemenu | text | 13.6 MiB | 37.1 MiB | 0 |
| rofi-emoji | text | 21.4 MiB | 40.3 MiB | 0 |
| rofimoji + rofi | text | 29.3 MiB | 57.1 MiB | 0 |
| wofi-emoji | text | 36.0 MiB | 83.7 MiB | 0 |
| bemoji + wofi | text | 52.5 MiB | 101.5 MiB | 0 |
| Smile 2.12.2 | grid | 188.7 MiB | 227.2 MiB | 105–121 MiB |
| Emote 5.2.0 | grid | 201.3 MiB | 230.7 MiB | 76–202 MiB |

Smile and Emote are the only daemons in the table. Smile's hidden autostart
sits at 105–121 MiB PSS and the app exits when its window is closed. Emote's
hidden service sits at 76 MiB and keeps the emoji model after first use,
settling around 200 MiB even with the window closed. Every other picker returns
to 0 MiB when closed.

## Sizes

| Picker | Install for the picker | Extra / shared |
|---|---|---|
| **cuimoji** native | 1.30 MiB binary + 61 KiB icon font | SDL2, X11, Wayland, Vulkan from the system |
| **cuimoji** Flatpak | 1.39 MiB app payload | freedesktop 25.08 runtime |
| fuzzel | 240 KiB binary + 156 KiB emoji list | cairo, fcft, pango, fontconfig from the system |
| bemenu | 89 KiB (client + Wayland renderer) | emoji list, Wayland from the system |
| rofi-emoji | 564 KiB rofi + 35 KiB plugin + 638 KiB emoji data | rofi themes; X11/Wayland libraries |
| rofimoji | 213 B launcher + 3.38 MiB data | Python (`libpython3.14.so` is 6.1 MiB) + rofi |
| wofi-emoji | 88 KiB script, emoji list embedded | wofi 100 KiB |
| bemoji | 9 KiB script + 156 KiB emoji database | wofi 100 KiB or bemenu 89 KiB |
| Smile | 8.31 MiB Flatpak payload | GNOME 49 runtime, 1.09 GB installed (shared) |
| Emote | 15.07 MiB Flatpak payload, 10.2 MiB of it is NotoColorEmoji.ttf | GNOME 51 runtime, 1.13 GB installed (shared) |

## Grid-based pickers

| Picker | Open PSS | Idle PSS | App payload |
|---|---:|---:|---:|
| **cuimoji** | **14.5 MiB** | **0** | 1.39 MiB (Flatpak) |
| Smile | 188.7 MiB | 105–121 MiB | 8.31 MiB |
| Emote | 201.3 MiB | 76–202 MiB | 15.07 MiB |

cuimoji uses about 13× less PSS than Smile and 14× less than Emote. Its payload
is about 6× smaller than Smile's and 11× smaller than Emote's. It is the only one
of the three that uses 0 MiB when closed.

## Text selectors

The four lowest open measurements are text selectors: fuzzel at 8.5–10.0 MiB
PSS with a 1,870–3,963-line list, bemenu at 11.4 MiB, and bemoji + bemenu at
13.6 MiB. fuzzel rises to 15.7 MiB with the 5,042-line tag list. These render
text rows rather than an emoji grid. cuimoji at 14.5 MiB is the lowest
grid-based picker measured.

## Peak cgroup memory

systemd recorded these peaks for the original long-running sessions:

| Picker | Memory peak | Swap peak |
|---|---:|---:|
| Smile | 297.7 MiB | 31.5 MiB |
| Emote | 284.8 MiB | 32.1 MiB |

cuimoji's peak in these tests was 14.5 MiB PSS (21.0 MiB RSS), with no daemon
or idle process.

## Reproducing

Build the same configuration as the benchmark:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Measure a picker's tree with its PSS and RSS from `/proc/<pid>/smaps_rollup`
after the window is open and idle, and take the peak over a few seconds.

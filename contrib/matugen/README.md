# Matugen theme for cuimoji

[Matugen](https://github.com/InioX/matugen) generates a Material You palette
from a wallpaper. These files map that palette onto cuimoji's `theme-dark` and
`theme-light` files.

There is **one template** and **two configs**, one per mode. Keeping the configs
separate matters: Matugen renders every template in the selected config, so a
single wrapper around your normal config would regenerate all your other app
themes twice and leave them in whichever mode ran last.

## Install

```sh
mkdir -p ~/.config/matugen/templates
cp contrib/matugen/cuimoji-theme.tmpl ~/.config/matugen/templates/
cp contrib/matugen/cuimoji-dark.toml  ~/.config/matugen/
cp contrib/matugen/cuimoji-light.toml ~/.config/matugen/
```

If you use a non-default `XDG_STATE_HOME`, adjust `output_path` in both configs.

## Generate

```sh
matugen image ~/Pictures/wallpaper.jpg -m dark  -c ~/.config/matugen/cuimoji-dark.toml
matugen image ~/Pictures/wallpaper.jpg -m light -c ~/.config/matugen/cuimoji-light.toml
```

Or both at once, without touching any other Matugen template:

```sh
wall=~/Pictures/wallpaper.jpg
for mode in dark light; do
    matugen image "$wall" -m "$mode" \
        -c "$HOME/.config/matugen/cuimoji-$mode.toml"
done
```

cuimoji reloads the active theme file while it runs, so no restart is needed.
Switch **Theme** in Settings to load the other mode's file.

## What is mapped

| cuimoji | Matugen |
| --- | --- |
| `background` | `surface` (picker panel) |
| `surface` | `surface_container_high` (raised layer) |
| `surface_muted` | `surface_container_low` |
| `foreground` | `on_surface` |
| `text_secondary` | `on_surface_variant` |
| `text_muted` | `outline` |
| `border` | `outline` |
| `border_subtle` | `outline_variant` |
| `border_strong` | `on_surface_variant` |
| `accent` / `accent_strong` | `primary` |
| `accent_subtle` | `primary_container` |
| `accent_foreground` | `on_primary` |
| `danger` | `error` |
| `info` | `secondary` |
| `control_on` | `primary` |
| `control_glyph` / `control_thumb_on` | `on_primary` |
| `control_track` | `surface_variant` |
| `control_track_hover` | `outline` |
| `control_thumb` | `on_surface` |

`background` is the picker panel itself. `surface` is for raised layers such as
popovers. `window` is the clear area behind the rounded panel and is left
transparent by default.

Material You has no dedicated success/warning roles, so those, the hover/active
overlays, the scrim, the focus ring, and the tone/series colors are left to
cuimoji's defaults. Add them yourself if you want to tune them.

## Transparency

The generated colors are opaque. On the Wayland window the panel can be made
translucent by replacing the `background` line in the template with:

```ini
background = {{ colors.surface.default.hex | to_color | set_alpha: 0.9 | format: "hex_alpha" }}
```

The window clear (`window`) is transparent by default, so the rounded corners
show the desktop. Set `window` to an opaque color only if you want a solid
frame around the panel. `hex_alpha` produces `#rrggbbaa`; cuimoji treats the
last two digits as the alpha channel. The X11 window stays opaque.

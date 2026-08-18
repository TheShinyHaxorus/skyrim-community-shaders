# Menu icon set

Regenerates every PNG under
`package/Interface/CommunityShaders/Icons/{Categories,Action Icons}`.

```bash
pip install pillow numpy
python tools/icons/make_icons.py "package/Interface/CommunityShaders/Icons"
```

The brand logo (`Bottled Shaders Logo/`) is **not** generated here — it is
hand-authored artwork and the script never touches it.

## SVG-sourced action icons

Four action icons are rasterised from checked-in SVG artwork in `svg/` instead
of being drawn procedurally, listed in `SVG_ACTION` in `make_icons.py`:

| Icon | Source SVG | Used by |
| ---- | ---------- | ------- |
| `restore-settings.png` | `undo.svg` | Restore defaults (feature settings), CS Editor revert |
| `save-settings.png` | `save.svg` | Save settings |
| `load-settings.png` | `loader-lines-alt.svg` | Restore saved settings (reload) |
| `clear-cache.png` | `refresh-cw-alt.svg` | Clear shader cache |

They are rendered pure white, so the colour and `Monochrome/` copies are
identical. `svg_to_icon.py` implements the SVG path grammar directly (no
rasteriser is available in this environment) and scales the artwork's ink
bounding box to fill the 104x104 safe area, so it matches the optical weight of
the procedural icons. It can also be run standalone:

```bash
python tools/icons/svg_to_icon.py tools/icons/svg/save.svg out.png --color 255,255,255
```

## Design system

| Rule | Value |
| ---- | ----- |
| Canvas | 128x128, transparent |
| Safe area | 104x104 (12px margin) |
| Stroke | 10px, round caps + round joins (~1.9px at the 24px UI size) |
| Accent | `#4CA5FF`; destructive actions use `#FF6D6D` |

**Category icons are pure white.** `Util::DrawCategoryHeader` tints them with
the theme text colour, so any baked-in colour would be multiplied away.

**Action icons ship a `Monochrome/` twin.** `IconLoader` falls back to the
colour version when a monochrome file is missing, which is why every action
icon needs one — otherwise monochrome themes render a mix. In the monochrome
variant the accent is drawn white with a halo knocked out of the base layer, so
the two shapes stay separable without colour (see `compose(..., mono=True)`).

Filenames are load-bearing: `IconLoader::GetIconDefinitions` looks them up by
name. Adding an icon means adding it there and to `Menu::UIIcons` too.

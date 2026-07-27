# Menu icon set

Regenerates every PNG under
`package/Interface/CommunityShaders/Icons/{Categories,Action Icons}`.

```bash
pip install pillow numpy
python tools/icons/make_icons.py "package/Interface/CommunityShaders/Icons"
```

The brand logo (`Bottled Shaders Logo/`) is **not** generated here — it is
hand-authored artwork and the script never touches it.

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

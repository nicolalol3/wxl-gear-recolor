# HorizontalTools addon snippets

These files are **not** a standalone addon. Install into an existing **HorizontalTools** tree:

| File | Target |
|---|---|
| `Recolor/Recolor.lua` | `Interface/AddOns/HorizontalTools/Recolor/Recolor.lua` |
| `Transmog/frames/Tints.lua` | `Interface/AddOns/HorizontalTools/Transmog/frames/Tints.lua` |

Ensure parent `HorizontalTools.toc` loads `Recolor\Recolor.lua` and transmog `.toc` loads `frames\Tints.lua` (see your HorizonTransmog / HorizontalTools install).

Requires **WarcraftXL** with `wxl-gear-recolor` module. For server-backed per-item tints, also run **mod-item-tint** on AzerothCore.

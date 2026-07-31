# wxl-gear-recolor

Per-equipment-slot luminance recolor for WarcraftXL, driven by the `/recolor` addon UI.

## Lua API

| Function | Description |
|---|---|
| `WXL_RecolorSetSlot(slot, r, g, b)` | Set color (EQUIPMENT_SLOT_*, rgb 0..1) |
| `WXL_RecolorClearSlot(slot)` | Clear one slot |
| `WXL_RecolorClearAll()` | Clear all slots |

## Char-select / login preview

Colors are stored in `WarcraftXL_gear-recolor.state` next to `Wow.exe` and loaded when
the DLL starts. That way the character-select model can tint **before** any addon
runs (GlueXML has no `/recolor` push). The addon still re-pushes from SavedVariables
on `PLAYER_LOGIN` / `VARIABLES_LOADED`.

## What works (v1 stable)

Tint is applied only to **`item\objectcomponents\`** meshes (head / shoulders / weapons / shields)
and to **`item\texturecomponents\`** body overlays (shirt, chest, waist, legs, feet, wrists, hands).

Body recolor tints the **source BLP** at CharComponent paste time (one TextureComponents layer
per call), keyed by ItemDisplayInfo `texture[0..7]` for the equipped item — never the player
composite or face/hair caches.

## Opt-out

Create `WarcraftXL_gear-recolor.disable` next to `Wow.exe`.

## Multi-player RE archive (July 2026)

Solo-use module above; multi-client tint research lives in [`research/`](research/)
(offsets, disasm, hook failures, status). Safe to delete or `git revert` that folder.
Mirror: https://github.com/nicolalol3/wxl-gear-recolor-research

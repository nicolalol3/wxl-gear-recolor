# wxl-gear-recolor

Client-side gear recolor for **WarcraftXL** (WotLK 3.3.5a client DLL / module system).

Pick a color per equipment slot (`/recolor`) and tint matching gear textures / ObjectComponent meshes. No server DB changes.

## Contents

| Path | What |
|---|---|
| `module/wxl-gear-recolor/` | WarcraftXL DLL module (`scripts/wxl-gear-recolor/`) |
| `addon/WXLRecolor/` | Standalone WotLK addon (`Interface/AddOns/WXLRecolor/`) |

## Install

1. Copy `module/wxl-gear-recolor` into your WarcraftXL tree as `wxl-core/scripts/wxl-gear-recolor/`.
2. Rebuild / redeploy `WarcraftXL.dll` into the client folder.
3. Copy `addon/WXLRecolor` into `Interface/AddOns/`.
4. In-game: `/recolor`.

Opt-out: create `WarcraftXL_gear-recolor.disable` next to `Wow.exe`.

## How it works (short)

- **Body armor** (chest, legs, hands, …): colorize paletted `Item\TextureComponents\…` before CharComponent paste.
- **3D attachments** (head, shoulders, weapons): live pixel-shader colorize on ObjectComponent draw.
- Tint math: keep luminance (shading), apply picked RGB as chroma.

## Current limitations

- **Unequip / re-equip (or relog)** is often needed for body pieces to fully refresh the character composite. Live slider/picker updates try to replay pastes, but the atlas is not always live.
- **Whole-texture tint only.** Trying to recolor *precise* colors / regions inside a texture (selective hue, per-material masks, etc.) leads to endless edge-case bugs on paletted WotLK gear. This module intentionally tints the full component.

## License

GPLv3 (same family as WarcraftXL).

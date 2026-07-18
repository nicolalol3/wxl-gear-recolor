# wxl-gear-recolor

Client-side gear recolor for **WarcraftXL** (WotLK 3.3.5a client DLL / module system).

Pick a color per equipment slot (`/recolor`) — **Solid** or **Selective** (from→to color rules). No server DB changes.

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
- Tint math: keep luminance (shading), apply picked RGB as chroma (selective uses soft match + neighbor cleanup on OC).
- Enter-world prefers natural paste tint (char-select quality); logout flushes TextureCache backups so glue/relog do not reuse stale pointers.
- Glue scoping: body tint is sticky/one-shot on the local CharacterComponent only; 3D gear (head/shoulder/weapon) uses the same sticky root on char-select and the live player model in-world.

## License

GPLv3 (same family as WarcraftXL).

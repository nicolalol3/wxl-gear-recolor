# wxl-gear-recolor

Per-**item**, per-**player** gear tinting for **WarcraftXL** (WotLK 3.3.5a client DLL) with optional **AzerothCore** server persistence and transmog UI integration.

Tints work like transmog appearances: each **item instance** (`item_guid`) stores its own color; the server broadcasts to nearby players so observers see the same tint.

---

## Contents

| Path | What |
|---|---|
| `module/wxl-gear-recolor/` | WarcraftXL DLL module → copy to `wxl-core/scripts/wxl-gear-recolor/` |
| `server/mod-item-tint/` | **AzerothCore-only** module: DB + `WXL_TINT` addon protocol (not generic AC) |
| `addon/HorizontalTools/Recolor/` | `/recolor` UI (drop into `Interface/AddOns/HorizontalTools/Recolor/`) |
| `addon/HorizontalTools/Transmog/frames/Tints.lua` | Transmog **Tints tab only** (requires HorizonTransmog / HorizontalTools transmog frame) |
| `addon/WXLRecolor/` | Standalone minimal addon (optional; HorizontalTools is the full UI) |

---

## 3D items vs texture items

WoW body gear uses two different client paths:

| | **Texture items** | **3D items** |
|---|---|---|
| **Slots** | Shirt, chest, waist, legs, feet, wrists, hands | Head, shoulders, back, tabard, weapons |
| **How they render** | BLP layers pasted onto the **character composite** during `CharComponent` assemble (`Item\TextureComponents\…`) | Separate **M2 models** attached on top (`Item\ObjectComponents\…`) |
| **Tint hook** | Paste-time palette/BGRA overlay on shared `TextureCache` | Live pixel colorize on ObjectComponent draw |
| **Nickname** | *textureitems* | *3Ditems* |

Both paths share the same **per-item** tint data from the server; only the client application mechanism differs.

---

## Install (client)

1. Copy `module/wxl-gear-recolor/` → your WarcraftXL tree: `wxl-core/scripts/wxl-gear-recolor/`
2. Rebuild / deploy `WarcraftXL.dll` (`wxl-core/build.ps1`)
3. Copy addon files into `Interface/AddOns/` (see table above)
4. Opt-out: `WarcraftXL_gear-recolor.disable` next to `Wow.exe`

Requires **WarcraftXL** (not stock WoW). Lua calls `WXL_Recolor*` exports from the DLL.

---

## Install (server — AzerothCore only)

> **Warning:** `server/mod-item-tint/` is written for **this project's AzerothCore fork** (custom transmog visibility hooks, `WXL_TINT` protocol). It may **not compile or run** on vanilla AzerothCore without porting.

1. Copy `server/mod-item-tint/` → `azerothcore-wotlk/modules/mod-item-tint/`
2. Run SQL once on **acore_characters**:
   - `server/mod-item-tint/data/sql/db-characters/custom_item_tint.sql`
3. Rebuild `worldserver`, enable module in CMake as usual for AC modules
4. Clients must register addon prefix `WXL_TINT` (handled by the Lua addons)

Protocol (server → client): `WXL_TINT PUSH\t<ownerGuid>\tslot\t…`  
Protocol (client → server): `WXL_TINT SET|CLEAR|REQ\t…` (`REQ` = re-PUSH all equipped after `/reload` / enter-world settle)

---

## How it works now

- **Persistence:** `custom_item_tint` table keyed by `item_guid` (not equip slot).
- **Visibility:** Same model as transmog — on equip / login / range enter, server PUSHes tint to self + observers.
- **Client:** `GearRecolor.cpp` applies tint on texture paste (body) or OC draw (3D); remotes use orphan paste resolve + natural `RenderPrep` (no forced full-body rebuild).

---

## Bugs we fixed (the hard ones)

### 1) Observer texture corruption (*textureitems*) — **fixed Jul 2026**

**Symptom:** Tint Apply looked perfect on **your** client; on **other** clients the tinted player’s **face/body broke** (wrong layers, wiped composite).

**Not the cause:** Server PUSH payload, wrong tint data, or “missing CharComponent for remotes” (RE proved RenderPrep + component lookup worked).

**Actual cause (three layers):**

1. **Paste outside RenderPrep** — Most `TextureComponents` pastes run with no assemble owner (`prepOwner=0`). Tint never applied; unrelated body sections kept repasting and mixing composites.
2. **Force rebuild on remote players** — After Apply, `ForceOwnerBodyRebuild` re-pasted large section masks **without tint**, undoing good work and corrupting the observer view.
3. **GUID map key mismatch** — Lua PUSH stores equip snap under one 64-bit GUID; RenderPrep/Force used another key for the **same player** (same `ownerLow`, different high bits). `EquipSnapKnown` used exact map lookup (no `GuidSamePlayer` fallback) → Force pasted chest **untinted** even when orphan tint had just succeeded.

**Fix:** `CanonicalTintOwner()`, no Force on remotes, `ResolveOrphanPasteOwner`, stem-based slot matching for full chest piece (sleeves + torso). Details: see `DBC_Tool/WXL-REMOTE-RECOLOR-SESSION.txt` in the dev tree if present.

### 2) NPC tint bleed (*3Ditems*) — **fixed (workaround)**

**Symptom:** Recoloring head/shoulder/weapons on your character also recolored **random NPCs** (and sometimes paperdoll previews).

**Cause:** We tried to sync tints onto **paperdoll, character-select (glue), and enter-world** preview models using the same sticky model root as the live player. Those UI/glue contexts **reuse model pointers and OC draw paths** that also serve NPCs → tint state leaked by shared `ObjectComponent` / model identity.

**Fix (current):** **Ignore paperdoll, glue, and character-panel (`C`) models entirely** for tint application. Live in-world self + other players only.

**Known limitation:** Character select, dressing room, and default paperdoll **do not show tints correctly** (intentionally left broken until we have a safe separate preview path).

### 3) `/reload` drops some textureitem tints — **fixed**

**Symptom:** After `/reload`, some slots lost their tint until full client restart / relog.

**Cause:** `/reload` does **not** fire `PLAYER_ENTERING_WORLD`. The DLL stays loaded (`g_slotHsl` OK) but UI reload swaps BLP handles; `g_texTint` stayed keyed to stale pointers. The server also never re-PUSHed (login-only).

**Fix:**
- `WXL_RecolorOnUiReload()` — drop stale tex handles, invalidate body `g_pathOrig`, rebuild tinted sections
- `SetSelfGuid` clears local HSL only on **character switch**, not same-guid reload
- Addon recovery: after `/reload` send `WXL_TINT REQ`; server re-`PushAllEquipped` + nearby sync
- Enter-world also schedules delayed `REQ` + rebuild (same handshake)

### 4) Cold login: no tints until unequip/reequip — **fixed**

**Symptom:** Close/reopen client, enter world → own gear untinted until bag swap.

**Cause:** Login `PUSH` often arrived **before** `SetSelfGuid`. `ApplyOwnerTint` stored rows in `g_remoteTints` (self was still 0). Local paste only reads `g_slotHsl` → invisible. Unequip/equip sent a fresh PUSH with self set → worked.

**Fix:** `PromoteRemoteTintsToLocal()` on `SetSelfGuid` + delayed enter-world `REQ` / equip-snap / section rebuild.

### 5) Other players' faces / underwear pick up *your* textureitem tint (after relog) — **fixed**

**Symptom:** Apply a textureitem tint; others look fine. Logout/login → from your client, other players' **faces / underwear / similar layers** show **your** tint color.

**Cause:** Post-relog, many `TextureComponents` pastes run with `prepOwner=0`. `ResolveOrphanPasteOwner` scanned **all** tinted owners **including self first**. Your HSL was painted into **other units' composites** whenever the BLP stem matched your ItemDisplayInfo (shared gear paths / section rebuild). Looked similar to bug (1) but milder and delayed until reassemble.

**Fix:** Orphan resolution **never considers self**. Local tints apply only via RenderPrep / `TryHslForComponentTexture` when prep is the local player. Remotes keep orphan + match fallbacks so **their** tints still show (without stealing yours).

---

## Known issues

| Issue | Notes |
|---|---|
| **Transmog Tints tab** | Less tint variety than `/recolor` (gradients, selective, etc.); will improve |
| **`/recolor` addon** | May be removed once transmog tints tab is feature-complete |
| **Paperdoll / glue / `C` panel** | No tint preview (see NPC bug workaround) |
| **AzerothCore module** | Project-specific; not guaranteed on other cores |

---

## License

GPLv3 (same family as WarcraftXL).

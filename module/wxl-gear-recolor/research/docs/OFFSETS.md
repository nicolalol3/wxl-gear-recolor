# Verified offsets — WoW 3.3.5a (`Wow.exe`)

**Client:** `WotLK for AZ` build (Horizon test client, July 2026)  
**Image base:** `0x00400000` (standard 3.3.5a)

> Addresses below are **RVAs** (add base for file offset in unrelocated PE).  
> Always re-verify on your build — 3.3.5a builds differ slightly.

---

## Globals

| Address | Name | Role |
|---------|------|------|
| `0x00B6B870` | `g_GearScratchMips` | **Shared** gear composite scratch (sections 0–7 paste dest) |
| `0x00B6B864` | related scratch metadata | read by flush family |
| `0x00B6B1A0` | `g_LocalCharComponent` | **Singleton** — local player body composite |
| `0x00B6B240` | `g_CharComponentPool` | Array of remote `CharComponent`, stride **`0x198`** |
| `0x00B6B88C` | section handler table (runtime) | copied from `0x00B6B928` @ init `0x4F1BA2` |
| `0x00B49C90` | `kMipTablePtr` | process-wide BLP decode mip table (texture upload) |

---

## CCharacterComponent fields (`this` = `esi`/`ecx`)

| Offset | Type | Meaning |
|--------|------|---------|
| `+0x08` | `uint8` flags | bit0 = needsRenderPrep, bit2 = skipPath |
| `+0x0C` | `uint32` | **sectionDirty** — bit N → rebuild section N (0–9) |
| `+0x34` | mip pool header | ring buffer ~40 slots; `RefCountCompositeMip` writes here |
| `+0x38` | `void*` | **bodyModelPtr** — must match `CGUnit+0xB4` |
| `+0x3C` | `uint32` | flags — **bit0 = NPC** → RenderPrep early out |
| `+0x194` | composite mip ptr | gear / body layer |
| `+0x1A4` | composite mip ptr | face upper (section 8) |
| `+0x1B0` | composite mip ptr | face lower / hair (section 9) |
| `+0x1C0` | composite mip ptr | variant in some handlers |

---

## CGUnit_C

| Offset | Meaning |
|--------|---------|
| `+0x30` | GUID (low used by WXL) |
| `+0xB4` | body model root pointer |

---

## Functions — assemble pipeline

| Address | Name | Calling convention | Notes |
|---------|------|-------------------|-------|
| `0x004F1520` | `CCharacterComponent::RenderPrep` | `thiscall`, `retn 4` (`a2`) | Main entry — hook fires |
| `0x004EE0D0` | `RenderPrepSections` | — | Loops sections 0–9 via dirty mask |
| `0x004F07D0` | `PasteSkinLayout` | `cdecl` | Base skin into sections — **never tint** |
| `0x004F08A0` | `PasteFromSkin` / item paste | `cdecl` | Item `TextureComponents` — **WXL tint hook** |
| `0x004F0AD0` | Section 8 handler | — | Dual-path face; see SECT8 doc |
| `0x004F2CE0` | `RefCountCompositeMip` | `retn 12` | Publishes mip into `[comp+0x34]` ring |
| `0x004F2D00` | `TextureCacheGetMip` | `cdecl` | CPU mip for paste |
| `0x004F2D40` | `TextureCacheGetPal` | `cdecl` | palette |
| `0x004F2D80` | `TextureCacheHasMips` | `cdecl` | |
| `0x004F2E50` | `TextureCacheGetInfo` | `cdecl` | |
| `0x004F3930` | `TextureCacheCreate` | `cdecl` | Armor overlay BLP load |
| `0x004F3BA0` | `TextureCacheHelper` | — | Gate before section 8 layout path |

### RenderPrep callers (partial)

| Address | Path | `a2` |
|---------|------|------|
| `0x004DFC98` | local singleton `[B6B1A0]` | 0 |
| `0x004E150F` | local singleton | 0 |
| `0x004E2FF8` | **remote pool** `[B6B240]` | 0 |
| `0x004E6B9A` | in-loop force rebuild | 1 |
| `0x00528085` | force rebuild | 1 |
| `0x0052FDAC` | force rebuild | 1 |

---

## FlushGearComposite family — **CORRECTED**

> **Wrong:** `0x4E9000` as hook entry — on this binary it is **not** the function prologue.  
> Hooking there received `ecx=1`, corrupted mip publish, black faces.

| Address | Role |
|---------|------|
| `0x004EE890` | Start of larger containing routine (unsafe hook) |
| `0x004E9550` | Flush family prologue A |
| `0x004E9590` | **Actual entry** used by callers (mid-function jump target) |
| `0x004E9690` | Flush family prologue B |
| `0x004E96B0` | **Actual entry** used by callers |

**Rule:** Do not detour at `0x4E9591`/`0x4E96B1` with a C trampoline that calls `orig(component)` — register setup (`esi`, stack locals) is skipped.

Touches at flush time (from disasm):

- `[esi+0x194]`, `[esi+0x1A4]`, `[esi+0x1B0]`, `[esi+0x1C0]`
- `[esi+0x34]` mip pool ring via `call 0x4F2CE0`

---

## Section semantics

| Section | Content | Paste destination |
|---------|---------|-------------------|
| 0–7 | Item `TextureComponents` (gear slots) | `*[B6B870]` scratch |
| 8 | Face upper | `[comp+0x1A4]` (+ reads scratch) |
| 9 | Face lower / hair | `[comp+0x1B0]` |

Dirty mask examples:

- `0x3FF` — all sections (full native rebuild)
- `0x0FF` — gear only (bits 0–7) — **face handlers skipped**
- `0x300` — face only (bits 8–9)

---

## Gx.hpp cross-reference (WarcraftXL)

These constants live in `WarcraftXL/wxl-core/src/offsets/engine/Gx.hpp`:

```cpp
kCharPasteToSection  = 0x004F07D0;  // PasteSkinLayout
kCharPasteFromSkin   = 0x004F08A0;  // item/face PasteToSection
kCharFlushGearComposite    = 0x004E9591;  // RE only — do not hook
kCharFlushGearCompositeAlt = 0x004E96B1;
kCharFlushGearCompositeAlt2 = 0x004EE890;
kCharRefCountCompositeMip = 0x004F2CE0;
kOffCcCompositeMip0 = 0x194;
kOffCcCompositeMip1 = 0x1A4;
kOffCcCompositeMip2 = 0x1B0;
kOffCcCompositeMip3 = 0x1C0;
```

`kCharRenderPrep` is hooked at `0x4F1520` in WXL; some RE notes mention `0x4F15F0` region on other builds — **re-verify per client**.

---

## New vs official wxl-gear-recolor repo

The frozen module README documents **hook names and solo behavior** only. It does **not** include:

- `B6B870` shared scratch invariant
- Pool stride `0x198` / dual storage paths
- Flush family correction
- Section dirty mask interaction with face blackening
- `RefCountCompositeMip` / `[comp+0x34]` pool
- Dual-path section 8 (`0x4F0AD0`)
- Runtime log regression statistics

All of the above are documented in this research repo.

# Project status — multi-player gear recolor

**Last updated:** 2026-07-31  
**Outcome:** Not shipped. Research preserved in this repo.

---

## Goal (where we are headed)

Enable **per-equipment-slot HSL tint** for:

1. **Local player** — `/recolor` UI, char-select preview, in-world self view.
2. **Remote players** — see each other's tints correctly (no cross-texture bleed).
3. **Stability** — no black faces, no invisible models, no flicker at login.

### Target architecture (evidence-based, not implemented)

```
┌─────────────────────────────────────────────────────────────┐
│  AssembleSession per RenderPrep (one owner, one component)  │
│  g_tlsPasteOwner = unit GUID                                │
│  mutex on shared B6B870 only during this session            │
└──────────────────────────┬──────────────────────────────────┘
                           │
     ┌─────────────────────▼─────────────────────┐
     │  Native RenderPrep (untinted) first pass    │
     │  dirty=0x3FF — face + gear together         │
     │  (mirror remote native_first for self)      │
     └─────────────────────┬─────────────────────┘
                           │
     ┌─────────────────────▼─────────────────────┐
     │  Tint pass — gear sections 0-7 only        │
     │  HSL overlay ONLY if session owner match  │
     │  NEVER tint scratch outside session       │
     └─────────────────────┬─────────────────────┘
                           │
     ┌─────────────────────▼─────────────────────┐
     │  Optional: per-owner scratch snapshot     │
     │  at FlushGearComposite (Layer 4)          │
     │  — needs runtime RE on flush prologue     │
     └───────────────────────────────────────────┘
```

### Alternative path (mentioned in frozen repo README)

Convert problematic **texture components** to **3D item displays** so tint applies on object meshes instead of the shared body composite scratch. That sidesteps `B6B870` entirely but is a content/pipeline change, not a hook fix.

---

## What works today

| Scenario | Status |
|----------|--------|
| Solo player, no other bodies visible | Works (official frozen module) |
| Char-select tint from state file | Works |
| Multi-client, remote tints | **Broken** — texture bleed |
| Self face after gear tint | **Broken** — often black |
| Login with aggressive paste hooks | **Broken** — black faces all players |

---

## Root causes (confirmed by static RE + logs)

### 1. Shared gear scratch `*[0x00B6B870]`

All players' gear sections 0–7 paste into the **same** CPU mip buffer during assemble. There is no per-unit scratch in the client. Any concurrent or interleaved assemble without serialization causes **pixel cross-contamination**.

### 2. Face depends on scratch, flush touches face anyway

Section 8 handler (`0x4F0AD0`) copies `B6B870` → `[comp+0x1A4]` / `[comp+0x1B0]`.  
`FlushGearComposite` refcount-swaps face mips **even when dirty mask has only bits 0–7**.

Self often rebuilds with **gear-only dirty** → face handlers never run → flush leaves face invalid → **black face**.

### 3. Local vs remote component paths differ

| Path | Storage | RenderPrep caller |
|------|---------|-------------------|
| Self | `*[0x00B6B1A0]` singleton | `0x4DFC98`, `0x4E150F` |
| Remote | `*[0x00B6B240]` pool, stride `0x198` | `0x4E2FF8`, … |

Same logical player can appear with **multiple `CharComponent*` pointers** in logs (pool recycle vs live model). Owner resolution via `comp+0x38 == unit+0xB4` is necessary but not sufficient when pool slots are stale.

### 4. Wrong `FlushGearComposite` hook address

Early WXL used `0x4E9000` as flush entry. On this binary that is **mid-function** inside a larger routine starting ~`0x4EE890`. Detour calling `orig(ecx)` skipped `mov ecx,[esi+28]` setup → mip publish broken → black faces.

Correct family (static RE, **do not hook mid-entry without naked asm**):

| Symbol | Address | Notes |
|--------|---------|-------|
| Flush prologue A | `0x4E9550` | callers jump to `0x4E9590` |
| Flush prologue B | `0x4E9690` | callers jump to `0x4E96B0` |
| Larger containing fn | `0x4EE890` | not a safe hook target |

Hooks at `0x4E9591` / `0x4E96B1` still caused regressions when calling original as a normal function.

### 5. WXL `face_scratch_skip` gate (login blackface)

Blocking `PasteFromSkin` when `globalSrc=B6B870` and `g_tlsFaceScratchReady != section` prevented native face composite at login. `/recolor` re-ran skin layout → faces appeared; bleed returned when tint applied outside session.

---

## Hook experiment chronology (summary)

Full detail: [`HOOK-ATTEMPTS.md`](HOOK-ATTEMPTS.md).

| Version | Idea | Result |
|---------|------|--------|
| v8–v9b | Mutex all RenderPrep; face_gear_block; R3 face strip | Black faces, worse bleed |
| v10 | Runtime probes (prep, scratch hash, flush) | Proved wrong flush `ecx=1` |
| v11 | Flush @ `0x4EE890` + alts | Still broken |
| v12 | Flush @ mid-entry `0x4E9591` | 0 flush events; login blackface |
| v13 | Remove all flush hooks; remove remote face strip | Login blackface; recolor fixes face, bleed returns |
| v14 | Remove face_scratch_skip; tint only in session; no orphan stem | User test: still not fixed |

---

## RE gaps blocking a definitive fix

From [`re/WXL-RE-COMPLETE-MAP.txt`](../re/WXL-RE-COMPLETE-MAP.txt) section **J**:

1. **CompositeMipObject** — CPU pixel buffer offset inside the object (not fully mapped).
2. **Mip pool display slot** — which `[comp+0x34]` ring index is bound for GPU draw vs staging.
3. **TextureCacheHelper** (`0x4F3BA0`) — whether cache entries are shared across units for the same BLP path.
4. **Server PUSH → client dirty `0x3FF`** — who sets full mask on remote components (client-side, not WXL).

Without (1)–(3), any fix remains trial-and-error.

### Required runtime session (not done)

Breakpoints from roadmap — **vanilla Wow.exe, no behavior hooks**:

1. `0x4F1520` RenderPrep — log `ecx`, `[ecx+0x0C]`, `[ecx+0x38]`
2. `0x4EE0D0` RenderPrepSections
3. `0x4F07D0` PasteSkinLayout
4. `0x4F08A0` PasteFromSkin — log `src==B6B870`, section, dst mip ptr
5. `0x4E9550` / `0x4E9690` flush family — log `[esi+0x34]` ring index before/after

Deliverable: `WXL-RE-RUNTIME-TRACE.txt` (does not exist yet).

---

## Log evidence (regression)

See [`re/WXL-RE-RECOLOR-REGRESSION.txt`](../re/WXL-RE-RECOLOR-REGRESSION.txt).

| Phase | face_skin_ok | face ptr races | remote_miss |
|-------|--------------|----------------|-------------|
| PRE login | 10 | 3 (same race) | 0 |
| POST /recolor | 764 | 55 (multi-race) | 1032 |

Login works because native `RenderPrep` runs **face + gear in one pass** (`dirty=0x3FF`).  
`/recolor` triggers PUSH storm → interleaved tint passes → shared scratch / pool corruption.

---

## Recommended next steps (for whoever continues)

1. **Complete runtime trace** (phase 3 roadmap) on vanilla client — no new hooks.
2. **Map `CompositeMipObject`** and pool display index — disassemble `0x4F2CE0` consumers.
3. **If hooks resume:** only tint when `g_tlsPasteOwner` matches and `IsPasteTintAllowed()`; never block native scratch→face; never hook flush mid-entry.
4. **Consider architectural escape:** 3D item path per frozen repo README, or server-driven model swap without shared scratch tint.

---

## Files in the experimental WXL tree

Not committed here (private monorepo):

- `WarcraftXL/wxl-core/scripts/wxl-gear-recolor/src/GearRecolor.cpp` — all hook logic
- `WarcraftXL/wxl-core/src/offsets/engine/Gx.hpp` — offset constants
- `C:\Azerothcore\debug-615e3b.log` — JSON runtime log (local only)

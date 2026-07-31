# Hook experiment chronology

Experimental code: `GearRecolor.cpp` in WarcraftXL workspace (not in this repo).  
Log file: `debug-615e3b.log` (JSON lines, hypothesis tags H1–H5, FACE, etc.).

---

## Baseline — official frozen module

- Solo `/recolor` works.
- No multi-player isolation.
- No `RenderPrep` gating beyond basic `IsPasteTintAllowed`.

---

## Phase A — remote owner resolution (July 29)

**RE:** [`re/WXL-REMOTE-RECOLOR-RE-FINDINGS.txt`](../re/WXL-REMOTE-RECOLOR-RE-FINDINGS.txt)

- Discovered `B6B1A0` singleton vs `B6B240` pool.
- `BodyModelOwnerGuid` strict equality failures → `remote_no_hsl`, `ownerLow=0`.
- Rewrote `ResolveAssembleUnit`, `RememberRemoteCc`, exclusive pool mapping.

**Result:** Better logging; bleed persisted.

---

## Phase B — face / gear separation (July 30–31)

**RE:** [`re/WXL-RE-FACE-ROOT-CAUSE.txt`](../re/WXL-RE-FACE-ROOT-CAUSE.txt), [`re/WXL-RE-RECOLOR-REGRESSION.txt`](../re/WXL-RE-RECOLOR-REGRESSION.txt)

| Attempt | Description | Outcome |
|---------|-------------|---------|
| `face_gear_block` | Block item gear paste on sec 8–9 for tinted remotes | Incomplete; self still black |
| OR dirty `0x300` inside gear RenderPrep | Force face rebuild nested | **Crash** — scratch still has gear |
| `RunSelfFaceRebuildNow` / `self_face_done` | Separate face-only RenderPrep after gear | Runs 125×; user still black face |
| `ScheduleOwnerSectionRebuild` coalesce | Deferred face rebuild | Never fired `ForceOwnerBodyRebuild` from preview path |
| `g_remoteAssembleMu` | Serialize remote tint RenderPrep | Reduced some races; bleed remained |

---

## Phase C — serialization on shared scratch (v8–v9b)

- `g_pasteGateMu` on **all** non-NPC `RenderPrep`.
- Mutex on every `hkPasteToSection` / `hkPasteSkinLayout`.
- Remote face dirty strip (R3).

**Result:** Black faces, flicker, worse regressions.

---

## Phase D — runtime RE probes (v10)

Added `ReProbeRenderPrep`, `ReProbeFaceScratch`, flush logging:

- Discovered flush hook received **`comp:1`** (wrong entry).
- `0x4E9000` not valid prologue on this binary.
- Zero `face_scratch` events when `face_scratch_skip` active.

---

## Phase E — flush hook address hunt (v11–v12)

| Target | Result |
|--------|--------|
| `0x4EE890` | Wrong context |
| `0x4E9550` / `0x4E9690` prologues | Callers jump to `+0x40` mid-entry |
| `0x4E9591` / `0x4E96B1` hooks | Black faces — register setup skipped |
| Remove flush hooks (v13) | No flush log events; login blackface |

---

## Phase F — face_scratch_skip (v12–v13)

Logic in `hkPasteToSection`:

```
if (faceLayer && globalSrc && !globalDst)
  if (g_tlsFaceScratchReady != section) return;  // BLOCK
```

Gear paste to scratch cleared `g_tlsFaceScratchReady` before face handler.

**Result:** Login blackface all players; `/recolor` fixed faces; bleed returned.

---

## Phase G — session-only tint (v14)

- Removed `face_scratch_skip` block.
- `allow = IsPasteTintAllowed()` only (not `inSession ||`).
- `doTint` requires `g_tlsPasteOwner` bound to `effectiveOwner`.
- Removed `stemTint` / `OrphanPasteScope` orphan paths.
- Mutex only on tint or `markRemoteNativeAfter` RenderPrep.

**User report:** Still not working (session continued after v14).

---

## Forbidden patterns (proven harmful)

Do **not** retry without new RE:

1. Hook `FlushGearComposite` via MinHook at mid-entry addresses.
2. Block `PasteFromSkin` returns on sections 8–9 without vanilla trace proof.
3. OR `0x300` into dirty mask during same `CallOrig` as gear tint.
4. `ForceOwnerBodyRebuild` loop every frame (`face_done` storm — 374 events).
5. `stemTint` / `ResolveUniqueStemPasteOwner` outside `RenderPrep` session.
6. Tint scratch when `g_tlsPasteOwner == 0` but HSL map matches stem.

---

## Instrumentation tags (log)

| Tag | Meaning |
|-----|---------|
| `prep_self` / `prep_remote` | RenderPrep probe |
| `gear_scratch` | Pixel hash of B6B870 after gear paste |
| `face_scratch` | Scratch read during face paste |
| `face_skin_ok` | PasteFromSkin on face path |
| `face_scratch_skip` | Blocked native face paste (v12–v13) |
| `face_gear_block` | Blocked gear on face section |
| `stem_tint` | Orphan stem path (removed v14) |
| `overlay_ok` | HSL overlay applied |
| `remote_miss` | Tint wanted but `allowed=0` |
| `flush` | Flush hook (when enabled) |

---

## Analysis tooling

```bash
python tools/wxl_analyze_rt_re.py debug-615e3b.log
```

Generates stats referenced in `WXL-RE-RECOLOR-REGRESSION.txt`.

# wxl-gear-recolor-research

**Research archive** for multi-player gear recolor on WoW **3.3.5a** (WarcraftXL).

This repo is **not** the playable module. The frozen, solo-only implementation lives here:

**https://github.com/nicolalol3/wxl-gear-recolor**

That repo documents a deliberate stop: per-slot tint works for **one player in isolation**, but **tmog / per-item / multi-client** integration is blocked by how the client shares composite scratch buffers and mip pools. This repository collects everything discovered **after** that freeze while trying to fix remote players and login regressions.

---

## What this repo contains

| Path | Contents |
|------|----------|
| [`docs/STATUS.md`](docs/STATUS.md) | Where we are, what failed, where we are headed |
| [`docs/OFFSETS.md`](docs/OFFSETS.md) | Verified & corrected function/global offsets |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | Client assemble pipeline (diagram + invariants) |
| [`docs/HOOK-ATTEMPTS.md`](docs/HOOK-ATTEMPTS.md) | Chronology of WXL hook experiments (v8–v14) |
| [`docs/INDEX.md`](docs/INDEX.md) | Index of every `re/*.txt` document |
| [`re/`](re/) | Full static RE dumps (disassembly, call graphs, regression logs) |
| [`tools/`](tools/) | Python scanners used to generate the RE dumps |

**Binary analyzed:** `Wow.exe` from client `WotLK for AZ` (3.3.5a build used in Horizon testing).

**Experimental code** (not in this repo): `WarcraftXL/wxl-core/scripts/wxl-gear-recolor/src/GearRecolor.cpp` in the private AzerothCore / WarcraftXL workspace.

---

## Executive summary (July 2026)

### The core problem

The client has **one global gear scratch buffer** (`*[0x00B6B870]`) shared by **all** visible player bodies during `CCharacterComponent::RenderPrep`. Gear sections 0–7 paste into that scratch; face sections 8–9 read from it and write into **per-component** mip fields (`+0x1A4`, `+0x1B0`, …). At the end of every `RenderPrep`, `FlushGearComposite` refcount-swaps mip pool slots — **including face mips even when sections 8–9 were not dirty**.

WXL tints gear by hooking paste and overlaying HSL on `item\texturecomponents\` BLPs **at paste time**. That is correct for solo play. For multiple players:

1. **Interleaved `RenderPrep`** on different `CharComponent`s without strict owner isolation → tinted pixels of player A land in `B6B870` while player B's face handler reads the same scratch.
2. **Self path** often enters gear-only rebuild (dirty mask 0–7, not 8–9) → face handlers skipped → `FlushGearComposite` still invalidates face mips → **black self face**.
3. **Hooking `FlushGearComposite` at a wrong entry** (mid-function) corrupts register setup → black faces for everyone.

### What the official repo already knew

- Tint only `item\texturecomponents\` and `item\objectcomponents\`, never face/hair caches.
- Char-select preview via `WarcraftXL_gear-recolor.state` file.
- Solo `/recolor` works.

### What this research adds (new vs official)

- Full **local vs remote `CharComponent` storage** map (`B6B1A0` singleton vs `B6B240` pool, stride `0x198`).
- **Section dispatch table** @ `0x00B6B88C`, handlers 0–9, dual-path section 8 @ `0x4F0AD0`.
- **`FlushGearComposite` family** — corrected addresses; proof that mid-entry hooks break mips.
- **`RefCountCompositeMip` @ `0x4F2CE0`** and mip ring @ `[comp+0x34]` (40 slots).
- **Regression timeline**: login OK → `/recolor` storm → texture bleed (log-backed).
- **WXL invariants** (R1–R7) for any future fix.
- **Runtime probe schema** (`debug-615e3b.log`, `wxl_analyze_rt_re.py`).

### Current status: **not fixed**

As of 2026-07-31, multi-player gear recolor is still broken. Latest symptoms:

- Black faces at login (when paste hooks block native scratch→face).
- Or: login faces OK, `/recolor` restores tint but **textures bleed across players**.

See [`docs/STATUS.md`](docs/STATUS.md) for the target architecture and remaining RE gaps.

---

## How to use the RE tools

```bash
# From repo root, point at your Wow.exe
python tools/wxl_re_char_assemble_master.py "C:/path/to/Wow.exe" > re/custom-run.txt
python tools/wxl_re_complete_map.py "C:/path/to/Wow.exe"
```

Analyze runtime logs from instrumented WarcraftXL builds:

```bash
python tools/wxl_analyze_rt_re.py "C:/Azerothcore/debug-615e3b.log"
```

---

## Relationship to other repos

| Repo | Role |
|------|------|
| [wxl-gear-recolor](https://github.com/nicolalol3/wxl-gear-recolor) | Frozen solo module + addon |
| [wxl-core](https://github.com/nicolalol3/wxl-core) | WarcraftXL framework (hosts GearRecolor script) |
| **this repo** | RE archive + failed-fix chronology |

---

## License

RE notes and Python tools: same spirit as the WarcraftXL project (document freely).

Disassembly excerpts are derived from Blizzard's `Wow.exe`; use only for interoperability research on private 3.3.5a servers.

---

## Contributing

If you continue this work:

1. Read [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) and [`re/WXL-RE-COMPLETE-MAP.txt`](re/WXL-RE-COMPLETE-MAP.txt) first.
2. Do **runtime** breakpoints (x64dbg) before adding hooks — see [`re/WXL-RE-CHAR-ASSEMBLE-ROADMAP.txt`](re/WXL-RE-CHAR-ASSEMBLE-ROADMAP.txt) phase 3.
3. Append findings to `re/` and update `docs/OFFSETS.md` if addresses change per build.

**Do not** merge speculative hook logic back into the frozen module without closing gap **J** in the complete map (mip pool display slot, `CompositeMipObject` layout, cache sharing).

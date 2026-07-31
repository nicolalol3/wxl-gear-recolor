# RE document index

All paths relative to repo root.

---

## Start here

| File | Description |
|------|-------------|
| [`WXL-RE-DISASSEMBLE-LIVE.txt`](../re/WXL-RE-DISASSEMBLE-LIVE.txt) | Living index; gaps list; points to complete map |
| [`WXL-RE-COMPLETE-MAP.txt`](../re/WXL-RE-COMPLETE-MAP.txt) | **Master map** — identity, pipeline, invariants, gaps (J) |
| [`WXL-RE-CHAR-ASSEMBLE-ROADMAP.txt`](../re/WXL-RE-CHAR-ASSEMBLE-ROADMAP.txt) | Phased RE plan (static → runtime → fix) |

---

## Architecture & isolation

| File | Description |
|------|-------------|
| [`WXL-RE-MODEL-ISOLATION-MAP.txt`](../re/WXL-RE-MODEL-ISOLATION-MAP.txt) | Player→model→CC chain; rules R1–R7; pool xrefs |
| [`WXL-RE-SECTION-DISPATCH.txt`](../re/WXL-RE-SECTION-DISPATCH.txt) | Section 0–9 handler table |
| [`WXL-RE-CHAR-ASSEMBLE-MASTER.txt`](../re/WXL-RE-CHAR-ASSEMBLE-MASTER.txt) | Master static dump (char assemble script output) |
| [`WXL-RE-RENDERPREP-DEEP.txt`](../re/WXL-RE-RENDERPREP-DEEP.txt) | RenderPrep inner calls |
| [`WXL-RE-POOL-RENDERPREP.txt`](../re/WXL-RE-POOL-RENDERPREP.txt) | Pool RenderPrep caller `0x4E2FF8` |

---

## Paste & composite

| File | Description |
|------|-------------|
| [`WXL-RE-PASTE-COMPOSITE.txt`](../re/WXL-RE-PASTE-COMPOSITE.txt) | PasteSkinLayout / PasteFromSkin basics |
| [`WXL-RE-COMPOSITE-FIELDS.txt`](../re/WXL-RE-COMPOSITE-FIELDS.txt) | CharComponent mip field xrefs |
| [`WXL-RE-FACE-GEAR-NATIVE.txt`](../re/WXL-RE-FACE-GEAR-NATIVE.txt) | Native face+gear interaction |

---

## Face-specific

| File | Description |
|------|-------------|
| [`WXL-RE-FACE-ROOT-CAUSE.txt`](../re/WXL-RE-FACE-ROOT-CAUSE.txt) | Black self face — dirty mask analysis |
| [`WXL-RE-SECT8-DUAL-PATH.txt`](../re/WXL-RE-SECT8-DUAL-PATH.txt) | Section 8 handler dual path @ `0x4F0AD0` |
| [`WXL-RE-FACE-COLLISIONS.txt`](../re/WXL-RE-FACE-COLLISIONS.txt) | Shared dst mip ptr across races |
| [`WXL-RE-FACE-SHARED-DST.txt`](../re/WXL-RE-FACE-SHARED-DST.txt) | Face destination sharing |

---

## Mip pool & flush

| File | Description |
|------|-------------|
| [`WXL-RE-MIP-POOL.txt`](../re/WXL-RE-MIP-POOL.txt) | FlushGearComposite disasm @ `0x4E9000` region |
| [`WXL-RE-MIP-SWAP-4F2CE0.txt`](../re/WXL-RE-MIP-SWAP-4F2CE0.txt) | RefCountCompositeMip deep dive |

---

## Remote recolor & regression

| File | Description |
|------|-------------|
| [`WXL-REMOTE-RECOLOR-RE.txt`](../re/WXL-REMOTE-RECOLOR-RE.txt) | Initial remote RE notes |
| [`WXL-REMOTE-RECOLOR-RE-FINDINGS.txt`](../re/WXL-REMOTE-RECOLOR-RE-FINDINGS.txt) | Singleton vs pool conclusion |
| [`WXL-REMOTE-RECOLOR-SESSION.txt`](../re/WXL-REMOTE-RECOLOR-SESSION.txt) | Debug session notes |
| [`WXL-RE-RECOLOR-REGRESSION.txt`](../re/WXL-RE-RECOLOR-REGRESSION.txt) | Login OK → /recolor broken (log stats) |
| [`WXL-RE-SCAN-OUTPUT.txt`](../re/WXL-RE-SCAN-OUTPUT.txt) | Raw `wxl_re_scan.py` output |

---

## Python tools (`tools/`)

| Script | Generates |
|--------|-----------|
| `wxl_re_scan.py` | Call graph landmarks |
| `wxl_re_char_assemble_master.py` | CHAR-ASSEMBLE-MASTER |
| `wxl_re_complete_map.py` | COMPLETE-MAP sections |
| `wxl_re_model_isolation_map.py` | MODEL-ISOLATION-MAP |
| `wxl_re_mip_pool.py` | MIP-POOL |
| `wxl_re_mip_swap.py` | MIP-SWAP-4F2CE0 |
| `wxl_re_paste_composite.py` | PASTE-COMPOSITE |
| `wxl_re_renderprep_deep.py` | RENDERPREP-DEEP |
| `wxl_re_section8_dualpath.py` | SECT8-DUAL-PATH |
| `wxl_re_face_gear_native.py` | FACE-GEAR-NATIVE |
| `wxl_re_pool_renderprep.py` | POOL-RENDERPREP |
| `wxl_analyze_rt_re.py` | Parses `debug-615e3b.log` |

---

## Markdown docs (`docs/`)

| File | Description |
|------|-------------|
| [`STATUS.md`](STATUS.md) | Project status & next steps |
| [`OFFSETS.md`](OFFSETS.md) | Consolidated offset table |
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | Pipeline diagrams & invariants |
| [`HOOK-ATTEMPTS.md`](HOOK-ATTEMPTS.md) | Failed fix chronology |

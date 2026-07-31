#!/usr/bin/env python3
"""
WXL — definitive static RE map: how WoW 3.3.5a builds per-player character models,
what is shared vs per-unit, and where recolor hooks must operate.

Output: DBC_Tool/WXL-RE-MODEL-ISOLATION-MAP.txt
"""
from __future__ import annotations

import struct
from collections import defaultdict
from pathlib import Path

WOW = Path(r"C:\Users\Shadowlands\Desktop\WotLK for AZ\Wow.exe")
OUT = Path(r"C:\Azerothcore\DBC_Tool\WXL-RE-MODEL-ISOLATION-MAP.txt")
IMAGE_BASE = 0x00400000

# --- Known anchors (verified in prior RE) ---
FUNCS = {
    "RenderPrep": 0x004F1520,
    "RenderPrepSections": 0x004EE0D0,
    "PasteSkinLayout": 0x004F07D0,
    "PasteFromSkin": 0x004F08A0,
    "FlushGearComposite": 0x004E9000,
    "RefCountCompositeMip": 0x004F2CE0,
    "TextureCacheHelper": 0x004F3BA0,
    "CharModelSlotDispatch": 0x004F2640,
}

GLOBALS = {
    "g_GlobalPasteDstMips": 0x00B6B870,
    "g_GlobalTextureCache": 0x00B6B864,
    "g_SectionDispatchPtr": 0x00B6B88C,
    "g_LocalCharComponent": 0x00B6B1A0,
    "g_LocalCharComponentModel": 0x00B6B1AC,
    "g_CharComponentPool": 0x00B6B240,
}

# Hardcoded section handlers (from disasm cluster 0x4F09D0..0x4F0E80)
SECTION_HANDLERS = {
    0: 0x004F09D0,
    1: 0x004F0A30,
    2: 0x004F0A90,
    3: 0x004F0C10,
    4: 0x004F0CA0,
    5: 0x004F0D04,
    6: 0x004F0DB4,
    7: 0x004F0E40,
    8: 0x004F0AD0,
    9: 0x004F0B90,
}

CC_OFF = {
    0x04: "linkedList?",
    0x08: "flags (bit0=needsPrep, bit2=skip)",
    0x0C: "sectionDirty (bits 0-9)",
    0x14: "raceSex?",
    0x18: "modelSub?",
    0x1C: "modelSub2?",
    0x28: "modelRoot?",
    0x34: "mipPoolIndex + mipSlot[40]",
    0x38: "bodyModelPtr (== unit+0xB4)",
    0x3C: "npcFlag",
    0x190: "sectionHandlerObj",
    0x194: "compositeMip0 / gear base",
    0x1A0: "sec9 mip A",
    0x1A4: "sec8 mip A",
    0x1AC: "sec9 mip B",
    0x1B0: "sec8 mip B",
    0x1BC: "sec9 mip C",
    0x1C0: "compositeMip3",
    0x1C8: "compositeMip4",
    0x248: "sec2 gear mip?",
    0x374: "sec7 gear mip?",
    0x52C: "faceFlushTemp",
}

CHAR_LO = 0x004D0000
CHAR_HI = 0x00550000
POOL_STRIDE = 0x198


def parse_pe(data: bytes):
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    coff = e_lfanew + 4
    _machine, num_sections, _ts, _symptr, _nsym, opt_size, _chars = struct.unpack_from(
        "<HHIIIHH", data, coff
    )
    opt = coff + 20
    sections = []
    sec_off = opt + opt_size
    for i in range(num_sections):
        off = sec_off + i * 40
        name = data[off : off + 8].split(b"\x00", 1)[0].decode("ascii", "ignore")
        vsize, vaddr, raw_size, raw_ptr = struct.unpack_from("<IIII", data, off + 8)
        sections.append(
            {"name": name, "vaddr": vaddr, "vsize": vsize, "raw_ptr": raw_ptr, "raw_size": raw_size}
        )
    return sections


def va_to_offset(va: int, sections) -> int | None:
    rva = va - IMAGE_BASE
    for s in sections:
        if s["vaddr"] <= rva < s["vaddr"] + max(s["vsize"], s["raw_size"]):
            return rva - s["vaddr"] + s["raw_ptr"]
    return None


def offset_to_va(off: int, sections) -> int | None:
    for s in sections:
        if s["raw_ptr"] <= off < s["raw_ptr"] + s["raw_size"]:
            return IMAGE_BASE + (off - s["raw_ptr"] + s["vaddr"])
    return None


def read_dword(data: bytes, va: int, sections) -> int | None:
    off = va_to_offset(va, sections)
    if off is None or off + 4 > len(data):
        return None
    return struct.unpack_from("<I", data, off)[0]


def find_callers(data: bytes, target: int, sections, lo=CHAR_LO, hi=CHAR_HI) -> list[int]:
    hits = []
    for i in range(len(data) - 5):
        if data[i] != 0xE8:
            continue
        rel = struct.unpack_from("<i", data, i + 1)[0]
        caller = offset_to_va(i, sections)
        if caller and lo <= caller <= hi and caller + 5 + rel == target:
            hits.append(caller)
    return sorted(set(hits))


def find_imm32_refs(data: bytes, imm: int, sections, lo=CHAR_LO, hi=CHAR_HI + 0x300000) -> list[int]:
    pat = struct.pack("<I", imm)
    hits: set[int] = set()
    start = 0
    while True:
        i = data.find(pat, start)
        if i < 0:
            break
        for pos in range(max(0, i - 3), i + 1):
            if pos + 5 > len(data):
                continue
            b0, b1 = data[pos], data[pos + 1] if pos + 1 < len(data) else 0
            rel = i - pos
            ok = (
                (b0 == 0xA1 and rel == 1)
                or (b0 == 0x8B and b1 in (0x0D, 0x15, 0x1D, 0x35, 0x3D) and rel == 2)
                or (b0 == 0x89 and b1 in (0x05, 0x15, 0x1D, 0x35, 0x3D) and rel == 2)
                or (b0 == 0x3B and b1 in (0x0D, 0x15, 0x1D, 0x35, 0x3D) and rel == 2)
                or (b0 == 0xFF and b1 == 0x35 and rel == 2)
            )
            if ok:
                va = offset_to_va(pos, sections)
                if va and lo <= va <= hi:
                    hits.add(va)
        start = i + 1
    return sorted(hits)


def scan_disp_refs(data: bytes, disp: int, sections, lo=CHAR_LO, hi=CHAR_HI) -> dict[str, list[int]]:
    out: dict[str, list[int]] = defaultdict(list)
    for reg, modrm_r, modrm_w in (
        ("esi", 0x86, 0x87),
        ("ecx", 0x81, 0x89),
        ("edi", 0x87, 0x85),
    ):
        for op, modrm in ((0x8B, modrm_r), (0x89, modrm_w)):
            pat = bytes([op, modrm]) + struct.pack("<I", disp)
            start = 0
            while True:
                i = data.find(pat, start)
                if i < 0:
                    break
                va = offset_to_va(i, sections)
                if va and lo <= va <= hi:
                    out[reg].append(va)
                start = i + 1
    return {k: sorted(set(v)) for k, v in out.items()}


def disasm_window(data: bytes, va: int, sections, before: int = 48, after: int = 32) -> list[str]:
    off = va_to_offset(va, sections)
    if off is None:
        return []
    start = max(0, off - before)
    chunk = data[start : off + after]
    base_va = offset_to_va(start, sections) or va
    lines = []
    i = 0
  # annotate from va-backward
    rel_start = va - base_va
    i = rel_start
    while i < len(chunk) and len(lines) < 30:
        addr = base_va + i
        b = chunk[i]
        mark = " <<" if addr == va else ""
        if b == 0xE8 and i + 4 < len(chunk):
            rel = struct.unpack_from("<i", chunk, i + 1)[0]
            tgt = addr + 5 + rel
            name = ""
            for n, f in FUNCS.items():
                if f == tgt:
                    name = f" ; {n}"
            lines.append(f"  0x{addr:08X}: call 0x{tgt:08X}{name}{mark}")
            i += 5
            continue
        if b == 0x8B and i + 1 < len(chunk) and chunk[i + 1] == 0xCE:
            lines.append(f"  0x{addr:08X}: mov ecx, esi  ; component{mark}")
            i += 2
            continue
        if b == 0x8B and i + 5 < len(chunk) and chunk[i + 1] in (0x86, 0x81, 0x87):
            disp = struct.unpack_from("<I", chunk, i + 2)[0]
            reg = {0x86: "esi", 0x81: "ecx", 0x87: "edi"}[chunk[i + 1]]
            label = CC_OFF.get(disp, "")
            tag = f"  ; [{reg}+0x{disp:X}] {label}" if label else ""
            lines.append(f"  0x{addr:08X}: mov r, [{reg}+0x{disp:X}]{tag}{mark}")
            i += 6
            continue
        if b == 0x6A and i + 1 < len(chunk):
            lines.append(f"  0x{addr:08X}: push {chunk[i+1]}{mark}")
            i += 2
            continue
        if b == 0x68 and i + 4 < len(chunk):
            imm = struct.unpack_from("<I", chunk, i + 1)[0]
            lines.append(f"  0x{addr:08X}: push 0x{imm:08X}{mark}")
            i += 5
            continue
        lines.append(f"  0x{addr:08X}: {b:02X}{mark}")
        i += 1
    return lines


def analyze_handler(data: bytes, fn: int, sections) -> dict:
    off = va_to_offset(fn, sections)
    if off is None:
        return {}
    chunk = data[off : off + 512]
    scratch = chunk.count(struct.pack("<I", GLOBALS["g_GlobalPasteDstMips"]))
    cache = chunk.count(struct.pack("<I", GLOBALS["g_GlobalTextureCache"]))
    uses_layout = b"\xE8" in chunk  # rough
    face_fields = []
    for disp in (0x1A0, 0x1A4, 0x1AC, 0x1B0, 0x1BC, 0x194, 0x248, 0x374):
        if struct.pack("<I", disp) in chunk:
            face_fields.append(f"+0x{disp:X}")
    return {
        "scratch_refs": scratch,
        "cache_refs": cache,
        "dst_fields": face_fields,
    }


def classify_renderprep_caller(data: bytes, va: int, sections) -> str:
    """Heuristic: local singleton vs pool remote."""
    ctx = disasm_window(data, va, sections, 80, 16)
    text = "\n".join(ctx)
    if "B6B1A0" in text or "g_Local" in text:
        return "LOCAL (uses g_LocalCharComponent @ B6B1A0)"
    if "B6B240" in text:
        return "POOL (uses g_CharComponentPool @ B6B240)"
    if "mov ecx, esi" in text:
        return "PASSTHROUGH (ecx already = component)"
    return "UNKNOWN"


def main() -> int:
    data = WOW.read_bytes()
    sections = parse_pe(data)
    lines: list[str] = []

    lines.append("WXL RE — MODEL ISOLATION MAP (WoW 3.3.5a)")
    lines.append("=" * 72)
    lines.append(f"Binary: {WOW}")
    lines.append("")
    lines.append("This document maps HOW the client distinguishes players and WHERE")
    lines.append("textures bleed if hooks ignore per-component context.")
    lines.append("")

    # ---- 1. Identity chain ----
    lines.append("## 1. PLAYER IDENTITY CHAIN (unit → visible model)")
    lines.append("")
    lines.append("  CGUnit_C* (player/NPC)")
    lines.append("    +0xB4  body model root (verified by WXL runtime: ComponentModel == unit+0xB4)")
    lines.append("    +0x30  GUID low (used by WXL for owner)")
    lines.append("")
    lines.append("  CharComponent* (per visible character body composite)")
    lines.append("    +0x38  bodyModelPtr — MUST match unit+0xB4 for correct owner binding")
    lines.append("    +0x0C  sectionDirty — bit N → rebuild section N")
    lines.append("    +0x194..+0x1C8  composite mip object pointers (PER COMPONENT)")
    lines.append("    +0x34   mip pool ring (40 slots) — RefCountCompositeMip rotates display")
    lines.append("")
    lines.append("  TWO storage sites for CharComponent*:")
    lines.append("    LOCAL PLAYER:  *[0x00B6B1A0]  singleton (NOT in pool)")
    lines.append("    OTHER PLAYERS: pool base *[0x00B6B240], stride 0x198 slots")
    lines.append("      index = (component - poolBase) / 0x198")
    lines.append("")

    # Pool allocation sites
    lines.append("### 1a. Pool @ B6B240 — allocation / lookup sites")
    pool_refs = find_imm32_refs(data, GLOBALS["g_CharComponentPool"], sections)
    for va in pool_refs[:25]:
        lines.append(f"  ref @ 0x{va:08X}")
    if len(pool_refs) > 25:
        lines.append(f"  ... +{len(pool_refs)-25} more")
    lines.append("")

    # Local singleton
    lines.append("### 1b. Local singleton @ B6B1A0 — RenderPrep entry points")
    local_refs = find_imm32_refs(data, GLOBALS["g_LocalCharComponent"], sections)
    rp_callers = find_callers(data, FUNCS["RenderPrep"], sections, 0x00400000, 0x00800000)
    for va in rp_callers:
        kind = classify_renderprep_caller(data, va, sections)
        lines.append(f"  RenderPrep called @ 0x{va:08X}  [{kind}]")
        for ln in disasm_window(data, va, sections, 40, 8)[:8]:
            lines.append(ln)
        lines.append("")
    lines.append("")

    # ---- 2. Shared vs per-unit ----
    lines.append("## 2. SHARED (GLOBAL) vs PER-UNIT (ISOLATED)")
    lines.append("")
    lines.append("| Resource | Address | Scope | Risk for recolor |")
    lines.append("|----------|---------|-------|------------------|")
    lines.append("| Paste scratch mips | *[0xB6B870] | **ONE for entire client** | "
                 "Gear paste writes here; last writer wins before flush |")
    lines.append("| TextureCache root | *[0xB6B864] | **ONE global cache** | "
                 "CPU pixel buffers shared; tint must be per-owner |")
    lines.append("| Section dispatch ptr | *[0xB6B88C] | Global ptr to handler table | Read-only at runtime |")
    lines.append("| CharComponent composites | [comp+0x194..] | **Per unit** | Safe if comp bound correctly |")
    lines.append("| sectionDirty mask | [comp+0x0C] | **Per unit** | WXL must not set 8-9 on gear-only pass |")
    lines.append("| Mip pool ring | [comp+0x34] | **Per unit** | FlushGearComposite swaps ALL slots incl face |")
    lines.append("| bodyModelPtr | [comp+0x38] | **Per unit** | Identity key: must == unit+0xB4 |")
    lines.append("")
    lines.append("ROOT CAUSE OF CROSS-PLAYER BLEED:")
    lines.append("  Client assumes single-threaded assemble: writes gear to B6B870, then")
    lines.append("  FlushGearComposite(comp) copies scratch → THAT comp's mips.")
    lines.append("  If two RenderPrep interleave OR scratch still has player-A pixels when")
    lines.append("  player-B's face handler reads B6B870 → face gets wrong texture.")
    lines.append("")

    # ---- 3. Pipeline ----
    lines.append("## 3. ASSEMBLE PIPELINE (pseudocode)")
    lines.append("")
    lines.append("```")
    lines.append("RenderPrep(CharComponent* comp, model*, int a2) @ 0x4F1520")
    lines.append("  if [comp+0x3C] NPC → early out")
    lines.append("  if [comp+0x0C] == 0 → early out")
    lines.append("  RenderPrepSections(comp) @ 0x4EE0D0")
    lines.append("    mask = [comp+0x0C]")
    lines.append("    for bit in 0..9:")
    lines.append("      if mask & (1<<bit):")
    lines.append("        handler = table[bit]  ; hardcoded cluster 0x4F09D0..0x4F0E40")
    lines.append("        handler(comp)")
    lines.append("    [comp+0x0C] &= ~processed_bits")
    lines.append("  FlushGearComposite(comp) @ 0x4E9000  ; ALWAYS runs if gear dirty path")
    lines.append("    RefCountCompositeMip([comp+0x194])")
    lines.append("    RefCountCompositeMip([comp+0x1A4])  ; FACE even if sec8 not dirty!")
    lines.append("    RefCountCompositeMip([comp+0x1B0])  ; FACE")
    lines.append("    RefCountCompositeMip([comp+0x1C0])")
    lines.append("    → publishes mip pool slot for GPU")
    lines.append("```")
    lines.append("")

  # ---- 4. Section table ----
    lines.append("## 4. SECTION HANDLERS (hardcoded, verified disasm)")
    lines.append("")
    lines.append("| Sec | Handler | Paste dst | Uses B6B870 scratch | Notes |")
    lines.append("|-----|---------|-----------|---------------------|-------|")
    sec_notes = {
        0: "skin base → scratch, then flush",
        1: "gear slot 1",
        2: "gear + [comp+0x248]",
        3: "gear slot 3",
        4: "gear slot 4",
        5: "gear slot 5",
        6: "gear slot 6",
        7: "gear + [comp+0x374]",
        8: "FACE: TextureCache → [+0x1A4][+0x1B0] THEN scratch→face",
        9: "HAIR: TextureCache → [+0x1A0][+0x1AC] THEN scratch→face",
    }
    for sec in range(10):
        fn = SECTION_HANDLERS.get(sec, 0)
        info = analyze_handler(data, fn, sections) if fn else {}
        scratch = info.get("scratch_refs", 0)
        dst = ", ".join(info.get("dst_fields", [])) or "scratch only"
        lines.append(
            f"| {sec} | 0x{fn:08X} | {dst} | {scratch}x | {sec_notes.get(sec,'')} |"
        )
    lines.append("")

    lines.append("### 4a. Section 8 handler sequence @ 0x4F0AD0 (FACE — critical)")
    for ln in disasm_window(data, 0x004F0AD0, sections, 0, 200):
        lines.append(ln)
    lines.append("")
    lines.append("  KEY: After skin paste to [comp+0x1A4/+0x1B0], ALWAYS pastes FROM")
    lines.append("  B6B870 scratch if [comp+0x1C0]!=0 — scratch content = LAST gear paste.")
    lines.append("  During /recolor PUSH with dirty=0x3FF, item gear textures hit sec 8-9")
    lines.append("  via TextureCacheHelper (face_gear_native in logs).")
    lines.append("")

    # ---- 5. FlushGearComposite ----
    lines.append("## 5. FlushGearComposite @ 0x4E9000 — per-component publish")
    lines.append("")
    lines.append("  esi = CharComponent* (thiscall)")
    lines.append("  Touches: +0x194, +0x1A4, +0x1B0, +0x1C0 — includes FACE mips")
    lines.append("  Uses global TextureCache @ B6B864 for composite texture object")
    lines.append("  RefCountCompositeMip @ 0x4F2CE0:")
    lines.append("    - inc [mip+0xA4] refcount")
    lines.append("    - store mip ptr in [comp+0x34] ring at index [comp+0x34]")
    lines.append("    - increment ring index")
    lines.append("")
    flush_callers = find_callers(data, FUNCS["FlushGearComposite"], sections, 0x004E0000, 0x00500000)
    lines.append(f"  Direct callers in char range: {len(flush_callers)}")
    for va in flush_callers[:10]:
        lines.append(f"    0x{va:08X}")
    lines.append("  (Also invoked indirectly from RenderPrep tail — scan RenderPrep for E8)")
    lines.append("")

    # Find FlushGearComposite call from RenderPrep
    rp_off = va_to_offset(FUNCS["RenderPrep"], sections)
    if rp_off:
        rp_chunk = data[rp_off : rp_off + 0x800]
        for i in range(len(rp_chunk) - 5):
            if rp_chunk[i] == 0xE8:
                rel = struct.unpack_from("<i", rp_chunk, i + 1)[0]
                tgt = FUNCS["RenderPrep"] + i + 5 + rel
                if tgt == FUNCS["FlushGearComposite"]:
                    lines.append(f"  RenderPrep → FlushGearComposite @ 0x{FUNCS['RenderPrep']+i:08X}")
    lines.append("")

    # ---- 6. CharComponent field xref density ----
    lines.append("## 6. CharComponent FIELD XREF MAP")
    lines.append("")
    for disp, name in sorted(CC_OFF.items()):
        refs = scan_disp_refs(data, disp, sections)
        total = sum(len(v) for v in refs.values())
        if total == 0:
            continue
        lines.append(f"  +0x{disp:03X} {name}: {total} sites")
    lines.append("")

    # ---- 7. WXL rules ----
    lines.append("## 7. RULES FOR WXL RECOLOR (derived from RE, NOT guesses)")
    lines.append("")
    lines.append("R1 IDENTITY: Never tint without verified CharComponent* where")
    lines.append("   ComponentModel(comp) == unit(owner)+0xB4.")
    lines.append("")
    lines.append("R2 SCRATCH SERIALIZATION: Only ONE assemble may use B6B870 at a time")
    lines.append("   across ALL players (mutex for entire RenderPrep+Flush window).")
    lines.append("")
    lines.append("R3 DIRTY MASK: Remote tint must set ONLY bits 0-7 (0xFF).")
    lines.append("   Bits 8-9 (0x300) trigger face handlers that read stale B6B870.")
    lines.append("   Login uses 0x3FF once per player sequentially — safe natively.")
    lines.append("   /recolor PUSH forces 0x3FF on remotes while scratch has gear tint.")
    lines.append("")
    lines.append("R4 FACE IS NOT GEAR: Section 8-9 handlers paste ITEM textures onto")
    lines.append("   face composite mips (+0x1A4/+0x1B0/+0x1A0) when ItemDisplayInfo")
    lines.append("   maps to those sections. Blocking must happen IN PasteFromSkin when")
    lines.append("   dst is face field AND src is item gear texture.")
    lines.append("")
    lines.append("R5 FLUSH IS PER-COMPONENT: FlushGearComposite cannot be skipped for")
    lines.append("   remotes (invisible model). It cannot be naively save/restored")
    lines.append("   (refcount crash). Must ensure scratch is correct BEFORE flush.")
    lines.append("")
    lines.append("R6 LOCAL VS REMOTE:")
    lines.append("   Self:  comp = *[B6B1A0], separate from pool")
    lines.append("   Remote: comp = pool[slot], stride 0x198")
    lines.append("   Same pipeline, different comp pointer — isolation is comp-level.")
    lines.append("")
    lines.append("R7 NATIVE-FIRST: Remote's first 0x3FF assemble after spawn must")
    lines.append("   complete untinted before any WXL gear tint (g_remoteNativeDone).")
    lines.append("")

    # ---- 8. Open RE ----
    lines.append("## 8. REMAINING RE (before next code change)")
    lines.append("")
    lines.append("  [ ] Trace exact caller @ 0x4E2FF8 / 0x4E6B9A — pool RenderPrep path")
    lines.append("  [ ] Find memcpy: B6B870 → composite mip object pixels (not just ptr)")
    lines.append("  [ ] Map [comp+0x34] ring: which slot index is displayed vs staging")
    lines.append("  [ ] Confirm: does PasteFromSkin(dst=B6B870) always precede face paste?")
    lines.append("  [ ] Runtime: log comp+0x38 vs unit+0xB4 for each bleed frame")
    lines.append("")

    text = "\n".join(lines)
    OUT.write_text(text, encoding="utf-8")
    print(f"Wrote {OUT} ({len(text)} chars, {len(lines)} lines)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

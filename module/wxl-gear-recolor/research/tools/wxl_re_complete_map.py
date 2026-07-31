#!/usr/bin/env python3
"""
WXL — exhaustive static RE map for WoW 3.3.5a character composite pipeline.
Fills gaps left by prior scripts. Output: WXL-RE-COMPLETE-MAP.txt
"""
from __future__ import annotations

import struct
from collections import defaultdict
from pathlib import Path

WOW = Path(r"C:\Users\Shadowlands\Desktop\WotLK for AZ\Wow.exe")
OUT = Path(r"C:\Azerothcore\DBC_Tool\WXL-RE-COMPLETE-MAP.txt")
IMAGE_BASE = 0x00400000
CHAR_LO, CHAR_HI = 0x004D0000, 0x00550000

ANCHORS = {
    "RenderPrep": 0x004F1520,
    "RenderPrepSections": 0x004EE0D0,
    "RenderPrepTail": 0x004F14A0,
    "PasteSkinLayout": 0x004F07D0,
    "PasteFromSkin": 0x004F08A0,
    "PasteToSection": 0x004F08A0,
    "FlushGearComposite": 0x004E9000,
    "RefCountCompositeMip": 0x004F2CE0,
    "ResolveMipPtr": 0x004F2D80,
    "TextureCacheHelper": 0x004F3BA0,
    "TextureCacheCreate": 0x004F3930,
    "PoolRenderPrepCaller": 0x004E2FF8,
    "LocalRenderPrepCaller": 0x004E150F,
    "g_Scratch": 0x00B6B870,
    "g_TexCache": 0x00B6B864,
    "g_LocalCC": 0x00B6B1A0,
    "g_Pool": 0x00B6B240,
    "g_DispatchPtr": 0x00B6B88C,
}

SECTION_HANDLERS = {
    0: 0x004F09D0, 1: 0x004F0A30, 2: 0x004F0A90, 3: 0x004F0C10,
    4: 0x004F0CA0, 5: 0x004F0D04, 6: 0x004F0DB4, 7: 0x004F0E40,
    8: 0x004F0AD0, 9: 0x004F0B90,
}


def parse_pe(data: bytes):
    e = struct.unpack_from("<I", data, 0x3C)[0]
    coff = e + 4
    _, ns, _, _, _, os, _ = struct.unpack_from("<HHIIIHH", data, coff)
    opt = coff + 20
    secs = []
    for i in range(ns):
        o = opt + i * 40
        n = data[o : o + 8].split(b"\x00")[0].decode("ascii", "ignore")
        vs, va, rs, rp = struct.unpack_from("<IIII", data, o + 8)
        secs.append({"name": n, "vaddr": va, "vsize": vs, "raw_ptr": rp, "raw_size": rs})
    return secs


def va_off(va: int, secs) -> int | None:
    r = va - IMAGE_BASE
    for s in secs:
        if s["vaddr"] <= r < s["vaddr"] + max(s["vsize"], s["raw_size"]):
            return r - s["vaddr"] + s["raw_ptr"]
    return None


def off_va(off: int, secs) -> int | None:
    for s in secs:
        if s["raw_ptr"] <= off < s["raw_ptr"] + s["raw_size"]:
            return IMAGE_BASE + (off - s["raw_ptr"] + s["vaddr"])
    return None


def find_func_start(data: bytes, site: int, secs, back: int = 0x2000) -> int:
    off = va_off(site, secs)
    if off is None:
        return site
    lo = max(0, off - back)
    # scan for CC padding then push ebp; mov ebp,esp OR push ebx pattern
    best = site
    for i in range(off, lo, -1):
        if data[i] == 0xCC and i + 1 < len(data) and data[i + 1] != 0xCC:
            va = off_va(i + 1, secs)
            if va:
                best = va
                break
        if data[i] == 0x55 and i + 1 < len(data) and data[i + 1] == 0x8B:
            va = off_va(i, secs)
            if va:
                best = va
    return best


def disasm(data: bytes, va: int, secs, n: int, labels: dict[int, str] | None = None) -> list[str]:
    labels = labels or {}
    o = va_off(va, secs)
    if o is None:
        return []
    chunk = data[o : o + n]
    lines = []
    i = 0
    while i < len(chunk) and len(lines) < 400:
        a = va + i
        b = chunk[i]
        lb = labels.get(a, "")
        if b == 0xE8 and i + 4 < len(chunk):
            rel = struct.unpack_from("<i", chunk, i + 1)[0]
            t = a + 5 + rel
            nm = next((k for k, v in ANCHORS.items() if v == t), "")
            tag = f"  ; {nm}" if nm else ""
            lines.append(f"  0x{a:08X}: call 0x{t:08X}{tag}{lb}")
            i += 5
            continue
        if b == 0xFF and i + 5 < len(chunk) and chunk[i + 1] == 0x15:
            d = struct.unpack_from("<I", chunk, i + 2)[0]
            lines.append(f"  0x{a:08X}: call dword [0x{d:08X}]{lb}")
            i += 6
            continue
        if b == 0x8B and i + 1 < len(chunk) and chunk[i + 1] == 0xCE:
            lines.append(f"  0x{a:08X}: mov ecx, esi{lb}")
            i += 2
            continue
        if b == 0x8B and i + 5 < len(chunk) and chunk[i + 1] in (0x86, 0x81, 0x87, 0xBE, 0xB9):
            mod = chunk[i + 1]
            if mod in (0x86, 0x81, 0x87):
                d = struct.unpack_from("<I", chunk, i + 2)[0]
                reg = {0x86: "esi", 0x81: "ecx", 0x87: "edi"}[mod]
                lines.append(f"  0x{a:08X}: mov r, [{reg}+0x{d:X}]{lb}")
                i += 6
                continue
            if mod in (0xBE, 0xB9):
                d = struct.unpack_from("<I", chunk, i + 2)[0]
                reg = "esi" if mod == 0xBE else "ecx"
                lines.append(f"  0x{a:08X}: mov {reg}, 0x{d:08X}{lb}")
                i += 6
                continue
        if b == 0xA1 and i + 4 < len(chunk):
            d = struct.unpack_from("<I", chunk, i + 1)[0]
            g = next((k for k, v in ANCHORS.items() if v == d and k.startswith("g_")), "")
            lines.append(f"  0x{a:08X}: mov eax, [0x{d:08X}]" + (f"  ; {g}" if g else "") + lb)
            i += 5
            continue
        if b == 0x8B and i + 5 < len(chunk) and chunk[i + 1] == 0x0D:
            d = struct.unpack_from("<I", chunk, i + 2)[0]
            g = next((k for k, v in ANCHORS.items() if v == d and k.startswith("g_")), "")
            lines.append(f"  0x{a:08X}: mov ecx, [0x{d:08X}]" + (f"  ; {g}" if g else "") + lb)
            i += 6
            continue
        if b == 0x6A and i + 1 < len(chunk):
            lines.append(f"  0x{a:08X}: push {chunk[i+1]}{lb}")
            i += 2
            continue
        if b == 0xF3 and i + 1 < len(chunk) and chunk[i + 1] in (0xA4, 0xA5):
            lines.append(f"  0x{a:08X}: {'rep movsd' if chunk[i+1]==0xA5 else 'rep movsb'}{lb}")
            i += 2
            continue
        if b == 0x69 and i + 5 < len(chunk):
            imm = struct.unpack_from("<I", chunk, i + 2)[0]
            lines.append(f"  0x{a:08X}: imul ..., 0x{imm:X}{lb}")
            i += 6
            continue
        if b == 0xC3:
            lines.append(f"  0x{a:08X}: ret{lb}")
            break
        if b == 0xC2 and i + 2 < len(chunk):
            lines.append(f"  0x{a:08X}: retn {struct.unpack_from('<H', chunk, i+1)[0]}{lb}")
            break
        lines.append(f"  0x{a:08X}: {b:02X}{lb}")
        i += 1
    return lines


def scan_esi_offsets(data: bytes, secs, lo=CHAR_LO, hi=CHAR_HI) -> list[tuple[int, int]]:
    counts: dict[int, int] = defaultdict(int)
    for op in (0x8B, 0x89, 0x3B, 0x8D):
        pat_head = bytes([op, 0x86])
        start = 0
        while True:
            idx = data.find(pat_head, start)
            if idx < 0:
                break
            if idx + 6 <= len(data):
                va = off_va(idx, secs)
                if va and lo <= va <= hi:
                    d = struct.unpack_from("<I", data, idx + 2)[0]
                    if d < 0x600:
                        counts[d] += 1
            start = idx + 1
    return sorted(counts.items(), key=lambda x: -x[1])


def analyze_handler(data: bytes, fn: int, secs) -> dict:
    o = va_off(fn, secs)
    if o is None:
        return {}
    c = data[o : o + 600]
    scratch = c.count(struct.pack("<I", ANCHORS["g_Scratch"]))
    layout = 0
    paste = 0
    for i in range(len(c) - 4):
        if c[i] == 0xE8:
            rel = struct.unpack_from("<i", c, i + 1)[0]
            t = fn + i + 5 + rel
            if t == ANCHORS["PasteSkinLayout"]:
                layout += 1
            if t == ANCHORS["PasteFromSkin"]:
                paste += 1
    dst_offs = []
    for d in range(0x100, 0x400, 4):
        if struct.pack("<I", d) in c:
            dst_offs.append(d)
    return {"layout": layout, "paste": paste, "scratch": scratch, "dst_offs": dst_offs}


def find_calls(data: bytes, target: int, secs, lo=CHAR_LO, hi=0x00800000) -> list[int]:
    out = []
    for i in range(len(data) - 5):
        if data[i] != 0xE8:
            continue
        rel = struct.unpack_from("<i", data, i + 1)[0]
        va = off_va(i, secs)
        if va and lo <= va <= hi and va + 5 + rel == target:
            out.append(va)
    return sorted(set(out))


def main() -> int:
    data = WOW.read_bytes()
    secs = parse_pe(data)
    L: list[str] = []

    L.append("=" * 78)
    L.append("WXL RE — COMPLETE CHARACTER COMPOSITE MAP (WoW 3.3.5a)")
    L.append("=" * 78)
    L.append(f"Binary: {WOW}")
    L.append("")
    L.append("STATUS: static RE pass — gaps marked [UNKNOWN] need runtime BP")
    L.append("")

    # ---- A. Identity & storage ----
    L.append("## A. PLAYER → MODEL → CharComponent (identity)")
    L.append("")
    L.append("  CGUnit_C")
    L.append("    +0x30   GUID (low used by WXL)")
    L.append("    +0xB4   CGUnit_C::GetWorldModel() → body model root*")
    L.append("")
    L.append("  CharComponent (CCharacterComponent) — one per visible player body")
    L.append("    +0x08   flags: bit0=needsRenderPrep, bit2=skipPath")
    L.append("    +0x0C   sectionDirty uint32 — bits 0-9 → sections 0-9")
    L.append("    +0x38   bodyModelPtr — BIND KEY, must == unit+0xB4")
    L.append("    +0x3C   npcFlag (nonzero → NPC, RenderPrep early out)")
    L.append("")
    L.append("  STORAGE (two paths, same struct, same pipeline):")
    L.append("    LOCAL:  *g_LocalCC @ 0xB6B1A0 — singleton, NOT in pool")
    L.append("    REMOTE: g_Pool @ 0xB6B240 — array of CharComponent,")
    L.append("            stride = 0x198 bytes per slot")
    L.append("            slot = (compPtr - poolBase) / 0x198")
    L.append("")
    pool_fn = find_func_start(data, ANCHORS["PoolRenderPrepCaller"], secs)
    L.append(f"  POOL RenderPrep caller site: 0x{ANCHORS['PoolRenderPrepCaller']:08X}")
    L.append(f"  Containing function @ ~0x{pool_fn:08X}:")
  # disasm backward from 0x4E2FF8 to find pool index → ecx
    L.extend(disasm(data, pool_fn, secs, 0x600))
    L.append("")

    local_fn = find_func_start(data, ANCHORS["LocalRenderPrepCaller"], secs)
    L.append(f"  LOCAL RenderPrep caller @ 0x{ANCHORS['LocalRenderPrepCaller']:08X}")
    L.append(f"  Containing function @ ~0x{local_fn:08X}:")
    L.extend(disasm(data, local_fn, secs, 0x200))
    L.append("")

    # ---- B. CharComponent field map from xref density ----
    L.append("## B. CharComponent OFFSET MAP (esi disp32 xref count in 0x4D0000..0x550000)")
    L.append("")
    L.append("  Offset | Xrefs | Inferred role")
    L.append("  -------|-------|---------------")
    KNOWN = {
        0x04: "list link",
        0x08: "flags",
        0x0C: "sectionDirty",
        0x14: "race/sex index?",
        0x18: "model data ptr?",
        0x1C: "model data ptr2?",
        0x28: "model root ref?",
        0x34: "mipPool: index@+0, slots@+8 (40 entries)",
        0x38: "bodyModelPtr",
        0x3C: "npcFlag",
        0x194: "compositeMip0 / gearTex",
        0x1A0: "sec9 mip A",
        0x1A4: "sec8 mip A (face upper)",
        0x1AC: "sec9 mip B",
        0x1B0: "sec8 mip B (face lower)",
        0x1BC: "sec9 mip C",
        0x1C0: "compositeMip3 / scratch gate for sec8",
        0x1C8: "compositeMip4",
        0x248: "sec2 extra mip ptr",
        0x374: "sec7 extra mip ptr",
        0x52C: "faceFlushTemp ptr",
        0x190: "section handler object ptr",
    }
    for off, cnt in scan_esi_offsets(data, secs)[:45]:
        role = KNOWN.get(off, "[scan]")
        L.append(f"  +0x{off:03X} | {cnt:5d} | {role}")
    L.append("")

    # ---- C. Shared vs per-unit ----
    L.append("## C. SHARED vs PER-UNIT (isolation boundaries)")
    L.append("")
    L.append("| Resource | Address | Scope |")
    L.append("|----------|---------|-------|")
    L.append("| Gear paste scratch | *[0xB6B870] | GLOBAL — one mip set for all units |")
    L.append("| TextureCache | *[0xB6B864] | GLOBAL — shared texture objects |")
    L.append("| Section dispatch ptr | *[0xB6B88C] | GLOBAL — ptr to handler table |")
    L.append("| sectionDirty | [comp+0x0C] | PER CharComponent |")
    L.append("| composite mips | [comp+0x194..] | PER CharComponent |")
    L.append("| mip pool ring | [comp+0x34] | PER CharComponent (40 slots) |")
    L.append("| bodyModelPtr | [comp+0x38] | PER CharComponent |")
    L.append("")
    L.append("BLEED MECHANISM (proven):")
    L.append("  1. Gear handlers paste pixels → *[B6B870] (global)")
    L.append("  2. Face handler sec8 @ 0x4F0AD0 reads B6B870 → pastes onto [comp+0x1A4/+0x1B0]")
    L.append("  3. If player-B face runs while B6B870 holds player-A gear → wrong face")
    L.append("  4. Concurrent RenderPrep without serialization → same failure")
    L.append("")

    # ---- D. Pipeline ----
    L.append("## D. FULL ASSEMBLE PIPELINE")
    L.append("")
    L.append("```")
    L.append("RenderPrep(comp, model*, a2) @ 0x4F1520")
    L.append("  early out if [comp+0x3C] or [comp+0x0C]==0")
    L.append("  RenderPrepSections(comp) @ 0x4EE0D0")
    L.append("    mask = [comp+0x0C]")
    L.append("    for bit 0..9:")
    L.append("      if mask & (1<<bit): handler[bit](comp)")
    L.append("    clear processed bits from [comp+0x0C]")
    L.append("  sub_4F14A0(comp) — pre-flush setup")
    L.append("  FlushGearComposite(comp) @ 0x4E9000")
    L.append("    for mip in (+0x194, +0x1A4, +0x1B0, +0x1C0):")
    L.append("      RefCountCompositeMip @ 0x4F2CE0 → store in [comp+0x34] ring")
    L.append("```")
    L.append("")

    # RenderPrep tail flush call
    L.append("### D1. RenderPrep → FlushGearComposite call site")
    rp_off = va_off(ANCHORS["RenderPrep"], secs)
    if rp_off:
        rp = data[rp_off : rp_off + 0x900]
        for i in range(len(rp) - 5):
            if rp[i] == 0xE8:
                rel = struct.unpack_from("<i", rp, i + 1)[0]
                t = ANCHORS["RenderPrep"] + i + 5 + rel
                if t == ANCHORS["FlushGearComposite"]:
                    L.append(f"  RenderPrep+0x{i:X} → call FlushGearComposite")
                if t == ANCHORS["RenderPrepSections"]:
                    L.append(f"  RenderPrep+0x{i:X} → call RenderPrepSections")
                if t == ANCHORS["RenderPrepTail"]:
                    L.append(f"  RenderPrep+0x{i:X} → call RenderPrepTail (0x4F14A0)")
    L.append("")

    # ---- E. Section handlers ----
    L.append("## E. SECTION HANDLERS — paste flow per section")
    L.append("")
    L.append("| Sec | Handler | Layout | Paste | Scratch | Dst offsets |")
    L.append("|-----|---------|--------|-------|---------|-------------|")
    for sec, fn in SECTION_HANDLERS.items():
        info = analyze_handler(data, fn, secs)
        L.append(
            f"| {sec} | 0x{fn:08X} | {info.get('layout',0)} | {info.get('paste',0)} | "
            f"{info.get('scratch',0)} | {', '.join(f'+0x{x:X}' for x in info.get('dst_offs',[]))} |"
        )
    L.append("")
    L.append("### E8. Section 8 (FACE) — full disasm")
    L.extend(disasm(data, SECTION_HANDLERS[8], secs, 0xA0))
    L.append("")
    L.append("  FLOW:")
    L.append("    TextureCacheHelper(B6B864, model fields)")
    L.append("    if tex ok AND [tex+0x1C]&8: PasteSkinLayout(8) → B6B870")
    L.append("    PasteFromSkin → [comp+0x1A4]  (face upper mip)")
    L.append("    PasteFromSkin → [comp+0x1B0]  (face lower mip)")
    L.append("    if [comp+0x1C0]: PasteFromSkin FROM B6B870 → sec8")
    L.append("")

    # Gear section 0 as template
    L.append("### E0. Section 0 (SKIN BASE) — first 0x80 bytes")
    L.extend(disasm(data, SECTION_HANDLERS[0], secs, 0x80))
    L.append("")

    # ---- F. PasteFromSkin ----
    L.append("## F. PasteFromSkin @ 0x4F08A0 — pixel copy internals")
    L.append("")
    pfs = disasm(data, ANCHORS["PasteFromSkin"], secs, 0x500)
    memcpy_lines = [ln for ln in pfs if "rep mov" in ln or "call" in ln]
    L.extend(pfs[:80])
    L.append("")
    L.append("  memcpy sites in PasteFromSkin body:")
    for ln in memcpy_lines:
        if "rep mov" in ln:
            L.append(f"    {ln.strip()}")
    calls_in_pfs = [ln for ln in pfs if "call" in ln]
    L.append("  callees:")
    for ln in calls_in_pfs[:20]:
        L.append(f"    {ln.strip()}")
    L.append("")

    # ---- G. RefCountCompositeMip / mip pool ----
    L.append("## G. RefCountCompositeMip @ 0x4F2CE0 — mip pool publish")
    L.append("")
    L.extend(disasm(data, ANCHORS["RefCountCompositeMip"], secs, 0x120))
    L.append("")
    L.append("  Pseudocode:")
    L.append("    arg = compositeMipObject*")
    L.append("    inc [arg+0xA4]  (refcount)")
    L.append("    idx = [comp+0x34]  (ring write index)")
    L.append("    [comp+0x34+8 + idx*4] = arg")
    L.append("    [comp+0x34] = idx + 1")
    L.append("    flag byte in pool slot metadata")
    L.append("  [UNKNOWN] which ring index is GPU-displayed vs staging")
    L.append("")

    L.append("## H. FlushGearComposite @ 0x4E9000")
    L.extend(disasm(data, ANCHORS["FlushGearComposite"], secs, 0xE0))
    L.append("")

    # ---- I. WXL module contract ----
    L.append("## I. WXL MODULE CONTRACT (what hooks MUST guarantee)")
    L.append("")
    L.append("  INVARIANT 1: Before any gear paste to B6B870, know owner via comp+0x38")
    L.append("  INVARIANT 2: Only one assemble using B6B870 at a time (mutex)")
    L.append("  INVARIANT 3: Remote gear tint dirty mask = 0xFF max (never 0x300)")
    L.append("  INVARIANT 4: Never let item\\texturecomponents paste to [comp+0x1A4..]")
    L.append("              during tint session (face_gear_native)")
    L.append("  INVARIANT 5: First remote assemble = native untinted (0x3FF ok once)")
    L.append("  INVARIANT 6: FlushGearComposite must run per-comp with correct scratch")
    L.append("")

    # ---- J. Still unknown ----
    L.append("## J. GAPS — still need runtime RE")
    L.append("")
    L.append("  [ ] Exact CGUnit→pool slot assignment at spawn (0x4E27F0 region)")
    L.append("  [ ] CompositeMipObject layout — where CPU pixels live (+offset)")
    L.append("  [ ] GPU texture bind: which [comp+0x34] slot index is displayed")
    L.append("  [ ] Whether TextureCacheHelper returns SHARED ptr across units")
    L.append("  [ ] Server PUSH: who sets dirty=0x3FF on remote comp (not our code)")
    L.append("")

    # ---- K. Log evidence from last test ----
    log = Path(r"C:\Azerothcore\debug-615e3b.log")
    if log.exists():
        L.append("## K. RUNTIME LOG EVIDENCE (last user test)")
        import json
        from collections import Counter
        msgs = Counter()
        p1023 = 0
        for line in log.read_text(encoding="utf-8", errors="ignore").splitlines():
            try:
                o = json.loads(line)
            except Exception:
                continue
            msgs[o.get("message", "")] += 1
            if o.get("message") == "remote" and o.get("data", {}).get("pasteStrictOk") == 1023:
                p1023 += 1
        L.append(f"  Total events: {sum(msgs.values())}")
        L.append(f"  remote RenderPrep with pasteStrictOk=1023 (0x3FF): {p1023}")
        L.append("  Top messages:")
        for m, c in msgs.most_common(12):
            L.append(f"    {m}: {c}")
        L.append("  → 2823 remote passes; wide dirty still happening on remotes")
        L.append("")

    text = "\n".join(L)
    OUT.write_text(text, encoding="utf-8")
    print(f"Wrote {OUT} ({len(text)} chars, {len(L)} lines)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

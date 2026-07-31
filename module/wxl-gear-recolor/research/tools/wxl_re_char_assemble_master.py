#!/usr/bin/env python3
"""
WXL gear-recolor — master static RE for Wow 3.3.5a CharComponent assemble pipeline.

Goal: map how the client builds player body/face/gear composites — offsets, call graph,
section dispatch, scratch B6B870 lifetime, local-vs-remote component paths.

Output: DBC_Tool/WXL-RE-CHAR-ASSEMBLE-MASTER.txt
"""
from __future__ import annotations

import struct
from collections import defaultdict
from pathlib import Path

WOW = Path(r"C:\Users\Shadowlands\Desktop\WotLK for AZ\Wow.exe")
OUT = Path(r"C:\Azerothcore\DBC_Tool\WXL-RE-CHAR-ASSEMBLE-MASTER.txt")
IMAGE_BASE = 0x00400000

GLOBALS = {
    "g_GlobalPasteDstMips": 0x00B6B870,
    "g_GlobalPasteAux": 0x00B6B864,
    "g_SectionDispatch": 0x00B6B88C,
    "g_LocalCharComponent": 0x00B6B1A0,
    "g_LocalCharComponentModel": 0x00B6B1AC,
    "g_CharComponentPool": 0x00B6B240,
}

FUNCS = {
    "RenderPrep": 0x004F1520,
    "RenderPrepSections": 0x004EE0D0,
    "PasteSkinLayout": 0x004F07D0,
    "PasteFromSkin": 0x004F08A0,
    "FlushGearComposite": 0x004E9000,
    "RefCountCompositeMip": 0x004F2CE0,
    "TextureCacheHelper": 0x004F3BA0,
    "TextureCacheCreate": 0x004F3930,
    "CharModelSlotDispatch": 0x004F2640,
}

# Suspected CCharacterComponent fields (this=esi/ecx in char render range)
CC_FIELDS = {
    0x08: "flags?",
    0x0C: "sectionDirty",
    0x38: "modelPtr",
    0x3C: "componentFlags",
    0x190: "sectionHandler?",
    0x194: "compositeMip0",
    0x1A0: "faceSec9_a",
    0x1A4: "faceSec8_a / compositeMip1",
    0x1AC: "faceSec9_b",
    0x1B0: "faceSec8_b / compositeMip2",
    0x1BC: "faceSec9_c",
    0x1C0: "compositeMip3",
    0x1C8: "compositeMip4?",
    0x52C: "faceFlushTemp?",
}

CHAR_CODE_LO = 0x004E0000
CHAR_CODE_HI = 0x00530000


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


def find_imm32_refs_fixed(data: bytes, imm: int, sections) -> list[int]:
    """All code sites where imm32 appears as a memory operand (fixed vs old script)."""
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
            b0 = data[pos]
            b1 = data[pos + 1] if pos + 1 < len(data) else 0
            rel_off = i - pos
            ok = False
            if b0 == 0xA1 and rel_off == 1:  # mov eax, [imm32]
                ok = True
            elif b0 == 0x8B and b1 in (0x0D, 0x15, 0x1D, 0x35, 0x3D) and rel_off == 2:
                ok = True
            elif b0 == 0x89 and b1 in (0x05, 0x15, 0x1D, 0x35, 0x3D) and rel_off == 2:
                ok = True
            elif b0 == 0xFF and b1 == 0x35 and rel_off == 2:  # push [imm32]
                ok = True
            elif b0 == 0xC7 and b1 == 0x05 and rel_off == 2:  # mov [imm32], imm
                ok = True
            elif b0 == 0x3B and b1 in (0x0D, 0x15, 0x1D, 0x35, 0x3D) and rel_off == 2:
                ok = True
            if ok:
                va = offset_to_va(pos, sections)
                if va and CHAR_CODE_LO <= va <= CHAR_CODE_HI + 0x200000:
                    hits.add(va)
        start = i + 1
    return sorted(hits)


def find_callers(data: bytes, target: int, sections) -> list[int]:
    hits = []
    for i in range(len(data) - 5):
        if data[i] != 0xE8:
            continue
        rel = struct.unpack_from("<i", data, i + 1)[0]
        caller = offset_to_va(i, sections)
        if caller and caller + 5 + rel == target:
            hits.append(caller)
    return sorted(set(hits))


def scan_disp_refs(data: bytes, base_reg: int, disp: int, sections, lo: int, hi: int) -> list[int]:
  # base_reg: 6=esi, 1=ecx, 7=edi
    modrm_map = {6: 0x86, 1: 0x81, 7: 0x87}  # [esi+disp32], [ecx+disp32], [edi+disp32]
    modrm = modrm_map.get(base_reg)
    if modrm is None:
        return []
    pat = bytes([0x8B, modrm]) + struct.pack("<I", disp)
    pat_st = bytes([0x89, modrm]) + struct.pack("<I", disp)
    hits: set[int] = set()
    for pat_use in (pat, pat_st):
        start = 0
        while True:
            i = data.find(pat_use, start)
            if i < 0:
                break
            va = offset_to_va(i, sections)
            if va and lo <= va <= hi:
                hits.add(va)
            start = i + 1
    return sorted(hits)


def disasm_range(data: bytes, start_va: int, sections, nbytes: int = 256, max_lines: int = 120) -> list[str]:
    off = va_to_offset(start_va, sections)
    if off is None:
        return []
    chunk = data[off : off + nbytes]
    lines = []
    i = 0
    while i < len(chunk) and len(lines) < max_lines:
        addr = start_va + i
        b = chunk[i]
        if b == 0xE8 and i + 4 < len(chunk):
            rel = struct.unpack_from("<i", chunk, i + 1)[0]
            lines.append(f"  0x{addr:08X}: call 0x{addr + 5 + rel:08X}")
            i += 5
            continue
        if b == 0xFF and i + 5 < len(chunk) and chunk[i + 1] == 0x15:
            disp = struct.unpack_from("<I", chunk, i + 2)[0]
            lines.append(f"  0x{addr:08X}: call dword ptr [0x{disp:08X}]")
            i += 6
            continue
        if b == 0xF3 and i + 5 < len(chunk) and chunk[i + 1] == 0xA5:
            lines.append(f"  0x{addr:08X}: rep movsd")
            i += 2
            continue
        if b == 0xF3 and i + 2 < len(chunk) and chunk[i + 1] == 0xA4:
            lines.append(f"  0x{addr:08X}: rep movsb")
            i += 2
            continue
        if b == 0x8B and i + 5 < len(chunk):
            modrm = chunk[i + 1]
            if modrm in (0x86, 0x81, 0x87):
                disp = struct.unpack_from("<I", chunk, i + 2)[0]
                reg = {0x86: "esi", 0x81: "ecx", 0x87: "edi"}[modrm]
                lines.append(f"  0x{addr:08X}: mov r, [{reg}+0x{disp:X}]")
                i += 6
                continue
            if modrm in (0x46, 0x41, 0x47):
                disp = chunk[i + 2]
                reg = {0x46: "esi", 0x41: "ecx", 0x47: "edi"}[modrm]
                lines.append(f"  0x{addr:08X}: mov r, [{reg}+0x{disp:X}]")
                i += 3
                continue
        if b == 0x89 and i + 5 < len(chunk) and chunk[i + 1] in (0x86, 0x81, 0x87):
            modrm = chunk[i + 1]
            disp = struct.unpack_from("<I", chunk, i + 2)[0]
            reg = {0x86: "esi", 0x81: "ecx", 0x87: "edi"}[modrm]
            lines.append(f"  0x{addr:08X}: mov [{reg}+0x{disp:X}], r")
            i += 6
            continue
        if b == 0xA1 and i + 4 < len(chunk):
            disp = struct.unpack_from("<I", chunk, i + 1)[0]
            lines.append(f"  0x{addr:08X}: mov eax, [0x{disp:08X}]")
            i += 5
            continue
        if b == 0x8B and i + 5 < len(chunk) and chunk[i + 1] == 0x0D:
            disp = struct.unpack_from("<I", chunk, i + 2)[0]
            lines.append(f"  0x{addr:08X}: mov ecx, [0x{disp:08X}]")
            i += 6
            continue
        if b == 0x68 and i + 4 < len(chunk):
            imm = struct.unpack_from("<I", chunk, i + 1)[0]
            lines.append(f"  0x{addr:08X}: push 0x{imm:08X}")
            i += 5
            continue
        if b == 0x6A and i + 1 < len(chunk):
            lines.append(f"  0x{addr:08X}: push {chunk[i+1]}")
            i += 2
            continue
        if b == 0xC3:
            lines.append(f"  0x{addr:08X}: ret")
            break
        if b == 0xC2 and i + 2 < len(chunk):
            n = struct.unpack_from("<H", chunk, i + 1)[0]
            lines.append(f"  0x{addr:08X}: retn {n}")
            break
        lines.append(f"  0x{addr:08X}: {b:02X}")
        i += 1
    return lines


def find_func_bounds(data: bytes, func_va: int, sections, max_scan: int = 0x4000) -> tuple[int, int]:
    """Naive: from func_va scan until int3 padding or second ret after substantial code."""
    off = va_to_offset(func_va, sections)
    if off is None:
        return func_va, func_va + 0x200
    end = min(len(data), off + max_scan)
    last_ret = off
    for i in range(off, end):
        if data[i] == 0xC3:
            last_ret = i
        if data[i] == 0xCC and i > off + 32:
            break
    end_va = offset_to_va(last_ret, sections) or (func_va + 0x200)
    return func_va, end_va + 1


def scan_memcpy_in_range(data: bytes, lo_va: int, hi_va: int, sections) -> list[str]:
    out = []
    lo_off = va_to_offset(lo_va, sections)
    hi_off = va_to_offset(hi_va, sections)
    if lo_off is None or hi_off is None:
        return out
    for i in range(lo_off, min(hi_off, len(data) - 2)):
        if data[i] == 0xF3 and data[i + 1] in (0xA4, 0xA5):
            va = offset_to_va(i, sections)
            if va:
                kind = "rep movsd" if data[i + 1] == 0xA5 else "rep movsb"
                out.append(f"  0x{va:08X}: {kind}")
    return out


def build_caller_graph(data: bytes, roots: dict[str, int], sections, depth: int = 2) -> list[str]:
    lines = []
    for name, va in roots.items():
        c1 = find_callers(data, va, sections)
        lines.append(f"{name} @ 0x{va:08X}: {len(c1)} direct callers")
        for c in c1[:20]:
            lines.append(f"  <- 0x{c:08X}")
            if depth >= 2:
                for c2 in find_callers(data, c, sections)[:6]:
                    lines.append(f"      <- 0x{c2:08X}")
        if len(c1) > 20:
            lines.append(f"  ... +{len(c1)-20} more")
        lines.append("")
    return lines


def main() -> int:
    data = WOW.read_bytes()
    sections = parse_pe(data)
    out: list[str] = []

    out.append("WXL RE — CharComponent player assemble master dump")
    out.append(f"Binary: {WOW}")
    out.append("=" * 72)
    out.append("")

    out.append("## 1. GLOBAL REFS (fixed imm32 scan)")
    out.append("")
    for gname, gva in GLOBALS.items():
        refs = find_imm32_refs_fixed(data, gva, sections)
        out.append(f"### {gname} @ 0x{gva:08X} — {len(refs)} code refs")
        for va in refs[:30]:
            out.append(f"  0x{va:08X}")
            for line in disasm_range(data, va, sections, 20, 8):
                out.append(line)
        if len(refs) > 30:
            out.append(f"  ... +{len(refs)-30} more")
        out.append("")

    out.append("## 2. CALL GRAPH (assemble pipeline)")
    out.append("")
    out.extend(build_caller_graph(data, FUNCS, sections, depth=2))

    out.append("## 3. RenderPrep @ 0x004F1520 (first 1.5 KB)")
    out.append("")
    for line in disasm_range(data, FUNCS["RenderPrep"], sections, 1536, 200):
        out.append(line)
    out.append("")

    out.append("## 4. RenderPrepSections @ 0x004EE0D0 (first 1 KB)")
    out.append("")
    for line in disasm_range(data, FUNCS["RenderPrepSections"], sections, 1024, 160):
        out.append(line)
    out.append("")

    out.append("## 5. FlushGearComposite @ 0x004E9000 (full)")
    lo, hi = find_func_bounds(data, FUNCS["FlushGearComposite"], sections)
    out.append(f"Bounds ~0x{lo:08X}..0x{hi:08X}")
    for line in disasm_range(data, lo, sections, hi - lo + 64, 120):
        out.append(line)
    out.append("")
    out.append("FlushGearComposite callers:")
    for c in find_callers(data, FUNCS["FlushGearComposite"], sections):
        out.append(f"  <- 0x{c:08X}")
        for line in disasm_range(data, c - 48, sections, 128, 24):
            out.append(line)
    out.append("")

    out.append("## 6. PasteFromSkin caller clusters (face path 0x4F0AF0+)")
    out.append("")
    for line in disasm_range(data, 0x004F0A80, sections, 512, 200):
        out.append(line)
    out.append("")

    out.append("## 7. CharComponent field xref map (esi/ecx in 0x4E0000..0x520000)")
    out.append("")
    for disp, label in sorted(CC_FIELDS.items()):
        for reg, rname in ((6, "esi"), (1, "ecx"), (7, "edi")):
            hits = scan_disp_refs(data, reg, disp, sections, CHAR_CODE_LO, CHAR_CODE_HI)
            if hits:
                out.append(f"  +0x{disp:03X} ({label}) [{rname}]: {len(hits)} sites")
                for va in hits[:8]:
                    out.append(f"    0x{va:08X}")
    out.append("")

    out.append("## 8. sectionDirty +0x0C writes in char range")
    out.append("")
    for reg, rname in ((6, "esi"), (1, "ecx"), (7, "edi")):
        hits = scan_disp_refs(data, reg, 0x0C, sections, CHAR_CODE_LO, CHAR_CODE_HI)
        out.append(f"  [{rname}+0x0C]: {len(hits)}")
        for va in hits[:12]:
            out.append(f"    0x{va:08X}")
    out.append("")

    out.append("## 9. memcpy (rep movs*) in B6B870-ref functions")
    out.append("")
    b870_refs = find_imm32_refs_fixed(data, GLOBALS["g_GlobalPasteDstMips"], sections)
    funcs_with_b870: set[int] = set()
    for va in b870_refs:
        # walk back to nearest push ebp / standard prologue — rough func start
        off = va_to_offset(va, sections)
        if off is None:
            continue
        for back in range(0, 0x800):
            p = off - back
            if p < 0:
                break
            if data[p] == 0x55 and data[p + 1] == 0x8B and data[p + 2] == 0xEC:
                fva = offset_to_va(p, sections)
                if fva:
                    funcs_with_b870.add(fva)
                break
    for fva in sorted(funcs_with_b870)[:25]:
        _, fhi = find_func_bounds(data, fva, sections, 0x2000)
        mem = scan_memcpy_in_range(data, fva, fhi, sections)
        if mem:
            out.append(f"  func ~0x{fva:08X}:")
            out.extend(mem[:12])
    out.append("")

    out.append("## 10. PasteSkinLayout + PasteFromSkin entry (128 B each)")
    out.append("")
    for fname in ("PasteSkinLayout", "PasteFromSkin"):
        out.append(f"### {fname}")
        for line in disasm_range(data, FUNCS[fname.replace("PasteFromSkin", "PasteFromSkin").replace("PasteSkinLayout", "PasteSkinLayout")], sections, 128, 40):
            out.append(line)
        out.append("")

    out.append("## 11. OPEN QUESTIONS (for interactive RE / breakpoints)")
    out.append("")
    out.append("  Q1: Where does scratch *[B6B870] copy INTO [esi+0x194..0x1C0]? (memcpy site)")
    out.append("  Q2: Does section 8 paste ALWAYS go PasteSkinLayout then PasteFromSkin(scratch→face)?")
    out.append("  Q3: Local self: which RenderPrep path skips face sections 8-9 dirty bits?")
    out.append("  Q4: Is g_LocalCharComponent @ B6B1A0 the ONLY self composite, or also pool slot?")
    out.append("  Q5: What does RenderPrepSections dispatch table @ B6B88C index per section?")
    out.append("  Q6: Does tinted gear paste change TextureCache pixels shared by face skin path?")
    out.append("")

    text = "\n".join(out)
    OUT.write_text(text, encoding="utf-8")
    print(f"Wrote {OUT} ({len(text)} chars, {len(out)} lines)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

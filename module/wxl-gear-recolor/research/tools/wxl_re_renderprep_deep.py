#!/usr/bin/env python3
"""Deep static RE: RenderPrep body, section dispatch table, per-section handlers."""
from __future__ import annotations

import struct
from pathlib import Path

WOW = Path(r"C:\Users\Shadowlands\Desktop\WotLK for AZ\Wow.exe")
OUT = Path(r"C:\Azerothcore\DBC_Tool\WXL-RE-RENDERPREP-DEEP.txt")
IMAGE_BASE = 0x00400000

RENDER_PREP = 0x004F1520
RENDER_PREP_SECTIONS = 0x004EE0D0
DISPATCH_TABLE = 0x00B6B88C  # dword[10] of handler fn ptrs
GLOBAL_SCRATCH = 0x00B6B870


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
        sections.append({"name": name, "vaddr": vaddr, "vsize": vsize, "raw_ptr": raw_ptr, "raw_size": raw_size})
    return sections


def va_to_offset(va: int, sections) -> int | None:
    rva = va - IMAGE_BASE
    for s in sections:
        if s["vaddr"] <= rva < s["vaddr"] + max(s["vsize"], s["raw_size"]):
            return rva - s["vaddr"] + s["raw_ptr"]
    return None


def read_dword(data: bytes, va: int, sections) -> int | None:
    off = va_to_offset(va, sections)
    if off is None or off + 4 > len(data):
        return None
    return struct.unpack_from("<I", data, off)[0]


def disasm(data: bytes, start_va: int, sections, nbytes: int, max_lines: int = 500) -> list[str]:
    off = va_to_offset(start_va, sections)
    if off is None:
        return [f"  (invalid va 0x{start_va:08X})"]
    chunk = data[off : off + nbytes]
    lines: list[str] = []
    i = 0
    while i < len(chunk) and len(lines) < max_lines:
        addr = start_va + i
        b = chunk[i]
        # call rel32
        if b == 0xE8 and i + 4 < len(chunk):
            rel = struct.unpack_from("<i", chunk, i + 1)[0]
            tgt = addr + 5 + rel
            tag = ""
            if tgt == RENDER_PREP_SECTIONS:
                tag = "  ; *** RenderPrepSections"
            elif tgt == 0x004F14CC - 5 + 5:
                pass
            lines.append(f"  0x{addr:08X}: call 0x{tgt:08X}{tag}")
            i += 5
            continue
        # jmp rel32
        if b == 0xE9 and i + 4 < len(chunk):
            rel = struct.unpack_from("<i", chunk, i + 1)[0]
            lines.append(f"  0x{addr:08X}: jmp 0x{addr + 5 + rel:08X}")
            i += 5
            continue
        # call [imm32]
        if b == 0xFF and i + 5 < len(chunk) and chunk[i + 1] == 0x15:
            disp = struct.unpack_from("<I", chunk, i + 2)[0]
            lines.append(f"  0x{addr:08X}: call dword ptr [0x{disp:08X}]")
            i += 6
            continue
        if b == 0xA1 and i + 4 < len(chunk):
            disp = struct.unpack_from("<I", chunk, i + 1)[0]
            note = "  ; scratch" if disp == GLOBAL_SCRATCH else ""
            lines.append(f"  0x{addr:08X}: mov eax, [0x{disp:08X}]{note}")
            i += 5
            continue
        if b == 0x8B and i + 5 < len(chunk):
            modrm = chunk[i + 1]
            if modrm == 0x0D:
                disp = struct.unpack_from("<I", chunk, i + 2)[0]
                lines.append(f"  0x{addr:08X}: mov ecx, [0x{disp:08X}]")
                i += 6
                continue
            if modrm in (0x86, 0x81, 0x87):
                disp = struct.unpack_from("<I", chunk, i + 2)[0]
                reg = {0x86: "esi", 0x81: "ecx", 0x87: "edi"}[modrm]
                note = ""
                if disp == 0x0C:
                    note = "  ; sectionDirty"
                elif disp == 0x190:
                    note = "  ; sectionHandler"
                elif disp in (0x194, 0x1A4, 0x1B0, 0x1C0, 0x1A0):
                    note = f"  ; compositeMip+0x{disp:X}"
                lines.append(f"  0x{addr:08X}: mov r, [{reg}+0x{disp:X}]{note}")
                i += 6
                continue
            if modrm in (0x46, 0x41, 0x47) and i + 2 < len(chunk):
                disp = chunk[i + 2]
                reg = {0x46: "esi", 0x41: "ecx", 0x47: "edi"}[modrm]
                note = "  ; sectionDirty" if disp == 0x0C else ""
                lines.append(f"  0x{addr:08X}: mov r, [{reg}+0x{disp:X}]{note}")
                i += 3
                continue
            if modrm == 0xF1:
                lines.append(f"  0x{addr:08X}: mov esi, ecx  ; this=component")
                i += 2
                continue
        if b == 0x89 and i + 5 < len(chunk) and chunk[i + 1] in (0x86, 0x81, 0x87):
            disp = struct.unpack_from("<I", chunk, i + 2)[0]
            reg = {0x86: "esi", 0x81: "ecx", 0x87: "edi"}[chunk[i + 1]]
            lines.append(f"  0x{addr:08X}: mov [{reg}+0x{disp:X}], r")
            i += 6
            continue
        if b == 0x85 and i + 2 < len(chunk) and chunk[i + 1] in (0x46, 0x41, 0x47):
            disp = chunk[i + 2]
            reg = {0x46: "esi", 0x41: "ecx", 0x47: "edi"}[chunk[i + 1]]
            note = "  ; test sectionDirty" if disp == 0x0C else ""
            lines.append(f"  0x{addr:08X}: test [{reg}+0x{disp:X}], r{note}")
            i += 3
            continue
        if b == 0xF7 and i + 3 < len(chunk) and chunk[i + 1] in (0x46, 0x41, 0x47):
            disp = chunk[i + 2]
            reg = {0x46: "esi", 0x41: "ecx", 0x47: "edi"}[chunk[i + 1]]
            note = "  ; test byte sectionDirty" if disp == 0x0C else ""
            lines.append(f"  0x{addr:08X}: test byte [{reg}+0x{disp:X}], imm{note}")
            i += 3
            continue
        if b == 0x68 and i + 4 < len(chunk):
            imm = struct.unpack_from("<I", chunk, i + 1)[0]
            note = f"  ; section {imm}" if imm <= 9 else ""
            lines.append(f"  0x{addr:08X}: push {imm}{note}")
            i += 5
            continue
        if b == 0x6A and i + 1 < len(chunk):
            lines.append(f"  0x{addr:08X}: push {chunk[i+1]}")
            i += 2
            continue
        if b == 0xC3:
            lines.append(f"  0x{addr:08X}: ret")
            i += 1
            continue
        if b == 0xC2 and i + 2 < len(chunk):
            n = struct.unpack_from("<H", chunk, i + 1)[0]
            lines.append(f"  0x{addr:08X}: retn {n}")
            i += 3
            continue
        lines.append(f"  0x{addr:08X}: {b:02X}")
        i += 1
    return lines


def find_calls_to(data: bytes, target: int, sections, lo: int, hi: int) -> list[int]:
    hits = []
    for i in range(len(data) - 5):
        if data[i] != 0xE8:
            continue
        rel = struct.unpack_from("<i", data, i + 1)[0]
        va = IMAGE_BASE
        # convert file offset to va
        for s in sections:
            if s["raw_ptr"] <= i < s["raw_ptr"] + s["raw_size"]:
                va = IMAGE_BASE + (i - s["raw_ptr"] + s["vaddr"])
                break
        if lo <= va <= hi and va + 5 + rel == target:
            hits.append(va)
    return sorted(set(hits))


def dump_handler(data: bytes, fn_va: int, sections, out: list[str], label: str):
    out.append(f"### Handler {label} @ 0x{fn_va:08X}")
    for line in disasm(data, fn_va, sections, 384, 80):
        out.append(line)
    out.append("")


def main() -> int:
    data = WOW.read_bytes()
    sections = parse_pe(data)
    out: list[str] = []
    out.append("WXL RE — RenderPrep deep + section dispatch table")
    out.append(f"Binary: {WOW}")
    out.append("=" * 72)
    out.append("")

    out.append("## 1. Section dispatch table @ 0x00B6B88C (10 x fn ptr)")
    out.append("")
    handlers: list[tuple[int, int]] = []
    for sec in range(10):
        ptr_va = DISPATCH_TABLE + sec * 4
        fn = read_dword(data, ptr_va, sections)
        if fn:
            handlers.append((sec, fn))
            out.append(f"  section {sec}: 0x{fn:08X}")
    out.append("")

    out.append("## 2. RenderPrep full (0x4F1520 .. 0x4F1800)")
    out.append("  esi=component (thiscall ecx). Early exits: [esi+0x3C] NPC, [esi+0x08] bit2")
    out.append("")
    for line in disasm(data, RENDER_PREP, sections, 0x400, 300):
        out.append(line)
    out.append("")

    out.append("## 3. Call sites -> RenderPrepSections")
    for va in find_calls_to(data, RENDER_PREP_SECTIONS, sections, 0x004F0000, 0x00500000):
        out.append(f"  call @ 0x{va:08X}")
        for line in disasm(data, va - 32, sections, 64, 20):
            out.append(line)
        out.append("")
    out.append("")

    out.append("## 4. RenderPrepSections full (0x4EE0D0 .. 0x4EE400)")
    out.append("  edi=component. Loop: ebx=bit, [edi+0x0C] dirty mask, table @ B6B88C")
    out.append("")
    for line in disasm(data, RENDER_PREP_SECTIONS, sections, 0x400, 350):
        out.append(line)
    out.append("")

    out.append("## 5. Per-section handler entry (first 80 lines each)")
    out.append("")
    seen = set()
    for sec, fn in handlers:
        if fn in seen:
            out.append(f"### section {sec}: same fn as earlier 0x{fn:08X}")
            continue
        seen.add(fn)
        dump_handler(data, fn, sections, out, f"section {sec}")

    out.append("## 6. Face cluster 0x4F0A80..0x4F0C80 (PasteSkinLayout + PasteFromSkin)")
    out.append("")
    for line in disasm(data, 0x004F0A80, sections, 0x200, 200):
        out.append(line)
    out.append("")

    out.append("## 7. KEY FINDINGS (auto)")
    out.append("")
    # Count which handlers reference B6B870
    for sec, fn in handlers:
        hits = 0
        off = va_to_offset(fn, sections)
        if off is None:
            continue
        chunk = data[off : off + 512]
        pat = struct.pack("<I", GLOBAL_SCRATCH)
        hits = chunk.count(pat)
        uses_scratch = "YES" if hits else "no"
        uses_face_mip = "?"
        off2 = va_to_offset(fn, sections)
        if off2:
            c = data[off2 : off2 + 512]
            if b"\xA4\x01" in c or b"\xB0\x01" in c:  # +0x1A4 +0x1B0 rough
                uses_face_mip = "likely"
        out.append(f"  sec {sec} handler 0x{fn:08X}: scratch_refs={hits} face_mip={uses_face_mip}")
    out.append("")
    out.append("## 8. HYPOTHESIS")
    out.append("  Gear-only dirty mask (bits 0-7) dispatches handlers 0-7 only.")
    out.append("  Sections 8-9 handlers run only when dirty bits 8-9 set (full body / login).")
    out.append("  Local gear tint rebuild NEVER sets bits 8-9 -> face not repasted -> black if corrupted.")
    out.append("  WXL must NOT block face path; must understand when 8-9 get set natively.")

    text = "\n".join(out)
    OUT.write_text(text, encoding="utf-8")
    print(f"Wrote {OUT} ({len(text)} chars)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Trace RenderPrep pool callers + memcpy from global scratch B6B870."""
from __future__ import annotations

import struct
from pathlib import Path

WOW = Path(r"C:\Users\Shadowlands\Desktop\WotLK for AZ\Wow.exe")
OUT = Path(r"C:\Azerothcore\DBC_Tool\WXL-RE-POOL-RENDERPREP.txt")
IMAGE_BASE = 0x00400000
RENDER_PREP = 0x004F1520
SCRATCH = 0x00B6B870
POOL = 0x00B6B240
PASTE_FROM_SKIN = 0x004F08A0


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


def offset_to_va(off: int, sections) -> int | None:
    for s in sections:
        if s["raw_ptr"] <= off < s["raw_ptr"] + s["raw_size"]:
            return IMAGE_BASE + (off - s["raw_ptr"] + s["vaddr"])
    return None


def disasm(data: bytes, start_va: int, sections, nbytes: int) -> list[str]:
    off = va_to_offset(start_va, sections)
    if off is None:
        return []
    chunk = data[off : off + nbytes]
    lines = []
    i = 0
    while i < len(chunk):
        addr = start_va + i
        b = chunk[i]
        if b == 0xE8 and i + 4 < len(chunk):
            rel = struct.unpack_from("<i", chunk, i + 1)[0]
            tgt = addr + 5 + rel
            tag = ""
            if tgt == RENDER_PREP:
                tag = " ; *** RenderPrep"
            elif tgt == PASTE_FROM_SKIN:
                tag = " ; PasteFromSkin"
            lines.append(f"  0x{addr:08X}: call 0x{tgt:08X}{tag}")
            i += 5
            continue
        if b == 0x8B and i + 5 < len(chunk) and chunk[i + 1] in (0x86, 0x81, 0x87):
            disp = struct.unpack_from("<I", chunk, i + 2)[0]
            reg = {0x86: "esi", 0x81: "ecx", 0x87: "edi"}[chunk[i + 1]]
            lines.append(f"  0x{addr:08X}: mov r, [{reg}+0x{disp:X}]")
            i += 6
            continue
        if b == 0xA1 and i + 4 < len(chunk):
            disp = struct.unpack_from("<I", chunk, i + 1)[0]
            g = ""
            if disp == POOL:
                g = " ; pool"
            elif disp == SCRATCH:
                g = " ; scratch"
            lines.append(f"  0x{addr:08X}: mov eax, [0x{disp:08X}]{g}")
            i += 5
            continue
        if b == 0x8B and i + 5 < len(chunk) and chunk[i + 1] == 0x0D:
            disp = struct.unpack_from("<I", chunk, i + 2)[0]
            g = " ; pool" if disp == POOL else ""
            lines.append(f"  0x{addr:08X}: mov ecx, [0x{disp:08X}]{g}")
            i += 6
            continue
        if b == 0xF3 and i + 1 < len(chunk) and chunk[i + 1] in (0xA4, 0xA5):
            kind = "rep movsd" if chunk[i + 1] == 0xA5 else "rep movsb"
            lines.append(f"  0x{addr:08X}: {kind}")
            i += 2
            continue
        if b == 0x69 and i + 5 < len(chunk):
            imm = struct.unpack_from("<I", chunk, i + 2)[0]
            lines.append(f"  0x{addr:08X}: imul ..., 0x{imm:X}")
            i += 6
            continue
        if b == 0xC3:
            lines.append(f"  0x{addr:08X}: ret")
            break
        lines.append(f"  0x{addr:08X}: {b:02X}")
        i += 1
    return lines


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


def main() -> int:
    data = WOW.read_bytes()
    sections = parse_pe(data)
    out: list[str] = []
    out.append("WXL RE — Pool RenderPrep callers + scratch memcpy")
    out.append("=" * 60)

    callers = find_callers(data, RENDER_PREP, sections)
    out.append(f"\n## All RenderPrep callers ({len(callers)})")
    for va in callers:
        out.append(f"\n### caller @ 0x{va:08X}")
        for ln in disasm(data, va - 64, sections, 128):
            out.append(ln)

    out.append("\n## PasteFromSkin body — memcpy scan (0x4F08A0 .. +0x600)")
    pfs_off = va_to_offset(PASTE_FROM_SKIN, sections)
    if pfs_off:
        chunk = data[pfs_off : pfs_off + 0x600]
        for i in range(len(chunk) - 2):
            if chunk[i] == 0xF3 and chunk[i + 1] in (0xA4, 0xA5):
                va = PASTE_FROM_SKIN + i
                out.append(f"  memcpy @ 0x{va:08X}: {'rep movsd' if chunk[i+1]==0xA5 else 'rep movsb'}")

    out.append("\n## Functions referencing B6B870 that also contain rep movs")
    pat = struct.pack("<I", SCRATCH)
    start = 0
    funcs_with_memcpy: set[int] = set()
    while True:
        i = data.find(pat, start)
        if i < 0:
            break
        va = offset_to_va(i, sections)
        if va and 0x004E0000 <= va <= 0x00530000:
            # scan ±512 bytes for rep movs
            off = va_to_offset(va, sections)
            if off:
                win = data[max(0, off - 256) : off + 256]
                if b"\xF3\xA5" in win or b"\xF3\xA4" in win:
                    # find function start (naive: scan back for CC padding)
                    funcs_with_memcpy.add(va & ~0xFF)
        start = i + 1

    out.append(f"  (rough clusters: {len(funcs_with_memcpy)})")

    out.append("\n## Pool slot math @ 0x4E2E96 region")
    for ln in disasm(data, 0x004E2E80, sections, 0x180):
        out.append(ln)

    text = "\n".join(out)
    OUT.write_text(text, encoding="utf-8")
    print(f"Wrote {OUT} ({len(text)} chars)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

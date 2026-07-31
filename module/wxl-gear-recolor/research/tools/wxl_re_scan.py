#!/usr/bin/env python3
"""Static RE scan of Wow.exe 3.3.5a for CharComponent / RenderPrep call graph."""
from __future__ import annotations

import struct
import sys
from pathlib import Path

WOW = Path(r"C:\Users\Shadowlands\Desktop\WotLK for AZ\Wow.exe")
IMAGE_BASE = 0x00400000

TARGETS = {
    "RenderPrep": 0x004F1520,
    "RenderPrepSections": 0x004EE0D0,
    "PasteSkinLayout": 0x004F07D0,
    "PasteFromSkin": 0x004F08A0,
    "TextureCacheCreate": 0x004F3930,
    "CharModelSlotDispatch": 0x004F2640,
    "GetRenderCtx": 0x0081F8F0,
    "M2PerFrameUpdate": 0x00828A00,
}


def parse_pe(data: bytes):
    if data[:2] != b"MZ":
        raise ValueError("not MZ")
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew : e_lfanew + 4] != b"PE\x00\x00":
        raise ValueError("not PE")
    coff = e_lfanew + 4
    _machine, num_sections, _ts, _symptr, _nsym, opt_size, _chars = struct.unpack_from(
        "<HHIIIHH", data, coff
    )
    opt = coff + 20
    magic = struct.unpack_from("<H", data, opt)[0]
    if magic != 0x10B:
        raise ValueError(f"expected PE32, got magic 0x{magic:X}")
    image_base = struct.unpack_from("<I", data, opt + 28)[0]
    sections = []
    sec_off = opt + opt_size
    for i in range(num_sections):
        off = sec_off + i * 40
        name = data[off : off + 8].split(b"\x00", 1)[0].decode("ascii", "ignore")
        vsize, vaddr, raw_size, raw_ptr = struct.unpack_from("<IIII", data, off + 8)
        sections.append(
            {
                "name": name,
                "vaddr": vaddr,
                "vsize": vsize,
                "raw_ptr": raw_ptr,
                "raw_size": raw_size,
            }
        )
    return image_base, sections


def va_to_offset(va: int, sections) -> int | None:
    rva = va - IMAGE_BASE
    for s in sections:
        if s["vaddr"] <= rva < s["vaddr"] + max(s["vsize"], s["raw_size"]):
            return rva - s["vaddr"] + s["raw_ptr"]
    return None


def offset_to_va(off: int, sections) -> int | None:
    for s in sections:
        if s["raw_ptr"] <= off < s["raw_ptr"] + s["raw_size"]:
            rva = off - s["raw_ptr"] + s["vaddr"]
            return IMAGE_BASE + rva
    return None


def find_callers(data: bytes, target_va: int, sections) -> list[tuple[int, int]]:
    """Find E8 rel32 call sites targeting target_va."""
    hits: list[tuple[int, int]] = []
    for i in range(len(data) - 5):
        if data[i] != 0xE8:
            continue
        rel = struct.unpack_from("<i", data, i + 1)[0]
        caller_va = offset_to_va(i, sections)
        if caller_va is None:
            continue
        dest = caller_va + 5 + rel
        if dest == target_va:
            hits.append((caller_va, i))
    return hits


def find_push_imm32_refs(data: bytes, target_va: int, sections) -> list[int]:
    hits: list[int] = []
    imm = struct.pack("<I", target_va)
    for i in range(len(data) - 5):
        if data[i] == 0x68 and data[i + 1 : i + 5] == imm:
            va = offset_to_va(i, sections)
            if va:
                hits.append(va)
    return hits


def disasm_window(data: bytes, func_va: int, sections, nbytes: int = 128) -> list[str]:
    off = va_to_offset(func_va, sections)
    if off is None:
        return [f"(no section for 0x{func_va:08X})"]
    chunk = data[off : off + nbytes]
    lines: list[str] = []
    i = 0
    while i < len(chunk) and len(lines) < 40:
        b = chunk[i]
        addr = func_va + i
        if b == 0xE8 and i + 4 < len(chunk):
            rel = struct.unpack_from("<i", chunk, i + 1)[0]
            dest = addr + 5 + rel
            lines.append(f"  0x{addr:08X}: call 0x{dest:08X}")
            i += 5
            continue
        if b == 0xFF and i + 1 < len(chunk) and chunk[i + 1] == 0x15:
            disp = struct.unpack_from("<I", chunk, i + 2)[0]
            lines.append(f"  0x{addr:08X}: call dword ptr [0x{disp:08X}]")
            i += 6
            continue
        if b in (0x8B, 0x89) and i + 1 < len(chunk):
            modrm = chunk[i + 1]
            # [reg+disp8/32] common in thiscall prologues
            if (modrm & 0xC0) == 0x80:
                lines.append(f"  0x{addr:08X}: mov {b:02X} modrm=0x{modrm:02X} [+disp32]")
                i += 6
                continue
            if (modrm & 0xC0) == 0x40:
                lines.append(f"  0x{addr:08X}: mov {b:02X} modrm=0x{modrm:02X} [+disp8]")
                i += 3
                continue
        if b == 0xA1 and i + 4 < len(chunk):
            disp = struct.unpack_from("<I", chunk, i + 1)[0]
            lines.append(f"  0x{addr:08X}: mov eax, [0x{disp:08X}]")
            i += 5
            continue
        if b == 0x83 and i + 2 < len(chunk):
            lines.append(f"  0x{addr:08X}: {b:02X} {chunk[i+1]:02X} {chunk[i+2]:02X}")
            i += 3
            continue
        if b == 0xC2 and i + 2 < len(chunk):
            n = struct.unpack_from("<H", chunk, i + 1)[0]
            lines.append(f"  0x{addr:08X}: retn {n}")
            i += 3
            continue
        if b == 0xC3:
            lines.append(f"  0x{addr:08X}: ret")
            i += 1
            continue
        lines.append(f"  0x{addr:08X}: {b:02X}")
        i += 1
    return lines


def scan_component_field_refs(data: bytes, sections) -> list[tuple[int, int]]:
    """Find cmp/test on [ecx+0x3C] (NPC flag) near RenderPrep."""
    hits: list[tuple[int, int]] = []
    # 83 79 3C 00 = cmp dword ptr [ecx+3Ch], 0
    pat = bytes([0x83, 0x79, 0x3C, 0x00])
    start = 0
    while True:
        i = data.find(pat, start)
        if i < 0:
            break
        va = offset_to_va(i, sections)
        if va and 0x004E0000 <= va <= 0x00500000:
            hits.append((va, i))
        start = i + 1
    return hits


def main() -> int:
    wow = WOW
    if len(sys.argv) > 1:
        wow = Path(sys.argv[1])
    data = wow.read_bytes()
    image_base, sections = parse_pe(data)
    out: list[str] = []
    out.append(f"File: {wow}")
    out.append(f"Size: {len(data)}")
    out.append(f"PE ImageBase: 0x{image_base:08X} (scanner uses 0x{IMAGE_BASE:08X})")
    out.append("")

    for name, va in sorted(TARGETS.items(), key=lambda x: x[1]):
        callers = find_callers(data, va, sections)
        pushes = find_push_imm32_refs(data, va, sections)
        out.append(f"=== {name} @ 0x{va:08X} ===")
        out.append(f"  E8 callers ({len(callers)}):")
        for cva, _ in callers[:24]:
            out.append(f"    0x{cva:08X}")
        if len(callers) > 24:
            out.append(f"    ... +{len(callers)-24} more")
        if pushes:
            out.append(f"  push imm32 refs ({len(pushes)}):")
            for p in pushes[:8]:
                out.append(f"    0x{p:08X}")
        out.append("  prologue disasm:")
        for line in disasm_window(data, va, sections):
            out.append(line)
        out.append("")

    out.append("=== cmp [ecx+3Ch], 0 in 0x004E0000..0x00500000 (CharComponent NPC?) ===")
    for va, _ in scan_component_field_refs(data, sections):
        out.append(f"  0x{va:08X}")

    # RenderPrep callers: disasm a few bytes before each call
    rp = TARGETS["RenderPrep"]
    callers = find_callers(data, rp, sections)
    out.append("")
    out.append(f"=== RenderPrep caller context ({len(callers)} sites) ===")
    for cva, file_off in callers[:16]:
        off = va_to_offset(cva, sections)
        if off is None:
            continue
        pre = data[max(0, off - 32) : off + 8]
        out.append(f"--- caller 0x{cva:08X} ---")
        for line in disasm_window(data, cva - 32, sections, 48):
            out.append(line)

    text = "\n".join(out)
    report = Path(r"C:\Azerothcore\DBC_Tool\WXL-RE-SCAN-OUTPUT.txt")
    report.write_text(text, encoding="utf-8")
    print(text)
    print(f"\nWrote {report}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

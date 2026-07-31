#!/usr/bin/env python3
"""RE: global paste dst B6B870, CharComponent composite fields, copy-out sites."""
from __future__ import annotations

import struct
from pathlib import Path

WOW = Path(r"C:\Users\Shadowlands\Desktop\WotLK for AZ\Wow.exe")
IMAGE_BASE = 0x00400000

GLOBALS = {
    "g_GlobalPasteDstMips": 0x00B6B870,
    "g_LocalCharComponent": 0x00B6B1A0,
    "g_CharComponentPool": 0x00B6B240,
}

FUNCS = {
    "RenderPrep": 0x004F1520,
    "RenderPrepSections": 0x004EE0D0,
    "PasteSkinLayout": 0x004F07D0,
    "PasteFromSkin": 0x004F08A0,
}

# Known / suspected CharComponent composite mip ptr fields (esi=this)
CC_OFFS = [0x194, 0x1A4, 0x1B0, 0x1BC, 0x1C8, 0x1D4, 0x1E0, 0x1EC, 0x1F8, 0x204]


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


def find_imm32_refs(data: bytes, imm: int, sections) -> list[tuple[int, int]]:
    pat = struct.pack("<I", imm)
    hits: list[tuple[int, int]] = []
    start = 0
    while True:
        i = data.find(pat, start)
        if i < 0:
            break
        # imm32 at i; instruction likely starts 1-2 bytes before
        for back in (0, 1, 2):
            pos = i - back
            if pos < 0:
                continue
            b0 = data[pos]
            b1 = data[pos + 1] if pos + 1 < len(data) else 0
            ok = False
            if b0 == 0xA1 and back == 1:  # mov eax, [imm32]
                ok = True
            elif b0 == 0x8B and b1 in (0x0D, 0x15, 0x1D, 0x35, 0x3D) and back == 2:
                ok = True
            elif b0 == 0xFF and b1 == 0x35 and back == 1:  # push [imm32]
                ok = True
            elif b0 == 0xC7 and back == 2:  # mov [imm32], imm (rare)
                ok = True
            if ok:
                va = offset_to_va(pos, sections)
                if va:
                    hits.append((va, pos))
                break
        start = i + 1
    # dedupe by va
    seen = set()
    out = []
    for va, pos in hits:
        if va not in seen:
            seen.add(va)
            out.append((va, pos))
    return sorted(out)


def find_callers(data: bytes, target: int, sections) -> list[int]:
    hits = []
    for i in range(len(data) - 5):
        if data[i] != 0xE8:
            continue
        rel = struct.unpack_from("<i", data, i + 1)[0]
        caller = offset_to_va(i, sections)
        if caller and caller + 5 + rel == target:
            hits.append(caller)
    return hits


def scan_esi_disp(data: bytes, disp: int, sections, lo=0x004E0000, hi=0x00520000) -> list[int]:
    """Find instructions referencing [esi+disp] in function range."""
    hits = []
    # 8B 86 disp32 = mov eax, [esi+disp32]
    pat_le = bytes([0x8B, 0x86]) + struct.pack("<I", disp)
    # 89 86 disp32 = mov [esi+disp32], eax
    pat_st = bytes([0x89, 0x86]) + struct.pack("<I", disp)
    # 8D b6 disp32 = lea esi, [esi+disp32] unlikely
    for pat in (pat_le, pat_st):
        start = 0
        while True:
            i = data.find(pat, start)
            if i < 0:
                break
            va = offset_to_va(i, sections)
            if va and lo <= va <= hi:
                hits.append(va)
            start = i + 1
    return sorted(set(hits))


def disasm_range(data: bytes, start_va: int, sections, nbytes: int = 256) -> list[str]:
    off = va_to_offset(start_va, sections)
    if off is None:
        return []
    chunk = data[off : off + nbytes]
    lines = []
    i = 0
    while i < len(chunk) and len(lines) < 80:
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
        if b == 0x8B and i + 5 < len(chunk):
            modrm = chunk[i + 1]
            if modrm == 0x86:  # [esi+disp32]
                disp = struct.unpack_from("<I", chunk, i + 2)[0]
                lines.append(f"  0x{addr:08X}: mov r, [esi+0x{disp:X}]")
                i += 6
                continue
            if modrm == 0x46 and i + 2 < len(chunk):  # [esi+disp8]
                disp = chunk[i + 2]
                lines.append(f"  0x{addr:08X}: mov r, [esi+0x{disp:X}]")
                i += 3
                continue
        if b == 0x89 and i + 5 < len(chunk) and chunk[i + 1] == 0x86:
            disp = struct.unpack_from("<I", chunk, i + 2)[0]
            lines.append(f"  0x{addr:08X}: mov [esi+0x{disp:X}], r")
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


def main() -> int:
    data = WOW.read_bytes()
    sections = parse_pe(data)
    out: list[str] = []
    out.append(f"File: {WOW}")
    out.append("")

    for name, gva in GLOBALS.items():
        refs = find_imm32_refs(data, gva, sections)
        out.append(f"=== {name} @ 0x{gva:08X} — {len(refs)} code refs ===")
        for va, _ in refs[:40]:
            out.append(f"  0x{va:08X}")
            for line in disasm_range(data, va, sections, 24):
                out.append(line)
        if len(refs) > 40:
            out.append(f"  ... +{len(refs)-40} more")
        out.append("")

    out.append("=== [esi+disp] in CharComponent range 0x4E0000..0x520000 ===")
    for disp in CC_OFFS:
        hits = scan_esi_disp(data, disp, sections)
        out.append(f"  +0x{disp:03X}: {len(hits)} sites")
        for va in hits[:12]:
            out.append(f"    0x{va:08X}")

    out.append("")
    out.append("=== PasteFromSkin callers — 96b context ===")
    for cva in find_callers(data, FUNCS["PasteFromSkin"], sections):
        out.append(f"--- caller 0x{cva:08X} ---")
        for line in disasm_range(data, cva - 64, sections, 160):
            out.append(line)

    out.append("")
    out.append("=== RenderPrepSections full (512b) ===")
    for line in disasm_range(data, FUNCS["RenderPrepSections"], sections, 512):
        out.append(line)

    out.append("")
    out.append("=== RenderPrepSections callers ===")
    for cva in find_callers(data, FUNCS["RenderPrepSections"], sections):
        out.append(f"  0x{cva:08X}")
        for line in disasm_range(data, cva - 32, sections, 64):
            out.append(line)

    report = Path(r"C:\Azerothcore\DBC_Tool\WXL-RE-PASTE-COMPOSITE.txt")
    text = "\n".join(out)
    report.write_text(text, encoding="utf-8")
    print(text[:12000])
    if len(text) > 12000:
        print(f"\n... truncated ({len(text)} chars total) -> {report}")
    else:
        print(f"\nWrote {report}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

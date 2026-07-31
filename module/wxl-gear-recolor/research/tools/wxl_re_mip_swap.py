#!/usr/bin/env python3
"""RE: Disassemble 0x4F2CE0 (mip pool swap) called from FlushGearComposite."""
from __future__ import annotations

import struct
from pathlib import Path

WOW = Path(r"C:\Users\Shadowlands\Desktop\WotLK for AZ\Wow.exe")
OUT = Path(r"C:\Azerothcore\DBC_Tool\WXL-RE-MIP-SWAP-4F2CE0.txt")
IMAGE_BASE = 0x00400000
MIP_SWAP = 0x004F2CE0
FLUSH = 0x004E9000


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


def find_calls_to(data: bytes, target: int, sections) -> list[int]:
    hits = []
    for i in range(len(data) - 5):
        if data[i] != 0xE8:
            continue
        rel = struct.unpack_from("<i", data, i + 1)[0]
        va = offset_to_va(i, sections)
        if va and va + 5 + rel == target:
            hits.append(va)
    return sorted(set(hits))


def disasm(data: bytes, start_va: int, sections, nbytes: int) -> list[str]:
    off = va_to_offset(start_va, sections)
    if off is None:
        return []
    chunk = data[off : off + nbytes]
    lines = []
    i = 0
    while i < len(chunk) and len(lines) < 200:
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
            if modrm == 0x86:
                disp = struct.unpack_from("<I", chunk, i + 2)[0]
                note = ""
                if disp == 0x34:
                    note = "  ; [esi+0x34] mip pool base/count"
                elif disp in (0x1A4, 0x1B0, 0x194):
                    note = f"  ; composite +0x{disp:X}"
                lines.append(f"  0x{addr:08X}: mov r, [esi+0x{disp:X}]{note}")
                i += 6
                continue
            if modrm == 0x46 and i + 2 < len(chunk):
                disp = chunk[i + 2]
                note = "  ; [esi+0x34]" if disp == 0x34 else ""
                lines.append(f"  0x{addr:08X}: mov r, [esi+0x{disp:X}]{note}")
                i += 3
                continue
            if modrm == 0x4E and i + 2 < len(chunk):
                disp = chunk[i + 2]
                lines.append(f"  0x{addr:08X}: mov r, [esi+0x{disp:X}]")
                i += 3
                continue
        if b == 0x89 and i + 5 < len(chunk) and chunk[i + 1] == 0x86:
            disp = struct.unpack_from("<I", chunk, i + 2)[0]
            lines.append(f"  0x{addr:08X}: mov [esi+0x{disp:X}], r")
            i += 6
            continue
        if b == 0x83 and i + 6 < len(chunk) and chunk[i + 1] == 0x7F:
            disp = chunk[i + 2]
            imm = chunk[i + 3]
            lines.append(f"  0x{addr:08X}: cmp dword [edi+0x{disp:X}], 0x{imm:X}")
            i += 7
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
    out = [
        "WXL RE — Mip pool swap @ 0x4F2CE0",
        f"Binary: {WOW}",
        "=" * 72,
        "",
        "## Callers of 0x4F2CE0",
    ]
    for va in find_calls_to(data, MIP_SWAP, sections):
        ctx = "FlushGearComposite" if FLUSH <= va <= FLUSH + 0x200 else ""
        out.append(f"  0x{va:08X}  {ctx}")
    out.append("")
    out.append("## Function body 0x4F2CE0")
    out.extend(disasm(data, MIP_SWAP, sections, 0x200))
    out.append("")
    out.append("## FlushGearComposite call sites to 0x4F2CE0 (context)")
    for va in find_calls_to(data, MIP_SWAP, sections):
        if not (FLUSH <= va <= FLUSH + 0x200):
            continue
        out.append(f"  --- around 0x{va:08X} ---")
        out.extend(disasm(data, va - 0x30, sections, 0x50))
    out.append("")
    out.append("## INTERPRETATION")
    out.append("  FlushGearComposite calls 0x4F2CE0 for +0x1A4 and +0x1B0 face mips.")
    out.append("  Uses [esi+0x34] as pool: cmp [edi+0x34], 0x28 — max 40 pool slots?")
    out.append("  Stores result into [edi+0x34+slot*4] array at offset 0x38.")
    out.append("  Gear-only dirty (bits 0-7) still triggers face mip pool rotation.")
    out.append("  Face repaste on +0x1A4 may not match displayed pool slot after swap.")

    OUT.write_text("\n".join(out), encoding="utf-8")
    print(f"Wrote {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

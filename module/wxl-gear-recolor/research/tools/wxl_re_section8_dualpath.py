#!/usr/bin/env python3
"""RE: Section 8 handler dual path (PasteSkinLayout optional) + call-site census."""
from __future__ import annotations

import struct
from pathlib import Path

WOW = Path(r"C:\Users\Shadowlands\Desktop\WotLK for AZ\Wow.exe")
OUT = Path(r"C:\Azerothcore\DBC_Tool\WXL-RE-SECT8-DUAL-PATH.txt")
IMAGE_BASE = 0x00400000

PASTE_LAYOUT = 0x004F07D0
PASTE_FROM_SKIN = 0x004F08A0
SECT8_HANDLER = 0x004F0AD0
SECT9_HANDLER = 0x004F0A90
STATIC_DISPATCH = 0x00B6B928
RUNTIME_DISPATCH = 0x00B6B88C
INIT_COPY = 0x004F1BA2
FLUSH_COMPOSITE = 0x004E9000


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


def read_dword(data: bytes, va: int, sections) -> int | None:
    off = va_to_offset(va, sections)
    if off is None or off + 4 > len(data):
        return None
    return struct.unpack_from("<I", data, off)[0]


def offset_to_va(off: int, sections) -> int | None:
    for s in sections:
        if s["raw_ptr"] <= off < s["raw_ptr"] + s["raw_size"]:
            return IMAGE_BASE + (off - s["raw_ptr"] + s["vaddr"])
    return None


def find_calls_to(data: bytes, target: int, sections) -> list[int]:
    hits: list[int] = []
    for i in range(len(data) - 5):
        if data[i] != 0xE8:
            continue
        rel = struct.unpack_from("<i", data, i + 1)[0]
        va = offset_to_va(i, sections)
        if va is None:
            continue
        if va + 5 + rel == target:
            hits.append(va)
    return sorted(set(hits))


def disasm_window(data: bytes, start_va: int, sections, size: int) -> list[str]:
    off = va_to_offset(start_va, sections)
    if off is None:
        return ["(invalid va)"]
    chunk = data[off : off + size]
    lines: list[str] = []
    i = 0
    while i < len(chunk):
        addr = start_va + i
        b = chunk[i]
        if b == 0xE8 and i + 4 < len(chunk):
            rel = struct.unpack_from("<i", chunk, i + 1)[0]
            tgt = addr + 5 + rel
            tag = ""
            if tgt == PASTE_LAYOUT:
                tag = "  ; PasteSkinLayout"
            elif tgt == PASTE_FROM_SKIN:
                tag = "  ; PasteFromSkin"
            lines.append(f"  0x{addr:08X}: call 0x{tgt:08X}{tag}")
            i += 5
            continue
        if b == 0x74 and i + 1 < len(chunk):
            rel = struct.unpack_from("<b", chunk, i + 1)[0]
            lines.append(f"  0x{addr:08X}: jz 0x{addr + 2 + rel:08X}")
            i += 2
            continue
        if b == 0x75 and i + 1 < len(chunk):
            rel = struct.unpack_from("<b", chunk, i + 1)[0]
            lines.append(f"  0x{addr:08X}: jnz 0x{addr + 2 + rel:08X}")
            i += 2
            continue
        if b == 0xF7 and i + 3 < len(chunk) and chunk[i + 1] == 0x40:
            disp = chunk[i + 2]
            imm = chunk[i + 3]
            lines.append(f"  0x{addr:08X}: test byte [eax+0x{disp:02X}], 0x{imm:02X}")
            i += 4
            continue
        if b == 0x85 and i + 1 < len(chunk) and chunk[i + 1] == 0xC0:
            lines.append(f"  0x{addr:08X}: test eax, eax")
            i += 2
            continue
        if b == 0xC3:
            lines.append(f"  0x{addr:08X}: ret")
            i += 1
            continue
        lines.append(f"  0x{addr:08X}: {b:02X}")
        i += 1
    return lines


def main() -> int:
    data = WOW.read_bytes()
    sections = parse_pe(data)
    out: list[str] = []
    out.append("WXL RE — Section 8 dual path + paste call census")
    out.append(f"Binary: {WOW}")
    out.append("=" * 72)
    out.append("")

    out.append("## 1. Static section dispatch @ 0x00B6B928 (10 handlers)")
    for sec in range(10):
        fn = read_dword(data, STATIC_DISPATCH + sec * 4, sections)
        out.append(f"  sec {sec}: 0x{fn:08X}" if fn else f"  sec {sec}: (unmapped)")
    out.append("")

    out.append("## 2. Section 8 handler @ 0x4F0AD0 (full annotated)")
    out.append("  TextureCacheHelper → optional PasteSkinLayout → PasteFromSkin mips")
    out.append("")
    for line in disasm_window(data, SECT8_HANDLER, sections, 0xA0):
        out.append(line)
    out.append("")

    out.append("## 3. Section 9 handler @ 0x4F0A90")
    for line in disasm_window(data, SECT9_HANDLER, sections, 0x50):
        out.append(line)
    out.append("")

    out.append("## 4. Call sites → PasteSkinLayout (0x4F07D0)")
    layout_calls = find_calls_to(data, PASTE_LAYOUT, sections)
    out.append(f"  total: {len(layout_calls)}")
    for va in layout_calls[:40]:
        out.append(f"    0x{va:08X}")
    if len(layout_calls) > 40:
        out.append(f"    ... +{len(layout_calls) - 40} more")
    out.append("")

    out.append("## 5. Call sites → PasteFromSkin (0x4F08A0)")
    skin_calls = find_calls_to(data, PASTE_FROM_SKIN, sections)
    out.append(f"  total: {len(skin_calls)}")
    for va in skin_calls[:40]:
        out.append(f"    0x{va:08X}")
    if len(skin_calls) > 40:
        out.append(f"    ... +{len(skin_calls) - 40} more")
    out.append("")

    out.append("## 6. FlushGearComposite @ 0x4E9000 (first 0x80 bytes)")
    for line in disasm_window(data, FLUSH_COMPOSITE, sections, 0x80):
        out.append(line)
    out.append("")

    out.append("## 7. KEY FINDING — why skin_layout=0 in logs")
    out.append("")
    out.append("  Section 8 handler @ 0x4F0AD0:")
    out.append("    1) call TextureCacheHelper @ 0x4F3BA0")
    out.append("    2) if !eax → ret (no face paste at all)")
    out.append("    3) test byte [eax+0x1C], 0x08 → jz SKIP_LAYOUT")
    out.append("    4) SKIP_LAYOUT target ≈ 0x4F0B15: PasteFromSkin only")
    out.append("       (no call 0x4F07D0 PasteSkinLayout)")
    out.append("    5) Layout path: PasteSkinLayout(8)→scratch, then PasteFromSkin→+0x1A4/+0x1B0")
    out.append("")
    out.append("  WXL hkPasteSkinLayout logs ONLY when 0x4F07D0 is called.")
    out.append("  face_skin_ok (hook 0x4F08A0) fires on BOTH paths.")
    out.append("  Log skin_layout=0 does NOT mean handler 8 skipped — direct path.")
    out.append("")
    out.append("## 8. Gear vs face ordering (root cause recap)")
    out.append("  Gear tint: dirty bits 0-7 only → handlers 0-7 → scratch B6B870")
    out.append("  FlushGearComposite: RefCountCompositeMip on +0x1A4/+0x1B0 even when")
    out.append("  sections 8-9 not dirty → face mips invalidated without repaste.")
    out.append("  Fix: face-only RenderPrep (dirty 0x300) AFTER EVERY gear pass,")
    out.append("  not one-shot per session.")

    OUT.write_text("\n".join(out), encoding="utf-8")
    print(f"Wrote {OUT} ({len(out)} lines)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

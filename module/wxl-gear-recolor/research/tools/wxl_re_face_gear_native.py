#!/usr/bin/env python3
"""RE: PasteFromSkin to sections 8-9 — face skin vs item gear composite paths."""
from __future__ import annotations

import struct
from pathlib import Path

WOW = Path(r"C:\Users\Shadowlands\Desktop\WotLK for AZ\Wow.exe")
OUT = Path(r"C:\Azerothcore\DBC_Tool\WXL-RE-FACE-GEAR-NATIVE.txt")
IMAGE_BASE = 0x00400000
PASTE_FROM_SKIN = 0x004F08A0
PASTE_LAYOUT = 0x004F07D0
SECT8 = 0x004F0AD0
SECT9 = 0x004F0B90
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


def disasm_calls(data: bytes, start_va: int, sections, size: int) -> list[str]:
    off = va_to_offset(start_va, sections)
    if off is None:
        return []
    chunk = data[off : off + size]
    lines = []
    i = 0
    pending_push = []
    while i < len(chunk):
        addr = start_va + i
        b = chunk[i]
        if b == 0x6A and i + 1 < len(chunk):
            pending_push.append(chunk[i + 1])
            lines.append(f"  0x{addr:08X}: push {chunk[i+1]}")
            i += 2
            continue
        if b == 0x68 and i + 4 < len(chunk):
            imm = struct.unpack_from("<I", chunk, i + 1)[0]
            pending_push.append(imm)
            lines.append(f"  0x{addr:08X}: push {imm}")
            i += 5
            continue
        if b == 0xE8 and i + 4 < len(chunk):
            rel = struct.unpack_from("<i", chunk, i + 1)[0]
            tgt = addr + 5 + rel
            sec = pending_push[-1] if pending_push else "?"
            tag = ""
            if tgt == PASTE_FROM_SKIN:
                tag = f"  ; PasteFromSkin(section={sec})"
            elif tgt == PASTE_LAYOUT:
                tag = f"  ; PasteSkinLayout(section={sec})"
            lines.append(f"  0x{addr:08X}: call 0x{tgt:08X}{tag}")
            if tgt in (PASTE_FROM_SKIN, PASTE_LAYOUT):
                pending_push = []
            i += 5
            continue
        if b == 0x8B and i + 5 < len(chunk) and chunk[i + 1] == 0x86:
            disp = struct.unpack_from("<I", chunk, i + 2)[0]
            note = {0x1A4: "faceUpperMip", 0x1B0: "faceLowerMip", 0x1C0: "faceExtraMip",
                    0x194: "gearBaseMip", 0x1AC: "hairLower"}.get(disp, "")
            suf = f"  ; {note}" if note else ""
            lines.append(f"  0x{addr:08X}: mov eax, [esi+0x{disp:X}]{suf}")
            i += 6
            continue
        if b == 0xA1 and i + 4 < len(chunk):
            disp = struct.unpack_from("<I", chunk, i + 1)[0]
            note = "  ; scratch B6B870" if disp == 0x00B6B870 else ""
            lines.append(f"  0x{addr:08X}: mov eax, [0x{disp:08X}]{note}")
            i += 5
            continue
        if b == 0xC3:
            lines.append(f"  0x{addr:08X}: ret")
            break
        i += 1
    return lines


def find_paste8_calls(data: bytes, sections) -> list[tuple[int, int]]:
    """Find call PasteFromSkin preceded by push 8 or 9 within 20 bytes."""
    hits = []
    for i in range(len(data) - 5):
        if data[i] != 0xE8:
            continue
        rel = struct.unpack_from("<i", data, i + 1)[0]
        va = offset_to_va(i, sections)
        if not va or va + 5 + rel != PASTE_FROM_SKIN:
            continue
        # scan back for push 8/9
        off = i
        for back in range(1, 25):
            if off - back < 0:
                break
            p = off - back
            if data[p] == 0x6A and data[p + 1] in (8, 9):
                hits.append((va, data[p + 1]))
                break
            if data[p] == 0x68:
                imm = struct.unpack_from("<I", data, p + 1)[0]
                if imm in (8, 9):
                    hits.append((va, imm))
                    break
    return sorted(set(hits))


def main() -> int:
    data = WOW.read_bytes()
    sections = parse_pe(data)
    out = [
        "WXL RE — face_gear_native: item PasteFromSkin on sections 8-9",
        f"Binary: {WOW}",
        "=" * 72,
        "",
        "## LOG EVIDENCE (debug-615e3b.log L248-252, ts=2693158156)",
        "  Same frame, /recolor just opened:",
        "    remote_no_hsl sec8 belt_lu → gear tex ptr 598411024",
        "    face_gear_native sec8 belt_lu → FACE composite ptr 551297208",
        "    face_gear_native sec9 Pant_LL → same face ptr 551297208",
        "    face_skin_ok owner 166 → ptr 550903856 (different mip!)",
        "",
        "  Native client PASTES ITEM GEAR into face/hair composite mips.",
        "  WXL face_skin_ok repastes skin to +0x1A4 ptr, but gear paste uses",
        "  different dst (composite layer) — then FlushGearComposite swaps pool.",
        "",
        "## Section 8 handler @ 0x4F0AD0 (annotated)",
    ]
    out.extend(disasm_calls(data, SECT8, sections, 0xA0))
    out.append("")
    out.append("## Section 9 handler @ 0x4F0B90")
    out.extend(disasm_calls(data, SECT9, sections, 0x90))
    out.append("")
    out.append("## All PasteFromSkin call sites with section 8 or 9")
    calls = find_paste8_calls(data, sections)
    out.append(f"  total: {len(calls)}")
    for va, sec in calls:
        region = ""
        if SECT8 <= va <= SECT8 + 0xA0:
            region = " [inside sec8 handler]"
        elif SECT9 <= va <= SECT9 + 0x90:
            region = " [inside sec9 handler]"
        elif FLUSH <= va <= FLUSH + 0x300:
            region = " [FlushGearComposite region]"
        out.append(f"    0x{va:08X} section={sec}{region}")
    out.append("")
    out.append("## WHY /recolor breaks but login OK")
    out.append("")
    out.append("  LOGIN:")
    out.append("    - One native 0x3FF pass per player, serialized by game")
    out.append("    - face_gear_native: 0 in pre-recolor log")
    out.append("    - face_skin_ok only (skin/hair textures)")
    out.append("")
    out.append("  /recolor:")
    out.append("    - PUSH storm → remote RenderPrep pasteStrictOk=1023 (0x3FF)")
    out.append("    - Full section dispatch including 8-9 WITH item components")
    out.append("    - face_gear_native x17: belt/pant/glove/chest onto sec 8-9")
    out.append("    - Interleaved with WXL stem_tint on gear + self_face_done")
    out.append("    - Order: gear-native-face → face_skin_ok → another 0x3FF → repeat")
    out.append("")
    out.append("  self_face_done CANNOT fix this alone:")
    out.append("    - Repastes skin to dstMips ptr (e.g. 550903856)")
    out.append("    - Native gear-on-face uses different composite ptr (551297208)")
    out.append("    - FlushGearComposite @ 0x4E9000 rotates [esi+0x34] pool after gear-only")
    out.append("")
    out.append("## FIX DIRECTION (RE-first, not implemented)")
    out.append("  1. Never trigger remote 0x3FF while tint active — only dirty gear bits")
    out.append("  2. After gear tint pass: face rebuild must include INVALIDATING gear-on-face")
    out.append("     layers (+0x1C0?) not just skin PasteFromSkin")
    out.append("  3. Serialize: one owner RenderPrep; clear B6B870 before each")
    out.append("  4. Runtime: log [comp+0x1A4] vs [comp+0x1C0] vs pool [+0x34] pre/post flush")

    OUT.write_text("\n".join(out), encoding="utf-8")
    print(f"Wrote {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

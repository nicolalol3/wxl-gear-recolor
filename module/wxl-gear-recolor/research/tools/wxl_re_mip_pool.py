#!/usr/bin/env python3
"""RE: RefCountCompositeMip, CharComponent pool, face mip field lifecycle."""
from __future__ import annotations

import struct
from pathlib import Path

WOW = Path(r"C:\Users\Shadowlands\Desktop\WotLK for AZ\Wow.exe")
OUT = Path(r"C:\Azerothcore\DBC_Tool\WXL-RE-MIP-POOL.txt")
IMAGE_BASE = 0x00400000

FLUSH_COMPOSITE = 0x004E9000
REFCOUNT_MIP = 0x004E9BA0  # called from RenderPrepSections gear handlers
CHAR_POOL = 0x00B6B240
GLOBAL_SCRATCH = 0x00B6B870
FACE_MIP_OFFS = [0x194, 0x1A4, 0x1B0, 0x1BC, 0x1C0, 0x1AC]


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


def find_calls_to(data: bytes, target: int, sections, lo=0, hi=0xFFFFFFFF) -> list[int]:
    hits = []
    for i in range(len(data) - 5):
        if data[i] != 0xE8:
            continue
        rel = struct.unpack_from("<i", data, i + 1)[0]
        va = offset_to_va(i, sections)
        if va and lo <= va <= hi and va + 5 + rel == target:
            hits.append(va)
    return sorted(set(hits))


def disasm(data: bytes, start_va: int, sections, nbytes: int, max_lines=120) -> list[str]:
    off = va_to_offset(start_va, sections)
    if off is None:
        return ["(bad va)"]
    chunk = data[off : off + nbytes]
    lines = []
    i = 0
    while i < len(chunk) and len(lines) < max_lines:
        addr = start_va + i
        b = chunk[i]
        if b == 0xE8 and i + 4 < len(chunk):
            rel = struct.unpack_from("<i", chunk, i + 1)[0]
            tgt = addr + 5 + rel
            tag = ""
            if tgt == REFCOUNT_MIP:
                tag = "  ; RefCountCompositeMip"
            lines.append(f"  0x{addr:08X}: call 0x{tgt:08X}{tag}")
            i += 5
            continue
        if b == 0xFF and i + 5 < len(chunk) and chunk[i + 1] == 0x15:
            disp = struct.unpack_from("<I", chunk, i + 2)[0]
            lines.append(f"  0x{addr:08X}: call dword ptr [0x{disp:08X}]")
            i += 6
            continue
        if b == 0x8B and i + 5 < len(chunk) and chunk[i + 1] == 0x86:
            disp = struct.unpack_from("<I", chunk, i + 2)[0]
            note = ""
            if disp in FACE_MIP_OFFS:
                note = f"  ; face/gear mip +0x{disp:X}"
            lines.append(f"  0x{addr:08X}: mov eax, [esi+0x{disp:X}]{note}")
            i += 6
            continue
        if b == 0x89 and i + 5 < len(chunk) and chunk[i + 1] == 0x86:
            disp = struct.unpack_from("<I", chunk, i + 2)[0]
            note = ""
            if disp in FACE_MIP_OFFS:
                note = f"  ; store mip +0x{disp:X}"
            lines.append(f"  0x{addr:08X}: mov [esi+0x{disp:X}], eax{note}")
            i += 6
            continue
        if b == 0x56 and i + 1 < len(chunk) and chunk[i + 1] == 0x8B:
            lines.append(f"  0x{addr:08X}: push esi / mov esi, ecx")
            i += 3
            continue
        if b == 0x8B and i + 1 < len(chunk) and chunk[i + 1] == 0xF1:
            lines.append(f"  0x{addr:08X}: mov esi, ecx  ; this=component")
            i += 2
            continue
        if b == 0xA1 and i + 4 < len(chunk):
            disp = struct.unpack_from("<I", chunk, i + 1)[0]
            note = "  ; scratch" if disp == GLOBAL_SCRATCH else ""
            lines.append(f"  0x{addr:08X}: mov eax, [0x{disp:08X}]{note}")
            i += 5
            continue
        if b == 0xC3:
            lines.append(f"  0x{addr:08X}: ret")
            i += 1
            continue
        if b == 0xC2 and i + 2 < len(chunk):
            n = struct.unpack_from("<H", chunk, i + 1)[0]
            lines.append(f"  0x{addr:08X}: retn {n}")
            break
        lines.append(f"  0x{addr:08X}: {b:02X}")
        i += 1
    return lines


def scan_store_to_disp(data: bytes, disp: int, sections) -> list[int]:
    pat = bytes([0x89, 0x86]) + struct.pack("<I", disp)
    hits = []
    start = 0
    while True:
        i = data.find(pat, start)
        if i < 0:
            break
        va = offset_to_va(i, sections)
        if va:
            hits.append(va)
        start = i + 1
    return sorted(set(hits))


def main() -> int:
    data = WOW.read_bytes()
    sections = parse_pe(data)
    out: list[str] = []
    out.append("WXL RE — Composite mip pool + FlushGearComposite")
    out.append(f"Binary: {WOW}")
    out.append("=" * 72)

    out.append("\n## 1. FlushGearComposite @ 0x4E9000")
    out.append("  Called after RenderPrepSections; touches gear AND face mip fields.")
    for line in disasm(data, FLUSH_COMPOSITE, sections, 0x120):
        out.append(line)

    out.append("\n## 2. RefCountCompositeMip @ 0x4E9BA0 (first 0x100 bytes)")
    for line in disasm(data, REFCOUNT_MIP, sections, 0x100):
        out.append(line)

    out.append("\n## 3. Callers of RefCountCompositeMip")
    for va in find_calls_to(data, REFCOUNT_MIP, sections, 0x004E0000, 0x00520000):
        out.append(f"  0x{va:08X}")

    out.append("\n## 4. Sites that WRITE [esi+0x1A4] (face upper mip ptr)")
    for va in scan_store_to_disp(data, 0x1A4, sections):
        if 0x004E0000 <= va <= 0x00520000:
            out.append(f"  0x{va:08X}")

    out.append("\n## 5. Sites that WRITE [esi+0x1B0] (face lower mip ptr)")
    for va in scan_store_to_disp(data, 0x1B0, sections):
        if 0x004E0000 <= va <= 0x00520000:
            out.append(f"  0x{va:08X}")

    out.append("\n## 6. CharComponent pool @ 0xB6B240 — code refs (first 15)")
    pat = struct.pack("<I", CHAR_POOL)
    refs = []
    start = 0
    while True:
        i = data.find(pat, start)
        if i < 0:
            break
        va = offset_to_va(i - 1, sections)  # A1 / 8B approx
        if va and 0x004D0000 <= va <= 0x00550000:
            refs.append(va)
        start = i + 1
    for va in sorted(set(refs))[:15]:
        out.append(f"  0x{va:08X}")

    out.append("\n## 7. RECOLOR REGRESSION — client model (from log + RE)")
    out.append("")
    out.append("  LOGIN OK:")
    out.append("    - Each player: 1 native RenderPrep, dirty often 0x3FF")
    out.append("    - Face mips assigned per CharComponent; few cross-race ptr reuse")
    out.append("")
    out.append("  AFTER /recolor:")
    out.append("    - PUSH storm → ApplyOwnerTint → remote RenderPrep dirty=0x3FF")
    out.append("    - remote_miss×1032: allowed=0 BUT CallOrig still runs (no tint ctx)")
    out.append("    - scratch B6B870: SINGLETON — last gear tint visible to next unit")
    out.append("    - face ptr collisions 3→55: same dstMips across Orc/BElf/BE races")
    out.append("    - Handler sec8 layout path: PasteFromSkin(scratch→+0x1A4) uses B6B870")
    out.append("")
    out.append("  SELF black face after recolor:")
    out.append("    - self_face_done×125 runs but gear-only passes still use dirty 0-7")
    out.append("    - FlushGearComposite @ end of gear prep invalidates +0x1A4/+0x1B0")
    out.append("    - Separate face prep may target POOL component not live render slot")
    out.append("")
    out.append("  WXL must NOT patch blindly; fix requires:")
    out.append("    A) Serialize: one assemble owner at a time + clear B6B870 before each")
    out.append("    B) Never CallOrig 0x3FF remote without owner paste context")
    out.append("    C) Face rebuild on LIVE component from model render, not stale pool CC")

    OUT.write_text("\n".join(out), encoding="utf-8")
    print(f"Wrote {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

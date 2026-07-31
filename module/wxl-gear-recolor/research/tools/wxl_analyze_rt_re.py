#!/usr/bin/env python3
"""Analyze rt-re runtime probes in debug-615e3b.log."""
import json
import sys
from collections import defaultdict
from pathlib import Path

LOG = Path(r"C:\Azerothcore\debug-615e3b.log")

def main() -> int:
    if not LOG.exists():
        print("missing", LOG)
        return 1
    by_msg = defaultdict(list)
    for line in LOG.read_text(encoding="utf-8", errors="ignore").splitlines():
        try:
            o = json.loads(line)
        except Exception:
            continue
        if o.get("runId") != "rt-re":
            continue
        by_msg[o.get("message", "")].append(o.get("data", {}))
    print("=== rt-re runtime probes ===")
    for msg, rows in sorted(by_msg.items()):
        print(f"\n{msg}: {len(rows)}")
        if msg == "face_scratch":
            # same scratchHash used on different owners = bleed proof
            h2owners = defaultdict(set)
            for r in rows:
                h2owners[r.get("scratchHash", 0)].add(r.get("ownerLow"))
            multi = [(h, o) for h, o in h2owners.items() if len(o) > 1 and h]
            print(f"  scratchHash shared across owners: {len(multi)} cases")
            for h, owners in sorted(multi, key=lambda x: -len(x[1]))[:5]:
                print(f"    hash={h} owners={owners}")
        if msg in ("prep_remote", "prep_self"):
            dirty = defaultdict(int)
            for r in rows:
                dirty[r.get("dirty", -1)] += 1
            print(f"  dirty dist: {dict(sorted(dirty.items()))}")
        if msg == "flush":
            idx = defaultdict(int)
            for r in rows:
                idx[r.get("mipIdx", -1)] += 1
            print(f"  mipIdx dist (top): {sorted(idx.items(), key=lambda x: -x[1])[:8]}")
    return 0

if __name__ == "__main__":
    sys.exit(main())

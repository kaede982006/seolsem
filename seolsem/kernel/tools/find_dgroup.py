#!/usr/bin/env python
import sys
from pathlib import Path


MARKER = b"DGROUP_MARKER_v2\x00"


def main() -> int:
    if len(sys.argv) != 2:
        print("Usage: find_dgroup.py <kernel.img>", file=sys.stderr)
        return 2

    path = Path(sys.argv[1])
    data = path.read_bytes()

    first = data.find(MARKER)
    if first < 0:
        print("DGROUP marker not found in kernel image", file=sys.stderr)
        return 1
    if data.find(MARKER, first + 1) >= 0:
        print("DGROUP marker is not unique in kernel image", file=sys.stderr)
        return 1
    off = first + len(MARKER)
    if off + 2 > len(data):
        print("DGROUP marker offset word out of range", file=sys.stderr)
        return 1
    marker_off = data[off] | (data[off + 1] << 8)

    # Compute DGROUP base offset relative to file start:
    # DGROUP_base + marker_off == marker_addr (file offset)
    dgroup_base = first - marker_off
    if dgroup_base % 16 != 0:
        print(
            f"DGROUP base not paragraph-aligned: 0x{dgroup_base:X}",
            file=sys.stderr,
        )
        return 1

    delta_para = dgroup_base // 16
    # Emit signed decimal for NASM; allow negative values.
    print(str(delta_para))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

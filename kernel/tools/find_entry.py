#!/usr/bin/env python
import sys
from pathlib import Path


MARKER = b"KENTRY_MARKER_v1\x00"


def main() -> int:
    if len(sys.argv) != 2:
        print("Usage: find_entry.py <kernel.img>", file=sys.stderr)
        return 2

    path = Path(sys.argv[1])
    data = path.read_bytes()

    off = data.find(MARKER)
    if off < 0:
        print("Kernel entry marker not found", file=sys.stderr)
        return 1
    if data.find(MARKER, off + 1) >= 0:
        print("Kernel entry marker is not unique", file=sys.stderr)
        return 1
    if off < 2:
        print("Kernel entry marker too close to start", file=sys.stderr)
        return 1

    # Expect: EB <rel8> immediately before marker (jmp short .real_start)
    if data[off - 2] != 0xEB:
        print("Unexpected pre-marker opcode (expected short JMP)", file=sys.stderr)
        return 1

    entry_off = off - 2
    print(f"0x{entry_off:X}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

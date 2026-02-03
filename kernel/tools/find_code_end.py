#!/usr/bin/env python
import sys
from pathlib import Path


MARKER = b"CODE_END_v1\x00"


def main() -> int:
    if len(sys.argv) != 2:
        print("Usage: find_code_end.py <kernel.img>", file=sys.stderr)
        return 2

    data = Path(sys.argv[1]).read_bytes()
    off = data.find(MARKER)
    if off < 0:
        print("CODE end marker not found", file=sys.stderr)
        return 1
    if data.find(MARKER, off + 1) >= 0:
        print("CODE end marker is not unique", file=sys.stderr)
        return 1
    print(f"0x{off:X}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

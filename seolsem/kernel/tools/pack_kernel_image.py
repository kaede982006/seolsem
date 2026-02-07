#!/usr/bin/env python3
from __future__ import annotations

import argparse
import re
from pathlib import Path


CODE_MARKER = b"CODE_MARKER_v1\x00"


def parse_symbol_addr(map_text: str, symbol: str) -> int:
    # Example line:
    #   00001258       __bss_start
    pattern = re.compile(rf"^([0-9A-Fa-f]{{8}})\s+{re.escape(symbol)}\b", re.MULTILINE)
    match = pattern.search(map_text)
    if not match:
        raise ValueError(f"Symbol not found in map: {symbol}")
    return int(match.group(1), 16)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Pack OpenWatcom RAW kernel image into a flat memory image.\n"
            "\n"
            "Some OpenWatcom/wlink RAW outputs omit BSS while others already include it.\n"
            "This tool normalizes the image so on-disk layout matches link-time addresses\n"
            "(DGROUP + BSS + CODE stays within the first 64KiB window)."
        )
    )
    parser.add_argument("--img", required=True, help="Path to kernel.img")
    parser.add_argument("--map", required=True, help="Path to kernel.map produced by wlink")

    args = parser.parse_args()

    img_path = Path(args.img)
    map_path = Path(args.map)

    map_text = map_path.read_text(encoding="utf-8", errors="replace")
    bss_start = parse_symbol_addr(map_text, "__bss_start")
    bss_end = parse_symbol_addr(map_text, "__bss_end")

    if bss_end < bss_start:
        raise SystemExit(f"Invalid BSS range: start=0x{bss_start:X}, end=0x{bss_end:X}")

    data = img_path.read_bytes()

    code_old = data.find(CODE_MARKER)
    if code_old < 0:
        raise SystemExit("CODE marker not found in RAW kernel image")
    if data.find(CODE_MARKER, code_old + 1) >= 0:
        raise SystemExit("CODE marker is not unique in RAW kernel image")

    # Toolchain behavior differs by OpenWatcom release:
    # - Older builds: RAW omits BSS, so CODE starts near __bss_start and needs zero insertion.
    # - Newer builds: RAW already reflects BSS layout, so CODE starts at/after __bss_end.
    #
    # Handle both safely:
    #   code_old < __bss_end  -> insert zeros up to __bss_end
    #   code_old >= __bss_end -> image is already packed (no-op)
    if code_old >= bss_end:
        return 0

    pad_len = bss_end - code_old
    packed = data[:code_old] + (b"\x00" * pad_len) + data[code_old:]

    tmp_path = img_path.with_suffix(img_path.suffix + ".tmp")
    tmp_path.write_bytes(packed)
    tmp_path.replace(img_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

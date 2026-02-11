from __future__ import annotations

import struct

from .codegen_x86_16 import CodegenResult

XEFN_MAGIC = b"XEFN"
XEFN_VERSION = 1
XEFN_HEADER_SIZE = 28


def _build_string_table(imports: list[str]) -> tuple[bytes, bytes]:
    table = bytearray()
    offsets = bytearray()
    off_by_name: dict[str, int] = {}

    for name in imports:
        if name in off_by_name:
            off = off_by_name[name]
        else:
            off = len(table)
            table.extend(name.encode("ascii", errors="replace"))
            table.append(0)
            off_by_name[name] = off
        offsets.extend(struct.pack("<H", off))

    return bytes(offsets), bytes(table)


def _build_reloc_table(result: CodegenResult) -> bytes:
    out = bytearray()
    for r in result.relocs:
        out.extend(struct.pack("<HBB", r.patch_off, r.reloc_type, r.arg))
    return bytes(out)


def pack_native(result: CodegenResult) -> bytes:
    import_blob, str_blob = _build_string_table(result.imports)
    reloc_blob = _build_reloc_table(result)

    import_off = 0
    import_count = 0
    reloc_off = 0
    reloc_count = 0
    str_off = 0
    str_size = 0

    out = bytearray()
    out.extend(b"\x00" * XEFN_HEADER_SIZE)
    out.extend(result.code)
    out.extend(result.data)

    if import_blob:
        import_off = len(out)
        import_count = len(result.imports)
        out.extend(import_blob)

    if reloc_blob:
        reloc_off = len(out)
        reloc_count = len(result.relocs)
        out.extend(reloc_blob)

    if str_blob:
        str_off = len(out)
        str_size = len(str_blob)
        out.extend(str_blob)

    header = struct.pack(
        "<4s12H",
        XEFN_MAGIC,
        XEFN_VERSION,
        0,
        result.entry,
        len(result.code),
        len(result.data),
        result.bss_size,
        import_off,
        import_count,
        reloc_off,
        reloc_count,
        str_off,
        str_size,
    )
    out[0:XEFN_HEADER_SIZE] = header
    return bytes(out)

#!/usr/bin/env python3
import argparse
import struct

SECTOR_SIZE = 512
FAT12_EOC = 0x0FFF
FAT12_MAX_CLUSTERS = 4084


def le16(value: int) -> bytes:
    return struct.pack("<H", value & 0xFFFF)


def le32(value: int) -> bytes:
    return struct.pack("<I", value & 0xFFFFFFFF)


def write_le16(buf: bytearray, offset: int, value: int) -> None:
    buf[offset : offset + 2] = le16(value)


def write_le32(buf: bytearray, offset: int, value: int) -> None:
    buf[offset : offset + 4] = le32(value)


def read_file(path: str) -> bytes:
    with open(path, "rb") as f:
        return f.read()


def set_fat12_entry(fat: bytearray, cluster: int, value: int) -> None:
    """Write one FAT12 12-bit entry with proper nibble packing."""
    offset = cluster + (cluster // 2)
    if offset + 1 >= len(fat):
        raise ValueError("FAT offset out of range")
    if cluster & 1:
        fat[offset] = (fat[offset] & 0x0F) | ((value << 4) & 0xF0)
        fat[offset + 1] = (value >> 4) & 0xFF
    else:
        fat[offset] = value & 0xFF
        fat[offset + 1] = (fat[offset + 1] & 0xF0) | ((value >> 8) & 0x0F)


def calc_fat_layout(total_sectors: int, reserved: int, fats: int, root_ents: int, sec_per_clus: int):
    fat_secs = 1
    while True:
        root_dir_sectors = (root_ents * 32 + SECTOR_SIZE - 1) // SECTOR_SIZE
        data_sectors = total_sectors - reserved - fats * fat_secs - root_dir_sectors
        if data_sectors <= 0:
            raise ValueError("Disk too small for layout")
        clusters = data_sectors // sec_per_clus
        fat_bytes = ((clusters + 2) * 3 + 1) // 2
        fat_secs_needed = (fat_bytes + SECTOR_SIZE - 1) // SECTOR_SIZE
        if fat_secs_needed == fat_secs:
            return fat_secs, root_dir_sectors, data_sectors, clusters
        fat_secs = fat_secs_needed


def patch_boot_sector(boot: bytearray, bpb: dict, label: str, vol_id: int) -> None:
    if len(boot) != SECTOR_SIZE:
        raise ValueError("boot sector must be 512 bytes")

    boot[3:11] = bpb["oem"].ljust(8).encode("ascii")
    write_le16(boot, 0x0B, bpb["bytes_per_sec"])
    boot[0x0D] = bpb["sec_per_clus"]
    write_le16(boot, 0x0E, bpb["res_sectors"])
    boot[0x10] = bpb["fats"]
    write_le16(boot, 0x11, bpb["root_ents"])
    write_le16(boot, 0x13, bpb["total_sectors16"])
    boot[0x15] = bpb["media"]
    write_le16(boot, 0x16, bpb["fat_secs"])
    write_le16(boot, 0x18, bpb["sec_per_track"])
    write_le16(boot, 0x1A, bpb["heads"])
    write_le32(boot, 0x1C, bpb["hidden_sectors"])
    write_le32(boot, 0x20, bpb["total_sectors32"])

    boot[0x24] = bpb["drive_num"]
    boot[0x25] = 0
    boot[0x26] = 0x29
    write_le32(boot, 0x27, vol_id)
    boot[0x2B:0x36] = label.ljust(11).encode("ascii")
    boot[0x36:0x3E] = b"FAT12   "

    boot[510] = 0x55
    boot[511] = 0xAA


def main() -> int:
    parser = argparse.ArgumentParser(description="Build FAT12 installer floppy image (boot + stage2 in reserved)")
    parser.add_argument("--boot", required=True)
    parser.add_argument("--stage2", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--label", default="SEOLSEMINS")
    parser.add_argument("--oem", default="SEOLSEM")
    parser.add_argument("--total-sectors", type=int, default=2880)
    parser.add_argument("--sec-per-clus", type=int, default=1)
    parser.add_argument("--root-ents", type=int, default=224)
    parser.add_argument("--fats", type=int, default=2)
    parser.add_argument("--sec-per-track", type=int, default=18)
    parser.add_argument("--heads", type=int, default=2)
    parser.add_argument("--media", type=lambda x: int(x, 0), default=0xF0)
    parser.add_argument("--drive-num", type=lambda x: int(x, 0), default=0x00)
    args = parser.parse_args()

    boot = bytearray(read_file(args.boot))
    stage2 = read_file(args.stage2)

    stage2_secs = (len(stage2) + SECTOR_SIZE - 1) // SECTOR_SIZE
    reserved = 1 + stage2_secs

    fat_secs, root_dir_sectors, data_sectors, clusters = calc_fat_layout(
        args.total_sectors, reserved, args.fats, args.root_ents, args.sec_per_clus
    )

    if clusters > FAT12_MAX_CLUSTERS:
        raise ValueError(f"Cluster count too large for FAT12: {clusters}")

    total_sectors16 = args.total_sectors if args.total_sectors < 0x10000 else 0
    total_sectors32 = 0 if total_sectors16 else args.total_sectors

    bpb = {
        "oem": args.oem,
        "bytes_per_sec": SECTOR_SIZE,
        "sec_per_clus": args.sec_per_clus,
        "res_sectors": reserved,
        "fats": args.fats,
        "root_ents": args.root_ents,
        "total_sectors16": total_sectors16,
        "media": args.media,
        "fat_secs": fat_secs,
        "sec_per_track": args.sec_per_track,
        "heads": args.heads,
        "hidden_sectors": 0,
        "total_sectors32": total_sectors32,
        "drive_num": args.drive_num,
    }

    patch_boot_sector(boot, bpb, args.label, 0x12345678)

    image = bytearray(args.total_sectors * SECTOR_SIZE)
    image[0:SECTOR_SIZE] = boot

    # Stage2 is stored in reserved sectors directly after the boot sector.
    stage2_padded = stage2.ljust(stage2_secs * SECTOR_SIZE, b"\x00")
    image[SECTOR_SIZE : SECTOR_SIZE * (1 + stage2_secs)] = stage2_padded

    # Build an empty FAT12 filesystem (not used by installer, but keeps it standard).
    fat_offset = reserved * SECTOR_SIZE
    root_offset = fat_offset + args.fats * fat_secs * SECTOR_SIZE

    fat = bytearray(fat_secs * SECTOR_SIZE)
    fat[0] = args.media
    fat[1] = 0xFF
    fat[2] = 0xFF

    # FAT12 reserved entries:
    #   - first 3 bytes encode media + EOC markers (standard: media, 0xFF, 0xFF)
    #   - keep the rest free (0x000)

    for i in range(args.fats):
        start = fat_offset + i * fat_secs * SECTOR_SIZE
        image[start : start + len(fat)] = fat

    # Root directory left as zeros (empty).
    # Data region left as zeros.
    _ = root_offset
    _ = root_dir_sectors
    _ = data_sectors

    with open(args.output, "wb") as f:
        f.write(image)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

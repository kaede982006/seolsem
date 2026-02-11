#!/usr/bin/env python3
import argparse
import json
import os
import struct

SECTOR_SIZE = 512
FAT12_EOC = 0x0FFF
FAT12_MAX_CLUSTERS = 4084
SEOLSEM_VOL_ID = 0x534D4C53
SEOLSEM_BOOT_MAGIC = b"SEOLSIG!"
SEOLSEM_BOOT_MAGIC_OFS = 0x1F0
SEOLSEM_DISK_ROLE_OFS = 0x1F8
SEOLSEM_DISK_ROLE_KERNEL = 0x01
SEOLSEM_DISK_ROLE_PROGRAMS = 0x02
SEOLSEM_DISK_ROLE_BOTH = 0x03


def le16(value: int) -> bytes:
    return struct.pack("<H", value & 0xFFFF)


def le32(value: int) -> bytes:
    return struct.pack("<I", value & 0xFFFFFFFF)


def write_le16(buf: bytearray, offset: int, value: int) -> None:
    buf[offset:offset + 2] = le16(value)


def write_le32(buf: bytearray, offset: int, value: int) -> None:
    buf[offset:offset + 4] = le32(value)


def read_file(path: str) -> bytes:
    with open(path, "rb") as f:
        return f.read()


def to_name83(filename: str) -> bytes:
    base, sep, ext = filename.partition(".")
    base = base.upper()
    ext = ext.upper() if sep else ""
    if len(base) > 8 or len(ext) > 3 or len(base) == 0:
        raise ValueError(f"Invalid 8.3 name: {filename}")
    return (base.ljust(8) + ext.ljust(3)).encode("ascii")


def name83_raw(base: str, ext: str = "") -> bytes:
    base = base.upper()
    ext = ext.upper()
    if len(base) > 8 or len(ext) > 3 or len(base) == 0:
        raise ValueError(f"Invalid 8.3 name: {base}.{ext}")
    return (base.ljust(8) + ext.ljust(3)).encode("ascii")


def dir_entry(name83: bytes, attr: int, start_cluster: int, size: int) -> bytearray:
    entry = bytearray(32)
    entry[0:11] = name83
    entry[11] = attr & 0xFF
    write_le16(entry, 26, start_cluster)
    write_le32(entry, 28, size)
    return entry


def set_fat12_entry(fat: bytearray, cluster: int, value: int) -> None:
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


def patch_boot_sector(boot: bytearray, bpb: dict, label: str, vol_id: int, disk_role: int) -> None:
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
    boot[SEOLSEM_BOOT_MAGIC_OFS:SEOLSEM_BOOT_MAGIC_OFS + len(SEOLSEM_BOOT_MAGIC)] = SEOLSEM_BOOT_MAGIC
    boot[SEOLSEM_DISK_ROLE_OFS] = disk_role & 0xFF
    boot[510] = 0x55
    boot[511] = 0xAA


def load_manifest(path: str):
    base_dir = os.path.dirname(path)
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    if isinstance(data, dict):
        entries = data.get("entries", [])
    elif isinstance(data, list):
        entries = data
    else:
        raise ValueError("manifest root must be object or array")

    out = []
    for idx, ent in enumerate(entries):
        if not isinstance(ent, dict):
            raise ValueError(f"manifest entry #{idx} must be object")
        img_path = ent.get("path")
        src = ent.get("src")
        if not img_path or not src:
            raise ValueError(f"manifest entry #{idx} requires path/src")
        img_path = img_path.replace("\\\\", "/").strip("/")
        if img_path == "":
            raise ValueError(f"manifest entry #{idx} invalid path")
        parts = [p for p in img_path.split("/") if p]
        if len(parts) > 2:
            raise ValueError(f"manifest path supports up to one subdir: {img_path}")
        src_path = src
        if not os.path.isabs(src_path):
            src_path = os.path.normpath(os.path.join(base_dir, src_path))
        out.append((parts, src_path))
    return out


def main() -> None:
    parser = argparse.ArgumentParser(description="Build FAT12 disk image for Seolsem media")
    parser.add_argument("--boot", required=True)
    parser.add_argument("--stage2", required=True)
    parser.add_argument("--kernel")
    parser.add_argument("--manifest", help="program payload manifest json")
    parser.add_argument("--no-kernel", action="store_true", help="do not include KERNEL.BIN in the image")
    parser.add_argument("--no-programs", action="store_true", help="do not include manifest payload files")
    parser.add_argument("--output", required=True)
    parser.add_argument("--label", default="SEOLSEM")
    parser.add_argument("--oem", default="SEOLSEM")
    parser.add_argument("--total-sectors", type=int, default=2880)
    parser.add_argument("--sec-per-clus", type=int, default=1)
    parser.add_argument("--root-ents", type=int, default=224)
    parser.add_argument("--fats", type=int, default=2)
    parser.add_argument("--sec-per-track", type=int, default=63)
    parser.add_argument("--heads", type=int, default=16)
    parser.add_argument("--media", type=lambda x: int(x, 0), default=0xF8)
    parser.add_argument("--drive-num", type=lambda x: int(x, 0), default=0x80)
    args = parser.parse_args()

    boot = bytearray(read_file(args.boot))
    stage2 = read_file(args.stage2)

    include_kernel = not args.no_kernel
    include_programs = not args.no_programs
    if not include_kernel and not include_programs:
        raise ValueError("at least one of kernel/program payloads must be enabled")
    if include_kernel and not args.kernel:
        raise ValueError("--kernel is required unless --no-kernel is used")
    if include_programs and not args.manifest:
        raise ValueError("--manifest is required unless --no-programs is used")

    kernel = read_file(args.kernel) if include_kernel else b""
    if include_kernel and include_programs:
        disk_role = SEOLSEM_DISK_ROLE_BOTH
    elif include_kernel:
        disk_role = SEOLSEM_DISK_ROLE_KERNEL
    else:
        disk_role = SEOLSEM_DISK_ROLE_PROGRAMS

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
    patch_boot_sector(boot, bpb, args.label, SEOLSEM_VOL_ID, disk_role)

    image = bytearray(args.total_sectors * SECTOR_SIZE)
    image[0:SECTOR_SIZE] = boot
    stage2_padded = stage2.ljust(stage2_secs * SECTOR_SIZE, b"\x00")
    image[SECTOR_SIZE:SECTOR_SIZE * (1 + stage2_secs)] = stage2_padded

    fat_offset = reserved * SECTOR_SIZE
    root_offset = fat_offset + args.fats * fat_secs * SECTOR_SIZE
    data_offset = root_offset + root_dir_sectors * SECTOR_SIZE
    data_start_lba = reserved + args.fats * fat_secs + root_dir_sectors
    cluster_size = args.sec_per_clus * SECTOR_SIZE

    fat = bytearray(fat_secs * SECTOR_SIZE)
    fat[0] = args.media
    fat[1] = 0xFF
    fat[2] = 0xFF
    next_cluster = 2

    def alloc_clusters(size_bytes: int, force_one: bool = False):
        nonlocal next_cluster
        count = (size_bytes + cluster_size - 1) // cluster_size
        if force_one and count == 0:
            count = 1
        clusters_list = list(range(next_cluster, next_cluster + count))
        next_cluster += count
        for i, cluster in enumerate(clusters_list):
            value = FAT12_EOC if i == count - 1 else clusters_list[i + 1]
            set_fat12_entry(fat, cluster, value)
        return clusters_list

    def write_clusters(data: bytes, clusters_list: list[int]) -> None:
        offset = 0
        for cluster in clusters_list:
            lba = data_start_lba + (cluster - 2) * args.sec_per_clus
            start = lba * SECTOR_SIZE
            chunk = data[offset:offset + cluster_size]
            image[start:start + cluster_size] = chunk.ljust(cluster_size, b"\x00")
            offset += cluster_size

    root_files: list[tuple[str, bytes]] = []
    subdirs: dict[str, list[tuple[str, bytes]]] = {}

    if include_kernel:
        root_files.append(("KERNEL.BIN", kernel))

    if include_programs:
        for parts, src_path in load_manifest(args.manifest):
            data = read_file(src_path)
            if len(parts) == 1:
                _ = to_name83(parts[0])
                root_files.append((parts[0], data))
            else:
                dirname, filename = parts
                _ = name83_raw(dirname)
                _ = to_name83(filename)
                subdirs.setdefault(dirname, []).append((filename, data))

    dir_clusters: dict[str, list[int]] = {}
    file_clusters: dict[tuple[str, str], list[int]] = {}

    for dirname in sorted(subdirs.keys()):
        dir_clusters[dirname] = alloc_clusters(0, force_one=True)

    for name, data in root_files:
        file_clusters[("", name)] = alloc_clusters(len(data))
        if file_clusters[("", name)]:
            write_clusters(data, file_clusters[("", name)])

    for dirname in sorted(subdirs.keys()):
        for filename, data in subdirs[dirname]:
            key = (dirname, filename)
            file_clusters[key] = alloc_clusters(len(data))
            if file_clusters[key]:
                write_clusters(data, file_clusters[key])

    max_cluster = clusters + 1
    if next_cluster - 1 > max_cluster:
        raise ValueError("Not enough clusters on disk for files")

    root_entries = []
    for name, data in root_files:
        start_cluster = file_clusters[("", name)][0] if file_clusters[("", name)] else 0
        root_entries.append(dir_entry(to_name83(name), 0x20, start_cluster, len(data)))
    for dirname in sorted(subdirs.keys()):
        root_entries.append(
            dir_entry(name83_raw(dirname), 0x10, dir_clusters[dirname][0], 32 * (2 + len(subdirs[dirname])))
        )

    if len(root_entries) > args.root_ents:
        raise ValueError("Too many root directory entries")

    root_dir = bytearray(root_dir_sectors * SECTOR_SIZE)
    for idx, entry in enumerate(root_entries):
        start = idx * 32
        root_dir[start:start + 32] = entry
    image[root_offset:root_offset + len(root_dir)] = root_dir

    for dirname in sorted(subdirs.keys()):
        entries = [
            dir_entry(name83_raw("."), 0x10, dir_clusters[dirname][0], 0),
            dir_entry(name83_raw(".."), 0x10, 0, 0),
        ]
        for filename, data in subdirs[dirname]:
            clusters_list = file_clusters[(dirname, filename)]
            start_cluster = clusters_list[0] if clusters_list else 0
            entries.append(dir_entry(to_name83(filename), 0x20, start_cluster, len(data)))

        dir_data = bytearray(cluster_size)
        if len(entries) * 32 > len(dir_data):
            raise ValueError(f"Directory too large for one-cluster model: {dirname}")
        for idx, entry in enumerate(entries):
            start = idx * 32
            dir_data[start:start + 32] = entry
        write_clusters(dir_data, dir_clusters[dirname])

    for i in range(args.fats):
        start = fat_offset + i * fat_secs * SECTOR_SIZE
        image[start:start + len(fat)] = fat

    with open(args.output, "wb") as f:
        f.write(image)

    mode = []
    if include_kernel:
        mode.append("kernel")
    if include_programs:
        mode.append("programs")
    print(
        "[FAT12] image=%s mode=%s sectors=%d reserved=%d fat=%d root=%d data=%d clusters=%d"
        % (args.output, "+".join(mode), args.total_sectors, reserved, fat_secs, root_dir_sectors, data_sectors, clusters)
    )


if __name__ == "__main__":
    main()

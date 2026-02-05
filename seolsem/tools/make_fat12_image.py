#!/usr/bin/env python3
import argparse
import os
import struct
import sys

SECTOR_SIZE = 512
FAT12_EOC = 0x0FFF
FAT12_MAX_CLUSTERS = 4084


def le16(value):
    return struct.pack('<H', value & 0xFFFF)


def le32(value):
    return struct.pack('<I', value & 0xFFFFFFFF)


def write_le16(buf, offset, value):
    buf[offset:offset + 2] = le16(value)


def write_le32(buf, offset, value):
    buf[offset:offset + 4] = le32(value)


def read_file(path):
    with open(path, 'rb') as f:
        return f.read()


def to_name83(filename):
    base, sep, ext = filename.partition('.')
    base = base.upper()
    ext = ext.upper() if sep else ''
    if len(base) > 8 or len(ext) > 3:
        raise ValueError('Invalid 8.3 name: %s' % filename)
    return (base.ljust(8) + ext.ljust(3)).encode('ascii')


def name83_raw(base, ext=''):
    base = base.upper()
    ext = ext.upper()
    if len(base) > 8 or len(ext) > 3:
        raise ValueError('Invalid 8.3 name: %s.%s' % (base, ext))
    return (base.ljust(8) + ext.ljust(3)).encode('ascii')


def dir_entry(name83, attr, start_cluster, size):
    entry = bytearray(32)
    entry[0:11] = name83
    entry[11] = attr & 0xFF
    write_le16(entry, 26, start_cluster)
    write_le32(entry, 28, size)
    return entry


def set_fat12_entry(fat, cluster, value):
    offset = cluster + (cluster // 2)
    if offset + 1 >= len(fat):
        raise ValueError('FAT offset out of range')
    if cluster & 1:
        fat[offset] = (fat[offset] & 0x0F) | ((value << 4) & 0xF0)
        fat[offset + 1] = (value >> 4) & 0xFF
    else:
        fat[offset] = value & 0xFF
        fat[offset + 1] = (fat[offset + 1] & 0xF0) | ((value >> 8) & 0x0F)


def calc_fat_layout(total_sectors, reserved, fats, root_ents, sec_per_clus):
    fat_secs = 1
    while True:
        root_dir_sectors = (root_ents * 32 + SECTOR_SIZE - 1) // SECTOR_SIZE
        data_sectors = total_sectors - reserved - fats * fat_secs - root_dir_sectors
        if data_sectors <= 0:
            raise ValueError('Disk too small for layout')
        clusters = data_sectors // sec_per_clus
        fat_bytes = ((clusters + 2) * 3 + 1) // 2
        fat_secs_needed = (fat_bytes + SECTOR_SIZE - 1) // SECTOR_SIZE
        if fat_secs_needed == fat_secs:
            return fat_secs, root_dir_sectors, data_sectors, clusters
        fat_secs = fat_secs_needed


def patch_boot_sector(boot, bpb, label, vol_id):
    if len(boot) != SECTOR_SIZE:
        raise ValueError('boot sector must be 512 bytes')

    boot[3:11] = bpb['oem'].ljust(8).encode('ascii')
    write_le16(boot, 0x0B, bpb['bytes_per_sec'])
    boot[0x0D] = bpb['sec_per_clus']
    write_le16(boot, 0x0E, bpb['res_sectors'])
    boot[0x10] = bpb['fats']
    write_le16(boot, 0x11, bpb['root_ents'])
    write_le16(boot, 0x13, bpb['total_sectors16'])
    boot[0x15] = bpb['media']
    write_le16(boot, 0x16, bpb['fat_secs'])
    write_le16(boot, 0x18, bpb['sec_per_track'])
    write_le16(boot, 0x1A, bpb['heads'])
    write_le32(boot, 0x1C, bpb['hidden_sectors'])
    write_le32(boot, 0x20, bpb['total_sectors32'])

    boot[0x24] = bpb['drive_num']
    boot[0x25] = 0
    boot[0x26] = 0x29
    write_le32(boot, 0x27, vol_id)
    boot[0x2B:0x36] = label.ljust(11).encode('ascii')
    boot[0x36:0x3E] = b'FAT12   '

    boot[510] = 0x55
    boot[511] = 0xAA


def main():
    parser = argparse.ArgumentParser(description='Build FAT12 disk image with boot+stage2+kernel')
    parser.add_argument('--boot', required=True)
    parser.add_argument('--stage2', required=True)
    parser.add_argument('--kernel', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--label', default='SEOLSEM')
    parser.add_argument('--oem', default='SEOLSEM')
    parser.add_argument('--total-sectors', type=int, default=2880)
    parser.add_argument('--sec-per-clus', type=int, default=1)
    parser.add_argument('--root-ents', type=int, default=224)
    parser.add_argument('--fats', type=int, default=2)
    parser.add_argument('--sec-per-track', type=int, default=63)
    parser.add_argument('--heads', type=int, default=16)
    parser.add_argument('--media', type=lambda x: int(x, 0), default=0xF8)
    parser.add_argument('--drive-num', type=lambda x: int(x, 0), default=0x80)
    args = parser.parse_args()

    boot = bytearray(read_file(args.boot))
    stage2 = read_file(args.stage2)
    kernel = read_file(args.kernel)

    stage2_secs = (len(stage2) + SECTOR_SIZE - 1) // SECTOR_SIZE
    reserved = 1 + stage2_secs

    fat_secs, root_dir_sectors, data_sectors, clusters = calc_fat_layout(
        args.total_sectors, reserved, args.fats, args.root_ents, args.sec_per_clus
    )

    if clusters > FAT12_MAX_CLUSTERS:
        raise ValueError('Cluster count too large for FAT12: %d' % clusters)

    total_sectors16 = args.total_sectors if args.total_sectors < 0x10000 else 0
    total_sectors32 = 0 if total_sectors16 else args.total_sectors

    bpb = {
        'oem': args.oem,
        'bytes_per_sec': SECTOR_SIZE,
        'sec_per_clus': args.sec_per_clus,
        'res_sectors': reserved,
        'fats': args.fats,
        'root_ents': args.root_ents,
        'total_sectors16': total_sectors16,
        'media': args.media,
        'fat_secs': fat_secs,
        'sec_per_track': args.sec_per_track,
        'heads': args.heads,
        'hidden_sectors': 0,
        'total_sectors32': total_sectors32,
        'drive_num': args.drive_num,
    }

    patch_boot_sector(boot, bpb, args.label, 0x12345678)

    image = bytearray(args.total_sectors * SECTOR_SIZE)
    image[0:SECTOR_SIZE] = boot

    stage2_padded = stage2.ljust(stage2_secs * SECTOR_SIZE, b'\x00')
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

    def alloc_clusters(size_bytes, force_one=False):
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

    def write_clusters(data, clusters_list):
        offset = 0
        for cluster in clusters_list:
            lba = data_start_lba + (cluster - 2) * args.sec_per_clus
            start = lba * SECTOR_SIZE
            chunk = data[offset:offset + cluster_size]
            image[start:start + cluster_size] = chunk.ljust(cluster_size, b'\x00')
            offset += cluster_size

    env_data = b"PATH=BIN\r\nHOME=/\r\n"
    hello_data = b"PRINT Hello from Seolsem\r\nEXIT\r\n"
    memo_data = b"PRINT Edit is supported on FAT32.\r\nEXIT\r\n"

    bin_dir_clusters = alloc_clusters(0, force_one=True)
    kernel_clusters = alloc_clusters(len(kernel))
    env_clusters = alloc_clusters(len(env_data))
    hello_clusters = alloc_clusters(len(hello_data))
    memo_clusters = alloc_clusters(len(memo_data))

    max_cluster = clusters + 1
    if next_cluster - 1 > max_cluster:
        raise ValueError('Not enough clusters on disk for files')

    write_clusters(kernel, kernel_clusters)
    write_clusters(env_data, env_clusters)
    write_clusters(hello_data, hello_clusters)
    write_clusters(memo_data, memo_clusters)

    bin_dir_entries = []
    bin_dir_entries.append(dir_entry(name83_raw('.'), 0x10, bin_dir_clusters[0], 0))
    bin_dir_entries.append(dir_entry(name83_raw('..'), 0x10, 0, 0))
    bin_dir_entries.append(dir_entry(to_name83('HELLO.PRG'), 0x20, hello_clusters[0], len(hello_data)))
    bin_dir_entries.append(dir_entry(to_name83('MEMO.PRG'), 0x20, memo_clusters[0], len(memo_data)))

    root_entries = []
    root_entries.append(dir_entry(to_name83('KERNEL.BIN'), 0x20, kernel_clusters[0], len(kernel)))
    root_entries.append(dir_entry(to_name83('XENV.ENV'), 0x20, env_clusters[0], len(env_data)))
    bin_dir_size = 32 * len(bin_dir_entries)
    root_entries.append(dir_entry(name83_raw('BIN'), 0x10, bin_dir_clusters[0], bin_dir_size))

    if len(root_entries) > args.root_ents:
        raise ValueError('Too many root entries')

    root_dir = bytearray(root_dir_sectors * SECTOR_SIZE)
    for idx, entry in enumerate(root_entries):
        start = idx * 32
        root_dir[start:start + 32] = entry
    image[root_offset:root_offset + len(root_dir)] = root_dir

    bin_dir_data = bytearray(cluster_size)
    for idx, entry in enumerate(bin_dir_entries):
        start = idx * 32
        bin_dir_data[start:start + 32] = entry
    write_clusters(bin_dir_data, bin_dir_clusters)

    for i in range(args.fats):
        start = fat_offset + i * fat_secs * SECTOR_SIZE
        image[start:start + len(fat)] = fat

    with open(args.output, 'wb') as f:
        f.write(image)

    print('[FAT12] image=%s sectors=%d reserved=%d fat=%d root=%d data=%d clusters=%d' % (
        args.output, args.total_sectors, reserved, fat_secs, root_dir_sectors, data_sectors, clusters
    ))


if __name__ == '__main__':
    main()

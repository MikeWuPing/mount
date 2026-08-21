#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""mkext4img.py - deterministic minimal ext4 raw image generator.

Builds a small ext4 filesystem the same way mkfatimg.py builds FAT32: by
hand, field by field, so the test asset is reproducible without any
external tool (this host has no WSL/Docker/mkfs.ext4, and CLAUDE.md forbids
installing toolchains).

Layout (8 MiB, 1 KiB blocks, 1 block group of 8192 blocks):
  block 0  boot (zeros)
  block 1  superblock (ext4 rev 1, 1024 bytes)
  block 2  group descriptor table (1 group, 32 bytes)
  block 3  reserved GDT (zeros)
  block 4  block bitmap
  block 5  inode bitmap
  blocks 6-37  inode table (128 inodes x 256 B)
  block 38 root directory data
  block 39 marker.txt data
  blocks 40+ free

Feature set is deliberately conservative (the point of Phase 4 is to learn
what efifs tolerates): compat FILE_TYPE, incompat EXTENTS only. No journal,
no metadata_csum, no 64bit, no dir_index - real-world ext4 volumes carry
more, and "mount -EXT4" Tested flag is about THIS feature baseline plus
whatever real images add later.

Superblock layout follows ext4 dynamic (rev 1) format, offsets per
https://www.kernel.org/doc/html/latest/filesystems/ext4/globals.html.

Usage: python test_images/mkext4img.py <out.img>
"""

import struct
import sys

BLOCK = 1024
BLOCKS_TOTAL = 8192                # 8 MiB
BLOCKS_PER_GROUP = 8192
INODES_PER_GROUP = 128
INODE_SIZE = 256
INODE_TABLE_BLOCKS = (INODES_PER_GROUP * INODE_SIZE) // BLOCK   # 32
ROOT_INODE = 2
FIRST_INO = 11
MARKER_INODE = 12
MARKER_TEXT = "EXT4-MOUNT-OK\n"

# Fixed timestamp 2026-08-01 12:00:00 UTC (deterministic, like mkfatimg).
FIXED_TS = 1787299200

# On-disk blocks used by metadata.
GDT_BLOCK = 2
RESERVED_GDT = 0
BLOCK_BITMAP_BLOCK = 4
INODE_BITMAP_BLOCK = 5
INODE_TABLE_BLOCK = 6
ROOT_DIR_BLOCK = 38
MARKER_DATA_BLOCK = 39
USED_BLOCKS = 40                   # blocks 0..39 allocated

EXT4_EXTENTS_FL = 0x00080000
EXT_MAGIC = 0xF30A


def dir_entry(inode, name, file_type, rec_len):
    b = bytearray()
    b += struct.pack("<HHI", 0, 0, 0)          # placeholder
    b = bytearray()
    b += struct.pack("<IHBB", inode, rec_len, len(name), file_type)
    b += name.encode("ascii")
    assert len(b) <= rec_len
    b += b"\x00" * (rec_len - len(b))
    return bytes(b)


def build_superblock():
    s = bytearray(BLOCK)
    struct.pack_into("<I", s, 0x00, INODES_PER_GROUP)
    struct.pack_into("<I", s, 0x04, BLOCKS_TOTAL)
    struct.pack_into("<I", s, 0x08, 0)                         # r_blocks
    struct.pack_into("<I", s, 0x0C, BLOCKS_TOTAL - USED_BLOCKS)
    struct.pack_into("<I", s, 0x10, INODES_PER_GROUP - 3)      # free inodes
    struct.pack_into("<I", s, 0x14, 1)                         # first_data_block
    struct.pack_into("<I", s, 0x18, 0)                         # log_block_size
    struct.pack_into("<I", s, 0x1C, 0)                         # log_cluster_size
    struct.pack_into("<I", s, 0x20, BLOCKS_PER_GROUP)
    struct.pack_into("<I", s, 0x24, BLOCKS_PER_GROUP)
    struct.pack_into("<I", s, 0x28, INODES_PER_GROUP)
    struct.pack_into("<I", s, 0x2C, FIXED_TS)                  # s_mtime
    struct.pack_into("<I", s, 0x30, FIXED_TS)                  # s_wtime
    struct.pack_into("<H", s, 0x34, 0)                         # mnt_count
    struct.pack_into("<H", s, 0x36, 0xFFFF)                    # max_mnt_count
    struct.pack_into("<H", s, 0x38, 0xEF53)                    # magic
    struct.pack_into("<H", s, 0x3A, 1)                         # state: clean
    struct.pack_into("<H", s, 0x3C, 1)                         # errors: continue
    struct.pack_into("<I", s, 0x40, FIXED_TS)                  # lastcheck
    struct.pack_into("<I", s, 0x44, 0)                         # checkinterval
    struct.pack_into("<I", s, 0x48, 0)                         # creator_os: Linux
    struct.pack_into("<I", s, 0x4C, 1)                         # rev_level: dynamic
    struct.pack_into("<H", s, 0x50, 0)                         # def_resuid
    struct.pack_into("<H", s, 0x52, 0)                         # def_resgid
    struct.pack_into("<I", s, 0x54, FIRST_INO)
    struct.pack_into("<H", s, 0x58, INODE_SIZE)
    struct.pack_into("<H", s, 0x5A, 0)                         # s_block_group_nr
    struct.pack_into("<I", s, 0x5C, 0x2)                       # compat: FILE_TYPE
    struct.pack_into("<I", s, 0x60, 0x40)                      # incompat: EXTENTS
    struct.pack_into("<I", s, 0x64, 0x0)                       # ro_compat: none
    s[0x68:0x78] = bytes.fromhex("5c6d7e8f1111400180010000000002")
    s[0x78:0x88] = b"EXT4TEST" + b"\x00" * 8                   # volume name (16 B)
    struct.pack_into("<H", s, 0xD0, RESERVED_GDT)
    s[0xEE:0xFE] = bytes.fromhex("00112233445566778899aabbccddeeff")
    s[0xFE] = 1                                                # s_def_hash_version
    struct.pack_into("<H", s, 0x100, 0x20)                     # s_desc_size
    struct.pack_into("<I", s, 0x10A, FIXED_TS)                 # s_mkfs_time
    struct.pack_into("<H", s, 0x15E, 32)                       # s_min_extra_isize
    struct.pack_into("<H", s, 0x160, 32)                       # s_want_extra_isize
    return bytes(s)


def build_group_desc():
    g = bytearray(32)
    struct.pack_into("<I", g, 0x00, BLOCK_BITMAP_BLOCK)
    struct.pack_into("<I", g, 0x04, INODE_BITMAP_BLOCK)
    struct.pack_into("<I", g, 0x08, INODE_TABLE_BLOCK)
    struct.pack_into("<H", g, 0x0C, BLOCKS_TOTAL - USED_BLOCKS)
    struct.pack_into("<H", g, 0x0E, INODES_PER_GROUP - 3)
    struct.pack_into("<H", g, 0x10, 1)                         # used_dirs
    return bytes(g)


def build_inode(mode, size, links, data_block):
    i = bytearray(INODE_SIZE)
    struct.pack_into("<H", i, 0x00, mode)
    struct.pack_into("<H", i, 0x02, 0)                         # uid
    struct.pack_into("<I", i, 0x04, size)
    for off in (0x08, 0x0C, 0x10, 0x14):                       # atime/ctime/mtime/dtime
        struct.pack_into("<I", i, off, FIXED_TS)
    struct.pack_into("<H", i, 0x18, 0)                         # gid
    struct.pack_into("<H", i, 0x1A, links)
    struct.pack_into("<I", i, 0x1C, 2)                         # blocks (512B units)
    struct.pack_into("<I", i, 0x20, EXT4_EXTENTS_FL)
    # Extent tree in i_block: 12-byte header at 0x28, first entry at 0x34.
    struct.pack_into("<HHHH", i, 0x28, EXT_MAGIC, 1, 4, 0)
    struct.pack_into("<IHHI", i, 0x28 + 12, 0, 1, 0, data_block)
    struct.pack_into("<H", i, 0x80, 32)                        # i_extra_isize
    return bytes(i)


def create_image(out_path):
    img = bytearray(BLOCKS_TOTAL * BLOCK)

    def put_block(n, data):
        # Multi-block aware: writes data starting at block n, zero-padding
        # the final block (inode table spans 32 contiguous blocks).
        assert len(data) <= (BLOCKS_TOTAL - n) * BLOCK
        pad = (-len(data)) % BLOCK
        img[n * BLOCK:n * BLOCK + len(data)] = data + b"\x00" * pad

    put_block(1, build_superblock())
    put_block(2, build_group_desc())

    # Block bitmap: blocks 0..USED_BLOCKS-1 used.
    bm = bytearray(BLOCK)
    for b in range(USED_BLOCKS):
        bm[b // 8] |= 1 << (b % 8)
    put_block(BLOCK_BITMAP_BLOCK, bytes(bm))

    # Inode bitmap: inodes 1, 2, 12 used.
    im = bytearray(BLOCK)
    for ino in (1, ROOT_INODE, MARKER_INODE):
        im[ino // 8] |= 1 << (ino % 8)
    put_block(INODE_BITMAP_BLOCK, bytes(im))

    # Inode table.
    table = bytearray(INODE_TABLE_BLOCKS * BLOCK)
    for ino, inode in ((ROOT_INODE, build_inode(0x41ED, BLOCK, 2, ROOT_DIR_BLOCK)),
                       (MARKER_INODE, build_inode(0x81A4, len(MARKER_TEXT), 1,
                                                  MARKER_DATA_BLOCK))):
        off = (ino - 1) * INODE_SIZE
        table[off:off + INODE_SIZE] = inode
    put_block(INODE_TABLE_BLOCK, bytes(table))

    # Root directory: '.', '..', 'marker.txt', terminator to end of block.
    root = b""
    root += dir_entry(ROOT_INODE, ".", 2, 12)
    root += dir_entry(ROOT_INODE, "..", 2, 12)
    root += dir_entry(MARKER_INODE, "marker.txt", 1, 20)
    root += struct.pack("<IHBB", 0, BLOCK - len(root), 0, 0)   # terminator
    put_block(ROOT_DIR_BLOCK, root)

    put_block(MARKER_DATA_BLOCK, MARKER_TEXT.encode("ascii"))

    with open(out_path, "wb") as fh:
        fh.write(img)
    print(f"wrote {out_path}: {BLOCKS_TOTAL} blocks x {BLOCK}B, "
          f"marker '{MARKER_TEXT.strip()}' at inode {MARKER_INODE}, "
          f"label EXT4TEST, compat=0x2 incompat=0x40")


def inspect_image(img_path):
    with open(img_path, "rb") as fh:
        img = fh.read()
    magic = struct.unpack_from("<H", img, 1024 + 0x38)[0]
    rev = struct.unpack_from("<I", img, 1024 + 0x4C)[0]
    label = img[1024 + 0x78:1024 + 0x88].rstrip(b"\x00").decode()
    compat = struct.unpack_from("<I", img, 1024 + 0x5C)[0]
    incompat = struct.unpack_from("<I", img, 1024 + 0x60)[0]
    ro_compat = struct.unpack_from("<I", img, 1024 + 0x64)[0]
    print(f"magic 0x{magic:04x} rev {rev} label '{label}' "
          f"compat=0x{compat:x} incompat=0x{incompat:x} ro_compat=0x{ro_compat:x}")
    # Re-read root dir entries from the image to prove the layout round-trips.
    root_data = img[ROOT_DIR_BLOCK * BLOCK:ROOT_DIR_BLOCK * BLOCK + 64]
    off = 0
    while off < len(root_data):
        ino, rec_len, nlen, ftype = struct.unpack_from("<IHBB", root_data, off)
        if ino == 0:
            print("dir end (terminator)")
            break
        name = root_data[off + 8:off + 8 + nlen].decode()
        print(f"entry inode={ino} type={ftype} name='{name}' rec_len={rec_len}")
        off += rec_len

    # Walk the SAME path a driver takes: superblock -> GDT -> inode -> extent
    # -> data block. Catches writer offset bugs (extent entry position, inode
    # table base, GDT fields) that a raw block dump cannot.
    inode_size = struct.unpack_from("<H", img, 1024 + 0x58)[0]
    inodes_per_group = struct.unpack_from("<I", img, 1024 + 0x28)[0]
    gdt_inode_table = struct.unpack_from("<I", img, 2048 + 0x08)[0]

    def read_inode(ino):
        off = gdt_inode_table * BLOCK + (ino - 1) * inode_size
        return img[off:off + inode_size]

    def follow_extent(inode, label):
        magic, entries, _max, depth, _gen = struct.unpack_from("<HHHHI", inode, 0x28)
        if magic != EXT_MAGIC or depth != 0:
            print(f"{label}: BAD extent header magic=0x{magic:x} depth={depth}")
            return None
        eb, elen, ehi, elo = struct.unpack_from("<IHHI", inode, 0x28 + 12)
        print(f"{label}: extent entries={entries} block={eb} len={elen} "
              f"start=0x{ehi:04x}{elo:04x}")
        return elo if (eb == 0 and ehi == 0) else None

    root_inode = read_inode(ROOT_INODE)
    dir_blk = follow_extent(root_inode, "root inode")
    if dir_blk is not None:
        mode = struct.unpack_from("<H", root_inode, 0)[0]
        print(f"root inode mode=0x{mode:04x} size="
              f"{struct.unpack_from('<I', root_inode, 4)[0]} links="
              f"{struct.unpack_from('<H', root_inode, 0x1A)[0]}")

    marker_inode = read_inode(MARKER_INODE)
    data_blk = follow_extent(marker_inode, "marker inode")
    if data_blk is not None:
        content = img[data_blk * BLOCK:data_blk * BLOCK + 32].rstrip(b"\x00")
        print(f"marker content: {content!r}")
        if content != MARKER_TEXT.encode("ascii"):
            print("MARKER MISMATCH")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    if sys.argv[1] == "inspect":
        inspect_image(sys.argv[2])
    else:
        create_image(sys.argv[1])
        inspect_image(sys.argv[1])


if __name__ == "__main__":
    main()

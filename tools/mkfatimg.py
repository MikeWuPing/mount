#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""mkfatimg.py — FAT32 原始镜像生成与检查（MBR 单分区 + LBA 63 起）。

设计依据：Microsoft FAT32 File System Specification (fatgen103.doc)；
与 EDK2 FatPkg (EnhancedFatDxe) 读取路径逐一核对（BPB 校验 / 目录遍历 /
LFN 解码 / ".." 父簇 / FAT32 最小簇数）。分区布局镜像 QEMU vvfat 的
呈现形态（见下"与规格的两处偏离"）。

用途：替代 QEMU vvfat 目录映射作为验证盘。vvfat 的 try_commit 对 guest
删除/簇复用写模式触发断言崩溃（block/vvfat.c:1906/2032/2429，
QEMU-for-Windows 通过率约 1/5）；真 FAT32 原始镜像上 FatPkg 的删除、
写回、簇回收全部走真实磁盘语义，稳定可复现。宿主侧 inspect 重新解析
镜像目录树，恢复"guest 写落盘"硬验证（selftest 残留清理、fixture
中文名条目 LFN 解码）。

命令：
  python tools/mkfatimg.py create <src_dir> <out_img> [--size-mb 257]
  python tools/mkfatimg.py inspect <img>

create 从 src_dir 递归扫描（目录/文件全量收录，不做伪影过滤），生成
FAT32 镜像：扇区 0 = 引导扇区（BPB，字段按 fatgen103 §3.2 精确布局）
+ FSInfo + 备份引导 + 双 FAT + 目录树（8.3 短名 + LFN UTF-16LE）+
连续簇链。inspect 输出目录树（短名/真实名/大小/首簇）供宿主侧验证
guest 写落盘。

与任务规格的一处偏离（"规格前提与 OVMF/FatPkg 实测不符"，实证见
task-5-report.md"FAT 镜像盘改造"章）：
容量：规格的 32MB/4KB 簇/8192 簇组合无法被 EDK2 FatPkg 挂载——其
FAT32 路径要求 MaxCluster >= FAT_MAX_FAT16_CLUSTER (0xFFF5=65525，
FatFileSystem.h:31 / Init.c:369)，8192 簇会判 EFI_VOLUME_CORRUPTED。
保持 4KB 簇（BPB_SecPerClus=8）下满足 ≥65525 簇的最小容量为 257MB
（65659 簇，FAT 514 扇区）；BPB_FATSz32 仍按规格公式
ceil((cluster_count+2)×4/512) 计算。BPB 其余字段与规格一致（含
HiddSec=0、TotSec32、RootClus=2、FSInfo=1、BkBootSec=6、0x55AA）。
superfloppy 形态实测可挂载：本 OVMF 的 FatPkg 直接绑定整盘 BlockIo
读取扇区 0 的 BPB（无需 MBR/分区；vvfat 的 HD0a1 分区只是 QEMU fat:
驱动自带的合成 MBR，非必需）。
"""

import argparse
import math
import os
import struct
import sys

SECTOR = 512                     # 扇区 512B（BPB_BytsPerSec）
SEC_PER_CLUS = 8                 # 簇 4KB（BPB_SecPerClus）
BYTES_PER_CLUSTER = SECTOR * SEC_PER_CLUS
RSVD_SEC_CNT = 32                # BPB_RsvdSecCnt（引导+FSInfo+保留）
NUM_FATS = 2                     # BPB_NumFATs（主+镜像）
ROOT_CLUS = 2                    # BPB_RootClus：根目录簇
FAT32_MIN_CLUSTERS = 0xFFF5      # FatPkg 要求 FAT32 簇数 >= 65525（FatFileSystem.h:31）
EOC = 0x0FFFFFFF                 # FAT EOF 标记（fatgen103 §5.4）
FAT0 = 0x0FFFFFF8                # FAT[0] 介质描述符条目
ATTR_LFN = 0x0F                  # LFN 条目属性
ATTR_DIRECTORY = 0x10
ATTR_ARCHIVE = 0x20
# 固定时间戳 2026-08-01 12:00:00（fatgen103 §4.7：年-1980 左移 9 | 月左移 5 | 日）
FIXED_DATE = ((2026 - 1980) << 9) | (8 << 5) | 1
FIXED_TIME = (12 << 11)
VOL_ID = 0x20260801              # BS_VolID（确定性取值）


def fat_sz_for(part_tot_sec, rsvd, num_fats, sec_per_clus):
    """FAT32 的 FAT 表扇区数：ceil((簇数+2)×4/512)，迭代至自洽。

    fatgen103 §3.2：FATSz32 必须覆盖全部数据簇条目（含 FAT[0]/FAT[1]
    预留项），每项 4 字节；数据簇数又依赖 FAT 表大小，迭代求不动点。"""
    fat_sec = 1
    while True:
        clus = (part_tot_sec - rsvd - num_fats * fat_sec) // sec_per_clus
        need_sec = ((clus + 2) * 4 + SECTOR - 1) // SECTOR
        if need_sec <= fat_sec:
            return fat_sec, clus
        fat_sec = need_sec


def bpb_bytes(tot_sec, rsvd, num_fats, fat_sec, root_clus):
    """引导扇区（BPB，fatgen103 §3.2 字段表）。FAT32 特有字段逐项精确。"""
    b = bytearray(SECTOR)
    b[0:3] = b"\xEB\x58\x90"                 # BS_JmpBoot
    b[3:11] = b"MSWIN4.1"                    # BS_OEMName
    struct.pack_into("<H", b, 11, SECTOR)    # BPB_BytsPerSec
    b[13] = SEC_PER_CLUS                     # BPB_SecPerClus
    struct.pack_into("<H", b, 14, rsvd)      # BPB_RsvdSecCnt
    b[16] = num_fats                         # BPB_NumFATs
    struct.pack_into("<H", b, 17, 0)         # BPB_RootEntCnt（FAT32 恒 0）
    struct.pack_into("<H", b, 19, 0)         # BPB_TotSec16（FAT32 恒 0）
    b[21] = 0xF8                             # BPB_Media
    struct.pack_into("<H", b, 22, 0)         # BPB_FATSz16（FAT32 恒 0）
    struct.pack_into("<H", b, 24, 63)        # BPB_SecPerTrk
    struct.pack_into("<H", b, 26, 255)       # BPB_NumHeads
    struct.pack_into("<I", b, 28, 0)         # BPB_HiddSec（superfloppy 恒 0）
    struct.pack_into("<I", b, 32, tot_sec)   # BPB_TotSec32
    struct.pack_into("<I", b, 36, fat_sec)   # BPB_FATSz32
    struct.pack_into("<H", b, 40, 0)         # BPB_ExtFlags（镜像 FAT 无需标志）
    struct.pack_into("<H", b, 42, 0)         # BPB_FSVer
    struct.pack_into("<I", b, 44, root_clus)  # BPB_RootClus
    struct.pack_into("<H", b, 48, 1)         # BPB_FSInfo
    struct.pack_into("<H", b, 50, 6)         # BPB_BkBootSec
    b[52:64] = b"\x00" * 12                  # BPB_Reserved
    b[64] = 0x80                             # BS_DrvNum
    b[65] = 0                                # BS_Reserved1
    b[66] = 0x29                             # BS_BootSig
    struct.pack_into("<I", b, 67, VOL_ID)    # BS_VolID
    b[71:82] = b"GUFILE      "               # BS_VolLab
    b[82:90] = b"FAT32   "                   # BS_FilSysType
    b[510:512] = b"\x55\xAA"                 # 引导签名
    return b


def fsinfo_bytes():
    """FSInfo 扇区（fatgen103 §6.2）。free/next 置 0xFFFFFFFF 表示未知，
    FatPkg 会自行扫描重算，无需精确。"""
    fi = bytearray(SECTOR)
    struct.pack_into("<I", fi, 0, 0x41615252)     # FSI_LeadSig
    struct.pack_into("<I", fi, 484, 0x61417272)   # FSI_StrucSig
    struct.pack_into("<I", fi, 488, 0xFFFFFFFF)   # FSI_Free_Count
    struct.pack_into("<I", fi, 492, 0xFFFFFFFF)   # FSI_Nxt_Free
    struct.pack_into("<I", fi, 508, 0xAA550000)   # FSI_TrailSig
    return fi


def short_to_name(short11):
    """8.3 字段还原可读名（去空格补丁，基名 + "." + 扩展名）。"""
    base = short11[:8].decode("latin-1").rstrip()
    ext = short11[8:].decode("latin-1").rstrip()
    return base + ("." + ext if ext else "")


def lfn_checksum(short11):
    """LFN 短名校验和（fatgen103 §7.2.1.5）：逐字节右环移累加。"""
    c = 0
    for byte in short11:
        c = (((c & 1) << 7) | (c >> 1)) + byte
        c &= 0xFF
    return c


def lfn_entries(name, short11):
    """LFN 目录项序列（attr 0x0F，UTF-16LE，每项 13 字符）。

    fatgen103 §7.2：条目倒序存放——磁盘顺序为序号 N（末项置 0x40
    LDIR_Last 标志）递减至 1，短名条目紧随其后。名字用尽后写 0x0000
    结束标志，余位补 0xFFFF（名字恰好填满 13 字符时无结束标志）。
    注意结束/填充顺序不能反：写成 0xFFFF 结束 + 0x0000 填充时，FatPkg
    会把 0xFFFF 当作名字字符，长名带垃圾尾、按真名打不开文件。"""
    cksum = lfn_checksum(short11)
    total = (len(name) + 12) // 13
    out = []
    for seq in range(total, 0, -1):
        chunk = name[(seq - 1) * 13: seq * 13]
        chars = list(chunk)
        if len(chunk) < 13:
            chars.append("\x00")                # LDIR name terminator: NUL (fatgen103 sec.7.2)
            chars += ["\uffff"] * (12 - len(chunk))  # remaining chars padded with 0xFFFF
        raw = "".join(chars).encode("utf-16-le")
        e = bytearray(32)
        e[0] = (0x40 if seq == total else 0) | seq    # LDIR_Ord
        e[1:11] = raw[0:10]                            # LDIR_Name1（5 字符）
        e[11] = ATTR_LFN                               # LDIR_Attr
        e[12] = 0                                      # LDIR_Type
        e[13] = cksum                                  # LDIR_Chksum
        e[14:26] = raw[10:22]                          # LDIR_Name2（6 字符）
        e[26:28] = b"\x00\x00"                         # LDIR_FstClusLo（恒 0）
        e[28:32] = raw[22:26]                          # LDIR_Name3（2 字符）
        out.append(e)
    return out


def short_name(name, taken):
    """8.3 短名生成（确定性，返回 11 字节 on-disk 形式）。

    ASCII 名转大写 8.3（基名 ≤8 且扩展名 ≤3 时直接容纳，如
    BOOTX64.EFI → "BOOTX64  EFI"）；超长或含非 ASCII 时用前 6 字符 +
    "~N"（N 从 1 递增至目录内无冲突）。非 ASCII 名用固定前缀 GUNAME
    （确定性选择，guest 侧经 LFN 取真实名）。"""
    base, dot, ext = name.rpartition(".")
    if not dot:
        base, ext = name, ""          # rpartition 无分隔符时名字在第三元组
    b = "".join(ch.upper() for ch in base if ch.isascii())
    e = "".join(ch.upper() for ch in ext if ch.isascii())
    ascii_ok = len(b) == len(base) and len(e) == len(ext)
    if ascii_ok and len(b) <= 8 and len(e) <= 3:
        short = f"{b:<8}{e:<3}"
        if short not in taken:
            taken.add(short)
            return short.encode("latin-1")
    prefix = (b if ascii_ok else "GUNAME")[:6].ljust(3, "_")
    for n in range(1, 1000):
        short = f"{prefix[:6]}~{n}"[:8].ljust(8) + f"{e[:3]:<3}"
        if short not in taken:
            taken.add(short)
            return short.encode("latin-1")
    raise RuntimeError(f"cannot generate short name for {name!r}")


def dir_entry(short11, attr, first_clus, size):
    """短名目录项（fatgen103 §4.4）。时间戳取固定值；目录大小恒 0。"""
    e = bytearray(32)
    e[0:11] = short11
    e[11] = attr
    e[12] = 0                                       # NT 保留
    e[13] = 0                                       # 创建时间 10ms 单位
    struct.pack_into("<H", e, 14, FIXED_TIME)       # CrtTime
    struct.pack_into("<H", e, 16, FIXED_DATE)       # CrtDate
    struct.pack_into("<H", e, 18, FIXED_DATE)       # LastAccDate
    struct.pack_into("<H", e, 20, (first_clus >> 16) & 0xFFFF)  # 首簇高 16 位
    struct.pack_into("<H", e, 22, FIXED_TIME)       # WrtTime
    struct.pack_into("<H", e, 24, FIXED_DATE)       # WrtDate
    struct.pack_into("<H", e, 26, first_clus & 0xFFFF)         # 首簇低 16 位
    struct.pack_into("<I", e, 28, size)             # FileSize
    return e


class Node:
    __slots__ = ("name", "is_dir", "size", "children", "first_clus",
                 "parent_clus", "dir_n")

    def __init__(self, name, is_dir, size, children=None):
        self.name = name
        self.is_dir = is_dir
        self.size = size
        self.children = children or []
        self.first_clus = 0
        self.parent_clus = 0
        self.dir_n = 0      # 目录条目所需簇数（layout 期按实际条目数预算）


def scan_tree(src):
    """递归扫描宿主暂存目录：目录/文件全量收录，不做伪影过滤。"""

    def rec(path):
        nodes = []
        for name in os.listdir(path):
            full = os.path.join(path, name)
            if os.path.isdir(full):
                nodes.append(Node(name, True, 0, rec(full)))
            elif os.path.isfile(full):
                nodes.append(Node(name, False, os.path.getsize(full)))
        # 目录在前 + ASCII 忽略大小写（确定性排序）
        nodes.sort(key=lambda nd: (not nd.is_dir, nd.name.lower()))
        return nodes

    # 根目录节点：名字无意义（根目录没有目录项），parent_clus=0 表示无
    # "."/".." 条目（FatPkg 对根目录不生成点条目）
    return Node("", True, 0, rec(src))


def create_image(src, out_img, size_mb):
    """组装镜像：BPB/FSInfo → 双 FAT → 目录树（LFN+簇链）→ 文件数据。

    superfloppy 形态（无 MBR/分区）：本 OVMF 的 FatPkg 直接绑定整盘
    BlockIo 并读取扇区 0 的 BPB，无需分区表。"""
    tot_sec = size_mb * 1024 * 1024 // SECTOR
    fat_sec, clus_count = fat_sz_for(tot_sec, RSVD_SEC_CNT, NUM_FATS,
                                     SEC_PER_CLUS)
    if clus_count < FAT32_MIN_CLUSTERS:
        raise SystemExit(
            f"size {size_mb}MB gives {clus_count} clusters < FAT32 minimum "
            f"{FAT32_MIN_CLUSTERS} (EDK2 FatPkg rejects); use --size-mb 257")
    data_sec = RSVD_SEC_CNT + NUM_FATS * fat_sec      # 簇 N 的扇区起点
    data_start = data_sec * SECTOR
    root = scan_tree(src)
    img = bytearray(tot_sec * SECTOR)

    # 扇区 0：引导扇区 / FSInfo（扇区 2-5 保持清零）/ 备份引导扇区
    bpb = bpb_bytes(tot_sec, RSVD_SEC_CNT, NUM_FATS, fat_sec, ROOT_CLUS)
    img[0:SECTOR] = bpb
    img[SECTOR:2 * SECTOR] = fsinfo_bytes()
    img[6 * SECTOR:7 * SECTOR] = bpb

    # 簇分配：目录簇链长度必须先于分配确定（审查修复——旧实现先给目录
    # 1 簇再顺次给子项，write_dir_entries 按实际条目数算出的多簇链会吞掉
    # 已分配给子项的簇且链末无 EOC）。现按"先收集本目录全部条目（含
    # LFN 条目数）算出 n，再为目录分配 n 簇链"分配，子项簇分配严格在
    # 目录链之后、不与目录链重叠；链尾由 alloc_run 写 EOC（0x0FFFFFFF）。
    # 目录保底 1 簇（"."/".." 项）；空文件 0 簇（首簇域 0，无链）。
    fat = {}
    next_clus = ROOT_CLUS

    def alloc_run(size):
        nonlocal next_clus
        if size == 0:
            return 0, []
        n = (size + BYTES_PER_CLUSTER - 1) // BYTES_PER_CLUSTER
        chain = list(range(next_clus, next_clus + n))
        next_clus += n
        for i, c in enumerate(chain):
            fat[c] = chain[i + 1] if i + 1 < len(chain) else EOC
        return chain[0], chain

    def build_dir_entries(node):
        """本目录全部目录项字节（"."/".."、每子项的 LFN+短名、结束符）。
        layout 期先跑一遍定链长（此时子项簇号未定、占位 0，不影响长度），
        write 期再跑一遍写真实簇号——两遍结构相同，链长必然相等。"""
        taken = set()
        entries = bytearray()
        if node.parent_clus != 0:
            entries += dir_entry(b"." + b" " * 10, ATTR_DIRECTORY, node.first_clus, 0)
            parent = node.parent_clus if node.parent_clus != ROOT_CLUS else 0
            entries += dir_entry(b".." + b" " * 9, ATTR_DIRECTORY, parent, 0)
        for child in node.children:
            s11 = short_name(child.name, taken)
            attr = ATTR_DIRECTORY if child.is_dir else ATTR_ARCHIVE
            first = child.first_clus if child.is_dir or child.size else 0
            if child.name != short_to_name(s11):
                entries += b"".join(lfn_entries(child.name, s11))
            entries += dir_entry(s11, attr, first, child.size)
        entries += b"\x00"
        return entries

    def layout_dir(node, parent_clus):
        node.parent_clus = parent_clus
        node.dir_n = (len(build_dir_entries(node)) + BYTES_PER_CLUSTER - 1) \
            // BYTES_PER_CLUSTER
        node.first_clus, _ = alloc_run(node.dir_n * BYTES_PER_CLUSTER)
        for child in node.children:
            if child.is_dir:
                layout_dir(child, node.first_clus)
            else:
                child.first_clus, _ = alloc_run(child.size)

    # 根目录链：无点条目，链长同样按根条目数预算（根条目超一簇也能容纳）
    root.parent_clus = 0
    root.dir_n = (len(build_dir_entries(root)) + BYTES_PER_CLUSTER - 1) \
        // BYTES_PER_CLUSTER
    root.first_clus, _ = alloc_run(root.dir_n * BYTES_PER_CLUSTER)
    for child in root.children:
        if child.is_dir:
            layout_dir(child, root.first_clus)
        else:
            child.first_clus, _ = alloc_run(child.size)

    # 目录项写入：用与 FAT 表一致的簇链（alloc_run 已写好链与链尾 EOC，
    # 此处只写数据，不再重算链）。末尾 0x00 结束符（FatPkg 遇 0 停读）。
    # ".." 父簇：父为根时写 0（fatgen103 §3.4 点条目；FatPkg 自建目录
    # 时对根父同样写 0，与 mkfs.fat 一致）。文件名无法由 8.3 短名直接
    # 表示（小写/中文/超长）时补 LFN 承载真实名。
    def write_dir_entries(node):
        entries = build_dir_entries(node)
        n = (len(entries) + BYTES_PER_CLUSTER - 1) // BYTES_PER_CLUSTER
        if n != node.dir_n:
            raise RuntimeError(
                f"dir {node.name!r} entry budget drift: layout {node.dir_n} "
                f"vs write {n}")
        for i in range(n):
            c = node.first_clus + i
            off = data_start + (c - ROOT_CLUS) * BYTES_PER_CLUSTER
            img[off:off + BYTES_PER_CLUSTER] = \
                entries[i * BYTES_PER_CLUSTER:(i + 1) * BYTES_PER_CLUSTER]

    def write_all_dir_entries(node):
        write_dir_entries(node)
        for child in node.children:
            if child.is_dir:
                write_all_dir_entries(child)

    write_all_dir_entries(root)

    # 文件数据写盘（连续簇区一次写入）
    def write_files(node, path):
        for child in node.children:
            p = os.path.join(path, child.name)
            if child.is_dir:
                write_files(child, p)
            elif child.first_clus:
                off = data_start + (child.first_clus - ROOT_CLUS) * BYTES_PER_CLUSTER
                with open(p, "rb") as fh:
                    img[off:off + child.size] = fh.read()

    write_files(root, src)

    # FAT 表：FAT[0]=介质描述符、FAT[1]=0x0FFFFFFF（fatgen103 §5.4）
    raw_fat = bytearray(fat_sec * SECTOR)
    for c, nxt in fat.items():
        struct.pack_into("<I", raw_fat, c * 4, nxt)
    struct.pack_into("<I", raw_fat, 0, FAT0)
    struct.pack_into("<I", raw_fat, 4, EOC)
    for f in range(NUM_FATS):
        off = (RSVD_SEC_CNT + f * fat_sec) * SECTOR
        img[off:off + len(raw_fat)] = raw_fat

    with open(out_img, "wb") as fh:
        fh.write(img)
    print(f"wrote {out_img}: {tot_sec} sectors, {clus_count} clusters, "
          f"FAT {fat_sec} sec x{NUM_FATS}, data at sec {data_sec}")


# ---------------------------------------------------------------- inspect --
# 独立于 create 的只读解析路径，用于宿主侧验证 guest 写落盘。

def partition_start(img):
    """MBR 分区 0 起始 LBA；无有效 MBR 时按 superfloppy（0）处理。"""
    if img[510:512] == b"\x55\xAA":
        indicator = img[446]
        if indicator in (0x00, 0x80) and img[450] != 0x00:
            start = struct.unpack_from("<I", img, 454)[0]
            if start > 0:
                return start
    return 0


def read_bpb(img, bpb_off):
    (sec, spc, rsvd, nfat, _root_ent, tot16, _media, fat16, _spt, _heads,
     _hid, _tot32) = struct.unpack_from("<HBHBHHBHHHII", img, 11 + bpb_off)
    tot = struct.unpack_from("<I", img, 32 + bpb_off)[0] if tot16 == 0 else tot16
    fat_sec = struct.unpack_from("<I", img, 36 + bpb_off)[0] if fat16 == 0 else fat16
    root = struct.unpack_from("<I", img, 44 + bpb_off)[0]
    if sec != SECTOR:
        raise ValueError(f"unexpected bytes/sector {sec}")
    return {
        "spc": spc, "rsvd": rsvd, "nfat": nfat, "tot": tot, "fat_sec": fat_sec,
        "root": root, "bpb_off": bpb_off,
        "data_start": bpb_off + (rsvd + nfat * fat_sec) * SECTOR,
    }


def inspect_image(img_path):
    """递归列出目录树：短名/真实名(UTF-8)/大小/首簇。"""
    with open(img_path, "rb") as fh:
        img = fh.read()
    g = read_bpb(img, partition_start(img) * SECTOR)
    bpc = SECTOR * g["spc"]
    fat_off = g["bpb_off"] + g["rsvd"] * SECTOR

    def read_clus(c):
        off = g["data_start"] + (c - ROOT_CLUS) * bpc
        return img[off:off + bpc]

    def next_clus(c):
        return struct.unpack_from("<I", img, fat_off + c * 4)[0]

    def lfn_decode(pending, short11):
        """磁盘顺序收集的 LFN（序号 N..1）还原真实名；校验和不符或
        残缺时回退 8.3（与 FatPkg 行为一致）。"""
        by_seq = sorted(pending, key=lambda e: e[0] & 0x3F)
        chars = []
        for e in by_seq:
            raw = e[1:11] + e[14:26] + e[28:32]
            for i in range(0, 26, 2):
                u = raw[i] | (raw[i + 1] << 8)
                if u == 0xFFFF:
                    return "".join(chars)
                if u != 0:
                    chars.append(chr(u))
        name = "".join(chars)
        if name and by_seq and lfn_checksum(short11) == by_seq[0][13]:
            return name
        return ""

    lines = []
    occup = {}          # 簇号 → 首个占用描述（"dir <path>" / "file <path>"）
    conflicts = []      # (簇号, 首占用, 次占用)

    def walk_chain_mark(clus, what):
        """沿 FAT 链记录每个簇的占用（文件数据链用；环/越界防护）。"""
        seen = set()
        while ROOT_CLUS <= clus < 0x0FFFFFF8 and clus not in seen:
            seen.add(clus)
            if clus in occup:
                conflicts.append((clus, occup[clus], what))
            else:
                occup[clus] = what
            clus = next_clus(clus)

    def walk_dir(clus, depth, visited, path):
        if clus in visited:
            return
        visited = visited | {clus}
        entries = []
        pending = []
        chain = []
        while True:
            chain.append(clus)
            data = read_clus(clus)
            done = False
            for off in range(0, bpc, 32):
                e = data[off:off + 32]
                b0 = e[0]
                if b0 == 0x00:                     # 目录结束
                    done = True
                    break
                if b0 == 0xE5:                     # 已删除条目，其后 LFN 作废
                    pending.clear()
                    continue
                if e[11] == ATTR_LFN:
                    pending.append(e)
                    continue
                short11 = e[0:11]
                name = lfn_decode(pending, short11) or short_to_name(short11)
                pending = []
                first = struct.unpack_from("<H", e, 26)[0] | \
                    (struct.unpack_from("<H", e, 20)[0] << 16)
                size = struct.unpack_from("<I", e, 28)[0]
                entries.append((name, short11, bool(e[11] & ATTR_DIRECTORY),
                                first, size))
            if done:
                break
            clus = next_clus(clus)
            if clus >= 0x0FFFFFF8 or clus in visited:
                break
        # 簇被多次占用检测：目录链与文件数据链共用占用集合，重复即报告
        for c in chain:
            if c in occup:
                conflicts.append((c, occup[c], f"dir {path}"))
            else:
                occup[c] = f"dir {path}"
        children = [x for x in entries if x[0] not in (".", "..")]
        if depth == 0:
            lines.append(f"[R] (root) {len(children)} entries")
        else:
            lines.append(f"{'  ' * depth}[D] {len(children)} entries")
        for name, short11, is_dir, first, size in children:
            lines.append(f"{'  ' * (depth + 1)}[{('D' if is_dir else 'F')}] "
                         f"short={short_to_name(short11)!r} real={name!r} "
                         f"size={size} clus={first}")
            if is_dir:
                walk_dir(first, depth + 1, visited, path + "/" + name)
            elif first >= ROOT_CLUS:
                walk_chain_mark(first, f"file {path}/{name}")

    walk_dir(g["root"], 0, set(), "")
    text = "\n".join(lines)
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    print(text)
    if conflicts:
        for c, first, second in conflicts:
            print(f"CLUSTER CONFLICT: cluster {c} used by {first} and {second}")
    else:
        print("CLUSTER CONFLICT: none")
    leftovers = [ln for ln in lines if "__gufile_st" in ln]
    print(f"LEFTOVERS __gufile_st*: {len(leftovers)}" if leftovers
          else "LEFTOVERS __gufile_st*: none")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("create")
    c.add_argument("src_dir")
    c.add_argument("out_img")
    c.add_argument("--size-mb", type=int, default=257)
    i = sub.add_parser("inspect")
    i.add_argument("img")
    a = ap.parse_args()
    if a.cmd == "create":
        create_image(a.src_dir, a.out_img, a.size_mb)
    else:
        inspect_image(a.img)


if __name__ == "__main__":
    main()

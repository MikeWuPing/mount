# mount — UEFI Shell 挂载工具

> [English README](README_en.md) | 中文说明

mount 是一个运行在 UEFI Shell 下的纯命令行挂载工具，对标 Linux 的 `mount` 命令：加载只读文件系统驱动、重扫卷、自动挂载，把固件里原本不可见的 NTFS / ext4 / ISO 等卷以 `fsN:` 的形式挂出来。项目基于 edk2（VS2019/X64）构建，所有功能经 QEMU + OVMF 闭环验证（串口版本断言 + 截屏证据）。

## 功能

- `mount -NTFS` / `mount -EXT4` / `mount -<FORMAT>`：加载对应格式的只读驱动（已加载则跳过），执行等效 `map -r` 的全量重扫，报告新增卷。格式由"格式名 → 驱动文件"映射表驱动，新增格式无需改动主流程。
- `mount -ISO <文件>`：把 ISO 文件虚拟成块设备（Linux loop device 的等价物），由固件驱动栈自动挂载出其文件系统。ISO9660 驱动自动加载，UDF 由固件内置 UdfDxe 认领；挂载由常驻驱动承载，**mount 退出后卷仍可访问**。
- `mount`：列出当前 `fsN:` 映射与支持的格式表（含 `[tested]` 实测标注）。
- `mount -h`：帮助信息（含作者署名）。
- `mount selftest`：隐藏自检命令（QEMU 无人值守回归的断言锚点）。

## 用法示例

```
Shell> mount -NTFS
MOUNT: driver drivers\ntfs_x64.efi loaded
MOUNT: new volume fs1:  label="MOUNTTEST"
Shell> dir fs1:\
Directory of: fs1:\
         r  15  ntfs_marker.txt
Shell> type fs1:\ntfs_marker.txt
NTFS-MOUNT-OK

Shell> mount -ISO win10_iot.iso
MOUNT: loop device created for win10_iot.iso
MOUNT: new volume fs1:  label="AMBM_x86FREO_EN-US_DV5"
```

## 架构

- `MountPkg/Application/Mount/`：mount.efi 本体（参数解析、驱动加载编排、loop 创建、map 刷新与报告）。
- `MountPkg/Drivers/LoopDxe/`：常驻 loop 驱动（部署为 `drivers\loop_x64.efi`），提供 `MOUNT_LOOP_FACTORY_PROTOCOL`；所有挂载存活所需的代码都在驱动镜像里，因此挂载能活过 mount.efi 的退出。
- `drivers/`：文件系统驱动部署目录（efifs 预编译件 + 自建 loop 驱动）。
- 测试资产生成器（`test_images/`）：`mkext4img.py`（手写 ext4 镜像）、`New-TestIso.ps1`（ISO9660/UDF）、`New-NtfsVhd.ps1`（NTFS VHD）。

## 构建与验证

环境要求：edk2（VS2019/X64/DEBUG）、QEMU、OVMF 固件、Python 3。

```powershell
# 版本化构建（BUILD+1、再生 Version.h、写 expected_version.txt、重建 qemu_disk 镜像）
powershell -ExecutionPolicy Bypass -File tools/Build-Mount.ps1

# QEMU 闭环运行（串口版本断言 + 定时截屏；-Window 开可见窗口，-Manual 仅启动供人工操作）
powershell -ExecutionPolicy Bypass -File tools/Run-MountQemu.ps1 -Script "t30 screendump main"
```

## 已实测格式

| 格式 | 挂载路径 | 状态 |
|---|---|---|
| NTFS | `mount -NTFS`（efifs ntfs 驱动） | ✅ tested |
| EXT4 | `mount -EXT4`（efifs ext2 驱动，覆盖 ext2/3/4） | ✅ tested |
| ISO9660 | `mount -ISO`（efifs iso9660 驱动，自动加载） | ✅ tested |
| UDF | `mount -ISO`（固件内置 UdfDxe） | ✅ tested |
| BTRFS / XFS | `mount -BTRFS` / `mount -XFS`（驱动已部署） | ⏳ 待实测 |

## 许可证与第三方组件

- **本项目代码**（MountPkg、tools、脚本、文档）：[PolyForm Noncommercial 1.0.0](LICENSE)——源码开放，禁止商业用途（个人学习、研究、非商业组织使用不受限）。
- **外部二进制**：`drivers/` 下的 5 个文件系统驱动（`ntfs_x64.efi`、`iso9660_x64.efi`、`ext2_x64.efi`、`btrfs_x64.efi`、`xfs_x64.efi`）来自 **pbatard/efifs v1.12**（GRUB2 只读文件系统驱动的 UEFI 移植，GPLv3+），预编译件取自 [efi.akeo.ie](https://efi.akeo.ie/downloads/efifs-1.12/x64/)；来源、SHA256 校验与 GPLv3 源码提供声明见 [`drivers/README.md`](drivers/README.md)。
- **`drivers/loop_x64.efi`** 是项目自建驱动（LoopDxe 构建产物），非外部二进制。

## 已知限制

- 全部挂载为只读；`mount -ISO` 需要镜像文件位于当前可访问卷上。
- 纯 ISO9660 镜像需要 `drivers\iso9660_x64.efi` 与 mount.efi 同目录部署（mount 会嗅探并自动加载）。
- 超过 4 GiB 的 ISO 无法放入 FAT32 卷，需先经 `mount -NTFS` 挂出 NTFS 数据盘中转（端到端场景规划中）。
- 本 OVMF 固件缺陷：对"无任何驱动认领"的块设备执行全局重扫可能死锁——规避方式为先加载对应文件系统驱动再重扫。

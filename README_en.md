# mount — UEFI Shell mount tool

> 中文说明：[README.md](README.md) | English README

mount is a pure command-line mount tool for the UEFI Shell, modeled after Linux `mount`: it loads read-only file system drivers, rescans block devices, and mounts volumes (NTFS, ext4, ISO, ...) that the firmware would otherwise not expose, making them available as `fsN:` mappings. Built on edk2 (VS2019/X64), every feature is verified through a QEMU + OVMF closed loop (serial version assertion + screendump evidence).

## Features

- `mount -NTFS` / `mount -EXT4` / `mount -<FORMAT>`: loads the read-only driver for the format (skipped if already loaded), performs a `map -r`-equivalent rescan, and reports newly mounted volumes. Formats are driven by a "format name → driver file" table, so adding a format requires no changes to the main flow.
- `mount -ISO <file>`: presents an ISO file as a virtual block device (the equivalent of a Linux loop device) so the firmware driver stack mounts its file system automatically. The ISO9660 driver is auto-loaded when sniffed; UDF is claimed by the firmware's built-in UdfDxe. The mount is carried by a resident driver, so **volumes stay accessible after mount.efi exits**.
- `mount`: lists current `fsN:` mappings and the supported-format table (with `[tested]` marks).
- `mount -h`: help output (includes the author line).
- `mount selftest`: hidden self-test command (assertion anchor for unattended QEMU regression).

## Usage examples

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

## Architecture

- `MountPkg/Application/Mount/`: mount.efi itself (argument parsing, driver-load orchestration, loop creation, map refresh and reporting).
- `MountPkg/Drivers/LoopDxe/`: the resident loop driver (deployed as `drivers\loop_x64.efi`), publishing `MOUNT_LOOP_FACTORY_PROTOCOL`. All code a mounted volume can touch lives in this driver image, which is why mounts survive mount.efi's exit.
- `drivers/`: deployment directory for file system drivers (efifs prebuilts + the project's own loop driver).
- Test asset generators (`test_images/`): `mkext4img.py` (hand-built ext4 image), `New-TestIso.ps1` (ISO9660/UDF), `New-NtfsVhd.ps1` (NTFS VHD).

## Build & verification

Requirements: edk2 (VS2019/X64/DEBUG), QEMU, an OVMF firmware, Python 3.

```powershell
# Versioned build (BUILD+1, regenerate Version.h, write expected_version.txt, rebuild the qemu_disk image)
powershell -ExecutionPolicy Bypass -File tools/Build-Mount.ps1

# QEMU closed-loop run (serial version assertion + timed screendumps; -Window opens a visible window, -Manual launches for hands-on use)
powershell -ExecutionPolicy Bypass -File tools/Run-MountQemu.ps1 -Script "t30 screendump main"
```

## Tested formats

| Format | Path | Status |
|---|---|---|
| NTFS | `mount -NTFS` (efifs ntfs driver) | ✅ tested |
| EXT4 | `mount -EXT4` (efifs ext2 driver; covers ext2/3/4) | ✅ tested |
| ISO9660 | `mount -ISO` (efifs iso9660 driver, auto-loaded) | ✅ tested |
| UDF | `mount -ISO` (firmware-built-in UdfDxe) | ✅ tested |
| BTRFS / XFS | `mount -BTRFS` / `mount -XFS` (drivers deployed) | ⏳ pending |

## License & third-party components

- **Project code** (MountPkg, tools, scripts, docs): [PolyForm Noncommercial 1.0.0](LICENSE) — source-available; commercial use is not permitted (personal study, research, and noncommercial organizations are free to use it).
- **External binaries**: the 5 file system drivers in `drivers/` (`ntfs_x64.efi`, `iso9660_x64.efi`, `ext2_x64.efi`, `btrfs_x64.efi`, `xfs_x64.efi`) come from **pbatard/efifs v1.12** (a UEFI port of GRUB2's read-only file system drivers, GPLv3+), prebuilt artifacts from [efi.akeo.ie](https://efi.akeo.ie/downloads/efifs-1.12/x64/). Origin, SHA256 checksums and the GPLv3 source offer are recorded in [`drivers/README.md`](drivers/README.md).
- **`drivers/loop_x64.efi`** is the project's own driver (built from LoopDxe), not an external binary.

## Known limitations

- All mounts are read-only; `mount -ISO` requires the image to live on a currently accessible volume.
- Pure ISO9660 images need `drivers\iso9660_x64.efi` deployed next to mount.efi (mount sniffs and auto-loads it).
- ISOs larger than 4 GiB do not fit on a FAT32 volume; staging them on an NTFS data volume via `mount -NTFS` first is the planned end-to-end path.
- This OVMF firmware deadlocks when a global rescan hits a block device that no driver claims — the workaround is to load the matching file system driver before rescanning.

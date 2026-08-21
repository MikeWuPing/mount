# Prebuilt efifs file system drivers (x64)

These binaries are vendored deployment artifacts for the mount project. They are
loaded at runtime by `mount.efi` (`LoadImage` + `StartImage`) to add read-only
file system support to the firmware, and are intentionally committed to this
repository. DECISION (2026-08-21): source-ification of these binaries (building
efifs from source under VS2019) is DEFERRED until after the project wrap-up and
release; revisit only then.

## Origin

- Listing page: https://efi.akeo.ie/downloads/efifs-1.12/x64/
  (the `efifs-latest` symlink is not resolvable on this host; v1.12 is the
  release it points to as of the download date)
- Actual binaries: GitHub release assets at
  https://github.com/pbatard/efifs/releases/download/v1.12/<fs>_x64.efi
- Upstream project: https://github.com/pbatard/efifs
- Version: efifs v1.12
- Download date: 2026-08-20

## License and source offer

All efifs drivers are licensed GPLv3+ (GRUB2-derived read-only file system
drivers ported to UEFI). Complete corresponding source code is available at
https://github.com/pbatard/efifs (tag v1.12). This redistribution satisfies the
GPLv3 source offer by pointing to the upstream source; anyone who receives
these binaries from us may obtain the source there, or request it from us per
GPLv3 section 6.

## Files and SHA256

Verified: all files are PE32+ (magic 0x20B), machine 0x8664 (x64),
subsystem 11 (EFI boot service driver).

```
8ae24aa9f38f71a1e347fb6d0646b4678e04466aadab9919fb2ad133d5ee879c *btrfs_x64.efi
e009f02f25b9c5ad3beaf0d3a04f89042985eea1d90187b888130a708c35ca61 *ext2_x64.efi
f96b897f49aa43fdbbc6162d7e252acff36feeaad3ca798956a0cf90ede471b0 *iso9660_x64.efi
59c37d5026ca14553a158939e3f2cf20286b6135a713a62c08b569ac9caedcb7 *ntfs_x64.efi
f75d595d8037d0f5612b4eaec2d6f4a581353530a792d904c2c6988d358e07f6 *xfs_x64.efi
```

## Format mapping notes

- `ext2_x64.efi` is the GRUB ext2 module; it also covers ext3/ext4 read-only.
- UDF volumes are covered by the firmware's built-in edk2 UdfDxe (proven on
  the loop device, Task 9); no efifs UDF driver is vendored. If a target
  firmware ever lacks UdfDxe, fetch `udf_x64.efi` from the efifs release
  above and add a `mount -UDF` format-table row.

## loop_x64.efi is NOT from efifs

The `drivers\loop_x64.efi` that lands in `qemu_disk\drivers\` at build time is
the project's own LoopDxe driver (source: `MountPkg/Drivers/LoopDxe/`, built by
`tools/Build-Mount.ps1` and renamed to `loop_x64.efi`). It is the resident
loop-device factory behind `mount -ISO` and shares MountPkg's license; it is
not an efifs binary and is never committed to this directory.

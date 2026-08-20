# Prebuilt efifs file system drivers (x64)

These binaries are vendored deployment artifacts for the mount project. They are
loaded at runtime by `mount.efi` (`LoadImage` + `StartImage`) to add read-only
file system support to the firmware, and are intentionally committed to this
repository until we switch to building the drivers from source.

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
bf09c7808d9585ac03f0f61d8e5e08065ce4f2ada26d4c2671a043f3a7fe65a6 *udf_x64.efi
f75d595d8037d0f5612b4eaec2d6f4a581353530a792d904c2c6988d358e07f6 *xfs_x64.efi
```

## Format mapping notes

- `ext2_x64.efi` is the GRUB ext2 module; it also covers ext3/ext4 read-only.
- `udf_x64.efi` overlaps with edk2's UdfDxe; only one should bind a given UDF
  volume (deployment avoids loading both onto the same handle).

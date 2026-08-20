#ifndef MOUNT_FS_DRIVER_H
#define MOUNT_FS_DRIVER_H

#include <Uefi.h>

#define MOUNT_SELFTEST_OK   "SELFTEST: ALL PASS"

typedef struct {
  CONST CHAR16 *Name;        // L"NTFS" -- also the -<NAME> option
  CONST CHAR16 *DriverFile;  // L"drivers\\ntfs_x64.efi", relative to mount.efi dir
  CONST CHAR16 *Notes;       // English-only; printed by `mount` and `mount -?`
  BOOLEAN       Tested;      // updated as formats are proven in QEMU
} MOUNT_FORMAT_ENTRY;

// NULL when Name matches no table row. Case-insensitive compare.
CONST MOUNT_FORMAT_ENTRY *
MountFindFormat (
  IN CONST CHAR16  *Name
  );

#endif

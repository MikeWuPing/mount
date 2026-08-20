#ifndef MOUNT_FS_DRIVER_H
#define MOUNT_FS_DRIVER_H

#include <Uefi.h>

#define MOUNT_SELFTEST_OK   "SELFTEST: ALL PASS"

// Exit-code convention (spec s9): a literal small integer is returned to
// the Shell as the app exit status. "Driver loaded but no volume found"
// is a legitimate result and returns EFI_SUCCESS (0).
#define STATUS_INVALID_PARAMETER  ((EFI_STATUS)1)  // parse error (usage printed)
#define STATUS_DRIVER_MISSING     ((EFI_STATUS)2)  // drivers\xxx.efi not found
#define STATUS_SECURE_BOOT        ((EFI_STATUS)3)  // driver blocked by Secure Boot
#define STATUS_ISO_ERROR          ((EFI_STATUS)4)  // ISO file open/validation error

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

UINTN
MountFormatCount (
  VOID
  );

// NULL when Index is out of range.
CONST MOUNT_FORMAT_ENTRY *
MountFormatEntry (
  IN UINTN  Index
  );

// Open a DriverFile (relative to the mount.efi directory) read-only.
// Resolution goes through the single canonical self-dir resolver in
// FsDriver.c (LoadedImage->DeviceHandle + FilePath directory); the Shell
// CWD cannot be trusted (startup.nsh runs with no current directory).
EFI_STATUS
MountOpenDriverFile (
  IN  CONST CHAR16       *DriverFile,
  OUT EFI_FILE_PROTOCOL  **FileHandle
  );

// Full `mount -<FORMAT>` flow: LoadImage/StartImage the format's driver
// (deduped by loaded-image path suffix), ConnectController rescan,
// "map -r" Shell refresh, new-volume report. Returns EFI_SUCCESS even
// when no matching volume exists; STATUS_* on hard errors.
EFI_STATUS
MountRunFormat (
  IN CONST MOUNT_FORMAT_ENTRY  *Entry
  );

// Hidden `mount selftest` regression anchor (spec s9): prints
// "SELFTEST: <name> ..." per item, ends "SELFTEST: ALL PASS" or
// "SELFTEST: FAILED (<n>)". EFI_SUCCESS when all items pass,
// EFI_DEVICE_ERROR otherwise.
EFI_STATUS
MountSelfTest (
  VOID
  );

#endif

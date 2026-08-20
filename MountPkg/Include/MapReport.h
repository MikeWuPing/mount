#ifndef MOUNT_MAP_REPORT_H
#define MOUNT_MAP_REPORT_H

#include <Uefi.h>

// Snapshot all current SimpleFileSystem handles. On success *Buffer is
// pool-allocated (caller frees; NULL when no FS handle exists) and
// *Count is the handle count.
EFI_STATUS
MapSnapshotFsHandles (
  OUT EFI_HANDLE  **Buffer,
  OUT UINTN       *Count
  );

BOOLEAN
MapIsInSnapshot (
  IN EFI_HANDLE  Handle,
  IN EFI_HANDLE  *Buffer,
  IN UINTN       Count
  );

// ConnectController (recursive) on every handle: binds a newly loaded FS
// driver to every BlockIo device it supports. The driver-binding half of
// the Shell's "map -r".
EFI_STATUS
MapConnectAllControllers (
  VOID
  );

// The Shell half of "map -r": give every unmapped SimpleFileSystem
// handle the next free fsN: name via EFI_SHELL_PROTOCOL.SetMap, i.e. in
// the RUNNING shell's own map list, so new volumes stay visible to
// Shell commands after mount exits. (ShellExecute("map -r") only
// refreshes a nested shell instance whose map list dies with it.)
EFI_STATUS
MapRefreshShell (
  VOID
  );

// Print the current Shell mappings (plain "map" output). Used by the
// no-arg `mount` mode.
VOID
MapPrintCurrentMappings (
  VOID
  );

// Diff the current FS handles against a pre-connect snapshot and report
// each new volume (fsN: name + volume label).
VOID
MapPrintNewVolumes (
  IN EFI_HANDLE  *Old,
  IN UINTN       OldCount
  );

#endif

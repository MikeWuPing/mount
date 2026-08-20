#ifndef MOUNT_LOOP_DISK_H
#define MOUNT_LOOP_DISK_H

#include <Uefi.h>

// Full `mount -ISO <file>` flow: open the image, install a read-only
// 2048-byte BlockIo/BlockIo2 pair backed by the file on a fresh handle
// (device path = the file's own path + a unique vendor-media node), then
// ConnectController + Shell map refresh + new-volume report. The loop
// instance is intentionally never freed: the mount must outlive this app.
EFI_STATUS
MountRunIso (
  IN CONST CHAR16  *Path
  );

// Selftest item 3: install/read(BlockIo + BlockIo2)/uninstall roundtrip
// on a temp 16-block file under fs0:. EFI_SUCCESS when every check passes.
EFI_STATUS
LoopDiskSelfTest (
  VOID
  );

#endif

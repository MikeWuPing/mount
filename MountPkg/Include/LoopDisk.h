#ifndef MOUNT_LOOP_DISK_H
#define MOUNT_LOOP_DISK_H

#include <Uefi.h>

// Task 9 implements the loop BlockIo install + ConnectController; the
// Task 6 stub prints "not implemented" and returns EFI_SUCCESS.
EFI_STATUS
MountRunIso (
  IN CONST CHAR16  *Path
  );

#endif

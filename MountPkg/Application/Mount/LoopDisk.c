/** @file
  LoopDisk -- virtual block device over a file (Linux loop device analog).

  Stub for the MountPkg skeleton task; the EFI_BLOCK_IO_PROTOCOL
  implementation backed by EFI_FILE_PROTOCOL reads lands with mount -ISO.

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/

#include <Uefi.h>

// Global on purpose: this module builds /W4 /WX, where an unreferenced
// STATIC function is a fatal C4505. Removed when real code lands.
VOID
MountLoopDiskPad (
  VOID
  )
{
}

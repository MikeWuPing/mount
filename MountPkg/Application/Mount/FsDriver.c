/** @file
  FsDriver -- format-name -> driver-file mapping and driver loading.

  Stub for the MountPkg skeleton task; the format table and
  LoadImage/StartImage + ConnectController logic land with mount -NTFS.

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/

#include <Uefi.h>

// Global on purpose: this module builds /W4 /WX, where an unreferenced
// STATIC function is a fatal C4505. Removed when real code lands.
VOID
MountFsDriverPad (
  VOID
  )
{
}

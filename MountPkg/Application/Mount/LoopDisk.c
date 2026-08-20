/** @file
  LoopDisk -- virtual block device over a file (Linux loop device analog).

  Task 9 lands the EFI_BLOCK_IO_PROTOCOL implementation backed by
  EFI_FILE_PROTOCOL reads; for now only the MountRunIso dispatch stub.

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include "LoopDisk.h"

// Task 9 lands the loop BlockIo install + ConnectController body.
EFI_STATUS
MountRunIso (
  IN CONST CHAR16  *Path
  )
{
  Print (L"MOUNT: -ISO loop mounting not implemented yet (Task 9): %s\n", Path);
  return EFI_SUCCESS;
}

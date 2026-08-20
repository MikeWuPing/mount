/** @file
  mount -- UEFI Shell mount tool. Entry point: version stamp, arg dispatch.

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/ShellLib.h>
#include <Protocol/ShellParameters.h>
#include "Version.h"
#include "FsDriver.h"
#include "LoopDisk.h"
#include "MapReport.h"

STATIC
VOID
PrintVersion (
  VOID
  )
{
  // First stdout line AND identical serial line: the closed-loop
  // version assertion greps APP_VERSION= from both.
  Print (L"APP_VERSION=%s\n", APP_VERSION_STRING);
  DEBUG ((DEBUG_INFO, "APP_VERSION=%a\n", APP_VERSION_ASCII));
}

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  PrintVersion ();
  Print (L"mount: skeleton, CLI framework comes next\n");
  return EFI_SUCCESS;
}

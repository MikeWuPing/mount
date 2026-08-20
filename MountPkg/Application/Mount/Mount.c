/** @file
  mount -- UEFI Shell mount tool. Entry point: version stamp, CLI dispatch.

  "Option is the format name": -<FORMAT> dispatch is a MountFindFormat
  table lookup with no per-format branches. -ISO and selftest are the
  only reserved words.

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

STATIC
VOID
PrintHelp (
  VOID
  )
{
  UINTN  I;

  Print (L"MOUNT - UEFI Shell mount tool  v%s\n", APP_VERSION_STRING);
  Print (L"Author: Mike Wu\n\n");
  Print (L"Usage:\n");
  Print (L"  mount              List current fsN: mappings and supported formats\n");
  Print (L"  mount -NTFS        Load NTFS support and mount all NTFS volumes\n");
  Print (L"  mount -ISO <file>  Mount an ISO image file as a new volume\n");
  Print (L"  mount -<FORMAT>    Load support for FORMAT (EXT4/BTRFS/XFS...)\n\n");
  Print (L"Supported formats:\n");
  for (I = 0; I < MountFormatCount (); I++) {
    CONST MOUNT_FORMAT_ENTRY  *E = MountFormatEntry (I);
    Print (
      L"  %-6s %-24s %s%s\n",
      E->Name,
      E->DriverFile,
      E->Notes,
      E->Tested ? L" [tested]" : L""
      );
  }
}

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                    Status;
  EFI_SHELL_PARAMETERS_PROTOCOL *Params;
  CONST MOUNT_FORMAT_ENTRY      *Entry;
  EFI_STATUS                    CmdStatus;

  PrintVersion ();

  Status = gBS->HandleProtocol (
                  ImageHandle,
                  &gEfiShellParametersProtocolGuid,
                  (VOID **)&Params
                  );
  if (EFI_ERROR (Status)) {
    // Not launched from a UEFI Shell: no argv available.
    PrintHelp ();
    return EFI_SUCCESS;
  }

  if (Params->Argc < 2) {
    // No args: current mappings + usage + format table.
    // NOTE: this tree's ShellExecute is the 5-arg form and takes the
    // parent image handle by pointer.
    ShellExecute (&ImageHandle, L"map", FALSE, NULL, &CmdStatus);
    PrintHelp ();
    return EFI_SUCCESS;
  }

  if ((StrCmp (Params->Argv[1], L"-?") == 0) ||
      (StrCmp (Params->Argv[1], L"-h") == 0))
  {
    PrintHelp ();
    return EFI_SUCCESS;
  }

  if (StrCmp (Params->Argv[1], L"selftest") == 0) {
    return MountSelfTest ();
  }

  if (StrCmp (Params->Argv[1], L"-ISO") == 0) {
    if (Params->Argc < 3) {
      Print (L"MOUNT: error - -ISO requires a file path\n");
      return STATUS_INVALID_PARAMETER;
    }
    return MountRunIso (Params->Argv[2]);
  }

  if (Params->Argv[1][0] == L'-') {
    Entry = MountFindFormat (&Params->Argv[1][1]);
    if (Entry == NULL) {
      Print (L"MOUNT: error - unknown option/format '%s'\n", &Params->Argv[1][1]);
      PrintHelp ();
      return STATUS_INVALID_PARAMETER;
    }
    return MountRunFormat (Entry);
  }

  Print (L"MOUNT: error - unknown option '%s'\n", Params->Argv[1]);
  return STATUS_INVALID_PARAMETER;
}

/** @file
  SelfTest -- hidden `mount selftest` regression anchor (spec s9).

  Items 1-2 land here (format table integrity, driver file presence);
  Task 9 adds the loop BlockIo install/read/uninstall roundtrip.

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include "FsDriver.h"

STATIC UINTN mFailures;

STATIC
VOID
Report (
  IN CONST CHAR16  *Name,
  IN BOOLEAN       Ok
  )
{
  Print (L"SELFTEST: %-28s %s\n", Name, Ok ? L"PASS" : L"FAIL");
  if (!Ok) {
    mFailures++;
  }
}

EFI_STATUS
MountSelfTest (
  VOID
  )
{
  UINTN             I;
  UINTN             J;
  BOOLEAN           Unique;
  BOOLEAN           All;
  EFI_STATUS        Status;
  EFI_FILE_PROTOCOL *File;

  mFailures = 0;

  // 1. Format table integrity: non-null fields, unique names.
  Unique = TRUE;
  for (I = 0; I < MountFormatCount (); I++) {
    CONST MOUNT_FORMAT_ENTRY  *A = MountFormatEntry (I);
    if ((A->Name == NULL) || (A->DriverFile == NULL) || (A->Notes == NULL)) {
      Unique = FALSE;
    }
    for (J = I + 1; J < MountFormatCount (); J++) {
      if (StrCmp (A->Name, MountFormatEntry (J)->Name) == 0) {
        Unique = FALSE;
      }
    }
  }
  Report (L"format table integrity", Unique);

  // 2. Every driver file opens. Resolved relative to the mount.efi
  // directory (NOT the Shell CWD, which is unset under startup.nsh).
  All = TRUE;
  for (I = 0; I < MountFormatCount (); I++) {
    CONST MOUNT_FORMAT_ENTRY  *E = MountFormatEntry (I);
    Status = MountOpenDriverFile (E->DriverFile, &File);
    if (EFI_ERROR (Status)) {
      Print (L"SELFTEST:   missing %s\n", E->DriverFile);
      All = FALSE;
    } else {
      File->Close (File);
    }
  }
  Report (L"driver files present", All);

  // (Task 9 adds: loop BlockIo install/read/uninstall roundtrip)
  if (mFailures == 0) {
    Print (L"SELFTEST: ALL PASS\n");
    DEBUG ((DEBUG_INFO, "%a\n", MOUNT_SELFTEST_OK));
    return EFI_SUCCESS;
  }
  Print (L"SELFTEST: FAILED (%u)\n", (UINT32)mFailures);
  DEBUG ((DEBUG_INFO, "SELFTEST: FAILED (%u)\n", (UINT32)mFailures));
  return EFI_DEVICE_ERROR;
}

/** @file
  FsDriver -- format-name -> driver-file mapping, driver locate/load/connect.

  One table row per supported format; `mount -<FORMAT>` dispatch is a pure
  table lookup, so adding a format touches only this table (plus a driver
  file under drivers/), never the main flow.

  All paths that resolve a file relative to mount.efi's own directory go
  through the single canonical resolver MountGetSelfDir(): the Shell CWD
  cannot be trusted (this OVMF shell runs startup.nsh with NO current
  directory, prompt stays "Shell>").

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DevicePathLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include "FsDriver.h"
#include "MapReport.h"

#define MOUNT_MAX_PATH  256

// Notes are user-visible (mount / mount -?): English only (req 9).
// NTFS Tested=TRUE per the Task 4 Phase 0 QEMU evidence.
STATIC CONST MOUNT_FORMAT_ENTRY mFormats[] = {
  { L"NTFS",  L"drivers\\ntfs_x64.efi",  L"efifs, read-only",                      TRUE  },
  { L"EXT4",  L"drivers\\ext2_x64.efi",  L"efifs ext2 covers ext2/3/4, read-only", FALSE },
  { L"BTRFS", L"drivers\\btrfs_x64.efi", L"efifs, read-only; RAID/zstd may fail",  FALSE },
  { L"XFS",   L"drivers\\xfs_x64.efi",   L"efifs, read-only; v5/CRC TBD",          FALSE },
};

#define MOUNT_FORMAT_COUNT  (sizeof (mFormats) / sizeof (mFormats[0]))

UINTN
MountFormatCount (
  VOID
  )
{
  return (UINTN)MOUNT_FORMAT_COUNT;
}

CONST MOUNT_FORMAT_ENTRY *
MountFormatEntry (
  IN UINTN  Index
  )
{
  return (Index < MOUNT_FORMAT_COUNT) ? &mFormats[Index] : NULL;
}

// ASCII-only case-insensitive compare; format names/options are short ASCII.
STATIC
INTN
StaticStriCmp (
  IN CONST CHAR16  *A,
  IN CONST CHAR16  *B
  )
{
  CHAR16  Ca;
  CHAR16  Cb;

  do {
    Ca = *A++;
    Cb = *B++;
    if ((Ca >= L'a') && (Ca <= L'z')) {
      Ca = (CHAR16)(Ca - L'a' + L'A');
    }
    if ((Cb >= L'a') && (Cb <= L'z')) {
      Cb = (CHAR16)(Cb - L'a' + L'A');
    }
    if (Ca != Cb) {
      return (INTN)Ca - (INTN)Cb;
    }
  } while (Ca != L'\0');
  return 0;
}

CONST MOUNT_FORMAT_ENTRY *
MountFindFormat (
  IN CONST CHAR16  *Name
  )
{
  UINTN  Index;

  if (Name == NULL) {
    return NULL;
  }
  for (Index = 0; Index < MOUNT_FORMAT_COUNT; Index++) {
    if (StaticStriCmp (mFormats[Index].Name, Name) == 0) {
      return &mFormats[Index];
    }
  }
  return NULL;
}

// The one canonical self-location resolver. Returns the volume handle
// mount.efi was loaded from (LoadedImage->DeviceHandle: for a file-loaded
// image this is exactly the SimpleFileSystem device) plus the image's
// directory relative to that volume root: L"" when mount.efi sits at the
// root, else L"\sub\dir" (leading backslash, no trailing backslash).
// MountOpenDriverFile() and MountBuildDriverDevicePath() both build on
// this; there is deliberately no second parser.
STATIC
EFI_STATUS
MountGetSelfDir (
  OUT EFI_HANDLE  *VolHandle,
  OUT CHAR16      *DirPath   // size MOUNT_MAX_PATH; L"" at volume root
  )
{
  EFI_STATUS                 Status;
  EFI_LOADED_IMAGE_PROTOCOL  *LoadedImage;
  EFI_DEVICE_PATH_PROTOCOL   *Node;
  CONST CHAR16               *ImagePath;
  CONST CHAR16               *P;
  CONST CHAR16               *LastSlash;

  Status = gBS->HandleProtocol (
                  gImageHandle,
                  &gEfiLoadedImageProtocolGuid,
                  (VOID **)&LoadedImage
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  *VolHandle = LoadedImage->DeviceHandle;
  DirPath[0] = L'\0';
  if (LoadedImage->FilePath == NULL) {
    return EFI_NOT_FOUND;
  }

  // Last FILEPATH node of the image path, e.g. "\mount.efi".
  ImagePath = NULL;
  for (Node = LoadedImage->FilePath;
       !IsDevicePathEnd (Node);
       Node = NextDevicePathNode (Node))
  {
    if ((DevicePathType (Node) == MEDIA_DEVICE_PATH) &&
        (DevicePathSubType (Node) == MEDIA_FILEPATH_DP))
    {
      ImagePath = ((FILEPATH_DEVICE_PATH *)Node)->PathName;
    }
  }
  if (ImagePath == NULL) {
    return EFI_NOT_FOUND;
  }

  // Strip the last path component -> directory: "\mount.efi" -> L"",
  // "\tools\mount.efi" -> L"\tools".
  LastSlash = NULL;
  for (P = ImagePath; *P != L'\0'; P++) {
    if (*P == L'\\') {
      LastSlash = P;
    }
  }
  if (LastSlash == NULL) {
    return EFI_NOT_FOUND;
  }
  StrnCpyS (DirPath, MOUNT_MAX_PATH, ImagePath, (UINTN)(LastSlash - ImagePath));
  return EFI_SUCCESS;
}

// Join the self directory with a mount.efi-relative file path:
// (L"", L"drivers\x.efi") -> L"drivers\x.efi" (root-relative open),
// (L"\t", L"drivers\x.efi") -> L"\t\drivers\x.efi".
STATIC
VOID
MountJoinSelfPath (
  IN  CONST CHAR16  *SelfDir,
  IN  CONST CHAR16  *Relative,
  OUT CHAR16        *FullPath  // size MOUNT_MAX_PATH
  )
{
  if (SelfDir[0] == L'\0') {
    StrnCpyS (FullPath, MOUNT_MAX_PATH, Relative, MOUNT_MAX_PATH - 1);
  } else {
    UnicodeSPrint (FullPath, MOUNT_MAX_PATH * sizeof (CHAR16), L"%s\\%s", SelfDir, Relative);
  }
}

// Open a DriverFile (relative to the mount.efi directory) read-only,
// resolved via MountGetSelfDir() against the volume mount.efi was loaded
// from. Used by the selftest driver-presence check.
EFI_STATUS
MountOpenDriverFile (
  IN  CONST CHAR16       *DriverFile,
  OUT EFI_FILE_PROTOCOL  **FileHandle
  )
{
  EFI_STATUS                       Status;
  EFI_HANDLE                       VolHandle;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Sfs;
  EFI_FILE_PROTOCOL                *Root;
  CHAR16                           Dir[MOUNT_MAX_PATH];
  CHAR16                           Full[MOUNT_MAX_PATH];

  if ((DriverFile == NULL) || (FileHandle == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  *FileHandle = NULL;

  Status = MountGetSelfDir (&VolHandle, Dir);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = gBS->HandleProtocol (
                  VolHandle,
                  &gEfiSimpleFileSystemProtocolGuid,
                  (VOID **)&Sfs
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  MountJoinSelfPath (Dir, DriverFile, Full);

  Status = Sfs->OpenVolume (Sfs, &Root);
  if (!EFI_ERROR (Status)) {
    Status = Root->Open (Root, FileHandle, Full, EFI_FILE_MODE_READ, 0);
    Root->Close (Root);
  }
  return Status;
}

// Build a full device path (volume device path + filepath node) for a
// DriverFile relative to the mount.efi directory, for LoadImage().
STATIC
EFI_STATUS
MountBuildDriverDevicePath (
  IN  CONST CHAR16              *DriverFile,
  OUT EFI_DEVICE_PATH_PROTOCOL  **DevicePath
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  VolHandle;
  CHAR16      Dir[MOUNT_MAX_PATH];
  CHAR16      Full[MOUNT_MAX_PATH];

  Status = MountGetSelfDir (&VolHandle, Dir);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  MountJoinSelfPath (Dir, DriverFile, Full);
  *DevicePath = FileDevicePath (VolHandle, Full);
  return (*DevicePath != NULL) ? EFI_SUCCESS : EFI_OUT_OF_RESOURCES;
}

// Dedupe: any loaded image whose FilePath device path text ends with the
// driver's relative path counts as already loaded (exact case: the table
// built every path this flow loads). Exported for LoopDisk's ISO9660
// missing-driver warning.
BOOLEAN
MountDriverLoaded (
  IN CONST CHAR16  *DriverFile
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *Handles;
  UINTN       Count;
  UINTN       Index;
  BOOLEAN     Found;

  Found = FALSE;
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiLoadedImageProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return FALSE;
  }
  for (Index = 0; Index < Count && !Found; Index++) {
    EFI_LOADED_IMAGE_PROTOCOL  *Li;
    CHAR16                     *Text;
    UINTN                      TextLen;
    UINTN                      FileLen;

    if (EFI_ERROR (gBS->HandleProtocol (
                          Handles[Index],
                          &gEfiLoadedImageProtocolGuid,
                          (VOID **)&Li
                          )) ||
        (Li->FilePath == NULL))
    {
      continue;
    }
    Text = ConvertDevicePathToText (Li->FilePath, FALSE, FALSE);
    if (Text != NULL) {
      TextLen = StrLen (Text);
      FileLen = StrLen (DriverFile);
      // Suffix match, but only on a path-component boundary: the match
      // must be the whole text or be preceded by a backslash, so
      // "\xdrivers\ntfs_x64.efi" does not false-positive.
      if ((TextLen >= FileLen) &&
          (StrCmp (Text + TextLen - FileLen, DriverFile) == 0) &&
          ((TextLen == FileLen) || (Text[TextLen - FileLen - 1] == L'\\')))
      {
        Found = TRUE;
      }
      FreePool (Text);
    }
  }
  FreePool (Handles);
  return Found;
}

// Full `mount -<FORMAT>` flow: load the format's driver (once), reconnect
// controllers so it binds every matching BlockIo device, refresh the
// Shell mapping table, and report the volumes that appeared. "Driver
// loaded but no volume found" is a legitimate result (EFI_SUCCESS).
EFI_STATUS
MountRunFormat (
  IN CONST MOUNT_FORMAT_ENTRY  *Entry
  )
{
  EFI_STATUS                Status;
  EFI_DEVICE_PATH_PROTOCOL  *Dp;
  EFI_HANDLE                Image;
  EFI_HANDLE                *OldFs;
  UINTN                     OldCount;

  if (Entry == NULL) {
    return STATUS_INVALID_PARAMETER;
  }

  // Snapshot BEFORE load/connect: the report is the diff against this.
  OldFs    = NULL;
  OldCount = 0;
  MapSnapshotFsHandles (&OldFs, &OldCount);

  if (MountDriverLoaded (Entry->DriverFile)) {
    Print (L"MOUNT: %s driver already loaded\n", Entry->Name);
    DEBUG ((DEBUG_INFO, "MOUNT: %S driver already loaded\n", Entry->Name));
  } else {
    Status = MountBuildDriverDevicePath (Entry->DriverFile, &Dp);
    if (EFI_ERROR (Status)) {
      Print (L"MOUNT: error - cannot locate self directory (%r)\n", Status);
      if (OldFs != NULL) {
        FreePool (OldFs);
      }
      return STATUS_DRIVER_MISSING;
    }
    Status = gBS->LoadImage (FALSE, gImageHandle, Dp, NULL, 0, &Image);
    FreePool (Dp);
    if ((Status == EFI_NOT_FOUND) ||
        (Status == EFI_LOAD_ERROR) ||
        (Status == EFI_INVALID_PARAMETER))
    {
      Print (L"MOUNT: error - %s not found (get it from efi.akeo.ie)\n", Entry->DriverFile);
      DEBUG ((DEBUG_INFO, "MOUNT: driver file missing\n"));
      if (OldFs != NULL) {
        FreePool (OldFs);
      }
      return STATUS_DRIVER_MISSING;
    }
    if (Status == EFI_SECURITY_VIOLATION) {
      Print (L"MOUNT: error - driver blocked by Secure Boot\n");
      DEBUG ((DEBUG_INFO, "MOUNT: driver blocked by Secure Boot\n"));
      if (OldFs != NULL) {
        FreePool (OldFs);
      }
      return STATUS_SECURE_BOOT;
    }
    if (EFI_ERROR (Status)) {
      Print (L"MOUNT: error - LoadImage %s failed (%r)\n", Entry->DriverFile, Status);
      if (OldFs != NULL) {
        FreePool (OldFs);
      }
      return STATUS_DRIVER_MISSING;
    }
    // A driver image returns from StartImage once its entry point has
    // registered DriverBinding; it does not "run" like an app.
    Status = gBS->StartImage (Image, NULL, NULL);
    if (EFI_ERROR (Status)) {
      Print (L"MOUNT: error - StartImage failed (%r)\n", Status);
      DEBUG ((DEBUG_INFO, "MOUNT: StartImage failed (%r)\n", Status));
      // Do not leave a resident image whose DriverBinding never started.
      gBS->UnloadImage (Image);
      if (OldFs != NULL) {
        FreePool (OldFs);
      }
      return STATUS_DRIVER_MISSING;
    }
    Print (L"MOUNT: driver %s loaded\n", Entry->DriverFile);
    DEBUG ((DEBUG_INFO, "MOUNT: driver %S loaded\n", Entry->DriverFile));
  }

  MapConnectAllControllers ();
  MapRefreshShell ();
  MapPrintNewVolumes (OldFs, OldCount);
  if (OldFs != NULL) {
    FreePool (OldFs);
  }
  DEBUG ((DEBUG_INFO, "MOUNT: -%S flow done\n", Entry->Name));
  return EFI_SUCCESS;
}
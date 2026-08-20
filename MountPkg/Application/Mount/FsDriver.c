/** @file
  FsDriver -- format-name -> driver-file mapping table and lookup.

  One table row per supported format; `mount -<FORMAT>` dispatch is a
  pure table lookup, so adding a format touches only this table (plus a
  driver file under drivers/), never the main flow.

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include "FsDriver.h"

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

// Resolve a DriverFile (relative to the mount.efi directory) against the
// volume mount.efi was loaded from, and open it read-only. The Shell CWD
// is unreliable for this: this OVMF shell runs startup.nsh with NO current
// directory (prompt stays "Shell>"), so CWD-relative opens fail. Task 7
// uses the same resolution for LoadImage.
EFI_STATUS
MountOpenDriverFile (
  IN  CONST CHAR16       *DriverFile,
  OUT EFI_FILE_PROTOCOL  **FileHandle
  )
{
  EFI_STATUS                       Status;
  EFI_LOADED_IMAGE_PROTOCOL        *LoadedImage;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Sfs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_DEVICE_PATH_PROTOCOL         *Node;
  CONST CHAR16                     *ImagePath;
  CONST CHAR16                     *P;
  CONST CHAR16                     *LastSlash;
  UINTN                            DirLen;
  CHAR16                           *FullPath;

  if ((DriverFile == NULL) || (FileHandle == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  *FileHandle = NULL;

  Status = gBS->HandleProtocol (
                  gImageHandle,
                  &gEfiLoadedImageProtocolGuid,
                  (VOID **)&LoadedImage
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = gBS->HandleProtocol (
                  LoadedImage->DeviceHandle,
                  &gEfiSimpleFileSystemProtocolGuid,
                  (VOID **)&Sfs
                  );
  if (EFI_ERROR (Status)) {
    return Status;
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

  // Directory prefix of the image: everything up to and including the
  // last backslash ("\mount.efi" -> "\", "\tools\mount.efi" -> "\tools\").
  LastSlash = NULL;
  for (P = ImagePath; *P != L'\0'; P++) {
    if (*P == L'\\') {
      LastSlash = P;
    }
  }
  DirLen = (LastSlash == NULL) ? 0 : (UINTN)(LastSlash - ImagePath + 1);

  FullPath = AllocatePool ((DirLen + StrLen (DriverFile) + 1) * sizeof (CHAR16));
  if (FullPath == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  if (DirLen > 0) {
    CopyMem (FullPath, ImagePath, DirLen * sizeof (CHAR16));
  }
  StrCpyS (FullPath + DirLen, StrLen (DriverFile) + 1, DriverFile);

  Status = Sfs->OpenVolume (Sfs, &Root);
  if (!EFI_ERROR (Status)) {
    Status = Root->Open (Root, FileHandle, FullPath, EFI_FILE_MODE_READ, 0);
    Root->Close (Root);
  }
  FreePool (FullPath);
  return Status;
}

// Task 7 lands the real LoadImage/StartImage + ConnectController body.
EFI_STATUS
MountRunFormat (
  IN CONST MOUNT_FORMAT_ENTRY  *Entry
  )
{
  Print (L"MOUNT: -%s driver loading not implemented yet (Task 7)\n", Entry->Name);
  return EFI_SUCCESS;
}

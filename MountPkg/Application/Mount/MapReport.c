/** @file
  MapReport -- controller rescan, Shell mapping refresh ("map -r"
  equivalent) and the English new-volume report.

  The Shell's `map -r` is two halves: ConnectController on all handles so
  drivers bind (MapConnectAllControllers), then a refresh of the Shell's
  fsN: mapping table (MapRefreshShell). The second half CANNOT be done
  with ShellExecute("map -r") on this shell: nesting is enabled, so the
  command runs in a nested Shell.efi instance whose map list dies with
  it (observed: the nested instance prints its own banner + table, and
  the outer shell never gains fs1:). EFI_SHELL_PROTOCOL.SetMap() belongs
  to the OUTER shell instance, so mappings made through it survive
  mount's exit -- the acceptance criterion.

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
#include <Library/ShellLib.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/BlockIo.h>
#include <Protocol/Shell.h>
#include <Guid/FileSystemVolumeLabelInfo.h>
#include "MapReport.h"

#define MOUNT_LABEL_MAX  64
#define MOUNT_NAME_MAX   32

EFI_STATUS
MapSnapshotFsHandles (
  OUT EFI_HANDLE  **Buffer,
  OUT UINTN       *Count
  )
{
  EFI_STATUS  Status;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  Count,
                  Buffer
                  );
  if (Status == EFI_NOT_FOUND) {
    // No FS handle at all: an empty snapshot, not an error.
    *Buffer = NULL;
    *Count  = 0;
    return EFI_SUCCESS;
  }
  return Status;
}

BOOLEAN
MapIsInSnapshot (
  IN EFI_HANDLE  Handle,
  IN EFI_HANDLE  *Buffer,
  IN UINTN       Count
  )
{
  UINTN  Index;

  for (Index = 0; Index < Count; Index++) {
    if (Buffer[Index] == Handle) {
      return TRUE;
    }
  }
  return FALSE;
}

EFI_STATUS
MapConnectAllControllers (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *Handles;
  UINTN       Count;
  UINTN       Index;
  UINTN       Connected;

  // Scope: handles that have BlockIo but no SimpleFileSystem yet -- the
  // exact set a newly loaded FS driver can produce volumes on. Do NOT
  // ConnectController(AllHandles): a recursive connect of every handle
  // (PCI roots, consoles, serial, the boot disk's ATA stack) leaves this
  // OVMF shell unable to read the next startup.nsh line after mount
  // exits (observed hang; serial just stops). Handle count is small.
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiBlockIoProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (Status == EFI_NOT_FOUND) {
    return EFI_SUCCESS;
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Connected = 0;
  for (Index = 0; Index < Count; Index++) {
    VOID  *Sfs;

    if (!EFI_ERROR (gBS->HandleProtocol (
                           Handles[Index],
                           &gEfiSimpleFileSystemProtocolGuid,
                           &Sfs
                           )))
    {
      continue;  // already has a filesystem
    }
    // Recursive: bind PartitionDxe children first, then the FS driver
    // on those. EFI_ALREADY_STARTED / no-match are expected; ignore.
    if (!EFI_ERROR (gBS->ConnectController (Handles[Index], NULL, NULL, TRUE))) {
      Connected++;
    }
  }
  FreePool (Handles);
  DEBUG ((DEBUG_INFO, "MOUNT: ConnectController rescan done (%u of %u blk handles)\n",
          (UINT32)Connected, (UINT32)Count));
  return EFI_SUCCESS;
}

// The Shell returns EVERY alias of a device path, semicolon-separated
// (e.g. L"BLK2:;fs1:"). Both the refresh and the report care about the
// fs-style alias. Returns a pointer INTO MapNames (not pool memory) or
// NULL when no fs-style alias exists.
//
// OWNERSHIP WARNING (this tree, verified empirically): EFI_SHELL_PROTOCOL
// string returns go through AddBufferToFreeList() -- the shell itself
// frees them at the next command boundary. Callers must NOT FreePool
// them (double free -> pool corruption -> the shell hangs right after
// mount exits). The MdePkg header comment "should be freed by the
// caller" does not match this implementation.
STATIC
CONST CHAR16 *
MapFindFsAlias (
  IN CONST CHAR16  *MapNames
  )
{
  CONST CHAR16  *Tok;

  if (MapNames == NULL) {
    return NULL;
  }
  for (Tok = MapNames; *Tok != L'\0'; Tok++) {
    if (((Tok == MapNames) || (Tok[-1] == L';')) &&
        ((*Tok == L'f') || (*Tok == L'F')) &&
        ((Tok[1] == L's') || (Tok[1] == L'S')))
    {
      return Tok;
    }
  }
  return NULL;
}

// Copy one alias token (up to ';' or end) into Name.
STATIC
VOID
MapCopyAlias (
  IN CONST CHAR16  *Alias,
  OUT CHAR16       *Name,
  IN UINTN         NameSize
  )
{
  UINTN  Len;

  Len = 0;
  while ((Alias[Len] != L'\0') && (Alias[Len] != L';') && (Len < NameSize - 1)) {
    Len++;
  }
  StrnCpyS (Name, NameSize, Alias, Len);
}

EFI_STATUS
MapRefreshShell (
  VOID
  )
{
  EFI_STATUS          Status;
  EFI_SHELL_PROTOCOL  *Shell;
  EFI_HANDLE          *Handles;
  UINTN               Count;
  UINTN               Index;
  UINTN               Assigned;

  // See the file header: SetMap (outer shell's list), not ShellExecute.
  Status = gBS->LocateProtocol (&gEfiShellProtocolGuid, NULL, (VOID **)&Shell);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "MOUNT: no Shell protocol, mapping refresh skipped\n"));
    return Status;
  }
  Status = MapSnapshotFsHandles (&Handles, &Count);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Assigned = 0;
  for (Index = 0; Index < Count; Index++) {
    EFI_DEVICE_PATH_PROTOCOL  *Dp;
    EFI_DEVICE_PATH_PROTOCOL  *DpWalk;
    CONST CHAR16              *Names;
    UINTN                     Fs;
    CHAR16                    NewName[16];

    Dp = DevicePathFromHandle (Handles[Index]);
    if (Dp == NULL) {
      continue;
    }
    // Already has an fsN: name? (GetMapFromDevicePath advances the
    // passed pointer -- hand it a walk copy; the path itself is const.
    // The returned string is shell-owned: do NOT FreePool it.)
    DpWalk = Dp;
    Names  = Shell->GetMapFromDevicePath (&DpWalk);
    if (MapFindFsAlias (Names) != NULL) {
      continue;
    }
    // First free fs index. Map lookup is case-insensitive (StriColl),
    // so the stored "FS0:" blocks "fs0:" -- no clobbering.
    for (Fs = 0; ; Fs++) {
      UnicodeSPrint (NewName, sizeof (NewName), L"fs%u:", Fs);
      if (Shell->GetDevicePathFromMap (NewName) == NULL) {
        break;
      }
    }
    Status = Shell->SetMap (Dp, NewName);
    if (!EFI_ERROR (Status)) {
      Assigned++;
      DEBUG ((DEBUG_INFO, "MOUNT: shell mapping %S assigned\n", NewName));
    }
  }
  if (Handles != NULL) {
    FreePool (Handles);
  }
  DEBUG ((DEBUG_INFO, "MOUNT: map refreshed, %u new mapping(s)\n", (UINT32)Assigned));
  return EFI_SUCCESS;
}

VOID
MapPrintCurrentMappings (
  VOID
  )
{
  EFI_STATUS  Status;

  // A nested shell rebuilds its map list from live handles at startup,
  // so its "map" output reflects the CURRENT volumes. Fine for display.
  Status = EFI_SUCCESS;
  ShellExecute (&gImageHandle, L"map", FALSE, NULL, &Status);
}

VOID
MapPrintNewVolumes (
  IN EFI_HANDLE  *Old,
  IN UINTN       OldCount
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *Now;
  UINTN       NowCount;
  UINTN       Index;
  UINTN       NewCount;

  NewCount = 0;
  Status   = MapSnapshotFsHandles (&Now, &NowCount);
  if (EFI_ERROR (Status)) {
    return;
  }
  for (Index = 0; Index < NowCount; Index++) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Sfs;
    EFI_FILE_PROTOCOL                *Root;
    EFI_SHELL_PROTOCOL               *Shell;
    EFI_DEVICE_PATH_PROTOCOL         *Dp;
    EFI_DEVICE_PATH_PROTOCOL         *DpWalk;
    CONST CHAR16                     *MapName;
    CONST CHAR16                     *FsAlias;
    CHAR16                           Name[MOUNT_NAME_MAX];
    CHAR16                           Label[MOUNT_LABEL_MAX];

    if (MapIsInSnapshot (Now[Index], Old, OldCount)) {
      continue;
    }
    NewCount++;

    // fsN: name via the Shell protocol (MapRefreshShell already ran, so
    // a new volume has one; older blk-style aliases may be returned
    // too -- prefer the fs alias for display).
    StrCpyS (Name, MOUNT_NAME_MAX, L"?");
    if (!EFI_ERROR (gBS->LocateProtocol (
                           &gEfiShellProtocolGuid,
                           NULL,
                           (VOID **)&Shell
                           )))
    {
      Dp = DevicePathFromHandle (Now[Index]);
      if (Dp != NULL) {
        DpWalk  = Dp;
        MapName = Shell->GetMapFromDevicePath (&DpWalk);
        if (MapName != NULL) {
          // MapName is shell-owned (BufferToFreeList): no FreePool.
          FsAlias = MapFindFsAlias (MapName);
          if (FsAlias != NULL) {
            MapCopyAlias (FsAlias, Name, MOUNT_NAME_MAX);
          } else {
            MapCopyAlias (MapName, Name, MOUNT_NAME_MAX);
          }
        }
      }
    }

    // Volume label via SFS OpenVolume + GetInfo; "-" when unavailable.
    StrCpyS (Label, MOUNT_LABEL_MAX, L"-");
    if (!EFI_ERROR (gBS->HandleProtocol (
                           Now[Index],
                           &gEfiSimpleFileSystemProtocolGuid,
                           (VOID **)&Sfs
                           )) &&
        !EFI_ERROR (Sfs->OpenVolume (Sfs, &Root)))
    {
      UINTN                        Size;
      EFI_FILE_SYSTEM_VOLUME_LABEL *Info;

      Size = sizeof (EFI_FILE_SYSTEM_VOLUME_LABEL) + sizeof (Label);
      Info = AllocatePool (Size);
      if (Info != NULL) {
        if (!EFI_ERROR (Root->GetInfo (
                               Root,
                               &gEfiFileSystemVolumeLabelInfoIdGuid,
                               &Size,
                               Info
                               )))
        {
          StrnCpyS (Label, MOUNT_LABEL_MAX, Info->VolumeLabel, MOUNT_LABEL_MAX - 1);
        }
        FreePool (Info);
      }
      Root->Close (Root);
    }

    Print (L"MOUNT: new volume %s  label=\"%s\"\n", Name, Label);
    DEBUG ((DEBUG_INFO, "MOUNT: new volume %S label=\"%S\"\n", Name, Label));
  }
  if (Now != NULL) {
    FreePool (Now);
  }
  Print (L"MOUNT: map refreshed, %d new volume(s)\n", NewCount);
  DEBUG ((DEBUG_INFO, "MOUNT: map refreshed, %d new volume(s)\n", NewCount));
}

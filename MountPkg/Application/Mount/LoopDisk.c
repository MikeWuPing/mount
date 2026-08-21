/** @file
  LoopDisk (application side) -- `mount -ISO <file>` orchestration.

  The loop device itself lives in the resident LoopDxe driver
  (drivers\loop_x64.efi, MOUNT_LOOP_FACTORY_PROTOCOL); this file only
  validates the image, resolves its device path, makes sure the factory
  driver is loaded, and asks it to create the device. Splitting it this
  way is what lets a mount survive mount.efi's exit: an application's
  protocol callbacks die with its image (Task 9 measured the #UD), a
  boot-service driver's do not.

  Path resolution goes through EFI_SHELL_PROTOCOL.GetDevicePathFromFilePath
  -- the same resolver ShellOpenFileByName uses internally -- so both
  "fsN:\dir\a.iso" and CWD-relative forms resolve identically for the open
  and for the device path.

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DevicePathLib.h>
#include <Library/ShellLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/BlockIo2.h>
#include <Protocol/Shell.h>
#include <Protocol/MountLoopFactory.h>
#include <Guid/FileInfo.h>
#include "LoopDisk.h"
#include "FsDriver.h"
#include "MapReport.h"

#define LOOP_BLOCK_SIZE   2048
#define LOOP_MIN_SIZE     (16 * LOOP_BLOCK_SIZE)  // PVD at LBA16 must fit
#define LOOP_DRIVER_FILE  L"drivers\\loop_x64.efi"

// ---------------------------------------------------------------------------
// Factory locate/load
// ---------------------------------------------------------------------------

// Locate the resident loop factory, loading drivers\loop_x64.efi (relative
// to the mount.efi directory) when no instance exists yet. Dedupe is by
// protocol GUID, not file path: a second `mount -ISO` reuses the resident
// driver, and each of its loop devices gets a unique vendor node from the
// driver-global sequence counter.
STATIC
EFI_STATUS
MountLoopFactoryGet (
  OUT MOUNT_LOOP_FACTORY_PROTOCOL  **Factory
  )
{
  EFI_STATUS                Status;
  EFI_DEVICE_PATH_PROTOCOL  *Dp;
  EFI_HANDLE                Image;

  Status = gBS->LocateProtocol (
                  &gMountLoopFactoryProtocolGuid,
                  NULL,
                  (VOID **)Factory
                  );
  if (!EFI_ERROR (Status)) {
    return EFI_SUCCESS;
  }

  Status = MountBuildDriverDevicePath (LOOP_DRIVER_FILE, &Dp);
  if (EFI_ERROR (Status)) {
    Print (L"MOUNT: error - cannot locate self directory (%r)\n", Status);
    return Status;
  }
  Status = gBS->LoadImage (FALSE, gImageHandle, Dp, NULL, 0, &Image);
  FreePool (Dp);
  if (EFI_ERROR (Status)) {
    if (Status == EFI_SECURITY_VIOLATION) {
      Print (L"MOUNT: error - %s blocked by Secure Boot\n", LOOP_DRIVER_FILE);
      return STATUS_SECURE_BOOT;
    }
    Print (L"MOUNT: error - cannot load %s (%r)\n", LOOP_DRIVER_FILE, Status);
    DEBUG ((DEBUG_INFO, "MOUNT: LoadImage %S failed (%r)\n", LOOP_DRIVER_FILE, Status));
    return STATUS_DRIVER_MISSING;
  }
  // A driver image returns from StartImage once its entry point has
  // installed the factory; it stays resident, unlike an application.
  Status = gBS->StartImage (Image, NULL, NULL);
  if (EFI_ERROR (Status)) {
    Print (L"MOUNT: error - StartImage %s failed (%r)\n", LOOP_DRIVER_FILE, Status);
    gBS->UnloadImage (Image);
    return STATUS_DRIVER_MISSING;
  }
  DEBUG ((DEBUG_INFO, "MOUNT: driver %S loaded\n", LOOP_DRIVER_FILE));

  Status = gBS->LocateProtocol (
                  &gMountLoopFactoryProtocolGuid,
                  NULL,
                  (VOID **)Factory
                  );
  if (EFI_ERROR (Status)) {
    Print (L"MOUNT: error - %s published no loop factory\n", LOOP_DRIVER_FILE);
    return STATUS_DRIVER_MISSING;
  }
  return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// Image validation helpers (unchanged from the app-resident design)
// ---------------------------------------------------------------------------

// Sniff the image: ISO9660 PVD "CD001" at sector 16 +1; UDF bridge
// descriptors "BEA01"/"NSR02"/"NSR03" at sector 16..19 +1. Advisory only
// (spec section 8): the result never blocks the mount, it only drives the
// missing-driver warning. File position is restored to 0 in all paths.
STATIC
VOID
LoopSniff (
  IN EFI_FILE_PROTOCOL  *File,
  OUT BOOLEAN           *Iso9660,
  OUT BOOLEAN           *Udf
  )
{
  UINT8  Buf[6 * LOOP_BLOCK_SIZE];
  UINTN  Size = sizeof (Buf);

  *Iso9660 = FALSE;
  *Udf     = FALSE;
  if (EFI_ERROR (File->SetPosition (File, 16 * LOOP_BLOCK_SIZE)) ||
      EFI_ERROR (File->Read (File, &Size, Buf)) || (Size != sizeof (Buf)))
  {
    File->SetPosition (File, 0);
    return;
  }
  File->SetPosition (File, 0);

  if (CompareMem (Buf + 1, "CD001", 5) == 0) {
    *Iso9660 = TRUE;
  }
  if ((CompareMem (Buf + 1, "BEA01", 5) == 0) ||          // ECMA-167 VRS @16
      (CompareMem (Buf + 2048 + 1, "BEA01", 5) == 0) ||
      (CompareMem (Buf + 2 * 2048 + 1, "NSR02", 5) == 0) ||
      (CompareMem (Buf + 2 * 2048 + 1, "NSR03", 5) == 0) ||
      (CompareMem (Buf + 3 * 2048 + 1, "NSR02", 5) == 0) ||
      (CompareMem (Buf + 3 * 2048 + 1, "NSR03", 5) == 0))
  {
    *Udf = TRUE;
  }
}

// Full device path of IsoPath (volume path + filepath node), pool-allocated
// (caller frees). Uses the Shell's own file-path resolver, so the fsN: and
// CWD-relative forms behave exactly like ShellOpenFileByName. NULL when the
// path cannot be resolved (bad map name, or relative path with no CWD).
STATIC
EFI_DEVICE_PATH_PROTOCOL *
MountIsoFileDevicePath (
  IN CONST CHAR16  *IsoPath
  )
{
  EFI_STATUS          Status;
  EFI_SHELL_PROTOCOL  *Shell;

  Status = gBS->LocateProtocol (&gEfiShellProtocolGuid, NULL, (VOID **)&Shell);
  if (EFI_ERROR (Status)) {
    return NULL;
  }
  // Callee-allocated (DuplicateDevicePath/FileDevicePath inside the Shell),
  // NOT a BufferToFreeList string: the caller owns and frees it.
  return Shell->GetDevicePathFromFilePath (IsoPath);
}

// Validate IsoPath and ask the resident factory to create the loop device
// over it. On success *LoopHandle owns the mount (the factory owns the open
// backing file) and *Iso9660/*Udf carry the content sniff; on failure
// everything touched here is closed/freed.
STATIC
EFI_STATUS
LoopCreate (
  IN CONST CHAR16  *IsoPath,
  OUT EFI_HANDLE   *LoopHandle,
  OUT BOOLEAN      *Iso9660,
  OUT BOOLEAN      *Udf
  )
{
  EFI_STATUS                  Status;
  SHELL_FILE_HANDLE           File;
  EFI_FILE_INFO               *Info;
  UINTN                       InfoSize;
  EFI_DEVICE_PATH_PROTOCOL    *FileDp;
  MOUNT_LOOP_FACTORY_PROTOCOL *Factory;
  UINT64                      FileSize;

  *LoopHandle = NULL;
  *Iso9660    = FALSE;
  *Udf        = FALSE;

  Status = ShellOpenFileByName (IsoPath, &File, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    Print (L"MOUNT: error - cannot open %s (%r)\n", IsoPath, Status);
    return STATUS_ISO_ERROR;
  }

  InfoSize = 0;
  ((EFI_FILE_PROTOCOL *)File)->GetInfo (
                                 (EFI_FILE_PROTOCOL *)File,
                                 &gEfiFileInfoGuid,
                                 &InfoSize,
                                 NULL
                                 );
  Info = AllocatePool (InfoSize);
  if (Info == NULL) {
    ShellCloseFile (&File);
    return EFI_OUT_OF_RESOURCES;
  }
  Status = ((EFI_FILE_PROTOCOL *)File)->GetInfo (
                                          (EFI_FILE_PROTOCOL *)File,
                                          &gEfiFileInfoGuid,
                                          &InfoSize,
                                          Info
                                          );
  if (EFI_ERROR (Status)) {
    Print (L"MOUNT: error - cannot stat %s\n", IsoPath);
    FreePool (Info);
    ShellCloseFile (&File);
    return STATUS_ISO_ERROR;
  }
  FileSize = Info->FileSize;
  FreePool (Info);

  if (FileSize < LOOP_MIN_SIZE) {
    Print (L"MOUNT: error - file too small, not a valid ISO\n");
    ShellCloseFile (&File);
    return STATUS_ISO_ERROR;
  }
  if ((FileSize % LOOP_BLOCK_SIZE) != 0) {
    Print (L"MOUNT: warn - size not multiple of 2048, tail ignored\n");
  }

  LoopSniff ((EFI_FILE_PROTOCOL *)File, Iso9660, Udf);
  DEBUG ((DEBUG_INFO, "MOUNT: sniff iso9660=%d udf=%d\n", *Iso9660, *Udf));

  FileDp = MountIsoFileDevicePath (IsoPath);
  if (FileDp == NULL) {
    Print (L"MOUNT: error - cannot build device path for %s\n", IsoPath);
    ShellCloseFile (&File);
    return STATUS_ISO_ERROR;
  }

  Status = MountLoopFactoryGet (&Factory);
  if (Status == EFI_SUCCESS) {
    Status = Factory->Create (
                        Factory,
                        (EFI_FILE_PROTOCOL *)File,
                        FileDp,
                        FileSize / LOOP_BLOCK_SIZE - 1,
                        LoopHandle
                        );
  }
  FreePool (FileDp);
  if (Status != EFI_SUCCESS) {
    // MountLoopFactoryGet printed its own message; a factory Create()
    // failure is reported here.
    if ((Status != STATUS_DRIVER_MISSING) && (Status != STATUS_SECURE_BOOT)) {
      Print (L"MOUNT: error - loop device create failed (%r)\n", Status);
    }
    ShellCloseFile (&File);
    return Status;
  }
  // The factory owns the open file from here on (intentionally never
  // closed while the mount lives, spec section 8).
  return EFI_SUCCESS;
}

EFI_STATUS
MountRunIso (
  IN CONST CHAR16  *IsoPath
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  LoopHandle;
  EFI_HANDLE  *OldFs;
  UINTN       OldCount;
  BOOLEAN     Iso9660;
  BOOLEAN     Udf;

  OldFs    = NULL;
  OldCount = 0;
  MapSnapshotFsHandles (&OldFs, &OldCount);

  Status = LoopCreate (IsoPath, &LoopHandle, &Iso9660, &Udf);
  // LoopCreate also returns STATUS_ISO_ERROR (a plain small integer with
  // no EFI error bit) -- test against EFI_SUCCESS, not EFI_ERROR().
  if (Status != EFI_SUCCESS) {
    if (OldFs != NULL) {
      FreePool (OldFs);
    }
    return Status;
  }
  Print (L"MOUNT: loop device created for %s\n", IsoPath);
  DEBUG ((DEBUG_INFO, "MOUNT: loop handle %x created\n", LoopHandle));

  // Pure ISO9660 with no ISO9660 driver loaded is provably unclaimable:
  // the stock firmware stack produces no SimpleFileSystem for it (Task 4
  // negative control), and ConnectController on a handle no driver claims
  // deadlocks this OVMF (Task 9: recursive EfiAcquireLock ASSERT in an FV
  // driver, vCPU spins in CpuDeadLoop). Auto-load the ISO9660 driver here
  // -- the same chain as `mount -<FORMAT>` -- so `mount -ISO` stays fully
  // automatic (req: "automatically mount the ISO's filesystem") and the
  // claimed handle is also immune to that deadlock. Only when the driver
  // FILE is missing does the flow degrade to the keep-loop + manual
  // load + map -r recovery.
  if (Iso9660 && !Udf && !MountDriverLoaded (L"drivers\\iso9660_x64.efi")) {
    Status = MountLoadDriver (L"drivers\\iso9660_x64.efi");
    if (Status != EFI_SUCCESS) {
      Print (L"MOUNT: warn - ISO9660 detected but drivers\\iso9660_x64.efi missing\n");
      Print (L"MOUNT: loop device kept; load drivers\\iso9660_x64.efi, then map -r\n");
      DEBUG ((DEBUG_INFO, "MOUNT: no ISO9660 driver, connect skipped\n"));
      if (OldFs != NULL) {
        FreePool (OldFs);
      }
      return EFI_SUCCESS;
    }
  }

  // Bind the firmware FS stack (PartitionDxe probe, DiskIoDxe, UdfDxe and
  // any loaded efifs driver) on just the new handle, then run the same
  // rescan/refresh/report chain as `mount -<FORMAT>`.
  gBS->ConnectController (LoopHandle, NULL, NULL, TRUE);
  MapConnectAllControllers ();
  MapRefreshShell ();
  MapPrintNewVolumes (OldFs, OldCount);
  if (OldFs != NULL) {
    FreePool (OldFs);
  }
  // Intentionally NOT destroyed: the loop device must outlive this app
  // (spec section 8: the mount persists after exit).
  return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// Selftest item 3: loop BlockIo install/read/uninstall roundtrip.
// ---------------------------------------------------------------------------

#define LOOP_SELFTEST_PATH    L"fs0:\\loop_selftest.bin"
#define LOOP_SELFTEST_BLOCKS  16                  // == LOOP_MIN_SIZE, sniff reads past EOF quietly
#define LOOP_SELFTEST_MARK0   0x4D                // 'M'
#define LOOP_SELFTEST_MARK1   0x55                // 'U'

EFI_STATUS
LoopDiskSelfTest (
  VOID
  )
{
  EFI_STATUS                  Status;
  SHELL_FILE_HANDLE           File;
  MOUNT_LOOP_FACTORY_PROTOCOL *Factory;
  EFI_HANDLE                  LoopHandle;
  EFI_BLOCK_IO_PROTOCOL       *BlockIo;
  EFI_BLOCK_IO2_PROTOCOL      *BlockIo2;
  EFI_FILE_PROTOCOL           *BackingFile;
  UINT8                       *Pattern;
  UINT8                       *ReadBack;
  UINTN                       Size;
  UINT32                      MediaId;
  BOOLEAN                     Ok;
  BOOLEAN                     Iso9660;
  BOOLEAN                     Udf;
  EFI_BLOCK_IO2_TOKEN         Token;

  Pattern = AllocatePool (LOOP_SELFTEST_BLOCKS * LOOP_BLOCK_SIZE);
  ReadBack = AllocatePool (LOOP_BLOCK_SIZE);
  if ((Pattern == NULL) || (ReadBack == NULL)) {
    if (Pattern != NULL) {
      FreePool (Pattern);
    }
    if (ReadBack != NULL) {
      FreePool (ReadBack);
    }
    return EFI_OUT_OF_RESOURCES;
  }

  // Temp file on fs0: (map-name form: no CWD under startup.nsh). Block 0
  // filled with MARK0, block 1 with MARK1, the remaining blocks zeroed.
  SetMem (Pattern, LOOP_SELFTEST_BLOCKS * LOOP_BLOCK_SIZE, 0);
  SetMem (Pattern, LOOP_BLOCK_SIZE, LOOP_SELFTEST_MARK0);
  SetMem (Pattern + LOOP_BLOCK_SIZE, LOOP_BLOCK_SIZE, LOOP_SELFTEST_MARK1);
  Status = ShellOpenFileByName (
             LOOP_SELFTEST_PATH,
             &File,
             EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
             0
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "MOUNT: selftest cannot create %S (%r)\n", LOOP_SELFTEST_PATH, Status));
    FreePool (Pattern);
    FreePool (ReadBack);
    return Status;
  }
  Size   = LOOP_SELFTEST_BLOCKS * LOOP_BLOCK_SIZE;
  Status = ((EFI_FILE_PROTOCOL *)File)->Write ((EFI_FILE_PROTOCOL *)File, &Size, Pattern);
  ShellCloseFile (&File);
  if (EFI_ERROR (Status) || (Size != LOOP_SELFTEST_BLOCKS * LOOP_BLOCK_SIZE)) {
    FreePool (Pattern);
    FreePool (ReadBack);
    ShellDeleteFileByName (LOOP_SELFTEST_PATH);
    return EFI_DEVICE_ERROR;
  }

  // Install the loop device over the temp file (through the resident
  // factory, same as a real mount) and read both marker blocks back
  // through BlockIo; exercise the BlockIo2 path on block 1.
  Ok         = FALSE;
  LoopHandle = NULL;
  Status     = LoopCreate (LOOP_SELFTEST_PATH, &LoopHandle, &Iso9660, &Udf);
  // STATUS_ISO_ERROR carries no EFI error bit: compare EFI_SUCCESS.
  if ((Status == EFI_SUCCESS) &&
      !EFI_ERROR (gBS->HandleProtocol (
                         LoopHandle,
                         &gEfiBlockIoProtocolGuid,
                         (VOID **)&BlockIo
                         )) &&
      !EFI_ERROR (gBS->HandleProtocol (
                         LoopHandle,
                         &gEfiBlockIo2ProtocolGuid,
                         (VOID **)&BlockIo2
                         )))
  {
    Ok      = TRUE;
    MediaId = BlockIo->Media->MediaId;
    Size    = LOOP_BLOCK_SIZE;
    Status  = BlockIo->ReadBlocks (BlockIo, MediaId, 0, Size, ReadBack);
    if (EFI_ERROR (Status) ||
        (CompareMem (ReadBack, Pattern, LOOP_BLOCK_SIZE) != 0))
    {
      Ok = FALSE;
    }
    Status = BlockIo->ReadBlocks (BlockIo, MediaId, 1, Size, ReadBack);
    if (EFI_ERROR (Status) ||
        (CompareMem (ReadBack, Pattern + LOOP_BLOCK_SIZE, LOOP_BLOCK_SIZE) != 0))
    {
      Ok = FALSE;
    }
    ZeroMem (&Token, sizeof (Token));
    Status = BlockIo2->ReadBlocksEx (BlockIo2, MediaId, 1, &Token, Size, ReadBack);
    if (EFI_ERROR (Status) || EFI_ERROR (Token.TransactionStatus) ||
        (CompareMem (ReadBack, Pattern + LOOP_BLOCK_SIZE, LOOP_BLOCK_SIZE) != 0))
    {
      Ok = FALSE;
    }
    // Read-only contract: writes must be refused.
    if (BlockIo->WriteBlocks (BlockIo, MediaId, 0, Size, ReadBack) !=
        EFI_WRITE_PROTECTED)
    {
      Ok = FALSE;
    }
  }
  if (LoopHandle != NULL) {
    // Destroy hands the backing file back; close it through the Shell it
    // was opened with so the Shell's file-handle log stays consistent.
    Status = MountLoopFactoryGet (&Factory);
    if ((Status == EFI_SUCCESS) &&
        (Factory->Destroy (Factory, LoopHandle, &BackingFile) == EFI_SUCCESS))
    {
      File = (SHELL_FILE_HANDLE)BackingFile;
      ShellCloseFile (&File);
    } else {
      Ok = FALSE;
    }
  }
  ShellDeleteFileByName (LOOP_SELFTEST_PATH);

  FreePool (Pattern);
  FreePool (ReadBack);
  return Ok ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

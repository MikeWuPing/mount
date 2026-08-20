/** @file
  LoopDisk -- virtual block device over a file (Linux loop device analog).

  A LOOP_DISK carries a read-only 2048-byte-block EFI_BLOCK_IO_PROTOCOL
  plus the matching EFI_BLOCK_IO2_PROTOCOL on a fresh handle; reads are
  forwarded to SetPosition/Read on the backing EFI_FILE_PROTOCOL. The
  device path is the image file's own device path plus a unique
  vendor-media node, so several loop devices coexist and `map` output
  stays readable.

  BlockIo2 is installed because this OVMF's efifs drivers only bind
  "modern-stack" handles (Task 4 evidence: ATAPI/IDE handles with
  BlockIo2/DiskIo2 bind, old-style BlockIo-only virtio handles never do).
  efifs itself opens DiskIo/DiskIo2 (pbatard/efifs src/driver.c
  FSBindingSupported), which DiskIoDxe produces from our BlockIo/BlockIo2.

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
#include <Guid/FileInfo.h>
#include "LoopDisk.h"
#include "FsDriver.h"
#include "MapReport.h"

#define LOOP_BLOCK_SIZE  2048
#define LOOP_MEDIA_ID    0x4D4F554E          // "MOUN"
#define LOOP_MIN_SIZE    (16 * LOOP_BLOCK_SIZE)  // PVD at LBA16 must fit

typedef struct {
  EFI_BLOCK_IO_PROTOCOL    BlockIo;        // first member: This -> LOOP_DISK cast
  EFI_BLOCK_IO_MEDIA       Media;          // BlockIo.Media/BlockIo2.Media point here
  EFI_BLOCK_IO2_PROTOCOL   BlockIo2;
  EFI_DEVICE_PATH_PROTOCOL *DevicePath;
  EFI_FILE_PROTOCOL        *BackingFile;   // SHELL_FILE_HANDLE is EFI_FILE_PROTOCOL*
  EFI_HANDLE               Handle;
  UINT64                   LastBlock;
  UINT32                   Seq;
} LOOP_DISK;

typedef struct {
  EFI_DEVICE_PATH_PROTOCOL  Header;
  EFI_GUID                  Guid;
} LOOP_VENDOR_MEDIA_NODE;

STATIC UINT32  mLoopSeq = 0;

// ---------------------------------------------------------------------------
// BlockIo (blocking) -- This IS the LOOP_DISK pointer (BlockIo is member 0).
// ---------------------------------------------------------------------------

STATIC
EFI_STATUS
EFIAPI
LoopReset (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN BOOLEAN                ExtendedVerification
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
LoopReadBlocks (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  OUT VOID                  *Buffer
  )
{
  LOOP_DISK   *Disk = (LOOP_DISK *)This;
  EFI_STATUS  Status;
  UINTN       Size = BufferSize;

  if ((MediaId != Disk->Media.MediaId) || (Buffer == NULL) ||
      ((BufferSize % LOOP_BLOCK_SIZE) != 0))
  {
    return EFI_INVALID_PARAMETER;
  }
  if (((UINT64)Lba > Disk->LastBlock) ||
      ((UINT64)(BufferSize / LOOP_BLOCK_SIZE) > (Disk->LastBlock + 1 - (UINT64)Lba)))
  {
    return EFI_INVALID_PARAMETER;
  }
  Status = Disk->BackingFile->SetPosition (
                                Disk->BackingFile,
                                MultU64x32 ((UINT64)Lba, LOOP_BLOCK_SIZE)
                                );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = Disk->BackingFile->Read (Disk->BackingFile, &Size, Buffer);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  // A short read means the backing file shrank under us: device error.
  return (Size == BufferSize) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC
EFI_STATUS
EFIAPI
LoopWriteBlocks (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  IN CONST VOID             *Buffer
  )
{
  return EFI_WRITE_PROTECTED;
}

STATIC
EFI_STATUS
EFIAPI
LoopFlushBlocks (
  IN EFI_BLOCK_IO_PROTOCOL  *This
  )
{
  return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// BlockIo2 -- recover LOOP_DISK via BASE_CR. Always blocking internally;
// on success with a non-NULL event the token is completed synchronously
// (UEFI allows a blocking engine for BlockIo2). Per spec the event is NOT
// signaled on error returns.
// ---------------------------------------------------------------------------

STATIC
VOID
LoopCompleteToken (
  IN EFI_BLOCK_IO2_TOKEN  *Token,
  IN EFI_STATUS           Status
  )
{
  if (Token == NULL) {
    return;
  }
  Token->TransactionStatus = Status;
  if (!EFI_ERROR (Status) && (Token->Event != NULL)) {
    gBS->SignalEvent (Token->Event);
  }
}

STATIC
EFI_STATUS
EFIAPI
LoopResetEx (
  IN EFI_BLOCK_IO2_PROTOCOL  *This,
  IN BOOLEAN                 ExtendedVerification
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
LoopReadBlocksEx (
  IN     EFI_BLOCK_IO2_PROTOCOL  *This,
  IN     UINT32                  MediaId,
  IN     EFI_LBA                 Lba,
  IN OUT EFI_BLOCK_IO2_TOKEN     *Token,
  IN     UINTN                   BufferSize,
  OUT    VOID                    *Buffer
  )
{
  LOOP_DISK   *Disk = BASE_CR (This, LOOP_DISK, BlockIo2);
  EFI_STATUS  Status;

  Status = LoopReadBlocks (&Disk->BlockIo, MediaId, Lba, BufferSize, Buffer);
  LoopCompleteToken (Token, Status);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
LoopWriteBlocksEx (
  IN     EFI_BLOCK_IO2_PROTOCOL  *This,
  IN     UINT32                  MediaId,
  IN     EFI_LBA                 Lba,
  IN OUT EFI_BLOCK_IO2_TOKEN     *Token,
  IN     UINTN                   BufferSize,
  IN     CONST VOID              *Buffer
  )
{
  LoopCompleteToken (Token, EFI_WRITE_PROTECTED);
  return EFI_WRITE_PROTECTED;
}

STATIC
EFI_STATUS
EFIAPI
LoopFlushBlocksEx (
  IN     EFI_BLOCK_IO2_PROTOCOL  *This,
  IN OUT EFI_BLOCK_IO2_TOKEN     *Token
  )
{
  LoopCompleteToken (Token, EFI_SUCCESS);
  return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// Helpers
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

// Install the loop block device for IsoPath. On success *DiskOut owns the
// open backing file and the installed protocols; LoopDestroy reverses both.
STATIC
EFI_STATUS
LoopCreate (
  IN CONST CHAR16  *IsoPath,
  OUT LOOP_DISK    **DiskOut
  )
{
  EFI_STATUS               Status;
  LOOP_DISK                *Disk;
  SHELL_FILE_HANDLE        File;
  EFI_FILE_INFO            *Info;
  UINTN                    InfoSize;
  EFI_DEVICE_PATH_PROTOCOL *FileDp;
  LOOP_VENDOR_MEDIA_NODE   VendorNode;
  BOOLEAN                  Iso9660;
  BOOLEAN                  Udf;
  UINT64                   FileSize;

  *DiskOut = NULL;

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

  LoopSniff ((EFI_FILE_PROTOCOL *)File, &Iso9660, &Udf);
  if (Iso9660 && !Udf && !MountDriverLoaded (L"drivers\\iso9660_x64.efi")) {
    Print (L"MOUNT: warn - ISO9660 detected but drivers\\iso9660_x64.efi missing\n");
  }
  DEBUG ((DEBUG_INFO, "MOUNT: sniff iso9660=%d udf=%d\n", Iso9660, Udf));

  FileDp = MountIsoFileDevicePath (IsoPath);
  if (FileDp == NULL) {
    Print (L"MOUNT: error - cannot build device path for %s\n", IsoPath);
    ShellCloseFile (&File);
    return STATUS_ISO_ERROR;
  }

  Disk = AllocateZeroPool (sizeof (*Disk));
  if (Disk == NULL) {
    FreePool (FileDp);
    ShellCloseFile (&File);
    return EFI_OUT_OF_RESOURCES;
  }
  Disk->BackingFile = (EFI_FILE_PROTOCOL *)File;
  Disk->LastBlock   = FileSize / LOOP_BLOCK_SIZE - 1;
  Disk->Seq         = ++mLoopSeq;

  Disk->Media.MediaId          = LOOP_MEDIA_ID;
  Disk->Media.RemovableMedia   = TRUE;
  Disk->Media.MediaPresent     = TRUE;
  Disk->Media.LogicalPartition = FALSE;
  Disk->Media.ReadOnly         = TRUE;
  Disk->Media.WriteCaching     = FALSE;
  Disk->Media.BlockSize        = LOOP_BLOCK_SIZE;
  Disk->Media.IoAlign          = 0;
  Disk->Media.LastBlock        = Disk->LastBlock;

  Disk->BlockIo.Revision    = EFI_BLOCK_IO_PROTOCOL_REVISION;
  Disk->BlockIo.Media       = &Disk->Media;
  Disk->BlockIo.Reset       = LoopReset;
  Disk->BlockIo.ReadBlocks  = LoopReadBlocks;
  Disk->BlockIo.WriteBlocks = LoopWriteBlocks;
  Disk->BlockIo.FlushBlocks = LoopFlushBlocks;

  Disk->BlockIo2.Media         = &Disk->Media;
  Disk->BlockIo2.Reset         = LoopResetEx;
  Disk->BlockIo2.ReadBlocksEx  = LoopReadBlocksEx;
  Disk->BlockIo2.WriteBlocksEx = LoopWriteBlocksEx;
  Disk->BlockIo2.FlushBlocksEx = LoopFlushBlocksEx;

  // Device path: the ISO file's own device path + a unique vendor-media
  // node, so each loop instance is distinct and `map` output stays
  // readable (spec section 8).
  VendorNode.Header.Type    = MEDIA_DEVICE_PATH;
  VendorNode.Header.SubType = MEDIA_VENDOR_DP;   // 0x03
  SetDevicePathNodeLength (&VendorNode.Header, sizeof (VendorNode));
  VendorNode.Guid.Data1     = 0x5C6D7E8F;
  VendorNode.Guid.Data2     = 0x1111;
  VendorNode.Guid.Data3     = 0x4001;
  VendorNode.Guid.Data4[0]  = 0x80;
  VendorNode.Guid.Data4[1]  = 0x01;
  VendorNode.Guid.Data4[7]  = (UINT8)Disk->Seq;
  Disk->DevicePath = AppendDevicePathNode (FileDp, &VendorNode.Header);
  FreePool (FileDp);
  if (Disk->DevicePath == NULL) {
    ShellCloseFile (&File);
    FreePool (Disk);
    return EFI_OUT_OF_RESOURCES;
  }

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Disk->Handle,
                  &gEfiDevicePathProtocolGuid, Disk->DevicePath,
                  &gEfiBlockIoProtocolGuid,    &Disk->BlockIo,
                  &gEfiBlockIo2ProtocolGuid,   &Disk->BlockIo2,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    Print (L"MOUNT: error - InstallProtocolInterfaces failed (%r)\n", Status);
    FreePool (Disk->DevicePath);
    ShellCloseFile (&File);
    FreePool (Disk);
    return Status;
  }
  *DiskOut = Disk;
  return EFI_SUCCESS;
}

// Reverse of LoopCreate. Used by the selftest only -- MountRunIso never
// destroys its loop device because the mount must outlive the app (spec
// section 8). Closing goes through ShellCloseFile so the Shell's
// file-handle side log (mFileHandleList) stays consistent.
STATIC
VOID
LoopDestroy (
  IN LOOP_DISK  *Disk
  )
{
  SHELL_FILE_HANDLE  File;

  gBS->UninstallMultipleProtocolInterfaces (
         Disk->Handle,
         &gEfiDevicePathProtocolGuid, Disk->DevicePath,
         &gEfiBlockIoProtocolGuid,    &Disk->BlockIo,
         &gEfiBlockIo2ProtocolGuid,   &Disk->BlockIo2,
         NULL
         );
  File = (SHELL_FILE_HANDLE)Disk->BackingFile;
  ShellCloseFile (&File);
  FreePool (Disk->DevicePath);
  FreePool (Disk);
}

EFI_STATUS
MountRunIso (
  IN CONST CHAR16  *IsoPath
  )
{
  EFI_STATUS  Status;
  LOOP_DISK   *Disk;
  EFI_HANDLE  *OldFs;
  UINTN       OldCount;

  OldFs    = NULL;
  OldCount = 0;
  MapSnapshotFsHandles (&OldFs, &OldCount);

  Status = LoopCreate (IsoPath, &Disk);
  if (EFI_ERROR (Status)) {
    if (OldFs != NULL) {
      FreePool (OldFs);
    }
    return Status;
  }
  Print (
    L"MOUNT: loop device created for %s (%d blocks)\n",
    IsoPath,
    (UINT32)(Disk->LastBlock + 1)
    );
  DEBUG ((DEBUG_INFO, "MOUNT: loop handle %x seq %u\n", Disk->Handle, Disk->Seq));

  // Bind the firmware FS stack (PartitionDxe probe, DiskIoDxe, UdfDxe and
  // any loaded efifs driver) on just the new handle, then run the same
  // rescan/refresh/report chain as `mount -<FORMAT>`.
  gBS->ConnectController (Disk->Handle, NULL, NULL, TRUE);
  MapConnectAllControllers ();
  MapRefreshShell ();
  MapPrintNewVolumes (OldFs, OldCount);
  if (OldFs != NULL) {
    FreePool (OldFs);
  }
  // Intentionally NOT freed: the loop device must outlive this app
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
  EFI_STATUS         Status;
  SHELL_FILE_HANDLE  File;
  LOOP_DISK          *Disk;
  UINT8              *Pattern;
  UINT8              *ReadBack;
  UINTN              Size;
  BOOLEAN            Ok;
  EFI_BLOCK_IO2_TOKEN Token;

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

  // Install the loop device over the temp file and read both marker
  // blocks back through BlockIo; exercise the BlockIo2 path on block 1.
  Ok = FALSE;
  Status = LoopCreate (LOOP_SELFTEST_PATH, &Disk);
  if (!EFI_ERROR (Status)) {
    Ok = TRUE;
    Size   = LOOP_BLOCK_SIZE;
    Status = Disk->BlockIo.ReadBlocks (&Disk->BlockIo, LOOP_MEDIA_ID, 0, Size, ReadBack);
    if (EFI_ERROR (Status) ||
        (CompareMem (ReadBack, Pattern, LOOP_BLOCK_SIZE) != 0))
    {
      Ok = FALSE;
    }
    Status = Disk->BlockIo.ReadBlocks (&Disk->BlockIo, LOOP_MEDIA_ID, 1, Size, ReadBack);
    if (EFI_ERROR (Status) ||
        (CompareMem (ReadBack, Pattern + LOOP_BLOCK_SIZE, LOOP_BLOCK_SIZE) != 0))
    {
      Ok = FALSE;
    }
    ZeroMem (&Token, sizeof (Token));
    Status = Disk->BlockIo2.ReadBlocksEx (&Disk->BlockIo2, LOOP_MEDIA_ID, 1, &Token, Size, ReadBack);
    if (EFI_ERROR (Status) || EFI_ERROR (Token.TransactionStatus) ||
        (CompareMem (ReadBack, Pattern + LOOP_BLOCK_SIZE, LOOP_BLOCK_SIZE) != 0))
    {
      Ok = FALSE;
    }
    // Read-only contract: writes must be refused.
    if (Disk->BlockIo.WriteBlocks (&Disk->BlockIo, LOOP_MEDIA_ID, 0, Size, ReadBack) !=
        EFI_WRITE_PROTECTED)
    {
      Ok = FALSE;
    }
    LoopDestroy (Disk);
  }
  ShellDeleteFileByName (LOOP_SELFTEST_PATH);

  FreePool (Pattern);
  FreePool (ReadBack);
  return Ok ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

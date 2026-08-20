/** @file
  LoopDxe -- resident loop-device driver (Linux loop.ko analog).

  Entry point installs a single MOUNT_LOOP_FACTORY_PROTOCOL; the mount.efi
  application then asks the factory to create loop devices over open image
  files. Every byte of code a mounted volume can touch -- the factory, the
  BlockIo/BlockIo2 callbacks -- lives in THIS driver image. A boot-service
  driver stays loaded after its entry point returns, so the mounts it
  creates survive the exit of the application that requested them (Task 9
  evidence: app-resident callbacks die with the app image; the first
  post-exit `dir` on such a volume jumps into freed pages and #UDs).

  A LOOP_DISK carries a read-only 2048-byte-block EFI_BLOCK_IO_PROTOCOL
  plus the matching EFI_BLOCK_IO2_PROTOCOL on a fresh handle; reads are
  forwarded to SetPosition/Read on the backing EFI_FILE_PROTOCOL. The
  device path is the image file's own device path plus a unique
  vendor-media node, so several loop devices coexist and `map` output
  stays readable. The sequence counter lives in this resident image, so
  nodes stay unique across any number of mount.efi processes.

  BlockIo2 is installed because this OVMF's efifs drivers only bind
  "modern-stack" handles (Task 4 evidence: ATAPI/IDE handles with
  BlockIo2/DiskIo2 bind, old-style BlockIo-only virtio handles never do).
  efifs itself opens DiskIo/DiskIo2 (pbatard/efifs src/driver.c
  FSBindingSupported), which DiskIoDxe produces from our BlockIo/BlockIo2.

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
#include <Protocol/BlockIo.h>
#include <Protocol/BlockIo2.h>
#include <Protocol/MountLoopFactory.h>

#define LOOP_BLOCK_SIZE  2048
#define LOOP_MEDIA_ID    0x4D4F554E          // "MOUN"
#define LOOP_DISK_SIG    SIGNATURE_32 ('M', 'L', 'O', 'P')

typedef struct {
  UINTN                      Signature;
  EFI_BLOCK_IO_PROTOCOL      BlockIo;        // member: This -> LOOP_DISK cast
  EFI_BLOCK_IO_MEDIA         Media;          // BlockIo.Media/BlockIo2.Media point here
  EFI_BLOCK_IO2_PROTOCOL     BlockIo2;
  EFI_DEVICE_PATH_PROTOCOL   *DevicePath;
  EFI_FILE_PROTOCOL          *BackingFile;
  EFI_HANDLE                 Handle;
  UINT64                     LastBlock;
  UINT32                     Seq;
} LOOP_DISK;

typedef struct {
  EFI_DEVICE_PATH_PROTOCOL  Header;
  EFI_GUID                  Guid;
} LOOP_VENDOR_MEDIA_NODE;

// Resident globals: the factory handle and the cross-process loop counter.
STATIC EFI_HANDLE  mFactoryHandle = NULL;
STATIC UINT32      mLoopSeq       = 0;

// ---------------------------------------------------------------------------
// BlockIo (blocking) -- recover LOOP_DISK via BASE_CR (Signature is the
// first member, so a plain cast would land 8 bytes early).
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
  LOOP_DISK   *Disk = BASE_CR (This, LOOP_DISK, BlockIo);
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
// Factory
// ---------------------------------------------------------------------------

STATIC
EFI_STATUS
EFIAPI
LoopFactoryCreate (
  IN MOUNT_LOOP_FACTORY_PROTOCOL  *This,
  IN EFI_FILE_PROTOCOL            *BackingFile,
  IN EFI_DEVICE_PATH_PROTOCOL     *FileDevicePath,
  IN UINT64                       LastBlock,
  OUT EFI_HANDLE                  *LoopHandle
  )
{
  EFI_STATUS             Status;
  LOOP_DISK              *Disk;
  LOOP_VENDOR_MEDIA_NODE VendorNode;

  if ((BackingFile == NULL) || (FileDevicePath == NULL) || (LoopHandle == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  *LoopHandle = NULL;

  Disk = AllocateZeroPool (sizeof (*Disk));
  if (Disk == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Disk->Signature   = LOOP_DISK_SIG;
  Disk->BackingFile = BackingFile;
  Disk->LastBlock   = LastBlock;
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
  // readable. ZeroMem first: only parts of the GUID are assigned below,
  // the rest must be the spec'd base (0x00), not stack.
  ZeroMem (&VendorNode, sizeof (VendorNode));
  VendorNode.Header.Type    = MEDIA_DEVICE_PATH;
  VendorNode.Header.SubType = MEDIA_VENDOR_DP;   // 0x03
  SetDevicePathNodeLength (&VendorNode.Header, sizeof (VendorNode));
  VendorNode.Guid.Data1     = 0x5C6D7E8F;
  VendorNode.Guid.Data2     = 0x1111;
  VendorNode.Guid.Data3     = 0x4001;
  VendorNode.Guid.Data4[0]  = 0x80;
  VendorNode.Guid.Data4[1]  = 0x01;
  VendorNode.Guid.Data4[7]  = (UINT8)Disk->Seq;
  Disk->DevicePath = AppendDevicePathNode (FileDevicePath, &VendorNode.Header);
  if (Disk->DevicePath == NULL) {
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
    FreePool (Disk->DevicePath);
    FreePool (Disk);
    return Status;
  }
  *LoopHandle = Disk->Handle;
  DEBUG ((DEBUG_INFO, "LOOPDXE: loop handle %x seq %u, %u blocks\n",
          Disk->Handle, Disk->Seq, (UINT32)(Disk->LastBlock + 1)));
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
LoopFactoryDestroy (
  IN MOUNT_LOOP_FACTORY_PROTOCOL  *This,
  IN EFI_HANDLE                   LoopHandle,
  OUT EFI_FILE_PROTOCOL           **BackingFile  OPTIONAL
  )
{
  EFI_STATUS             Status;
  EFI_BLOCK_IO_PROTOCOL  *BlockIo;
  LOOP_DISK              *Disk;

  Status = gBS->HandleProtocol (
                  LoopHandle,
                  &gEfiBlockIoProtocolGuid,
                  (VOID **)&BlockIo
                  );
  if (EFI_ERROR (Status)) {
    return EFI_NOT_FOUND;
  }
  Disk = BASE_CR (BlockIo, LOOP_DISK, BlockIo);
  if (Disk->Signature != LOOP_DISK_SIG) {
    return EFI_NOT_FOUND;
  }

  Status = gBS->UninstallMultipleProtocolInterfaces (
                  Disk->Handle,
                  &gEfiDevicePathProtocolGuid, Disk->DevicePath,
                  &gEfiBlockIoProtocolGuid,    &Disk->BlockIo,
                  &gEfiBlockIo2ProtocolGuid,   &Disk->BlockIo2,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (BackingFile != NULL) {
    *BackingFile = Disk->BackingFile;
  }
  FreePool (Disk->DevicePath);
  FreePool (Disk);
  return EFI_SUCCESS;
}

STATIC MOUNT_LOOP_FACTORY_PROTOCOL  mFactory = {
  MOUNT_LOOP_FACTORY_REVISION,
  LoopFactoryCreate,
  LoopFactoryDestroy
};

/**
  Driver entry: publish the loop factory and stay resident. Any failure
  unloads the image (StartImage propagates it), which is exactly the
  cleanup a failed install needs.
**/
EFI_STATUS
EFIAPI
LoopDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  Status = gBS->InstallProtocolInterface (
                  &mFactoryHandle,
                  &gMountLoopFactoryProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mFactory
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "LOOPDXE: factory install failed (%r)\n", Status));
    return Status;
  }
  DEBUG ((DEBUG_INFO, "LOOPDXE: loop factory installed\n"));
  return EFI_SUCCESS;
}

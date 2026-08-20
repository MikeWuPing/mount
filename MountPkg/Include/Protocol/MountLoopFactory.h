/** @file
  MOUNT_LOOP_FACTORY_PROTOCOL -- resident loop-device factory.

  Produced once by the LoopDxe boot-service driver (drivers\loop_x64.efi),
  consumed by the mount.efi application. The protocol and every loop device
  it creates live in the DRIVER image, which stays resident after its entry
  point returns -- that is what makes a mount survive mount.efi's exit
  (an application's own protocol callbacks die with its image; Task 9
  measured the crash).

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/

#ifndef MOUNT_LOOP_FACTORY_H
#define MOUNT_LOOP_FACTORY_H

#define MOUNT_LOOP_FACTORY_PROTOCOL_GUID \
  { 0x8F3E2A41, 0x6B7C, 0x4D5E, { 0x9A, 0x1B, 0x2C, 0x3D, 0x4E, 0x5F, 0x6A, 0x7B } }

#define MOUNT_LOOP_FACTORY_REVISION  1

typedef struct _MOUNT_LOOP_FACTORY_PROTOCOL MOUNT_LOOP_FACTORY_PROTOCOL;

/**
  Install a read-only 2048-byte-block loop device over an open file.

  On success a fresh LoopHandle carries DevicePath (FileDevicePath plus a
  unique vendor-media node), EFI_BLOCK_IO_PROTOCOL and
  EFI_BLOCK_IO2_PROTOCOL, and the factory owns BackingFile (it stays open
  for the life of the mount, which intentionally outlives the caller).
  FileDevicePath is only read; the caller keeps and frees it.
**/
typedef
EFI_STATUS
(EFIAPI *MOUNT_LOOP_CREATE)(
  IN MOUNT_LOOP_FACTORY_PROTOCOL  *This,
  IN EFI_FILE_PROTOCOL            *BackingFile,
  IN EFI_DEVICE_PATH_PROTOCOL     *FileDevicePath,
  IN UINT64                       LastBlock,
  OUT EFI_HANDLE                  *LoopHandle
  );

/**
  Uninstall a loop device created by MOUNT_LOOP_CREATE.

  Protocols are uninstalled and the loop instance freed. The backing file
  is NOT closed: its ownership is handed back through BackingFile (when
  non-NULL) so the caller can close it through the Shell it was opened
  with. EFI_NOT_FOUND when LoopHandle carries no loop BlockIo.
**/
typedef
EFI_STATUS
(EFIAPI *MOUNT_LOOP_DESTROY)(
  IN MOUNT_LOOP_FACTORY_PROTOCOL  *This,
  IN EFI_HANDLE                   LoopHandle,
  OUT EFI_FILE_PROTOCOL           **BackingFile  OPTIONAL
  );

struct _MOUNT_LOOP_FACTORY_PROTOCOL {
  UINT64              Revision;
  MOUNT_LOOP_CREATE   Create;
  MOUNT_LOOP_DESTROY  Destroy;
};

extern EFI_GUID  gMountLoopFactoryProtocolGuid;

#endif

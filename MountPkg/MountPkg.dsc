## @file
# mount platform description. X64 only; edk2 tree not modified.
#
# DebugLib -> BaseDebugLibSerialPort + SerialIoLib (16550 COM1 0x3F8) so
# QEMU "-serial file:" captures DEBUG() output (version assert channel).
#
# Copyright (c) 2026, Mike Wu. All rights reserved.
#
##

[Defines]
  PLATFORM_NAME                  = MountPkg
  PLATFORM_GUID                  = 7C4D9E2A-1F5B-4A83-9C62-D0E3F5A7B914
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/MountPkg
  SUPPORTED_ARCHITECTURES        = X64
  BUILD_TARGETS                  = DEBUG|RELEASE|NOOPT
  SKUID_IDENTIFIER               = DEFAULT

[LibraryClasses]
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
  UefiDriverEntryPoint|MdePkg/Library/UefiDriverEntryPoint/UefiDriverEntryPoint.inf
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  DebugLib|MdePkg/Library/BaseDebugLibSerialPort/BaseDebugLibSerialPort.inf
  # Fixed mask instead of the PCD-backed MdePkg instance: BasePcdLibNull
  # returns 0 and would mute the serial log (and its ASSERT recurses).
  DebugPrintErrorLevelLib|MountPkg/Library/FixedDebugPrintErrorLevelLib/FixedDebugPrintErrorLevelLib.inf
  # BaseDebugLibSerialPort consumes SerialPortLib; SerialIoLib is the
  # PCD-free 16550 COM1 (0x3F8) instance, matching QEMU "-serial file:".
  SerialPortLib|PcAtChipsetPkg/Library/SerialIoLib/SerialIoLib.inf
  IoLib|MdePkg/Library/BaseIoLibIntrinsic/BaseIoLibIntrinsic.inf
  RegisterFilterLib|MdePkg/Library/RegisterFilterLibNull/RegisterFilterLibNull.inf
  StackCheckLib|MdePkg/Library/StackCheckLibNull/StackCheckLibNull.inf
  CompilerIntrinsicsLib|MdePkg/Library/CompilerIntrinsicsLib/CompilerIntrinsicsLib.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  # UefiShellLib deps (this edk2 tree): FileHandleLib lives in MdePkg,
  # SortLib in MdeModulePkg; UefiHiiLib itself needs UefiHiiServicesLib.
  # There is no HiiStringLib instance (or even header) in this tree.
  ShellLib|ShellPkg/Library/UefiShellLib/UefiShellLib.inf
  FileHandleLib|MdePkg/Library/UefiFileHandleLib/UefiFileHandleLib.inf
  SortLib|MdeModulePkg/Library/UefiSortLib/UefiSortLib.inf
  HiiLib|MdeModulePkg/Library/UefiHiiLib/UefiHiiLib.inf
  UefiHiiServicesLib|MdeModulePkg/Library/UefiHiiServicesLib/UefiHiiServicesLib.inf

# FixedAtBuild PCDs are baked into every module's AutoGen as
# _gPcd_FixedAtBuild_* constants; the access never routes through PcdLib,
# so value overrides must live in this section (a custom PcdLib instance
# would be dead code). The MdePkg.dec default PcdDebugPropertyMask=0 gates
# off DebugPrintEnabled()/DebugAssertEnabled() at every DEBUG()/ASSERT()
# call site in DebugLib.h, silently killing the serial assertion channel.
# 0x03 = DEBUG_PROPERTY_DEBUG_ASSERT_ENABLED | DEBUG_PROPERTY_DEBUG_PRINT_ENABLED.
[PcdsFixedAtBuild]
  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x03
  gEfiMdePkgTokenSpaceGuid.PcdMaximumAsciiStringLength|1000000
  gEfiMdePkgTokenSpaceGuid.PcdMaximumUnicodeStringLength|1000000
  gEfiMdePkgTokenSpaceGuid.PcdDebugClearMemoryValue|0xAF
  # Backstop for the _DEBUG_PRINT macro path only: DebugLib.h gates every
  # DEBUG() on DebugPrintLevelEnabled(), which tests the level against
  # this PCD, so 0xFFFFFFFF lets all levels through that first gate. The
  # real filtering happens one layer down in BaseDebugLibSerialPort's
  # DebugPrintMarker(), which masks against GetDebugPrintErrorLevel() --
  # provided here by FixedDebugPrintErrorLevelLib (ERROR|INIT|WARN|LOAD|
  # INFO; VERBOSE is intentionally dropped).
  gEfiMdePkgTokenSpaceGuid.PcdFixedDebugPrintErrorLevel|0xFFFFFFFF

[Components]
  MountPkg/Application/Mount/Mount.inf
  MountPkg/Drivers/LoopDxe/LoopDxe.inf

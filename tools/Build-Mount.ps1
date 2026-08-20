param(
  [string]$ProjectRoot = 'D:\Work\Code\mount',
  [string]$Edk2 = 'D:\Work\Code\edk2',
  [string]$Target = 'DEBUG',
  [string]$Startup = 'mount.efi'
)
# Versioned build, the ONLY entry point: BUILD+1, regen Version.h, build,
# regen qemu_disk (Shell.efi + mount.efi + drivers/ + startup.nsh + markers),
# write expected_version.txt, build FAT image.
# NOTE: comments pure ASCII (PS 5.1 BOM-less .ps1 is read as ANSI/GBK).
$ErrorActionPreference = 'Stop'
$skill = Join-Path $ProjectRoot '.claude\skills\emulator-uefi-shell-app\templates'
$versionHeader = Join-Path $ProjectRoot 'MountPkg\Application\Mount\Version.h'
$expected = Join-Path $ProjectRoot 'qemu_disk\expected_version.txt'
# PACKAGES_PATH must point at the project root: BaseTools searches WORKSPACE
# (the edk2 tree) plus each PACKAGES_PATH entry for MountPkg/... paths in
# the DSC. .\edksetup.bat: this machine sets
# NoDefaultCurrentDirectoryInExePath=1, so cmd.exe will NOT search the
# current directory for a bare edksetup.bat.
$buildCmd = "set PACKAGES_PATH=D:\Work\Code\mount&& .\edksetup.bat&& build -p MountPkg/MountPkg.dsc -a X64 -t VS2019 -b $Target"
# Version.h regen is invisible to the incremental builder (no header dep
# tracking): nuke the module output so every build recompiles it.
$modOut = Join-Path $Edk2 "Build\MountPkg\${Target}_VS2019\X64\MountPkg\Application\Mount"
if (Test-Path $modOut) { Remove-Item $modOut -Recurse -Force }
# qemu_disk is gitignored and regenerated deterministically here.
$disk = Join-Path $ProjectRoot 'qemu_disk'
if (Test-Path $disk) { Remove-Item $disk -Recurse -Force }
New-Item -ItemType Directory -Force -Path "$disk\EFI\BOOT", "$disk\drivers" | Out-Null
$shellEfi = Join-Path $Edk2 'Build\EmulatorX64\DEBUG_VS2019\X64\Shell.efi'
if (!(Test-Path $shellEfi)) { throw "Shell.efi not found: $shellEfi (build EmulatorPkg first)" }
Copy-Item $shellEfi "$disk\EFI\BOOT\BOOTX64.EFI"
Copy-Item (Join-Path $ProjectRoot 'drivers\*_x64.efi') "$disk\drivers\" -Force
Set-Content -Path (Join-Path $disk 'startup.nsh') -Value $Startup
Set-Content -Path (Join-Path $disk 'marker.txt') -Value 'host marker'   # default; tests may rely on ISO/VHD markers instead
# test ISOs ride on fs0: for -ISO scenarios (copy if present)
foreach ($iso in @('iso9660_test.iso','udf_test.iso')) {
  $src = Join-Path $ProjectRoot "test_images\$iso"
  if (Test-Path $src) { Copy-Item $src $disk -Force }
}
$v = & (Join-Path $skill 'Build-UefiApp.ps1') -ProjectRoot $ProjectRoot -Edk2Workspace $Edk2 -BuildCommand $buildCmd -OutputHeader $versionHeader -ExpectedVersionFile $expected
$efi = Join-Path $Edk2 "Build\MountPkg\${Target}_VS2019\X64\mount.efi"
if (!(Test-Path $efi)) { throw "built efi not found: $efi" }
Copy-Item $efi (Join-Path $disk 'mount.efi') -Force
# LoopDxe (built from MountPkg above) deploys as drivers\loop_x64.efi:
# mount.efi LoadImage/StartImages it on first `mount -ISO`.
$loopEfi = Join-Path $Edk2 "Build\MountPkg\${Target}_VS2019\X64\LoopDxe.efi"
if (!(Test-Path $loopEfi)) { throw "built efi not found: $loopEfi" }
Copy-Item $loopEfi (Join-Path $disk 'drivers\loop_x64.efi') -Force
python (Join-Path $ProjectRoot 'tools\mkfatimg.py') create $disk (Join-Path $ProjectRoot 'qemu_disk.img')
if ($LASTEXITCODE -ne 0) { throw "mkfatimg create failed (exit $LASTEXITCODE)" }
Write-Host "BUILT: $($v.VersionString)"
Write-Host "DISK IMG: $ProjectRoot\qemu_disk.img"

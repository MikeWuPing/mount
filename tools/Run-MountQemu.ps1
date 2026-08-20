param(
  [string]$ProjectRoot = 'D:\Work\Code\mount',
  [string]$OvmfCode = 'D:\Work\Code\ContraQwen\OVMF_CODE.fd',
  [int]$QmpPort = 4556,
  [string]$Script = '',
  [int]$MaxShots = 40,
  [string]$DiskImage = 'D:\Work\Code\mount\qemu_disk.img',
  [string]$SecondImage = '',
  [switch]$SecondImageReadOnly,
  [switch]$SecondImageCdrom,
  [switch]$NoVersionCheck
)
# NOTE: keep comments pure ASCII. PowerShell 5.1 reads BOM-less .ps1 as
# ANSI (GBK); a UTF-8 comment ending in a GBK lead byte swallows the
# next source line.
$ErrorActionPreference = 'Stop'
$qemu = 'C:\Program Files\qemu\qemu-system-x86_64.exe'
$qemuImg = 'C:\Program Files\qemu\qemu-img.exe'
$runLogs = Join-Path $ProjectRoot 'run_logs'
$snapshot = Join-Path $ProjectRoot 'snapshot'
New-Item -ItemType Directory -Force -Path $runLogs, $snapshot | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$serial = Join-Path $runLogs ($stamp + '_serial.log')
$stderrLog = Join-Path $runLogs ($stamp + '_stderr.log')
# Fresh copy of the disk image per run: guest writes land on the temp
# copy, the source image stays clean. Temp image kept for inspection.
$tempImage = Join-Path $env:TEMP ("mount_disk_$stamp.img")
Copy-Item $DiskImage $tempImage -Force
# cache=directsync (gufile-proven): default/writethrough cache makes
# QEMU-for-Windows exit silently mid-run (Windows file-cache issue).
$driveArgs = "-drive format=raw,file=`"$tempImage`",cache=directsync"
$tempImage2 = ''
if ($SecondImage) {
  # Second-disk presentation matrix (Task 4 Phase 0 findings, this OVMF):
  #  - vpc on ide0-hd1: silently NOT enumerated by AtaBusDxe.
  #  - vpc/raw on virtio-blk: enumerated, but the handle is old-style
  #    (BlockIo only, no BlockIo2/DiskInfo): PartitionDxe never creates
  #    partition children and efifs drivers never bind it.
  #  - raw on ide0-hd1: enumerated, partitioned, efifs binds.  (NTFS)
  #  - raw ISO as ide-cd (2048B ATAPI): full modern stack, efifs binds.
  # Therefore: .vhd is converted to raw via qemu-img and attached on IDE;
  # optical media goes to the CD-ROM slot; plain readonly raw disks keep
  # the virtio path (QEMU refuses readonly nodes on ide-hd).
  if ($SecondImageCdrom) {
    # Guest cannot write the CD; attach the source directly (no temp copy).
    $driveArgs += " -drive media=cdrom,format=raw,readonly=on,file=`"$SecondImage`",cache=directsync"
  } elseif ($SecondImageReadOnly) {
    $tempImage2 = Join-Path $env:TEMP ("mount_disk2_$stamp.img")
    Copy-Item $SecondImage $tempImage2 -Force
    $driveArgs += " -drive if=none,format=raw,file=`"$tempImage2`",cache=directsync,readonly=on,id=mount2 -device virtio-blk-pci,drive=mount2"
  } else {
    $tempImage2 = Join-Path $env:TEMP ("mount_disk2_$stamp.img")
    if ([IO.Path]::GetExtension($SecondImage) -ieq '.vhd') {
      & $qemuImg convert -f vpc -O raw $SecondImage $tempImage2 | Out-Null
      if ($LASTEXITCODE -ne 0) { throw "qemu-img convert failed for $SecondImage" }
    } else {
      Copy-Item $SecondImage $tempImage2 -Force
    }
    $driveArgs += " -drive format=raw,file=`"$tempImage2`",cache=directsync"
  }
}
# Proven OVMF launch: no -machine q35, no vars pflash (falls back to the
# internal UEFI Shell which runs startup.nsh from fs0:), -net none.
$args = "-m 256M -vga std -net none -display none -serial file:`"$serial`" " +
  "-qmp tcp:127.0.0.1:$QmpPort,server,nowait " +
  "-drive if=pflash,format=raw,readonly=on,file=`"$OvmfCode`" " + $driveArgs
$proc = Start-Process -FilePath $qemu -ArgumentList $args -PassThru `
  -RedirectStandardError $stderrLog -RedirectStandardOutput (Join-Path $runLogs ($stamp + '_stdout.log'))
$py = Join-Path $ProjectRoot 'tools\qmp_drive.py'
python $py --port $QmpPort --shot-dir $snapshot --qemu-pid $proc.Id --max-shots $MaxShots --script $Script
if ($LASTEXITCODE -ne 0) {
  if (-not $proc.HasExited) { $proc.Kill(); $proc.WaitForExit() }
  throw "qmp_drive.py failed with exit code $LASTEXITCODE (serial: $serial)"
}
if (-not $proc.HasExited) { $proc.Kill(); $proc.WaitForExit() }
$err = if (Test-Path $stderrLog) { Get-Content -Raw $stderrLog } else { '' }
if ($err -match 'assertion|Assertion failed|ERROR:') {
  Write-Host 'QEMU STDERR:'; Write-Host $err
  throw "QEMU reported failures (see $stderrLog)"
}
if (!$NoVersionCheck) {
  & (Join-Path $ProjectRoot 'tools\Test-AppVersion.ps1') `
    -ExpectedVersionFile (Join-Path $ProjectRoot 'qemu_disk\expected_version.txt') -SerialLog $serial -SnapshotDir $snapshot
}
Write-Host "SERIAL: $serial"
Write-Host "STDERR: $stderrLog"
Write-Host "DISK: $tempImage (kept for inspection)"
if ($tempImage2) { Write-Host "DISK2: $tempImage2 (kept for inspection)" }

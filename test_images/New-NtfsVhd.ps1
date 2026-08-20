# Create a 64MB dynamic VHD with one NTFS partition (needs admin).
# Usage (elevated): powershell -ExecutionPolicy Bypass -File New-NtfsVhd.ps1
# NOTE: comments pure ASCII (PS 5.1 BOM-less .ps1 is read as ANSI/GBK).
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (!$isAdmin) { throw 'run from an ELEVATED PowerShell (diskpart needs admin)' }
if (Test-Path 'X:\') { throw 'drive letter X: is already in use; free it or edit this script' }
$vhd = Join-Path $PWD 'ntfs64.vhd'
$dp = Join-Path $PWD 'dp_script.txt'
if (Test-Path $vhd) {
  # detach stale attach, then recreate
  "select vdisk file=`"$vhd`"`ndetach vdisk" | Set-Content -Path "$($dp).old"
  diskpart /s "$($dp).old" | Out-Null
  Remove-Item $vhd -Force
}
@"
create vdisk file="$vhd" maximum=64 type=expandable
attach vdisk
create partition primary
format fs=ntfs label=MOUNTTEST quick
assign letter=X
"@ | Set-Content -Path $dp
$attached = $false
try {
  diskpart /s $dp
  if ($LASTEXITCODE -ne 0) { throw "diskpart failed (see output)" }
  $attached = $true
  Set-Content -Path 'X:\ntfs_marker.txt' -Value 'NTFS-MOUNT-OK'
  New-Item -ItemType Directory -Force -Path 'X:\docs' | Out-Null
  Set-Content -Path 'X:\docs\readme.txt' -Value 'readme on ntfs'
} finally {
  if ($attached) {
    "select vdisk file=`"$vhd`"`ndetach vdisk" | Set-Content -Path $dp
    diskpart /s $dp | Out-Null
  }
}
Remove-Item $dp, "$($dp).old" -Force -ErrorAction SilentlyContinue
Write-Host "created $vhd"

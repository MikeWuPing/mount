# Create small test ISOs: pure ISO9660 and pure UDF, with ASCII marker
# files (UEFI Shell console has no CJK glyphs).
# Usage: powershell -ExecutionPolicy Bypass -File New-TestIso.ps1
# NOTE: comments pure ASCII (PS 5.1 BOM-less .ps1 is read as ANSI/GBK).
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$work = Join-Path $PWD 'iso_stage'

# Fallback image saver. Two PowerShell 5.1 COM quirks force this into C#:
#  1) the result property is ImageStream (not Image), and
#  2) a C# cast does a proper QueryInterface on the RCW, which PowerShell's
#     -as operator does not do reliably for ComTypes.IStream.
# IID and vtable order verified against SDK um/imapi2fs.h (10.0.22621.0).
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;

[ComImport]
[Guid("2C941FD8-975B-59BE-A960-9A2A262853A5")]
[InterfaceType(ComInterfaceType.InterfaceIsIDispatch)]
public interface IFileSystemImageResult {
    IStream ImageStream { [return: MarshalAs(UnmanagedType.Interface)] get; }
    object ProgressItems { [return: MarshalAs(UnmanagedType.Interface)] get; }
    int TotalBlocks { get; }
    int BlockSize { get; }
    string DiscId { get; }
}

public static class IStreamSaver {
    public static void SaveResultImage(object comResult, string path) {
        IFileSystemImageResult r = (IFileSystemImageResult)comResult;
        IStream s = r.ImageStream;
        if (s == null) throw new InvalidOperationException("result image stream is null");
        s.Seek(0, 0, IntPtr.Zero);  // STREAM_SEEK_SET, rewind to be safe
        using (FileStream fs = File.Create(path)) {
            byte[] buf = new byte[1048576];
            IntPtr pcb = Marshal.AllocHGlobal(8);
            try {
                while (true) {
                    s.Read(buf, buf.Length, pcb);
                    int read = Marshal.ReadInt32(pcb);
                    if (read <= 0) break;
                    fs.Write(buf, 0, read);
                }
            } finally {
                Marshal.FreeHGlobal(pcb);
            }
        }
    }
}
'@

function Build-Stage([string]$MarkerContent) {
  if (Test-Path $work) { Remove-Item $work -Recurse -Force }
  New-Item -ItemType Directory -Force -Path "$work\sub" | Out-Null
  Set-Content -Path "$work\marker.txt" -Value $MarkerContent
  Set-Content -Path "$work\sub\inner.txt" -Value 'nested content'
}

# FsiFileSystems: ISO9660=1, Joliet=2, UDF=4
function New-Iso([int]$FsFlag, [string]$Label, [string]$Marker, [string]$Out) {
  Build-Stage $Marker
  $fsi = New-Object -ComObject IMAPI2FS.MsftFileSystemImage
  $fsi.VolumeName = $Label
  $fsi.FileSystemsToCreate = $FsFlag
  $fsi.Root.AddTree($work, $false)
  $result = $fsi.CreateResultImage()
  $stream = $result.ImageStream   # real property name per imapi2fs.h
  $ado = $null
  try {
    $ado = New-Object -ComObject ADODB.Stream
    $ado.Type = 1  # binary
    $ado.Open()
    $ado.Write($stream)   # ADODB.Write needs a byte array; IStream -> fallback
    $ado.SaveToFile($Out, 2)  # overwrite
    $ado.Close()
    $ado = $null
  } catch {
    if ($null -ne $ado) { try { $ado.Close() } catch {} }
    Write-Host "ADODB path failed; using .NET IStream fallback"
    [IStreamSaver]::SaveResultImage($result, $Out)
  }
  [System.Runtime.InteropServices.Marshal]::ReleaseComObject($fsi) | Out-Null
  Write-Host "created $Out ($((Get-Item $Out).Length) bytes)"
}

New-Iso 1 'ISO9660TEST' 'ISO9660-MOUNT-OK' (Join-Path $PWD 'iso9660_test.iso')
New-Iso 4 'UDFTEST'     'UDF-MOUNT-OK'     (Join-Path $PWD 'udf_test.iso')
Remove-Item $work -Recurse -Force

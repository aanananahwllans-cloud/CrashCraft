# Captures the CrashCraft (c1) window as it appears on screen. Usage: capture.ps1 [out.png]
param([string]$Out = "$PSScriptRoot\..\_tmp\screen.png", [string]$Proc = "c1")
Add-Type -AssemblyName System.Drawing
if (-not ("SmCap" -as [type])) {
    Add-Type @"
using System; using System.Runtime.InteropServices;
public class SmCap {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@
}
[SmCap]::SetProcessDPIAware() | Out-Null
$p = Get-Process $Proc -ErrorAction Stop | Select-Object -First 1
$r = New-Object SmCap+RECT
[SmCap]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
$w = $r.R - $r.L; $h = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
$bmp.Save($Out)
$g.Dispose(); $bmp.Dispose()
"$Out ($w x $h)"

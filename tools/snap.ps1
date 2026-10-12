# ytm-lite の窓だけを撮る(前面に出さない)。検証用。
#   powershell -File tools\snap.ps1 -ProcId <PID> -Out <png> [-Scale 0.5]
param([int]$ProcId, [string]$Out, [double]$Scale = 0.5)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class W {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    public static IntPtr Find(uint want) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            var sb = new StringBuilder(64); GetClassName(h, sb, 64);
            if (pid == want && sb.ToString() == "ytm-lite") { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
"@
[W]::SetProcessDPIAware() | Out-Null
$h = [W]::Find([uint32]$ProcId)
if ($h -eq [IntPtr]::Zero) { Write-Output "window not found"; exit 1 }
$r = New-Object W+RECT
[W]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.R - $r.L; $hh = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap $w, $hh
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
[W]::PrintWindow($h, $dc, 2) | Out-Null   # PW_RENDERFULLCONTENT
$g.ReleaseHdc($dc); $g.Dispose()
$sw = [int]($w * $Scale); $sh = [int]($hh * $Scale)
$small = New-Object System.Drawing.Bitmap $bmp, $sw, $sh
$small.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
Write-Output "saved $Out ($w x $hh -> $sw x $sh)"

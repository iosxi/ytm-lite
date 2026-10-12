# Move yum-lite's seek bar to a position (seconds) without input events. Test helper.
#   powershell -File tools\seek.ps1 -ProcId <PID> -Sec <seconds>
# Finds the first trackbar (the seek bar), sets its position with TBM_SETPOS (no pointers),
# then posts WM_HSCROLL(TB_ENDTRACK) to the main window, which performs the seek.
param([int]$ProcId, [int]$Sec)
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class K {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr p, IntPtr a, string c, string w);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    public static IntPtr Find(uint want) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            var sb = new StringBuilder(64); GetClassName(h, sb, 64);
            if (pid == want && sb.ToString() == "yum-lite") { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
"@
$h = [K]::Find([uint32]$ProcId)
if ($h -eq [IntPtr]::Zero) { Write-Output "window not found"; exit 1 }
$tb = [K]::FindWindowEx($h, [IntPtr]::Zero, "msctls_trackbar32", $null)
$max = [K]::SendMessage($tb, 0x402, [IntPtr]0, [IntPtr]0)          # TBM_GETRANGEMAX
[K]::SendMessage($tb, 0x405, [IntPtr]1, [IntPtr]$Sec) | Out-Null   # TBM_SETPOS
[K]::PostMessage($h, 0x114, [IntPtr]8, $tb) | Out-Null             # WM_HSCROLL, TB_ENDTRACK
Write-Output "seek to $Sec s (max $max)"

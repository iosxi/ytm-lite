# ytm-lite の窓へメッセージを PostMessage する(キー操作は送らない)。検証用。
#   powershell -File tools\post.ps1 -ProcId <PID> -Msg <番号> [-WParam n] [-LParam n]
#   例: WM_COMMAND(0x111) で再生ボタン(202)  -> -Msg 0x111 -WParam 202
#       WM_HOTKEY(0x312) 1=再生/一時停止 2=次 3=前 4=停止
param([int]$ProcId, [int]$Msg, [long]$WParam = 0, [long]$LParam = 0)
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class P {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
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
$h = [P]::Find([uint32]$ProcId)
if ($h -eq [IntPtr]::Zero) { Write-Output "window not found"; exit 1 }
[P]::PostMessage($h, [uint32]$Msg, [IntPtr]$WParam, [IntPtr]$LParam) | Out-Null
Write-Output "posted"

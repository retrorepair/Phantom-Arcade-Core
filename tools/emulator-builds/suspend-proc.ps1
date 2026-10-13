# suspend-proc.ps1 PID SECONDS - freeze a process (NtSuspendProcess) for SECONDS, then resume it.
param([int]$ProcId, [int]$Seconds)
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class Nt {
    [DllImport("ntdll.dll")] public static extern int NtSuspendProcess(IntPtr h);
    [DllImport("ntdll.dll")] public static extern int NtResumeProcess(IntPtr h);
}
"@
$p = [System.Diagnostics.Process]::GetProcessById($ProcId)
$t0 = Get-Date
[void][Nt]::NtSuspendProcess($p.Handle)
"suspended pid $ProcId at $($t0.ToString('HH:mm:ss.fff'))"
Start-Sleep -Seconds $Seconds
[void][Nt]::NtResumeProcess($p.Handle)
"resumed at $((Get-Date).ToString('HH:mm:ss.fff'))"

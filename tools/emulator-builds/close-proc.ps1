# close-proc.ps1 PID - ask a process to close its main window (WM_CLOSE), the way a user quitting would.
param([int]$ProcId)
$p = Get-Process -Id $ProcId -ErrorAction SilentlyContinue
if (-not $p) { "not running"; exit }
"closing pid $ProcId at $((Get-Date).ToString('HH:mm:ss.fff'))"
[void]$p.CloseMainWindow()
[void]$p.WaitForExit(8000)
"exited=$($p.HasExited) at $((Get-Date).ToString('HH:mm:ss.fff'))"

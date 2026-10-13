# run-xemu-gpu.ps1 -Dir DIR -Seconds N -Tag NAME - run xemu on Halo 2 (Groovy log level 1), sampling the GPU once a second, then close it.
# Writes xemu-perf\NAME.log / NAME.log.err (xemu's output) and NAME.gpu.csv (nvidia-smi samples). Reads only; changes no settings.
param([string]$Dir, [int]$Seconds = 70, [string]$Tag)
$base = 'C:\Users\joelw\xemu-perf'
$out  = "$base\$Tag.log"
$gpu  = "$base\$Tag.gpu.csv"
Remove-Item $out, "$out.err", $gpu -ErrorAction SilentlyContinue
$p = Start-Process -FilePath "$Dir\xemu.exe" `
        -ArgumentList '-config_path', "$base\xemu-test.toml", '-dvd_path', '"C:/Games/Xbox/eps-h2u/eps-h2u.iso"' `
        -WorkingDirectory $Dir -PassThru -RedirectStandardOutput $out -RedirectStandardError "$out.err"
$t0 = Get-Date
"time,sm_mhz,mem_mhz,util_gpu,util_mem,pstate,power_w,temp_c,throttle" | Out-File $gpu -Encoding ascii
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    if ($p.HasExited) { "xemu exited at $([int]((Get-Date)-$t0).TotalSeconds) s, code $($p.ExitCode)"; break }
    $line = & nvidia-smi --query-gpu=clocks.sm,clocks.mem,utilization.gpu,utilization.memory,pstate,power.draw,temperature.gpu,clocks_throttle_reasons.active --format=csv,noheader,nounits 2>$null
    "$([int]((Get-Date)-$t0).TotalSeconds),$($line -replace ' ','')" | Out-File $gpu -Append -Encoding ascii
    Start-Sleep -Milliseconds 800
}
$p.Refresh()
if (-not $p.HasExited) {
    "alive at end: cpu $([math]::Round($p.CPU,1))s, mem $([math]::Round($p.WorkingSet64/1MB)) MB"
    [void]$p.CloseMainWindow(); [void]$p.WaitForExit(8000)
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
}
"stdout $((Get-Item $out -ErrorAction SilentlyContinue).Length) B, samples $((Get-Content $gpu).Count - 1)"

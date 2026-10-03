<#
  mister_serial.ps1 - talk to the MiSTer's HPS console over the DE10-Nano's USB UART.

  The HPS console is the one debug channel that keeps working when the core is wedged,
  the network is down, or the screen is showing nothing, which is exactly when you need
  it. MiSTer's main binary logs to it, so the launcher's [PHANTOM] lines land here.

    .\mister_serial.ps1 -Command "uname -a"
    .\mister_serial.ps1 -Command "ip -4 addr show eth0" -Timeout 6
    .\mister_serial.ps1 -Listen -Timeout 20          # just watch

  -Login sends the root credentials first (MiSTer default: root / 1).
#>
param(
    [string]$Port = "COM3",
    [int]$Baud = 115200,
    [string]$Command = "",
    [int]$Timeout = 5,
    [switch]$Listen,
    [switch]$Login
)

$ErrorActionPreference = "Stop"

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, "None", 8, "One"
$sp.ReadTimeout = 500
$sp.WriteTimeout = 1000
$sp.NewLine = "`n"
# No hardware flow control: the DE10-Nano's UART has no RTS/CTS wired, and leaving
# handshaking on makes writes block forever.
$sp.Handshake = "None"
$sp.DtrEnable = $true
$sp.RtsEnable = $true

try {
    $sp.Open()
} catch {
    Write-Output "ERROR: cannot open $Port - $($_.Exception.Message)"
    exit 1
}

function Read-For([int]$seconds) {
    $sb = New-Object System.Text.StringBuilder
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        try {
            $chunk = $sp.ReadExisting()
            if ($chunk) { [void]$sb.Append($chunk) }
        } catch {}
        Start-Sleep -Milliseconds 100
    }
    return $sb.ToString()
}

# Wait for a prompt rather than sleeping a fixed amount. Fixed delays race the getty:
# send "root" while it is still printing the banner and it is taken as the password.
function Wait-For([string[]]$patterns, [int]$seconds) {
    $sb = New-Object System.Text.StringBuilder
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        try {
            $chunk = $sp.ReadExisting()
            if ($chunk) { [void]$sb.Append($chunk) }
        } catch {}
        $seen = $sb.ToString()
        foreach ($p in $patterns) {
            if ($seen -match $p) { return @{ Matched = $p; Text = $seen } }
        }
        Start-Sleep -Milliseconds 80
    }
    return @{ Matched = $null; Text = $sb.ToString() }
}

try {
    if ($Login) {
        # Nudge the getty, then drive it prompt by prompt.
        $sp.Write("`n")
        $r = Wait-For @('login:', '[#\$] $') 6
        if ($r.Matched -eq 'login:') {
            $sp.Write("root`n")
            $r = Wait-For @('Password:', '[#\$] $') 6
            if ($r.Matched -eq 'Password:') {
                $sp.Write("1`n")
                $r = Wait-For @('[#\$] ') 8
            }
        }
        [void]$sp.ReadExisting()
    }

    if ($Listen) {
        Write-Output (Read-For $Timeout)
    }
    elseif ($Command) {
        [void]$sp.ReadExisting()      # drop anything already buffered
        $sp.Write("$Command`n")
        Write-Output (Read-For $Timeout)
    }
    else {
        $sp.Write("`n")
        Write-Output (Read-For 2)
    }
}
finally {
    $sp.Close()
}

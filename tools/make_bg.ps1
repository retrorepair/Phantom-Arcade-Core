<#
.SYNOPSIS
  Turn any picture (PNG, JPG, BMP...) into the 720x480 file the Phantom Arcade launcher puts behind its menu.

.DESCRIPTION
  The launcher reads /media/fat/config/phantom_bg.ppm on the MiSTer: a binary PPM (P6), 720x480. This makes one.

  The cabinet's picture is shown on a 4:3 screen, so by default the source is centre-cropped to 4:3 first, which keeps
  what is in it from being stretched; -Fit Stretch uses the whole picture and squeezes it to fit instead.

  Copy the result to the card (Samba, FTP or scp) as /media/fat/config/phantom_bg.ppm. It is picked up the next time
  the menu comes back to the screen; delete the file to go back to the plain menu. Optional settings, in
  /media/fat/config/phantom.ini under [LAUNCHER]:  BG_DIM=50 (brightness the picture is dimmed to, percent),
  BG_PANELS=200 and BG_LIST=102 (how solid the panels and the game list are, out of 255; 102 is 60% see-through).

.EXAMPLE
  .\make_bg.ps1 -In "C:\pics\ssf2t-title.png" -Out phantom_bg.ppm
#>
param(
    [Parameter(Mandatory = $true)][string]$In,
    [string]$Out = 'phantom_bg.ppm',
    [ValidateSet('Crop', 'Stretch')][string]$Fit = 'Crop'
)
Add-Type -AssemblyName System.Drawing

$src = [System.Drawing.Image]::FromFile((Resolve-Path $In))
try {
    $sx = 0; $sy = 0; $sw = $src.Width; $sh = $src.Height
    if ($Fit -eq 'Crop') {
        $want = 4.0 / 3.0
        if (($sw / $sh) -gt $want) { $nw = [int]($sh * $want); $sx = [int](($sw - $nw) / 2); $sw = $nw }
        else                       { $nh = [int]($sw / $want); $sy = [int](($sh - $nh) / 2); $sh = $nh }
    }
    $bmp = New-Object System.Drawing.Bitmap 720, 480
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.DrawImage($src, (New-Object System.Drawing.Rectangle 0, 0, 720, 480), $sx, $sy, $sw, $sh, [System.Drawing.GraphicsUnit]::Pixel)
    $g.Dispose()

    $data = $bmp.LockBits((New-Object System.Drawing.Rectangle 0, 0, 720, 480), 'ReadOnly', 'Format24bppRgb')
    $bytes = New-Object byte[] (720 * 480 * 3)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
    $bmp.UnlockBits($data)
    for ($i = 0; $i -lt $bytes.Length; $i += 3) { $b = $bytes[$i]; $bytes[$i] = $bytes[$i + 2]; $bytes[$i + 2] = $b }   # BGR -> RGB

    $hdr = [System.Text.Encoding]::ASCII.GetBytes("P6`n720 480`n255`n")
    $path = if ([System.IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path (Get-Location) $Out }
    $fs = [System.IO.File]::Create($path)
    $fs.Write($hdr, 0, $hdr.Length); $fs.Write($bytes, 0, $bytes.Length); $fs.Close()
    "wrote $path (720x480, $([math]::Round((720*480*3 + $hdr.Length)/1KB)) KB); copy it to /media/fat/config/phantom_bg.ppm on the MiSTer"
    $bmp.Dispose()
}
finally { $src.Dispose() }

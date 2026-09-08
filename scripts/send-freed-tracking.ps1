<#
.SYNOPSIS
    Sends FreeD D1 camera-tracking datagrams to a running Director.

.DESCRIPTION
    A stand-in for a real tracker, so the Live Data panel and the inner-frustum
    path can be driven without a camera on the stage. It emits the same 29-byte
    D1 message a FreeD encoder does, including the protocol's checksum, so the
    Director's parser accepts or refuses it for exactly the same reasons it
    would accept or refuse the real thing.

    Add a Camera from the Library and select it. In the Inspector's TRACKING
    section press "+ New feed", pick the feed that appears, and set its State
    to Listening with the same Port and Camera ID used here. A camera with a
    feed is a tracked camera. Then open Window -> Live Data and run this.

    For two cameras, add a second one and give it its own feed on another port;
    set Render Order higher on whichever plate should be on top where the two
    overlap.

.PARAMETER Port
    UDP port the Director is listening on. Must match the authored source.

.PARAMETER CameraId
    Camera id stamped into every datagram. A Director configured for a
    different id counts these under "Other camera" and tracks nothing, which
    is worth seeing once on purpose.

.PARAMETER Rate
    Datagrams per second. Real trackers run at the camera's frame rate.

.PARAMETER Seconds
    How long to send for. Use 0 to run until Ctrl+C.

.PARAMETER Motion
    Orbit  - dollies in a circle looking inward, the useful default: parallax
             on the wall is the whole point of the feature.
    Pan    - fixed position, sweeping left and right.
    Static - one unchanging pose, for checking that a held pose is steady.

.PARAMETER Corrupt
    Sends one in every ten datagrams with a broken checksum. Use it to see the
    refusal counters move while good packets keep tracking.

.EXAMPLE
    .\scripts\send-freed-tracking.ps1 -Port 40000
.EXAMPLE
    .\scripts\send-freed-tracking.ps1 -Port 40000 -Motion Pan -Rate 48
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidateRange(1, 65535)][int]$Port,
    [ValidateRange(0, 255)][int]$CameraId = 0,
    [ValidateRange(1, 240)][int]$Rate = 60,
    [ValidateRange(0, 86400)][int]$Seconds = 0,
    [ValidateSet('Orbit', 'Pan', 'Static')][string]$Motion = 'Orbit',
    [string]$Address = '127.0.0.1',
    [switch]$Corrupt
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# 24-bit big-endian two's complement, which is how every numeric field in a D1
# message is carried.
function Add-Signed24 {
    param([System.Collections.Generic.List[byte]]$Buffer, [int]$Value)
    $raw = $Value -band 0x00FFFFFF
    $Buffer.Add([byte](($raw -shr 16) -band 0xFF))
    $Buffer.Add([byte](($raw -shr 8) -band 0xFF))
    $Buffer.Add([byte]($raw -band 0xFF))
}

# Angles are 1/32768 of a degree; positions are 1/64 of a millimetre.
function ConvertTo-AngleCounts {
    param([double]$Degrees)
    return [int][math]::Round($Degrees * 32768.0)
}

function ConvertTo-PositionCounts {
    param([double]$Metres)
    return [int][math]::Round($Metres * 64000.0)
}

function New-FreeDDatagram {
    param(
        [int]$Camera,
        [double]$PanDegrees,
        [double]$TiltDegrees,
        [double]$RollDegrees,
        [double]$X,
        [double]$Y,
        [double]$Z,
        [bool]$Break
    )
    $bytes = [System.Collections.Generic.List[byte]]::new()
    $bytes.Add([byte]0xD1)
    $bytes.Add([byte]$Camera)
    Add-Signed24 $bytes (ConvertTo-AngleCounts $PanDegrees)
    Add-Signed24 $bytes (ConvertTo-AngleCounts $TiltDegrees)
    Add-Signed24 $bytes (ConvertTo-AngleCounts $RollDegrees)
    Add-Signed24 $bytes (ConvertTo-PositionCounts $X)
    Add-Signed24 $bytes (ConvertTo-PositionCounts $Y)
    Add-Signed24 $bytes (ConvertTo-PositionCounts $Z)
    Add-Signed24 $bytes 0   # zoom encoder: no calibration table, so no value
    Add-Signed24 $bytes 0   # focus encoder, likewise
    $bytes.Add([byte]0)     # reserved
    $bytes.Add([byte]0)

    # The protocol defines the checksum as the value that brings the sum of
    # every byte in the message to 0x40, modulo 256.
    $sum = 0x40
    foreach ($value in $bytes) { $sum -= $value }
    $checksum = $sum -band 0xFF
    if ($Break) { $checksum = ($checksum + 1) -band 0xFF }
    $bytes.Add([byte]$checksum)
    return $bytes.ToArray()
}

$client = [System.Net.Sockets.UdpClient]::new()
try {
    $endpoint = [System.Net.IPEndPoint]::new(
        [System.Net.IPAddress]::Parse($Address), $Port)
    $interval = [System.TimeSpan]::FromSeconds(1.0 / $Rate)
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    $sent = 0
    $broken = 0

    Write-Output ("Sending FreeD D1 to {0}:{1} as camera {2} at {3} Hz ({4})." -f `
        $Address, $Port, $CameraId, $Rate, $Motion)
    if ($Seconds -eq 0) { Write-Output 'Ctrl+C to stop.' }

    while ($true) {
        $elapsed = $watch.Elapsed.TotalSeconds
        if ($Seconds -gt 0 -and $elapsed -ge $Seconds) { break }

        switch ($Motion) {
            'Orbit' {
                # A four metre circle at eye height, three metres back from the
                # stage origin, always looking inward. This is the motion that
                # makes wrong parallax obvious: the geometry on the wall has to
                # shift against itself as the camera travels.
                $angle = $elapsed * 0.25 * 2.0 * [math]::PI
                $radius = 4.0
                $x = $radius * [math]::Sin($angle)
                $z = $radius * [math]::Cos($angle)
                $pan = -($angle * 180.0 / [math]::PI)
                $tilt = 0.0
            }
            'Pan' {
                $x = 0.0
                $z = 3.0
                $pan = 25.0 * [math]::Sin($elapsed * 0.5 * 2.0 * [math]::PI)
                $tilt = 5.0 * [math]::Sin($elapsed * 0.31 * 2.0 * [math]::PI)
            }
            default {
                $x = 0.0
                $z = 3.0
                $pan = 0.0
                $tilt = 0.0
            }
        }

        $break = $Corrupt.IsPresent -and (($sent % 10) -eq 9)
        $datagram = New-FreeDDatagram -Camera $CameraId -PanDegrees $pan `
            -TiltDegrees $tilt -RollDegrees 0.0 -X $x -Y 1.6 -Z $z `
            -Break $break
        [void]$client.Send($datagram, $datagram.Length, $endpoint)
        $sent++
        if ($break) { $broken++ }

        if (($sent % ($Rate * 5)) -eq 0) {
            Write-Output ("  {0} sent, {1} deliberately corrupt, {2:N0}s elapsed" -f `
                $sent, $broken, $elapsed)
        }
        Start-Sleep -Milliseconds ([math]::Max(1, $interval.TotalMilliseconds))
    }
    Write-Output ("Done. {0} datagrams sent, {1} deliberately corrupt." -f $sent, $broken)
} finally {
    $client.Close()
}

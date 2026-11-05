<#
.SYNOPSIS
    Stands in for OptiTrack Motive: streams NatNet frames of data to the
    multicast group and answers connect requests on the command port.

.DESCRIPTION
    Drives the Director's NatNet feed without a camera volume. It does the two
    things Motive does that the Director depends on:

      * answers a connect request on the command port (1510) with a server
        info reply naming a NatNet version, which the Director must learn
        before it decodes a single frame;
      * multicasts NatNet 4.1 frames of data to 239.255.42.99:1511 carrying one
        rigid body - the camera - as a position and a quaternion.

    Add a Camera from the Library and select it. In the Inspector's TRACKING
    section press "+ New feed", pick the feed, set Protocol to NatNet, Port to
    1511, Rigid body to the id used here (or 0 for the first body in every
    frame), and State to Listening. Then open Window -> Live Data and run this.
    The "No handshake" count rises until the first connect request is
    answered, then the Server row names this script and the pose rows move.

.PARAMETER RigidBodyId
    Streaming id of the rigid body carried as the camera.

.PARAMETER Group
    Multicast group to stream to. Motive's default is 239.255.42.99.

.PARAMETER DataPort
    Port the frames go to. Motive's default is 1511.

.PARAMETER CommandPort
    Port connect requests are answered on. Motive's default is 1510.

.PARAMETER Rate
    Frames per second.

.PARAMETER Seconds
    How long to stream for. Use 0 to run until Ctrl+C.

.PARAMETER Motion
    Orbit  - dollies in a circle looking inward.
    Pan    - fixed position, sweeping left and right.
    Static - one unchanging pose.

.PARAMETER Occlude
    Marks one frame in every ten as untracked, the way an occluded rigid body
    is streamed. Use it to see the "Not tracked" count move while the pose
    holds.

.PARAMETER Version
    NatNet version to claim and to write frames in: 4.1 (Motive 3.1 and later)
    or 3.0 (Motive 2.x and 3.0, which 4.0 also reads). The Director has to
    decode either from the version alone.

.EXAMPLE
    .\scripts\send-natnet-tracking.ps1 -RigidBodyId 1
.EXAMPLE
    .\scripts\send-natnet-tracking.ps1 -RigidBodyId 1 -Motion Pan -Version 3.0
#>
[CmdletBinding()]
param(
    [ValidateRange(0, 2147483647)][int]$RigidBodyId = 1,
    [string]$Group = '239.255.42.99',
    [ValidateRange(1, 65535)][int]$DataPort = 1511,
    [ValidateRange(1, 65535)][int]$CommandPort = 1510,
    [ValidateRange(1, 240)][int]$Rate = 60,
    [ValidateRange(0, 86400)][int]$Seconds = 0,
    [ValidateSet('Orbit', 'Pan', 'Static')][string]$Motion = 'Orbit',
    [ValidateSet('4.1', '3.0')][string]$Version = '4.1',
    [switch]$Occlude
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$sized = $Version -eq '4.1'
$major = [int]$Version.Split('.')[0]
$minor = [int]$Version.Split('.')[1]

function Add-U16 { param($Buffer, [int]$Value) $Buffer.AddRange([System.BitConverter]::GetBytes([uint16]$Value)) }
function Add-I32 { param($Buffer, [int]$Value) $Buffer.AddRange([System.BitConverter]::GetBytes([int32]$Value)) }
function Add-U32 { param($Buffer, [long]$Value) $Buffer.AddRange([System.BitConverter]::GetBytes([uint32]$Value)) }
function Add-U64 { param($Buffer, [long]$Value) $Buffer.AddRange([System.BitConverter]::GetBytes([uint64]$Value)) }
function Add-F32 { param($Buffer, [double]$Value) $Buffer.AddRange([System.BitConverter]::GetBytes([single]$Value)) }
function Add-F64 { param($Buffer, [double]$Value) $Buffer.AddRange([System.BitConverter]::GetBytes([double]$Value)) }

# An empty section: a zero count and, in 4.1, a zero byte size.
function Add-EmptySection {
    param($Buffer)
    Add-I32 $Buffer 0
    if ($sized) { Add-I32 $Buffer 0 }
}

function New-ServerInfo {
    $payload = [System.Collections.Generic.List[byte]]::new()
    $name = [System.Text.Encoding]::ASCII.GetBytes('send-natnet-tracking')
    $field = New-Object byte[] 256
    [Array]::Copy($name, $field, $name.Length)
    $payload.AddRange($field)
    $payload.AddRange([byte[]]@(0, 1, 0, 0))          # application version
    $payload.AddRange([byte[]]@($major, $minor, 0, 0)) # NatNet version
    if ($major -ge 3) {
        Add-U64 $payload 10000000                      # clock frequency
        Add-U16 $payload $DataPort
        $payload.Add([byte]1)                          # multicast
        $payload.AddRange([System.Net.IPAddress]::Parse($Group).GetAddressBytes())
        $payload.Add([byte]0)                          # struct padding
    }
    $datagram = [System.Collections.Generic.List[byte]]::new()
    Add-U16 $datagram 1                                # NAT_SERVERINFO
    Add-U16 $datagram $payload.Count
    $datagram.AddRange($payload)
    return $datagram.ToArray()
}

function New-Frame {
    param([int]$FrameNumber, [double]$X, [double]$Y, [double]$Z,
          [double]$PanDegrees, [double]$TiltDegrees, [bool]$Tracked)
    $payload = [System.Collections.Generic.List[byte]]::new()
    Add-I32 $payload $FrameNumber
    Add-EmptySection $payload   # marker sets
    Add-EmptySection $payload   # unlabeled markers

    # One rigid body. Orientation is Y(pan) * X(tilt) as a quaternion, in the
    # Y-up frame Motive streams by default.
    $body = [System.Collections.Generic.List[byte]]::new()
    Add-I32 $body $RigidBodyId
    Add-F32 $body $X; Add-F32 $body $Y; Add-F32 $body $Z
    $hp = $PanDegrees * [math]::PI / 360.0
    $ht = $TiltDegrees * [math]::PI / 360.0
    $qy = @(0.0, [math]::Sin($hp), 0.0, [math]::Cos($hp))
    $qx = @([math]::Sin($ht), 0.0, 0.0, [math]::Cos($ht))
    # Hamilton product qy * qx, components (x, y, z, w). One statement each:
    # a comma at the end of a line binds tighter than the arithmetic after it.
    $wx = $qy[3] * $qx[0] + $qy[0] * $qx[3] + $qy[1] * $qx[2] - $qy[2] * $qx[1]
    $wy = $qy[3] * $qx[1] - $qy[0] * $qx[2] + $qy[1] * $qx[3] + $qy[2] * $qx[0]
    $wz = $qy[3] * $qx[2] + $qy[0] * $qx[1] - $qy[1] * $qx[0] + $qy[2] * $qx[3]
    $ww = $qy[3] * $qx[3] - $qy[0] * $qx[0] - $qy[1] * $qx[1] - $qy[2] * $qx[2]
    Add-F32 $body $wx; Add-F32 $body $wy; Add-F32 $body $wz; Add-F32 $body $ww
    Add-F32 $body 0.0003        # mean error
    Add-U16 $body $(if ($Tracked) { 1 } else { 0 })
    Add-I32 $payload 1
    if ($sized) { Add-I32 $payload $body.Count }
    $payload.AddRange($body)

    Add-EmptySection $payload   # skeletons
    if ($sized) { Add-EmptySection $payload }   # assets (4.1)
    Add-EmptySection $payload   # labeled markers
    Add-EmptySection $payload   # force plates
    Add-EmptySection $payload   # devices
    Add-U32 $payload 0; Add-U32 $payload 0      # timecode
    Add-F64 $payload ([double]$FrameNumber / $Rate)
    Add-U64 $payload 0; Add-U64 $payload 0; Add-U64 $payload 0
    if ($sized) { Add-U32 $payload 0; Add-U32 $payload 0 }
    Add-U16 $payload 0          # frame params
    Add-I32 $payload 0          # end of data

    $datagram = [System.Collections.Generic.List[byte]]::new()
    Add-U16 $datagram 7         # NAT_FRAMEOFDATA
    Add-U16 $datagram $payload.Count
    $datagram.AddRange($payload)
    return $datagram.ToArray()
}

$groupAddress = [System.Net.IPAddress]::Parse($Group)
$data = [System.Net.Sockets.UdpClient]::new()
$command = [System.Net.Sockets.UdpClient]::new()
try {
    $command.Client.SetSocketOption(
        [System.Net.Sockets.SocketOptionLevel]::Socket,
        [System.Net.Sockets.SocketOptionName]::ReuseAddress, $true)
    $command.Client.Bind([System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any, $CommandPort))
    $destination = [System.Net.IPEndPoint]::new($groupAddress, $DataPort)
    $reply = New-ServerInfo
    $interval = 1.0 / $Rate
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    $sent = 0
    $occluded = 0
    $answered = 0

    Write-Output ("Streaming NatNet {0} frames to {1}:{2} as rigid body {3} at {4} Hz ({5}); answering connects on {6}." -f `
        $Version, $Group, $DataPort, $RigidBodyId, $Rate, $Motion, $CommandPort)
    if ($Seconds -eq 0) { Write-Output 'Ctrl+C to stop.' }

    while ($true) {
        $elapsed = $watch.Elapsed.TotalSeconds
        if ($Seconds -gt 0 -and $elapsed -ge $Seconds) { break }

        # Answer every connect request that has arrived, to wherever it came
        # from - which is the Director's data socket.
        while ($command.Available -gt 0) {
            $from = [System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any, 0)
            $request = $command.Receive([ref]$from)
            if ($request.Length -ge 2 -and $request[0] -eq 0 -and $request[1] -eq 0) {
                [void]$command.Send($reply, $reply.Length, $from)
                $answered++
                Write-Output ("  answered connect request from {0}" -f $from)
            }
        }

        switch ($Motion) {
            'Orbit' {
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

        $tracked = -not ($Occlude.IsPresent -and (($sent % 10) -eq 9))
        $frame = New-Frame -FrameNumber ($sent + 1) -X $x -Y 1.6 -Z $z `
            -PanDegrees $pan -TiltDegrees $tilt -Tracked $tracked
        [void]$data.Send($frame, $frame.Length, $destination)
        $sent++
        if (-not $tracked) { $occluded++ }

        if (($sent % ($Rate * 5)) -eq 0) {
            Write-Output ("  {0} frames, {1} untracked, {2} connects answered, {3:N0}s elapsed" -f `
                $sent, $occluded, $answered, $elapsed)
        }
        Start-Sleep -Milliseconds ([math]::Max(1, [int]($interval * 1000)))
    }
    Write-Output ("Done. {0} frames sent, {1} untracked, {2} connects answered." -f $sent, $occluded, $answered)
} finally {
    $data.Close()
    $command.Close()
}

<#
.SYNOPSIS
  Launches the replicated 3D scene demo: one Director plus two output nodes, each
  drawing its own half of a shared 2560x720 canvas.

.DESCRIPTION
  The Director authors a built-in scene (orbiting camera, spinning box, bobbing
  sphere, a ground plane and a key light) into the document and previews the
  whole canvas. Node 0 draws the left 1280x720 slice, node 1 the right. Every
  node evaluates the same replicated document at the same show frame, so the two
  windows are one continuous image split down the middle - place them side by
  side to see the wall.

  Each window's status line prints a scene digest. At the same show frame that
  digest is identical on the Director and both nodes; that equality is the
  "perfectly replicated" guarantee, checked directly rather than by eye.

  Close any window (or press Esc/Q on the Director preview, which is the only
  window that takes keyboard input) to stop. Press Enter in this console to tear
  the whole demo down.

.PARAMETER Exe
  Path to media-server.exe. Defaults to the windows-msvc-debug-gui build.

.PARAMETER Port
  TCP port the Director listens on. Default 45050.
#>
param(
    [string]$Exe = "$PSScriptRoot/../out/build/windows-msvc-debug-gui/apps/server/media-server.exe",
    [int]$Port = 45050
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Exe)) {
    throw "media-server.exe not found at $Exe. Build the windows-msvc-debug-gui preset first, or pass -Exe."
}

# Identities are 32-hex (128-bit). The values are arbitrary but the show-id must
# match across the Director and every node, and each node needs a distinct
# node-id.
$showId  = '00000000000000010000000000000002'
$epoch   = '00000000000000030000000000000004'
$incarn  = '00000000000000000000000000000001'
$node0Id = '0000000000000000000000000000000A'
$node1Id = '0000000000000000000000000000000B'
$listen  = "127.0.0.1:$Port"
$wal     = Join-Path $env:TEMP 'scene-demo.wal'

$procs = @()
function Start-Role([string[]]$roleArgs, [string]$title) {
    $script:procs += Start-Process -FilePath $Exe -PassThru -ArgumentList $roleArgs
    Write-Host "  started $title"
}

try {
    Write-Host "Director listening on $listen; authoring the built-in scene..."
    # Director previews the whole 2560x720 canvas (32:9) in a 1280x360 window.
    Start-Role @(
        '--role=director', "--wal=$wal", "--director-listen=$listen",
        "--show-id=$showId", "--director-epoch=$epoch",
        '--playback', '--scene', '--surface=1280x360',
        '--window-at=40,40', '--status-interval=60') 'director (whole canvas)'

    Start-Sleep -Milliseconds 1000

    # Two nodes, each a 1280x720 slice, placed side by side so the seam lines up.
    Start-Role @(
        '--role=output', "--director-connect=$listen", "--show-id=$showId",
        "--node-id=$node0Id", "--node-incarnation=$incarn",
        '--playback', '--scene', '--node-index=0', '--surface=1280x720',
        '--window-at=40,440', '--status-interval=60') 'node 0 (left slice)'

    Start-Role @(
        '--role=output', "--director-connect=$listen", "--show-id=$showId",
        "--node-id=$node1Id", "--node-incarnation=$incarn",
        '--playback', '--scene', '--node-index=1', '--surface=1280x720',
        '--window-at=1330,440', '--status-interval=60') 'node 1 (right slice)'

    Write-Host ''
    Write-Host 'Three windows are open: the Director preview (top) and the two node'
    Write-Host 'slices (bottom, side by side). Press Enter here to stop the demo.'
    [void](Read-Host)
}
finally {
    foreach ($p in $procs) {
        if ($p -and -not $p.HasExited) { $p.Kill() }
    }
    Write-Host 'demo stopped.'
}

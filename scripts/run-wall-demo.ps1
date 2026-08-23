<#
.SYNOPSIS
  Launches the wall demo: one Director plus two output nodes, each drawing its
  own region of a shared 2560x720 canvas through the output stage.

.DESCRIPTION
  This is the scene demo put through the OUTPUT stage. The Director authors both
  the built-in scene and a built-in feed map: node 0 outputs the canvas columns
  [0, 1280) and node 1 outputs [1280, 2560). This is an LED wall - the two
  regions abut at column 1280 with no overlap, so the seam is pixel-exact and
  there is nothing to blend. Adjacency is guaranteed by construction: the scene
  layer renders a bit-identical shared frustum plane, so the two nodes' edge
  columns line up exactly.

  The Director preview (top) shows the whole 2560x720 canvas: the wall the two
  nodes tile between them. The two node windows (bottom) show each node's half;
  placed side by side they reassemble the Director preview.

  Each window's status line prints a scene digest and a feed digest. At the same
  show frame both are identical on the Director and both nodes; that equality is
  the "perfectly replicated" guarantee, checked directly rather than by eye. The
  pixel-exact tiling is proven in the tests - mediaserver.render_output
  ("adjacent regions tile the source") and mediaserver.output_feed_map_model
  ("adjacent maps tile the canvas").

  Close any window (or press Esc/Q on the Director preview, the only window that
  takes keyboard input) to stop. Press Enter in this console to tear the whole
  demo down.

.PARAMETER Exe
  Path to media-server.exe. Defaults to the windows-msvc-debug-gui build.

.PARAMETER Port
  TCP port the Director listens on. Default 45051.
#>
param(
    [string]$Exe = "$PSScriptRoot/../out/build/windows-msvc-debug-gui/apps/server/media-server.exe",
    [int]$Port = 45051
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Exe)) {
    throw "media-server.exe not found at $Exe. Build the windows-msvc-debug-gui preset first, or pass -Exe."
}

# Identities are 32-hex (128-bit). The values are arbitrary but the show-id must
# match across the Director and every node, and each node needs a distinct
# node-id.
$showId  = '00000000000000010000000000000005'
$epoch   = '00000000000000030000000000000006'
$incarn  = '00000000000000000000000000000001'
$node0Id = '0000000000000000000000000000001A'
$node1Id = '0000000000000000000000000000001B'
$listen  = "127.0.0.1:$Port"
$wal     = Join-Path $env:TEMP 'wall-demo.wal'

$procs = @()
function Start-Role([string[]]$roleArgs, [string]$title) {
    $script:procs += Start-Process -FilePath $Exe -PassThru -ArgumentList $roleArgs
    Write-Host "  started $title"
}

try {
    Write-Host "Director listening on $listen; authoring the scene and feed map..."
    # Director previews the whole 2560x720 canvas (32:9) in a 1280x360 window.
    Start-Role @(
        '--role=director', "--wal=$wal", "--director-listen=$listen",
        "--show-id=$showId", "--director-epoch=$epoch",
        '--playback', '--output', '--surface=1280x360',
        '--window-at=40,40', '--status-interval=60') 'director (whole canvas)'

    Start-Sleep -Milliseconds 1000

    # Two nodes. Each renders a 1280x720 region at half scale (640x360), placed
    # side by side. The regions abut at column 1280 with no overlap.
    Start-Role @(
        '--role=output', "--director-connect=$listen", "--show-id=$showId",
        "--node-id=$node0Id", "--node-incarnation=$incarn",
        '--playback', '--output', '--node-index=0', '--surface=640x360',
        '--window-at=40,440', '--status-interval=60') 'node 0 (left region)'

    Start-Role @(
        '--role=output', "--director-connect=$listen", "--show-id=$showId",
        "--node-id=$node1Id", "--node-incarnation=$incarn",
        '--playback', '--output', '--node-index=1', '--surface=640x360',
        '--window-at=680,440', '--status-interval=60') 'node 1 (right region)'

    Write-Host ''
    Write-Host 'Three windows are open: the Director preview (top, the whole'
    Write-Host 'wall) and the two node regions (bottom, side by side). Press'
    Write-Host 'Enter here to stop the demo.'
    [void](Read-Host)
}
finally {
    foreach ($p in $procs) {
        if ($p -and -not $p.HasExited) { $p.Kill() }
    }
    Write-Host 'demo stopped.'
}

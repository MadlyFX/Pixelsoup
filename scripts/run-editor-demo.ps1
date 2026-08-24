<#
.SYNOPSIS
  Launches the operator editor: a Director with the ImGui editor plus one output
  node showing the same replicated scene, so a drag in the editor moves the clip
  on the node's wall.

.DESCRIPTION
  The Director opens the editor (a "Wall" viewport and an "Elements" list) and
  authors a built-in edit scene: a floor, a light, and one clip on a screen-
  facing quad. Click the clip in the Wall view and drag it - the move is authored
  as an ordinary document command, replicated to the node, whose output window
  shows the clip move in step. This is the interaction the whole direction rests
  on: the 2D view edits the one 3D scene, and "camera left" is a leftward drag.

  The clip is a short image sequence the Director writes to a temp directory, so
  the demo is self-contained; on this one machine the node reads the same files.

  Close the editor window (or press its close button) to stop. Press Enter in
  this console to tear the demo down.

.PARAMETER Exe
  Path to media-server.exe. Defaults to the windows-msvc-debug-gui build.

.PARAMETER Port
  TCP port the Director listens on. Default 45060.
#>
param(
    [string]$Exe = "$PSScriptRoot/../out/build/windows-msvc-debug-gui/apps/server/media-server.exe",
    [int]$Port = 45060
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Exe)) {
    throw "media-server.exe not found at $Exe. Build the windows-msvc-debug-gui preset first, or pass -Exe."
}

$showId  = '0000000000000001000000000000000B'
$epoch   = '0000000000000003000000000000000C'
$incarn  = '00000000000000000000000000000001'
$nodeId  = '0000000000000000000000000000002A'
$listen  = "127.0.0.1:$Port"
$wal     = Join-Path $env:TEMP 'editor-demo.wal'

$procs = @()
function Start-Role([string[]]$roleArgs, [string]$title) {
    $script:procs += Start-Process -FilePath $Exe -PassThru -ArgumentList $roleArgs
    Write-Host "  started $title"
}

try {
    Write-Host "Director listening on $listen; opening the editor..."
    # The editor on the left; --surface and --window-at size and place its
    # window just like an output shell.
    Start-Role @(
        '--role=director', "--wal=$wal", "--director-listen=$listen",
        "--show-id=$showId", "--director-epoch=$epoch",
        '--playback', '--edit', '--surface=1080x760', '--window-at=30,30',
        '--status-interval=120') 'director (editor)'

    Start-Sleep -Milliseconds 1200

    # One node shows the whole edit scene on the right, so a drag in the editor
    # is visible on a real output window as it replicates.
    Start-Role @(
        '--role=output', "--director-connect=$listen", "--show-id=$showId",
        "--node-id=$nodeId", "--node-incarnation=$incarn",
        '--playback', '--scene', '--node-index=0', '--surface=760x428',
        '--window-at=1130,30', '--status-interval=120') 'node (wall output)'

    Write-Host ''
    Write-Host 'The editor window is open (Wall + Elements). Drag the clip in the'
    Write-Host 'Wall view; the node window mirrors the move. Press Enter here to'
    Write-Host 'stop the demo.'
    [void](Read-Host)
}
finally {
    foreach ($p in $procs) {
        if ($p -and -not $p.HasExited) { $p.Kill() }
    }
    Write-Host 'demo stopped.'
}

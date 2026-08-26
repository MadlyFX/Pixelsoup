<#
.SYNOPSIS
  Launches the operator editor: a Director with the ImGui editor plus one
  lifecycle-supervised output node showing the same replicated scene, so a drag
  in the editor moves the clip on the node's wall.

.DESCRIPTION
  The Director opens the editor (a "Wall" viewport and an "Elements" list) and
  authors a built-in edit scene: a floor, a light, and one clip on a screen-
  facing quad. Click the clip in the Wall view and drag it - the move is authored
  as an ordinary document command, replicated to the node, whose output window
  shows the clip move in step. This is the interaction the whole direction rests
  on: the 2D view edits the one 3D scene, and "camera left" is a leftward drag.

  The clip is a short image sequence the Director writes to a temp directory, so
  the demo is self-contained; on this one machine the node reads the same files.
  A local lifecycle listener owns and auto-starts the output process, allowing
  Output Monitor to start, restart, and stop it after a matching Render Server
  is configured in Hardware Map.

  Close the editor window (or press its close button) to stop. Press Enter in
  this console to tear the demo down.

.PARAMETER Exe
  Path to media-server.exe. Defaults to the packaged executable beside the
  scripts directory, then falls back to the windows-msvc-debug-gui build.

.PARAMETER Port
  TCP port the Director listens on. Default 45060.

.PARAMETER LifecycleSecretFile
  Shared lifecycle credential used by the Director and local listener. If the
  file does not exist, the script creates a cryptographically random demo
  secret there. Defaults to a shared file in the system temp directory.

.PARAMETER LifecyclePortBase
  TCP port for node 0's local lifecycle listener. Default 47840.
#>
param(
    [string]$Exe = "",
    [int]$Port = 45060,
    [string]$LifecycleSecretFile = (Join-Path ([IO.Path]::GetTempPath()) 'mediaserver-demo-lifecycle.secret'),
    [int]$LifecyclePortBase = 47840
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($Exe)) {
    $packagedExe = Join-Path $PSScriptRoot '../media-server.exe'
    $Exe = if (Test-Path -LiteralPath $packagedExe -PathType Leaf) {
        $packagedExe
    } else {
        Join-Path $PSScriptRoot '../out/build/windows-msvc-debug-gui/apps/server/media-server.exe'
    }
}
if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) {
    throw "media-server.exe not found at $Exe. Extract the complete release package, build the windows-msvc-debug-gui preset, or pass -Exe."
}
$Exe = (Resolve-Path -LiteralPath $Exe).ProviderPath

if ($Port -lt 1 -or $Port -gt 65535) {
    throw "Director port must be between 1 and 65535 (received $Port)."
}
if ($LifecyclePortBase -lt 1 -or $LifecyclePortBase -gt 61440) {
    throw "LifecyclePortBase must be between 1 and 61440 so every project node index remains addressable (received $LifecyclePortBase)."
}
if ($LifecyclePortBase -eq $Port) {
    throw "Lifecycle port $LifecyclePortBase conflicts with Director port $Port."
}
if ([string]::IsNullOrWhiteSpace($LifecycleSecretFile)) {
    throw 'LifecycleSecretFile cannot be empty.'
}

if (Test-Path -LiteralPath $LifecycleSecretFile) {
    $secretItem = Get-Item -LiteralPath $LifecycleSecretFile
    if ($secretItem.PSIsContainer) {
        throw "LifecycleSecretFile must name a file, not a directory: $LifecycleSecretFile"
    }
    $LifecycleSecretFile = $secretItem.FullName
} else {
    $LifecycleSecretFile = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($LifecycleSecretFile)
    $secretParent = [IO.Path]::GetDirectoryName($LifecycleSecretFile)
    if ([string]::IsNullOrWhiteSpace($secretParent) -or
        -not (Test-Path -LiteralPath $secretParent -PathType Container)) {
        throw "Lifecycle secret directory does not exist: $secretParent"
    }

    $secretBytes = New-Object byte[] 32
    $random = [Security.Cryptography.RandomNumberGenerator]::Create()
    try {
        $random.GetBytes($secretBytes)
    } finally {
        $random.Dispose()
    }
    $secretHex = [BitConverter]::ToString($secretBytes).Replace('-', '')
    [IO.File]::WriteAllText($LifecycleSecretFile, $secretHex, [Text.Encoding]::ASCII)
    Write-Host "Created demo lifecycle secret at $LifecycleSecretFile"
}

$secretContent = [IO.File]::ReadAllBytes($LifecycleSecretFile)
if ($secretContent.Length -eq 0 -or $secretContent.Length -gt 258 -or
    $secretContent -contains 0) {
    throw "Lifecycle secret must be at most 258 raw bytes and contain no NUL bytes: $LifecycleSecretFile"
}
$secretLength = $secretContent.Length
while ($secretLength -gt 0 -and
       ($secretContent[$secretLength - 1] -eq 10 -or
        $secretContent[$secretLength - 1] -eq 13)) {
    --$secretLength
}
if ($secretLength -lt 32 -or $secretLength -gt 256) {
    throw "Lifecycle secret must contain 32-256 bytes after trailing CR/LF is removed; $LifecycleSecretFile contains $secretLength."
}

$showId  = '0000000000000001000000000000000B'
$epoch   = '0000000000000003000000000000000C'
$incarn  = '00000000000000000000000000000001'
$nodeId  = '0000000000000000000000000000002A'
$listen  = "127.0.0.1:$Port"
$wal     = Join-Path $env:TEMP 'editor-demo.wal'
$lifecycleListen = "127.0.0.1:$LifecyclePortBase"

$procs = @()
function ConvertTo-NativeArgument([string]$value) {
    if ($value.Length -gt 0 -and $value -notmatch '[\s"]') {
        return $value
    }

    $quoted = New-Object Text.StringBuilder
    [void]$quoted.Append('"')
    $backslashes = 0
    foreach ($character in $value.ToCharArray()) {
        if ($character -eq '\') {
            ++$backslashes
            continue
        }
        if ($character -eq '"') {
            [void]$quoted.Append(('\' * (($backslashes * 2) + 1)))
            [void]$quoted.Append('"')
            $backslashes = 0
            continue
        }
        if ($backslashes -gt 0) {
            [void]$quoted.Append(('\' * $backslashes))
            $backslashes = 0
        }
        [void]$quoted.Append($character)
    }
    if ($backslashes -gt 0) {
        [void]$quoted.Append(('\' * ($backslashes * 2)))
    }
    [void]$quoted.Append('"')
    return $quoted.ToString()
}

function Start-Role([string[]]$roleArgs, [string]$title) {
    $argumentLine = ($roleArgs | ForEach-Object { ConvertTo-NativeArgument $_ }) -join ' '
    $script:procs += Start-Process -FilePath $Exe -PassThru -ArgumentList $argumentLine
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
        '--status-interval=120',
        "--lifecycle-secret-file=$LifecycleSecretFile") 'director (editor)'

    Start-Sleep -Milliseconds 1200

    # A standalone local listener owns node 0. It auto-starts the same child the
    # script used to launch directly, and appends --node-index=0 itself.
    Start-Role @(
        "--lifecycle-listen=$lifecycleListen",
        '--lifecycle-node-index=0',
        "--lifecycle-secret-file=$LifecycleSecretFile",
        "--lifecycle-child=$Exe",
        '--lifecycle-autostart',
        '--lifecycle-child-arg=--role=render,output',
        "--lifecycle-child-arg=--director-connect=$listen",
        "--lifecycle-child-arg=--show-id=$showId",
        "--lifecycle-child-arg=--node-id=$nodeId",
        "--lifecycle-child-arg=--node-incarnation=$incarn",
        '--lifecycle-child-arg=--playback',
        # Physical/feed mapping is part of the output path. --scene would
        # intentionally bypass FeedMap and present the ordinary camera view.
        '--lifecycle-child-arg=--output',
        '--lifecycle-child-arg=--borderless',
        '--lifecycle-child-arg=--surface=1920x1080',
        '--lifecycle-child-arg=--window-at=3440,0',
        '--lifecycle-child-arg=--status-interval=120') 'node 0 lifecycle listener + wall output'

    Write-Host ''
    Write-Host 'The editor window is open (Wall + Elements). Drag the clip in the'
    Write-Host 'Wall view; the node window mirrors the move.'
    Write-Host "Lifecycle control is listening at $lifecycleListen. In Hardware"
    Write-Host 'Map, configure a Render Server with Node index 0, leave Host blank'
    Write-Host "for local, and keep the project Control port at $LifecyclePortBase."
    Write-Host 'Window mode/Desktop X/Y/size in that card override the child launch'
    Write-Host 'placement as soon as the project reaches the node.'
    Write-Host 'Press Enter here to stop the demo.'
    [void](Read-Host)
}
finally {
    foreach ($p in $procs) {
        if ($p -and -not $p.HasExited) { $p.Kill() }
    }
    Write-Host 'demo stopped.'
}

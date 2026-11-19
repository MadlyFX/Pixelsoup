<#
.SYNOPSIS
  Launches the wall demo: one Director plus two lifecycle-supervised output
  nodes, each drawing its own region of a shared 2560x720 canvas through the
  output stage.

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

  Local lifecycle listeners own and auto-start both output processes, matching
  the production process-supervision path while preserving the one-command
  demo. Each window's status line prints a scene digest and a feed digest. At
  the same
  show frame both are identical on the Director and both nodes; that equality is
  the "perfectly replicated" guarantee, checked directly rather than by eye. The
  pixel-exact tiling is proven in the tests - mediaserver.render_output
  ("adjacent regions tile the source") and mediaserver.output_feed_map_model
  ("adjacent maps tile the canvas").

  Close any window (or press Esc/Q on the Director preview, the only window that
  takes keyboard input) to stop. Press Enter in this console to tear the whole
  demo down.

.PARAMETER Exe
  Path to pixelsoup.exe. Defaults to the packaged executable beside the
  scripts directory, then the windows-msvc-release-gui build, and finally the
  windows-msvc-debug-gui build.

.PARAMETER Port
  TCP port the Director listens on. Default 45051.

.PARAMETER LifecycleSecretFile
  Shared lifecycle credential used by both local listeners. If the file does
  not exist, the script creates a cryptographically random demo secret there.
  Defaults to a shared file in the system temp directory.

.PARAMETER LifecyclePortBase
  TCP port for node 0's local lifecycle listener; node 1 uses the next port.
  Default 47840.
#>
param(
    [string]$Exe = "",
    [int]$Port = 45051,
    [string]$LifecycleSecretFile = (Join-Path ([IO.Path]::GetTempPath()) 'mediaserver-demo-lifecycle.secret'),
    [int]$LifecyclePortBase = 47840
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($Exe)) {
    $packagedExe = Join-Path $PSScriptRoot '../pixelsoup.exe'
    $releaseExe = Join-Path $PSScriptRoot '../out/build/windows-msvc-release-gui/apps/server/pixelsoup.exe'
    $debugExe = Join-Path $PSScriptRoot '../out/build/windows-msvc-debug-gui/apps/server/pixelsoup.exe'
    $Exe = if (Test-Path -LiteralPath $packagedExe -PathType Leaf) {
        $packagedExe
    } elseif (Test-Path -LiteralPath $releaseExe -PathType Leaf) {
        $releaseExe
    } else {
        $debugExe
    }
}
if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) {
    throw "pixelsoup.exe not found at $Exe. Extract the complete release package, build a GUI preset, or pass -Exe."
}
$Exe = (Resolve-Path -LiteralPath $Exe).ProviderPath

if ($Port -lt 1 -or $Port -gt 65535) {
    throw "Director port must be between 1 and 65535 (received $Port)."
}
if ($LifecyclePortBase -lt 1 -or $LifecyclePortBase -gt 61440) {
    throw "LifecyclePortBase must be between 1 and 61440 so every project node index remains addressable (received $LifecyclePortBase)."
}
if ($Port -eq $LifecyclePortBase -or $Port -eq ($LifecyclePortBase + 1)) {
    throw "Lifecycle listener range conflicts with Director port $Port."
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
$lifecycleListen0 = "127.0.0.1:$LifecyclePortBase"
$lifecycleListen1 = "127.0.0.1:$($LifecyclePortBase + 1)"

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
    Write-Host "Director listening on $listen; authoring the scene and feed map..."
    # Director previews the whole 2560x720 canvas (32:9) in a 1280x360 window.
    Start-Role @(
        '--role=director', "--wal=$wal", "--director-listen=$listen",
        "--show-id=$showId", "--director-epoch=$epoch",
        '--playback', '--output', '--surface=1280x360',
        '--window-at=40,40', '--status-interval=60') 'director (whole canvas)'

    Start-Sleep -Milliseconds 1000

    # Two listeners own and auto-start the mapped output regions. Each listener
    # appends its configured --node-index to the child itself.
    Start-Role @(
        "--lifecycle-listen=$lifecycleListen0",
        '--lifecycle-node-index=0',
        "--lifecycle-secret-file=$LifecycleSecretFile",
        "--lifecycle-child=$Exe",
        '--lifecycle-autostart',
        '--lifecycle-child-arg=--role=render,output',
        "--lifecycle-child-arg=--director-connect=$listen",
        "--lifecycle-child-arg=--show-id=$showId",
        "--lifecycle-child-arg=--node-id=$node0Id",
        "--lifecycle-child-arg=--node-incarnation=$incarn",
        '--lifecycle-child-arg=--playback',
        '--lifecycle-child-arg=--output',
        '--lifecycle-child-arg=--surface=640x360',
        '--lifecycle-child-arg=--window-at=40,440',
        '--lifecycle-child-arg=--status-interval=60') 'node 0 lifecycle listener + left region'

    Start-Role @(
        "--lifecycle-listen=$lifecycleListen1",
        '--lifecycle-node-index=1',
        "--lifecycle-secret-file=$LifecycleSecretFile",
        "--lifecycle-child=$Exe",
        '--lifecycle-autostart',
        '--lifecycle-child-arg=--role=render,output',
        "--lifecycle-child-arg=--director-connect=$listen",
        "--lifecycle-child-arg=--show-id=$showId",
        "--lifecycle-child-arg=--node-id=$node1Id",
        "--lifecycle-child-arg=--node-incarnation=$incarn",
        '--lifecycle-child-arg=--playback',
        '--lifecycle-child-arg=--output',
        '--lifecycle-child-arg=--surface=640x360',
        '--lifecycle-child-arg=--window-at=680,440',
        '--lifecycle-child-arg=--status-interval=60') 'node 1 lifecycle listener + right region'

    Write-Host ''
    Write-Host 'Three windows are open: the Director preview (top, the whole'
    Write-Host 'wall) and the two node regions (bottom, side by side). Lifecycle'
    Write-Host "listeners are at $lifecycleListen0 and $lifecycleListen1."
    Write-Host 'Press Enter here to stop the demo.'
    [void](Read-Host)
}
finally {
    foreach ($p in $procs) {
        if ($p -and -not $p.HasExited) { $p.Kill() }
    }
    Write-Host 'demo stopped.'
}

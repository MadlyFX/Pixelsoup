<#
.SYNOPSIS
  Launches the replicated 3D scene demo: one Director plus two lifecycle-
  supervised output nodes, each drawing its own half of a shared 2560x720
  canvas.

.DESCRIPTION
  The Director authors a built-in scene (orbiting camera, spinning box, bobbing
  sphere, a ground plane and a key light) into the document and previews the
  whole canvas. Node 0 draws the left 1280x720 slice, node 1 the right. Every
  node evaluates the same replicated document at the same show frame, so the two
  windows are one continuous image split down the middle - place them side by
  side to see the wall.

  Local lifecycle listeners own and auto-start both output processes, matching
  the production process-supervision path while preserving the one-command
  demo. Each window's status line prints a scene digest. At the same show frame,
  that
  digest is identical on the Director and both nodes; that equality is the
  "perfectly replicated" guarantee, checked directly rather than by eye.

  Close any window (or press Esc/Q on the Director preview, which is the only
  window that takes keyboard input) to stop. Press Enter in this console to tear
  the whole demo down.

.PARAMETER Exe
  Path to media-server.exe. Defaults to the packaged executable beside the
  scripts directory, then falls back to the windows-msvc-debug-gui build.

.PARAMETER Port
  TCP port the Director listens on. Default 45050.

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
    [int]$Port = 45050,
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
$showId  = '00000000000000010000000000000002'
$epoch   = '00000000000000030000000000000004'
$incarn  = '00000000000000000000000000000001'
$node0Id = '0000000000000000000000000000000A'
$node1Id = '0000000000000000000000000000000B'
$listen  = "127.0.0.1:$Port"
$wal     = Join-Path $env:TEMP 'scene-demo.wal'
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
    Write-Host "Director listening on $listen; authoring the built-in scene..."
    # Director previews the whole 2560x720 canvas (32:9) in a 1280x360 window.
    Start-Role @(
        '--role=director', "--wal=$wal", "--director-listen=$listen",
        "--show-id=$showId", "--director-epoch=$epoch",
        '--playback', '--scene', '--surface=1280x360',
        '--window-at=40,40', '--status-interval=60') 'director (whole canvas)'

    Start-Sleep -Milliseconds 1000

    # Two listeners own and auto-start the 1280x720 output slices. Each listener
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
        '--lifecycle-child-arg=--scene',
        '--lifecycle-child-arg=--surface=1280x720',
        '--lifecycle-child-arg=--window-at=40,440',
        '--lifecycle-child-arg=--status-interval=60') 'node 0 lifecycle listener + left slice'

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
        '--lifecycle-child-arg=--scene',
        '--lifecycle-child-arg=--surface=1280x720',
        '--lifecycle-child-arg=--window-at=1330,440',
        '--lifecycle-child-arg=--status-interval=60') 'node 1 lifecycle listener + right slice'

    Write-Host ''
    Write-Host 'Three windows are open: the Director preview (top) and the two node'
    Write-Host 'slices (bottom, side by side). Lifecycle listeners are available at'
    Write-Host "$lifecycleListen0 and $lifecycleListen1. Press Enter here to stop the demo."
    [void](Read-Host)
}
finally {
    foreach ($p in $procs) {
        if ($p -and -not $p.HasExited) { $p.Kill() }
    }
    Write-Host 'demo stopped.'
}

<#
.SYNOPSIS
  Runs one render/output node that connects to a Director started by
  scripts/run-editor.ps1, normally on another machine.

.DESCRIPTION
  This is the "render machine" half of a two-machine setup. It starts no
  Director and opens no editor.

  By default it runs the machine-local lifecycle agent, which owns the render
  process and starts it immediately. That is the mode worth using: the agent is
  what lets Output Monitor on the control machine START, RESTART, and STOP this
  node, and what reports its sync device, house reference, and display lock
  state. Pass -Direct to skip the agent and run the render process alone - no
  remote control, but one fewer moving part when first proving connectivity.

  The lifecycle secret must match the control machine's. Either copy that file
  here, or paste the value run-editor.ps1 printed into -Secret.

  Windows Firewall must allow the agent's control port inbound for Output
  Monitor to reach it. This script deliberately does not change firewall rules;
  from an elevated prompt on this machine:

    netsh advfirewall firewall add rule name="Media Server Lifecycle" `
      dir=in action=allow protocol=TCP localport=47840

  Press Ctrl+C in this console to stop.

.PARAMETER DirectorHost
  Numeric address of the machine running run-editor.ps1. Required. Host names
  are not accepted - the executable takes numeric endpoints only.

.PARAMETER Port
  Director TCP port. Default 45060. Must match run-editor.ps1's -Port.

.PARAMETER NodeIndex
  Which slice of the wall this process drives, and the index entered in the
  Director's Hardware Map Render Server card. Default 0. Use a distinct value
  per render machine; the node identity is derived from it.

.PARAMETER Secret
  The lifecycle secret text printed by run-editor.ps1. Written to
  -LifecycleSecretFile. Omit it if you copied that file here instead.

.PARAMETER Direct
  Run the render process directly instead of under the lifecycle agent.
  Output Monitor cannot start, restart, or stop a node started this way.

.PARAMETER Build
  Which executable to run: auto (default), packaged, release, or debug.

.PARAMETER SyncSimulate
  Fabricated physical output timing reported by the agent so the sync workflow
  can be rehearsed without a genlock card: off (default), locked,
  reference-lost, reference-idle, or no-device. Simulated samples are marked as
  such all the way to the operator surface and satisfy only a DEGRADED policy.

.PARAMETER DryRun
  Print the exact command line and exit without starting anything.
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$DirectorHost,
    [int]$Port = 45060,
    [int]$NodeIndex = 0,
    [string]$Secret = '',
    [string]$LifecycleSecretFile = (Join-Path $env:LOCALAPPDATA 'MediaServer/cluster-lifecycle.secret'),
    [switch]$Direct,
    [ValidateSet('auto', 'packaged', 'release', 'debug')]
    [string]$Build = 'auto',
    [string]$Exe = '',
    [string]$LifecycleListenAddress = '0.0.0.0',
    [int]$LifecyclePort = 47840,
    [string]$AssetStore = '',
    [string]$Surface = '1920x1080',
    [string]$WindowAt = '0,0',
    [switch]$Windowed,
    [switch]$Fullscreen,
    [string]$ShowId = '0000000000000001000000000000000B',
    [string]$NodeId = '',
    [string]$Incarnation = '00000000000000000000000000000001',
    [ValidateSet('off', 'locked', 'reference-lost', 'reference-idle', 'no-device')]
    [string]$SyncSimulate = 'off',
    [int]$StatusInterval = 120,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'cluster-common.ps1')

$Exe = Resolve-MediaServerExe -Exe $Exe -Build $Build -ScriptRoot $PSScriptRoot
if ($Port -lt 1 -or $Port -gt 65535) {
    throw "Director port must be between 1 and 65535 (received $Port)."
}
if ($LifecyclePort -lt 1 -or $LifecyclePort -gt 65535) {
    throw "Lifecycle port must be between 1 and 65535 (received $LifecyclePort)."
}
if ($Fullscreen -and $Windowed) {
    throw 'Choose either -Fullscreen or -Windowed, not both.'
}

Assert-NumericAddress -Address $DirectorHost -Label 'DirectorHost' -RejectUnspecified
Assert-NumericAddress -Address $LifecycleListenAddress -Label 'LifecycleListenAddress'
Assert-Id128 -Value $ShowId -Label 'ShowId'
Assert-Id128 -Value $Incarnation -Label 'Incarnation'
if ([string]::IsNullOrWhiteSpace($NodeId)) {
    $NodeId = Get-NodeIdForIndex -NodeIndex $NodeIndex
}
Assert-Id128 -Value $NodeId -Label 'NodeId'

if (-not [string]::IsNullOrWhiteSpace($Secret)) {
    $LifecycleSecretFile = Set-LifecycleSecret -Path $LifecycleSecretFile -Secret $Secret
}

# The render process's own arguments. In agent mode these become
# --lifecycle-child-arg values; the agent appends --node-index and
# --asset-store itself.
$childArgs = @(
    '--role=render,output'
    "--director-connect=${DirectorHost}:$Port"
    "--show-id=$ShowId"
    "--node-id=$NodeId"
    "--node-incarnation=$Incarnation"
    '--playback'
    # Physical/feed mapping is part of the output path. --scene would
    # intentionally bypass the Feed Map and present the plain camera view.
    '--output'
    "--surface=$Surface"
    "--window-at=$WindowAt"
    "--status-interval=$StatusInterval"
)
if ($Fullscreen) {
    $childArgs += '--fullscreen'
} elseif (-not $Windowed) {
    $childArgs += '--borderless'
}

if ($Direct) {
    $argv = $childArgs + @("--node-index=$NodeIndex")
    if (-not [string]::IsNullOrWhiteSpace($AssetStore)) {
        $argv += "--asset-store=$AssetStore"
    }
} else {
    $LifecycleSecretFile = New-LifecycleSecret -Path $LifecycleSecretFile
    # Validated here so a mismatched or malformed credential fails now rather
    # than on every control request Output Monitor sends later.
    Get-LifecycleSecretText -Path $LifecycleSecretFile | Out-Null
    $argv = @(
        "--lifecycle-listen=${LifecycleListenAddress}:$LifecyclePort"
        "--lifecycle-node-index=$NodeIndex"
        "--lifecycle-secret-file=$LifecycleSecretFile"
        "--lifecycle-child=$Exe"
        '--lifecycle-autostart'
        "--sync-simulate=$SyncSimulate"
    )
    foreach ($childArg in $childArgs) {
        $argv += "--lifecycle-child-arg=$childArg"
    }
    if (-not [string]::IsNullOrWhiteSpace($AssetStore)) {
        $argv += "--asset-store=$AssetStore"
    }
}

$addresses = Get-LocalIPv4Address
$thisMachine = if ($addresses.Count -gt 0) { $addresses[0] } else { '<this-machine-ip>' }

Write-Host ''
Write-Host 'Media Server - render/output node' -ForegroundColor Cyan
Write-Host "  executable   $Exe"
Write-Host "  director     ${DirectorHost}:$Port"
Write-Host "  node index   $NodeIndex"
Write-Host "  node id      $NodeId"
if ($Direct) {
    Write-Host '  mode         direct (no lifecycle agent; Output Monitor cannot control this node)' -ForegroundColor Yellow
} else {
    Write-Host "  mode         lifecycle agent on ${LifecycleListenAddress}:$LifecyclePort, autostart"
    Write-Host "  secret file  $LifecycleSecretFile"
    Write-Host "  sync report  $SyncSimulate"
    Write-Host ''
    Write-Host 'In the editor on the control machine, open Hardware Map and add a' -ForegroundColor Cyan
    Write-Host 'Render Server with:'
    Write-Host "  Node index    $NodeIndex"
    Write-Host "  Host          $thisMachine"
    Write-Host "  Control port  $LifecyclePort"
    if ($addresses.Count -gt 1) {
        Write-Host "  (other addresses on this machine: $($addresses[1..($addresses.Count - 1)] -join ', '))"
    }
}
Write-Host ''

if ($DryRun) {
    Write-Host 'Command line (dry run):' -ForegroundColor Yellow
    Write-Host ("  `"$Exe`" " + ($argv -join ' '))
    return
}

if (-not $Direct -and [string]::IsNullOrWhiteSpace($Secret)) {
    Write-Host 'Using the existing secret file. If Output Monitor reports the node as' -ForegroundColor Yellow
    Write-Host 'unreachable, confirm it matches the control machine, or rerun with -Secret.' -ForegroundColor Yellow
    Write-Host ''
}

Write-Host 'Press Ctrl+C to stop.'
Write-Host ''
& $Exe @argv
exit $LASTEXITCODE

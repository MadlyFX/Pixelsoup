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

  -Password must match the render node password in the control machine's
  Preferences. With none on both sides the node can be controlled without
  one - by any Media Server on the network.

  The agent also answers discovery, so the editor's Hardware Map lists this
  machine under DETECTED ON NETWORK - drag it onto the map and the Render
  Server card is already filled in. The answer carries this machine's name,
  node index, control port, and whether it has a password; never the password.

  Windows Firewall must allow the agent's control port (TCP) and the discovery
  port (UDP 47839) inbound for the editor to find and control it. This script
  deliberately does not change firewall rules; from an elevated prompt on this
  machine (47840 is node 0's control port; node N uses 47840 + N):

    netsh advfirewall firewall add rule name="Media Server Lifecycle" `
      dir=in action=allow protocol=TCP localport=47840
    netsh advfirewall firewall add rule name="Media Server Discovery" `
      dir=in action=allow protocol=UDP localport=47839

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

.PARAMETER Password
  The render node password set in the control machine's Preferences. Omit it
  when none is set there.

.PARAMETER LifecycleSecretFile
  Optional file holding the password instead of -Password.

.PARAMETER LifecyclePort
  TCP port the agent listens on for Output Monitor. Default 47840 + NodeIndex,
  which is where the editor's Hardware Map looks for node N with the default
  project control port.

.PARAMETER DiscoveryPort
  UDP port the agent answers discovery on. Default 47839; 0 turns it off.

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
    [string]$Password = '',
    [string]$LifecycleSecretFile = '',
    [switch]$Direct,
    [ValidateSet('auto', 'packaged', 'release', 'debug')]
    [string]$Build = 'auto',
    [string]$Exe = '',
    [string]$LifecycleListenAddress = '0.0.0.0',
    [int]$LifecyclePort = 0,
    [int]$DiscoveryPort = 47839,
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
if ($LifecyclePort -eq 0) {
    $LifecyclePort = 47840 + $NodeIndex
}
if ($LifecyclePort -lt 1 -or $LifecyclePort -gt 65535) {
    throw "Lifecycle port must be between 1 and 65535 (received $LifecyclePort)."
}
if ($DiscoveryPort -lt 0 -or $DiscoveryPort -gt 65535) {
    throw "Discovery port must be between 0 and 65535 (received $DiscoveryPort)."
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


# The render process's own arguments. In agent mode these become
# --lifecycle-child-arg values; the agent appends --node-index itself.
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
} else {
    $argv = @(
        "--lifecycle-listen=${LifecycleListenAddress}:$LifecyclePort"
        "--lifecycle-node-index=$NodeIndex"
        "--lifecycle-child=$Exe"
        '--lifecycle-autostart'
        "--sync-simulate=$SyncSimulate"
        "--lifecycle-discovery-port=$DiscoveryPort"
    )
    foreach ($childArg in $childArgs) {
        $argv += "--lifecycle-child-arg=$childArg"
    }
    if (-not [string]::IsNullOrEmpty($Password)) {
        $argv += "--lifecycle-password=$Password"
    } elseif (-not [string]::IsNullOrWhiteSpace($LifecycleSecretFile)) {
        $argv += "--lifecycle-secret-file=$LifecycleSecretFile"
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
    if (-not [string]::IsNullOrEmpty($Password)) {
        Write-Host '  password     set (-Password)'
    } elseif (-not [string]::IsNullOrWhiteSpace($LifecycleSecretFile)) {
        Write-Host "  password     from $LifecycleSecretFile"
    } else {
        Write-Host '  password     none - any Media Server on this network can control this node' -ForegroundColor Yellow
    }
    Write-Host "  sync report  $SyncSimulate"
    Write-Host ''
    Write-Host 'In the editor on the control machine, open Hardware Map: this machine' -ForegroundColor Cyan
    if ($DiscoveryPort -ne 0) {
        Write-Host 'appears under DETECTED ON NETWORK - drag it onto the map. If it does not,'
        Write-Host "allow UDP $DiscoveryPort and TCP $LifecyclePort through this machine's firewall,"
        Write-Host 'or add a Render Server by hand with:'
    } else {
        Write-Host 'add a Render Server with:'
    }
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

if (-not $Direct) {
    Write-Host 'If Output Monitor reports "password rejected", give this script the' -ForegroundColor Yellow
    Write-Host 'render node password from the editor''s Preferences with -Password.' -ForegroundColor Yellow
    Write-Host ''
}

Write-Host 'Press Ctrl+C to stop.'
Write-Host ''
& $Exe @argv
exit $LASTEXITCODE

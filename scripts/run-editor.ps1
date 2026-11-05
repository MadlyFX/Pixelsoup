<#
.SYNOPSIS
  Runs the Director with the operator editor and nothing else, listening for
  render/output nodes on the network.

.DESCRIPTION
  This is the "control machine" half of a two-machine setup. It opens the
  editor and a Director listener; it starts no output node of its own. Pair it
  with scripts/run-output-node.ps1 on each render machine.

  On start it prints what the other machine needs: this machine's addresses
  and the Director port. Output Monitor controls render machines with the
  render node password set in the editor's Preferences (none by default, which
  any Media Server on the network can use). -Password overrides it for one run.

  The Director listens on 0.0.0.0 by default so a node on another machine can
  reach it. Windows Firewall will normally prompt, or block silently, the first
  time. If nodes cannot connect, allow the port yourself from an elevated
  prompt - this script deliberately does not change firewall rules:

    netsh advfirewall firewall add rule name="Media Server Director" `
      dir=in action=allow protocol=TCP localport=45060

  Render machines can discover this editor automatically on the local IPv4
  network. Allow its discovery port as well:

    netsh advfirewall firewall add rule name="Media Server Discovery" `
      dir=in action=allow protocol=UDP localport=47839

  Close the editor window to stop, or press Ctrl+C in this console.

.PARAMETER Build
  Which executable to run: auto (default), packaged, release, or debug.
  'auto' prefers a packaged media-server.exe beside the scripts directory,
  then the windows-msvc-release-gui build, then windows-msvc-debug-gui.

.PARAMETER Exe
  Explicit path to media-server.exe. Overrides -Build.

.PARAMETER ListenAddress
  Numeric address the Director listens on. Default 0.0.0.0 (all interfaces).
  Use 127.0.0.1 to refuse everything except this machine.

.PARAMETER Port
  Director TCP port. Default 45060. Must match the node script's -Port.

.PARAMETER Password
  Render node password for this run, overriding the one in Preferences. Give
  each render machine the same -Password. Omit it to use Preferences.

.PARAMETER Wal
  Director write-ahead log. Default is a per-user file, not a temp file, so
  recovery survives a reboot.

.PARAMETER DryRun
  Print the exact command line and exit without starting anything.
#>
param(
    [ValidateSet('auto', 'packaged', 'release', 'debug')]
    [string]$Build = 'auto',
    [string]$Exe = '',
    [string]$ListenAddress = '0.0.0.0',
    [int]$Port = 45060,
    [string]$Password = '',
    [string]$Wal = (Join-Path $env:LOCALAPPDATA 'MediaServer/editor.wal'),
    [string]$Surface = '1600x900',
    [string]$WindowAt = '40,40',
    [string]$ShowId = '0000000000000001000000000000000B',
    [string]$Epoch = '0000000000000003000000000000000C',
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
Assert-NumericAddress -Address $ListenAddress -Label 'ListenAddress'
Assert-Id128 -Value $ShowId -Label 'ShowId'
Assert-Id128 -Value $Epoch -Label 'Epoch'


$Wal = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Wal)
$walParent = [IO.Path]::GetDirectoryName($Wal)
if (-not (Test-Path -LiteralPath $walParent -PathType Container)) {
    New-Item -ItemType Directory -Path $walParent -Force | Out-Null
}

$argv = @(
    '--role=director'
    "--wal=$Wal"
    "--director-listen=${ListenAddress}:$Port"
    "--show-id=$ShowId"
    "--director-epoch=$Epoch"
    '--playback'
    '--edit'
    "--surface=$Surface"
    "--window-at=$WindowAt"
    "--status-interval=$StatusInterval"
)
if (-not [string]::IsNullOrEmpty($Password)) {
    $argv += "--lifecycle-password=$Password"
}

Write-Host ''
Write-Host 'Media Server - Director + editor' -ForegroundColor Cyan
Write-Host "  executable   $Exe"
Write-Host "  listening    ${ListenAddress}:$Port"
Write-Host "  wal          $Wal"
if ([string]::IsNullOrEmpty($Password)) {
    Write-Host '  password     from Preferences (Render nodes)'
} else {
    Write-Host '  password     given with -Password for this run'
}
Write-Host ''
Write-Host 'On each render machine, run:' -ForegroundColor Cyan
$addresses = Get-LocalIPv4Address
$suggested = if ($addresses.Count -gt 0) { $addresses[0] } else { '<this-machine-ip>' }
Write-Host "  .\scripts\run-output-node.ps1 -DirectorHost $suggested -Port $Port -NodeIndex 0"
Write-Host '  adding -Password <the render node password> if one is set.'
if ($addresses.Count -gt 1) {
    Write-Host ''
    Write-Host "  This machine's other addresses: $($addresses[1..($addresses.Count - 1)] -join ', ')"
}
Write-Host ''
Write-Host 'Then in the editor, open Hardware Map: each render machine appears under' -ForegroundColor Cyan
Write-Host '  DETECTED ON NETWORK - drag it onto the map. (By hand: a Render Server with'
Write-Host '  Node index 0, Host = the render machine IP, Control port 47840.)'
Write-Host 'Output Monitor can then START / RESTART / STOP that machine.'
Write-Host ''

if ($DryRun) {
    Write-Host 'Command line (dry run):' -ForegroundColor Yellow
    Write-Host ("  `"$Exe`" " + ($argv -join ' '))
    return
}

Write-Host 'Close the editor window or press Ctrl+C to stop.'
Write-Host ''
& $Exe @argv
exit $LASTEXITCODE

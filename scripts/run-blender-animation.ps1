[CmdletBinding()]
param(
    [ValidateSet('debug','release')][string]$Build = 'release',
    [ValidateRange(1024,65535)][int]$Port = 45211,
    [string]$Directory = '',
    [switch]$NoLaunch
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (!$Directory) { $Directory = Join-Path $repo ('out/step11-' + $Build + '-' + [guid]::NewGuid().ToString('N')) }
$Directory = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Directory)
python (Join-Path $repo 'tests/blender/test_animation_stage.py') --fixture (Join-Path $Directory 'source')
if ($LASTEXITCODE -ne 0) { throw 'Animation fixture generation failed' }
$buildDirectory = Join-Path $repo "out/build/windows-msvc-$Build-gui"
& (Join-Path $buildDirectory 'engine/render/mediaserver_linked_animation_stage_tests.exe') (Join-Path $Directory 'source') $Directory
if ($LASTEXITCODE -ne 0) { throw 'Animation acceptance failed' }
Write-Output "Animation fixture and evidence: $Directory"
if ($NoLaunch) { return }
$previousLocalAppData = $env:LOCALAPPDATA
try {
    $env:LOCALAPPDATA = Join-Path $Directory 'local'
    $arguments = @('--role=director',"--director-listen=127.0.0.1:$Port",'--playback','--edit','--show-rate=24','--surface=1600x900','--status-interval=10',
        '--show-id=00000000000000110000000000000012','--director-epoch=00000000000000130000000000000014',
        ('--wal="' + (Join-Path $Directory 'director.wal') + '"'))
    $director = Start-Process -FilePath (Join-Path $buildDirectory 'apps/server/media-server.exe') -ArgumentList $arguments `
        -WorkingDirectory $repo -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $Directory 'director.log') `
        -RedirectStandardError (Join-Path $Directory 'director.err')
    Write-Output "Director PID: $($director.Id)"
} finally { $env:LOCALAPPDATA = $previousLocalAppData }

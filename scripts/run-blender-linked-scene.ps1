[CmdletBinding()]
param(
    [ValidateSet('debug','release')][string]$Build = 'release',
    [ValidateSet('linked-r1','linked-r2','source-removed','source-restored')]
    [string]$Scene = 'linked-r1',
    [string]$EditorPath = '',
    [switch]$NoLaunch,
    [ValidateRange(1024,65535)][int]$DirectorPort = 45163
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$buildDirectory = Join-Path $repo "out/build/windows-msvc-$Build-gui"
$checker = Join-Path $buildDirectory 'engine/scene/mediaserver_scene_linked_scene_tests.exe'
$editor = Join-Path $buildDirectory 'apps/server/media-server.exe'
if ($EditorPath) { $editor = (Resolve-Path -LiteralPath $EditorPath).Path }
if (!(Test-Path -LiteralPath $checker) -or !(Test-Path -LiteralPath $editor)) {
    throw "Build windows-msvc-$Build-gui first."
}
& (Join-Path $PSScriptRoot 'run-blender-scene-reference.ps1') -Build $Build -NoLaunch
$outputDirectory = Join-Path $repo 'out/blender-scene-reference/linked'
& $checker (Join-Path $repo 'out/blender-scene-reference/qualified/reference-r1.mstage') $outputDirectory
if ($LASTEXITCODE -ne 0) { throw 'Linked scene qualification failed.' }
$project = Join-Path $outputDirectory "$Scene.mstage"
Write-Output "Linked scene project: $project"
if ($NoLaunch) { return }
$localSettings = Join-Path $outputDirectory $Scene
$wal = Join-Path $localSettings ("run-" + [guid]::NewGuid().ToString('N') + '.wal')
$previousLocalAppData = $env:LOCALAPPDATA
try {
    $env:LOCALAPPDATA = $localSettings
    $editorArguments = @('--role=director', "--director-listen=127.0.0.1:$DirectorPort",
        '--show-id=00000000000000110000000000000012',
        '--director-epoch=00000000000000130000000000000014',
        '--playback', '--edit', '--surface=1600x900', ('--wal="' + $wal + '"'))
    $process = Start-Process -FilePath $editor -ArgumentList $editorArguments `
        -WorkingDirectory $repo -WindowStyle Hidden -PassThru
    Write-Output "Linked scene editor PID: $($process.Id)"
} finally {
    $env:LOCALAPPDATA = $previousLocalAppData
}

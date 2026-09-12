<#
.SYNOPSIS
Build the step 12 delivery bundle and open the delivered project in the editor.

Produces, in one directory: the installable add-on archive and its compatibility
manifest, the measured qualification report, the offline Preview and published
Program projects, and a sample Blender scene. Then it starts a Director editor on
the delivered Preview project with isolated local settings.
#>
[CmdletBinding()]
param(
    [ValidateSet('debug','release')][string]$Build = 'release',
    [ValidateRange(1024,65535)][int]$Port = 45213,
    [string]$Directory = '',
    [string]$Blender = 'C:/Program Files/Blender Foundation/Blender 5.2/blender.exe',
    [switch]$Gpu,
    [switch]$NoBlender,
    [switch]$NoLaunch
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
# Keep the path short: immutable package names must stay inside the legacy
# Windows path limit, and this directory holds a pairing resource cache.
if (!$Directory) { $Directory = Join-Path $repo ('out/step12-' + $Build) }
$Directory = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Directory)
if (Test-Path $Directory) { Remove-Item -Recurse -Force $Directory }
New-Item -ItemType Directory -Force $Directory | Out-Null
$buildDirectory = Join-Path $repo "out/build/windows-msvc-$Build-gui"

python (Join-Path $repo 'scripts/package-blender-livelink.py') `
    --output (Join-Path $Directory 'mediaserver_livelink.zip') `
    --report (Join-Path $Directory 'mediaserver_livelink.manifest.json')
if ($LASTEXITCODE -ne 0) { throw 'Add-on packaging failed' }

$arguments = @((Join-Path $repo 'tests/fixtures/blender-scene-link'), (Join-Path $Directory 'evidence'))
if ($Gpu) { $arguments += '--gpu' }
& (Join-Path $buildDirectory 'engine/render/mediaserver_linked_qualification_tests.exe') @arguments
if ($LASTEXITCODE -ne 0) { throw 'Qualification harness failed' }

if (!$NoBlender) {
    if (!(Test-Path $Blender)) { throw "Blender not found: $Blender" }
    & $Blender --background --factory-startup --python-exit-code 1 `
        --python (Join-Path $repo 'scripts/blender-livelink-demo.py') -- `
        --save-demo (Join-Path $Directory 'sample-collection.blend')
    if ($LASTEXITCODE -ne 0) { throw 'Sample blend generation failed' }
}

Write-Output "Delivery bundle: $Directory"
Get-ChildItem -Recurse -File $Directory | Where-Object { $_.Extension -in '.zip','.json','.mstage','.blend' } |
    ForEach-Object { Write-Output ('  ' + $_.FullName.Substring($Directory.Length + 1) + '  ' + $_.Length + ' bytes') }
if ($NoLaunch) { return }

$project = Join-Path $Directory 'evidence/preview.mstage'
if (!(Test-Path $project)) { throw "Delivered project missing: $project" }
$previousLocalAppData = $env:LOCALAPPDATA
try {
    $env:LOCALAPPDATA = Join-Path $Directory 'evidence/local'
    $editorArguments = @('--role=director',"--director-listen=127.0.0.1:$Port",'--playback','--edit','--show-rate=24',
        '--surface=1600x900','--status-interval=10','--show-id=0000000000000012000000000000001a',
        '--director-epoch=0000000000000012000000000000001b',
        ('--wal="' + (Join-Path $Directory 'director.wal') + '"'))
    $director = Start-Process -FilePath (Join-Path $buildDirectory 'apps/server/media-server.exe') -ArgumentList $editorArguments `
        -WorkingDirectory $repo -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $Directory 'director.log') `
        -RedirectStandardError (Join-Path $Directory 'director.err')
    Write-Output "Director PID: $($director.Id) - open $project from File > Open Project"
} finally { $env:LOCALAPPDATA = $previousLocalAppData }

[CmdletBinding()]
param(
    [ValidateSet('debug', 'release')][string]$Build = 'release',
    [ValidateSet(1, 2)][int]$Revision = 1,
    [switch]$Benchmark,
    [switch]$GenerateBlender,
    [switch]$NoLaunch,
    [string]$BlenderPath = 'C:\Program Files\Blender Foundation\Blender 5.2\blender.exe',
    [ValidateRange(1024, 65535)][int]$DirectorPort = 45162
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$preset = "windows-msvc-$Build-gui"
$buildDirectory = Join-Path $repo "out/build/$preset"
$checker = Join-Path $buildDirectory 'engine/render/mediaserver_blender_scene_reference_tests.exe'
$editor = Join-Path $buildDirectory 'apps/server/pixelsoup.exe'
$outputDirectory = Join-Path $repo 'out/blender-scene-reference'
$qualifiedDirectory = Join-Path $outputDirectory 'qualified'
$fixtures = Join-Path $repo 'tests/fixtures/blender-scene-link'
if (!(Test-Path -LiteralPath $checker) -or !(Test-Path -LiteralPath $editor)) {
    throw "Build $preset before running the reference scene."
}
if ($GenerateBlender) {
    if (!(Test-Path -LiteralPath $BlenderPath)) { throw "Blender not found: $BlenderPath" }
    & $BlenderPath --background --factory-startup --python-exit-code 1 `
        --python (Join-Path $PSScriptRoot 'blender-scene-reference.py') -- `
        --output $outputDirectory --samples 20
    if ($LASTEXITCODE -ne 0) { throw 'Blender reference generation failed.' }
    $fixtures = $outputDirectory
}
$checkArguments = @($fixtures, $qualifiedDirectory)
if ($Benchmark) { $checkArguments += '--benchmark' }
& $checker @checkArguments
if ($LASTEXITCODE -ne 0) { throw 'Reference qualification failed.' }
$project = Join-Path $qualifiedDirectory "reference-r$Revision.mstage"
Write-Output "Reference project: $project"
if ($NoLaunch) { return }

# Scope settings to this child process so the user's recent project, workspace,
# preferences and WAL are preserved. Each launch has a fresh WAL for the chosen
# revision; an old recovery record must not replace the generated fixture.
$localSettings = Join-Path $qualifiedDirectory "ui-r$Revision"
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
    Write-Output "Reference editor PID: $($process.Id)"
} finally {
    $env:LOCALAPPDATA = $previousLocalAppData
}

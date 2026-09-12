[CmdletBinding()]
param(
    [ValidateSet('debug','release')][string]$Build = 'release',
    [ValidateRange(1024,65535)][int]$Port = 45188,
    [string]$Directory = '',
    [switch]$NoLaunch
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (!$Directory) { $Directory = Join-Path $repo ('out/step08-' + $Build + '-' + [guid]::NewGuid().ToString('N')) }
$Directory = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Directory)
$buildDirectory = Join-Path $repo "out/build/windows-msvc-$Build-gui"
$checker = Join-Path $buildDirectory 'engine/render/mediaserver_linked_publication_tests.exe'
$exe = Join-Path $buildDirectory 'apps/server/media-server.exe'
& $checker (Join-Path $repo 'tests/fixtures/blender-scene-link') $Directory
if ($LASTEXITCODE -ne 0) { throw 'Publication qualification failed.' }
# The test writes a separate one-node fixture too. This startup profile opens
# the two-node project through the ordinary recent-project path.
Copy-Item -LiteralPath (Join-Path $Directory 'two-nodes.mstage') -Destination (Join-Path $Directory 'preview.mstage') -Force
Write-Output "Publication fixture and logs: $Directory"
if ($NoLaunch) { return }
$previousLocalAppData = $env:LOCALAPPDATA
try {
    $env:LOCALAPPDATA = Join-Path $Directory 'local'
    $arguments = @('--role=director',"--director-listen=127.0.0.1:$Port",
        '--show-id=00000000000000110000000000000012','--director-epoch=00000000000000130000000000000014',
        '--playback','--edit','--show-rate=10','--surface=1600x900','--status-interval=10',
        ('--wal="' + (Join-Path $Directory ('director-' + [guid]::NewGuid().ToString('N') + '.wal')) + '"'))
    $director = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $repo -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $Directory 'director.log') -RedirectStandardError (Join-Path $Directory 'director.err')
    Write-Output "Director PID: $($director.Id)"
} finally { $env:LOCALAPPDATA = $previousLocalAppData }
foreach ($index in 0,1) {
    $nodeId = if ($index -eq 0) { '0000000000000000000000000000000a' } else { '0000000000000000000000000000000b' }
    $position = if ($index -eq 0) { '20,40' } else { '670,40' }
    $arguments = @('--role=render,output',"--director-connect=127.0.0.1:$Port",
        '--show-id=00000000000000110000000000000012',"--node-id=$nodeId",
        '--node-incarnation=00000000000000000000000000000001',"--node-index=$index",
        '--playback','--scene','--show-rate=10','--surface=640x360',"--window-at=$position",'--status-interval=10')
    $node = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $repo -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $Directory "node$index.log") -RedirectStandardError (Join-Path $Directory "node$index.err")
    Write-Output "Node $index PID: $($node.Id)"
}

<#
.SYNOPSIS
  Creates a portable Windows x64 Media Server test package.

.DESCRIPTION
  Copies the optimized executable, app-local dependency DLLs, VC++ runtime,
  demo launchers, and operator notes into a versioned folder, then emits a ZIP
  and SHA-256 checksum. Run after building windows-msvc-release-gui.
#>
param(
    [string]$BuildDir = "$PSScriptRoot/../out/build/windows-msvc-release-gui",
    [string]$Destination = "$PSScriptRoot/../out/release",
    [string]$Version = "0.1.0"
)

$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).ProviderPath
$BuildDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($BuildDir)
$Destination = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Destination)
$executable = Join-Path $BuildDir 'apps/server/media-server.exe'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Release executable not found at $executable. Build the windows-msvc-release-gui preset first."
}

$packageName = "MediaServer-$Version-windows-x64"
$packageRoot = Join-Path $Destination $packageName
$zipPath = Join-Path $Destination "$packageName.zip"
$checksumPath = "$zipPath.sha256"

New-Item -ItemType Directory -Path $Destination -Force | Out-Null
$resolvedDestination = (Resolve-Path -LiteralPath $Destination).ProviderPath
if (Test-Path -LiteralPath $packageRoot) {
    $resolvedPackage = (Resolve-Path -LiteralPath $packageRoot).ProviderPath
    $prefix = $resolvedDestination.TrimEnd('\') + '\'
    if (-not $resolvedPackage.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to replace package outside destination: $resolvedPackage"
    }
    Remove-Item -LiteralPath $resolvedPackage -Recurse -Force
}
New-Item -ItemType Directory -Path $packageRoot | Out-Null
New-Item -ItemType Directory -Path (Join-Path $packageRoot 'scripts') | Out-Null
New-Item -ItemType Directory -Path (Join-Path $packageRoot 'licenses') | Out-Null

Copy-Item -LiteralPath $executable -Destination $packageRoot
$bakeExecutable = Join-Path $BuildDir 'apps/bake/media-bake.exe'
if (-not (Test-Path -LiteralPath $bakeExecutable -PathType Leaf)) {
    throw "Build media-bake before packaging: $bakeExecutable"
}
Copy-Item -LiteralPath $bakeExecutable -Destination $packageRoot
$runtimeDir = Split-Path -Parent $executable
Get-ChildItem -LiteralPath $runtimeDir -Filter '*.dll' -File |
    Copy-Item -Destination $packageRoot

$vcRuntime = $null
if (-not [string]::IsNullOrWhiteSpace($env:VCToolsRedistDir)) {
    $candidate = Join-Path $env:VCToolsRedistDir 'x64/Microsoft.VC143.CRT'
    if (Test-Path -LiteralPath $candidate -PathType Container) {
        $vcRuntime = $candidate
    }
}
if ($null -eq $vcRuntime) {
    $redistRoots = Get-ChildItem -Path 'C:\Program Files\Microsoft Visual Studio\2022\*\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT' -Directory -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending
    if ($redistRoots.Count -gt 0) {
        $vcRuntime = $redistRoots[0].FullName
    }
}
if ($null -eq $vcRuntime) {
    throw 'Visual C++ x64 redistributable files were not found. Run from a Visual Studio 2022 developer installation.'
}
Get-ChildItem -LiteralPath $vcRuntime -Filter '*.dll' -File |
    Copy-Item -Destination $packageRoot -Force

$packagedScripts = @(
    'cluster-common.ps1',
    'run-editor.ps1',
    'run-output-node.ps1',
    'run-editor-demo.ps1',
    'run-scene-demo.ps1',
    'run-wall-demo.ps1'
)
foreach ($script in $packagedScripts) {
    Copy-Item -LiteralPath (Join-Path $repository "scripts/$script") `
        -Destination (Join-Path $packageRoot 'scripts')
}
Copy-Item -LiteralPath (Join-Path $repository 'packaging/windows/README.txt') `
    -Destination (Join-Path $packageRoot 'README.txt')

# Open in Blender starts Blender with this add-on and script; media-server.exe
# finds them in blender/ beside itself.
$blenderRoot = Join-Path $packageRoot 'blender'
New-Item -ItemType Directory -Path (Join-Path $blenderRoot 'mediaserver_livelink') | Out-Null
Copy-Item -LiteralPath (Join-Path $repository 'integrations/blender/mediaserver_open.py') -Destination $blenderRoot
Get-ChildItem -LiteralPath (Join-Path $repository 'integrations/blender/mediaserver_livelink') -Filter '*.py' -File |
    Copy-Item -Destination (Join-Path $blenderRoot 'mediaserver_livelink')

$vcpkgShare = Join-Path $BuildDir 'vcpkg_installed/x64-windows/share'
if (Test-Path -LiteralPath $vcpkgShare -PathType Container) {
    Get-ChildItem -LiteralPath $vcpkgShare -Directory | ForEach-Object {
        $copyright = Join-Path $_.FullName 'copyright'
        if (Test-Path -LiteralPath $copyright -PathType Leaf) {
            Copy-Item -LiteralPath $copyright `
                -Destination (Join-Path $packageRoot "licenses/$($_.Name).txt")
        }
    }
}

Copy-Item -LiteralPath (Join-Path $repository 'third_party/bcdec/LICENSE') `
    -Destination (Join-Path $packageRoot 'licenses/bcdec.txt')

# The worker's BC7 encoder is vendored rather than installed by vcpkg.
Copy-Item -LiteralPath (Join-Path $repository 'third_party/bc7enc/LICENSE') `
    -Destination (Join-Path $packageRoot 'licenses/bc7enc.txt')

Copy-Item -LiteralPath (Join-Path $repository 'third_party/ltc/LICENSE') `
    -Destination (Join-Path $packageRoot 'licenses/ltc.txt')

Copy-Item -LiteralPath (Join-Path $repository 'third_party/cie/README.md') `
    -Destination (Join-Path $packageRoot 'licenses/cie-chromaticity.txt')

if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}
if (Test-Path -LiteralPath $checksumPath) {
    Remove-Item -LiteralPath $checksumPath -Force
}
Compress-Archive -LiteralPath $packageRoot -DestinationPath $zipPath -CompressionLevel Optimal
$hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -LiteralPath $checksumPath -Value "$hash  $packageName.zip" -Encoding ASCII

Write-Host "Package:  $packageRoot"
Write-Host "Archive:  $zipPath"
Write-Host "SHA-256:  $hash"

<#
.SYNOPSIS
  Shared helpers for run-editor.ps1 and run-output-node.ps1.

.DESCRIPTION
  Dot-sourced, not run directly. Everything here is about locating the
  executable and agreeing on identities and the lifecycle secret across two
  machines; nothing here starts a process.
#>

# Locates media-server.exe for a -Build choice.
#
# 'packaged' looks beside the scripts directory, which is the layout the
# release ZIP extracts to, so these scripts work unchanged on a machine that
# has no repository and no build tree. 'auto' prefers that, then an optimized
# build, then a debug build.
function Resolve-MediaServerExe {
    param(
        [string]$Exe,
        [string]$Build,
        [Parameter(Mandatory = $true)][string]$ScriptRoot
    )

    if (-not [string]::IsNullOrWhiteSpace($Exe)) {
        if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) {
            throw "media-server.exe not found at $Exe."
        }
        return (Resolve-Path -LiteralPath $Exe).ProviderPath
    }

    $packaged = Join-Path $ScriptRoot '../media-server.exe'
    $release = Join-Path $ScriptRoot '../out/build/windows-msvc-release-gui/apps/server/media-server.exe'
    $debug = Join-Path $ScriptRoot '../out/build/windows-msvc-debug-gui/apps/server/media-server.exe'

    $candidates = switch ($Build) {
        'packaged' { , $packaged }
        'release' { , $release }
        'debug' { , $debug }
        default { @($packaged, $release, $debug) }
    }

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).ProviderPath
        }
    }

    $tried = ($candidates | ForEach-Object { "  $_" }) -join [Environment]::NewLine
    throw @"
media-server.exe not found for -Build $Build. Tried:
$tried
Extract the release package, build a GUI preset, or pass -Exe.
"@
}

# The CLI accepts only numeric addresses, never host names, so a typo becomes a
# clear message here rather than an "invalid endpoint" exit two layers down.
# Returns nothing: callers only ever want the check.
function Assert-NumericAddress {
    param(
        [Parameter(Mandatory = $true)][string]$Address,
        [Parameter(Mandatory = $true)][string]$Label,
        [switch]$RejectUnspecified
    )
    $parsed = [System.Net.IPAddress]::None
    if (-not [System.Net.IPAddress]::TryParse($Address, [ref]$parsed)) {
        throw "$Label must be a numeric IPv4 or IPv6 address, not a host name (received '$Address'). Run 'ipconfig' on that machine to find it."
    }
    if ($RejectUnspecified -and
        ($parsed.Equals([System.Net.IPAddress]::Any) -or
         $parsed.Equals([System.Net.IPAddress]::IPv6Any))) {
        throw "$Label must be a specific address, not $Address."
    }
}

function Assert-Id128 {
    param(
        [Parameter(Mandatory = $true)][string]$Value,
        [Parameter(Mandatory = $true)][string]$Label
    )
    if ($Value -notmatch '^[0-9a-fA-F]{32}$') {
        throw "$Label must be exactly 32 hexadecimal digits (received '$Value')."
    }
    if ($Value -match '^0{32}$') {
        throw "$Label must be nonzero."
    }
}

# Node identities have to be distinct per render process or two machines claim
# the same session. Derived from the node index so the operator never has to
# invent a 128-bit value, and so node 0 keeps the identity the bundled demo
# uses.
function Get-NodeIdForIndex {
    param([Parameter(Mandatory = $true)][int]$NodeIndex)
    if ($NodeIndex -lt 0 -or $NodeIndex -gt 200) {
        throw "NodeIndex must be between 0 and 200 (received $NodeIndex)."
    }
    return ('0' * 30) + ('{0:X2}' -f (0x2A + $NodeIndex))
}

# Creates a cryptographically random 64-character hex secret when the file is
# absent. Returns the resolved full path.
function New-LifecycleSecret {
    param([Parameter(Mandatory = $true)][string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw 'LifecycleSecretFile cannot be empty.'
    }

    if (Test-Path -LiteralPath $Path) {
        $item = Get-Item -LiteralPath $Path
        if ($item.PSIsContainer) {
            throw "LifecycleSecretFile must name a file, not a directory: $Path"
        }
        return $item.FullName
    }

    $full = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
    $parent = [IO.Path]::GetDirectoryName($full)
    if ([string]::IsNullOrWhiteSpace($parent)) {
        throw "Cannot determine a directory for $Path"
    }
    if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }

    $bytes = New-Object byte[] 32
    $random = [Security.Cryptography.RandomNumberGenerator]::Create()
    try { $random.GetBytes($bytes) } finally { $random.Dispose() }
    $hex = [BitConverter]::ToString($bytes).Replace('-', '')
    [IO.File]::WriteAllText($full, $hex, [Text.Encoding]::ASCII)
    Write-Host "Created lifecycle secret at $full"
    return $full
}

# Writes a secret supplied on the command line, so the operator can copy the
# value the editor machine printed instead of copying a file.
function Set-LifecycleSecret {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Secret
    )
    $trimmed = $Secret.Trim()
    if ($trimmed.Length -lt 32 -or $trimmed.Length -gt 256) {
        throw "Secret must be 32-256 characters (received $($trimmed.Length))."
    }
    if ($trimmed -match "[`0`r`n]") {
        throw 'Secret must not contain NUL, CR, or LF.'
    }
    $full = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
    $parent = [IO.Path]::GetDirectoryName($full)
    if (-not [string]::IsNullOrWhiteSpace($parent) -and
        -not (Test-Path -LiteralPath $parent -PathType Container)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    if (Test-Path -LiteralPath $full -PathType Leaf) {
        $existing = [IO.File]::ReadAllText($full).Trim()
        if ($existing -ne $trimmed) {
            Write-Host "Replacing the lifecycle secret at $full with the supplied value."
        }
    }
    [IO.File]::WriteAllText($full, $trimmed, [Text.Encoding]::ASCII)
    return $full
}

# Reads and validates a secret file against the same rules the executable
# applies, so a bad credential fails here with an explanation instead of
# failing every control request later.
function Get-LifecycleSecretText {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Lifecycle secret file not found: $Path"
    }
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -eq 0 -or $bytes.Length -gt 258 -or $bytes -contains 0) {
        throw "Lifecycle secret must be at most 258 raw bytes and contain no NUL bytes: $Path"
    }
    $length = $bytes.Length
    while ($length -gt 0 -and ($bytes[$length - 1] -eq 10 -or $bytes[$length - 1] -eq 13)) {
        --$length
    }
    if ($length -lt 32 -or $length -gt 256) {
        throw "Lifecycle secret must contain 32-256 bytes after trailing CR/LF is removed; $Path contains $length."
    }
    return [Text.Encoding]::ASCII.GetString($bytes, 0, $length)
}

# Non-loopback, non-link-local IPv4 addresses, most likely first. Used only to
# print a suggestion; the operator confirms which network the two machines
# actually share.
#
# "Most likely" means the adapter carrying the default route. A developer
# machine is usually thick with Hyper-V, WSL, VirtualBox, and VPN adapters
# whose addresses no other machine can reach, and interface metric alone
# happily ranks one of those first.
function Get-LocalIPv4Address {
    $virtualAlias = 'vEthernet|WSL|Hyper-V|VirtualBox|VMware|Loopback|Bluetooth|Docker'

    $defaultRouteIndexes = @()
    try {
        $defaultRouteIndexes = @(
            Get-NetRoute -DestinationPrefix '0.0.0.0/0' -ErrorAction Stop |
                Sort-Object -Property RouteMetric |
                Select-Object -ExpandProperty InterfaceIndex -Unique
        )
    } catch {
        $defaultRouteIndexes = @()
    }

    # Sort keys are computed rather than named properties: which properties a
    # net adapter object actually carries varies by Windows build, and a
    # missing one makes Sort-Object throw straight past this into the fallback.
    $candidates = @()
    try {
        $candidates = @(
            Get-NetIPAddress -AddressFamily IPv4 -ErrorAction Stop |
                Where-Object {
                    $_.IPAddress -ne '127.0.0.1' -and
                    -not $_.IPAddress.StartsWith('169.254.')
                } |
                Sort-Object -Property `
                    @{ Expression = { if ($defaultRouteIndexes -contains $_.InterfaceIndex) { 0 } else { 1 } } }, `
                    @{ Expression = { if ($_.InterfaceAlias -match $virtualAlias) { 1 } else { 0 } } }, `
                    @{ Expression = { $_.IPAddress } } |
                Select-Object -ExpandProperty IPAddress
        )
    } catch {
        $candidates = @()
    }

    if ($candidates.Count -eq 0) {
        try {
            $candidates = @(
                [System.Net.Dns]::GetHostAddresses([System.Net.Dns]::GetHostName()) |
                    Where-Object { $_.AddressFamily -eq 'InterNetwork' } |
                    ForEach-Object { $_.IPAddressToString } |
                    Where-Object { $_ -ne '127.0.0.1' -and -not $_.StartsWith('169.254.') }
            )
        } catch {
            $candidates = @()
        }
    }
    return $candidates
}

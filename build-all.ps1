<#
.SYNOPSIS
Build OpenTyrian 2000 Engaged for Windows, Nintendo Switch, and/or PlayStation Vita.

.DESCRIPTION
Builds each selected target independently, validates its required artifact, and
copies successful outputs into the repository's build directory. By default all
targets build incrementally and a failure does not prevent the remaining targets
from running.

.PARAMETER Target
One or more of All, PC, Switch, or Vita. Defaults to All.

.PARAMETER Configuration
The MSVC configuration for the PC target. Defaults to Release.

.PARAMETER Platform
The MSVC platform for the PC target. Defaults to x64.

.PARAMETER Clean
Clean before building. PC uses MSBuild's Rebuild target; console targets run
their existing clean command before their normal build.

.PARAMETER NoCollect
Validate the build outputs but do not copy them into the build directory.

.PARAMETER FailFast
Stop after the first failed target. The default is to try every selected target.

.PARAMETER Help
Print command-line usage without starting a build.

.EXAMPLE
.\build-all.ps1

.EXAMPLE
.\build-all.ps1 -Target PC -Clean

.EXAMPLE
.\build-all.ps1 -Target PC,Switch -Configuration Debug -NoCollect

.NOTES
Tool paths can be overridden with MSBUILD_EXE, DEVKITPRO_BASH, VITASDK,
CMAKE_EXE, and NINJA_EXE environment variables.
#>
[CmdletBinding()]
param(
    [string[]] $Target = @('All'),

    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release',

    [ValidateSet('x64', 'Win32')]
    [string] $Platform = 'x64',

    [switch] $Clean,
    [switch] $NoCollect,
    [switch] $FailFast,
    [switch] $Help
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($Help) {
    @'
Usage:
  .\build-all.ps1 [options]

Options:
  -Target <All|PC|Switch|Vita>[,...]  Targets to build (default: All)
  -Configuration <Debug|Release>      PC configuration (default: Release)
  -Platform <x64|Win32>               PC platform (default: x64)
  -Clean                              Clean before building
  -NoCollect                          Do not copy outputs into .\build
  -FailFast                           Stop after the first failed target
  -Help                               Show this help

Tool overrides:
  MSBUILD_EXE, DEVKITPRO_BASH, VITASDK, CMAKE_EXE, NINJA_EXE
'@ | Write-Host
    exit 0
}

$RepoRoot = $PSScriptRoot
$CollectionDirectory = Join-Path $RepoRoot 'build'
$Project = Join-Path $RepoRoot 'visualc\opentyrian.vcxproj'
$PcBaseName = "opentyrian-$Platform-$Configuration"
$PackageBaseName = 'OpenTyrian2000-Engaged'
$PcPackagePlatform = if ($Platform -eq 'x64') { 'Win64' } else { 'Win32' }

function Get-EnvironmentOverride {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Name
    )

    $value = [Environment]::GetEnvironmentVariable($Name)
    if ([string]::IsNullOrWhiteSpace($value)) {
        return $null
    }
    if (-not (Test-Path -LiteralPath $value -PathType Leaf)) {
        throw "$Name points to a missing file: $value"
    }
    return (Get-Item -LiteralPath $value).FullName
}

function Find-MSBuild {
    $override = Get-EnvironmentOverride 'MSBUILD_EXE'
    if ($override) {
        return $override
    }

    $command = Get-Command 'MSBuild.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) {
        return $command.Source
    }

    $programFilesX86 = [Environment]::GetFolderPath('ProgramFilesX86')
    $vswhere = Join-Path $programFilesX86 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
        $matches = @(& $vswhere -latest -prerelease -products '*' `
            -requires Microsoft.Component.MSBuild `
            -find 'MSBuild\**\Bin\MSBuild.exe')
        if ($LASTEXITCODE -eq 0) {
            $match = $matches |
                Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
                Select-Object -First 1
            if ($match) {
                return (Get-Item -LiteralPath $match).FullName
            }
        }
    }

    throw 'MSBuild was not found. Install Visual Studio C++ tools or set MSBUILD_EXE.'
}

function Find-DevkitProBash {
    $override = Get-EnvironmentOverride 'DEVKITPRO_BASH'
    if ($override) {
        return $override
    }

    $candidates = [System.Collections.Generic.List[string]]::new()
    $devkitPro = [Environment]::GetEnvironmentVariable('DEVKITPRO')
    if ($devkitPro -and -not $devkitPro.StartsWith('/')) {
        $candidates.Add((Join-Path $devkitPro 'msys2\usr\bin\bash.exe'))
    }

    foreach ($drive in [System.IO.DriveInfo]::GetDrives()) {
        if ($drive.IsReady -and $drive.DriveType -eq [System.IO.DriveType]::Fixed) {
            $candidates.Add((Join-Path $drive.RootDirectory.FullName `
                'devkitPro\msys2\usr\bin\bash.exe'))
        }
    }

    $match = $candidates |
        Select-Object -Unique |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Select-Object -First 1
    if ($match) {
        return (Get-Item -LiteralPath $match).FullName
    }

    throw 'devkitPro bash was not found. Install devkitPro or set DEVKITPRO_BASH.'
}

function Invoke-NativeCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string] $FilePath,

        [string[]] $Arguments = @()
    )

    Write-Host "  > $FilePath $($Arguments -join ' ')" -ForegroundColor DarkGray
    & $FilePath @Arguments
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "$(Split-Path $FilePath -Leaf) exited with code $exitCode."
    }
}

function Build-PC {
    if (-not (Test-Path -LiteralPath $Project -PathType Leaf)) {
        throw "Visual C++ project not found: $Project"
    }

    $msbuild = Find-MSBuild
    $buildTarget = if ($Clean) { 'Rebuild' } else { 'Build' }
    Write-Host "  MSBuild: $msbuild"
    Invoke-NativeCommand $msbuild @(
        $Project,
        "/target:$buildTarget",
        "/property:Configuration=$Configuration",
        "/property:Platform=$Platform",
        '/maxCpuCount',
        '/nologo',
        '/verbosity:minimal'
    )
}

function Build-Switch {
    $bash = Find-DevkitProBash
    $switchDirectory = Join-Path $RepoRoot 'switch'
    $buildScript = Join-Path $switchDirectory 'build.sh'
    if (-not (Test-Path -LiteralPath $buildScript -PathType Leaf)) {
        throw "Switch build script not found: $buildScript"
    }

    Write-Host "  devkitPro bash: $bash"
    $oldDevkitPro = [Environment]::GetEnvironmentVariable('DEVKITPRO')
    try {
        # build.sh expects an MSYS path. A Windows-style inherited value breaks it.
        if ($oldDevkitPro -and -not $oldDevkitPro.StartsWith('/')) {
            Remove-Item Env:DEVKITPRO -ErrorAction SilentlyContinue
        }

        Push-Location $switchDirectory
        try {
            if ($Clean) {
                Invoke-NativeCommand $bash @('build.sh', 'clean')
            }
            Invoke-NativeCommand $bash @('build.sh')
        }
        finally {
            Pop-Location
        }
    }
    finally {
        if ($null -eq $oldDevkitPro) {
            Remove-Item Env:DEVKITPRO -ErrorAction SilentlyContinue
        }
        else {
            $env:DEVKITPRO = $oldDevkitPro
        }
    }
}

function Build-Vita {
    $buildScript = Join-Path $RepoRoot 'vita\build.ps1'
    if (-not (Test-Path -LiteralPath $buildScript -PathType Leaf)) {
        throw "Vita build script not found: $buildScript"
    }

    if ($Clean) {
        & $buildScript -Clean
    }
    & $buildScript
}

function Assert-Artifact {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Build succeeded but the expected artifact is missing: $Path"
    }
}

function Copy-Artifact {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path,

        [Parameter(Mandatory = $true)]
        [string] $DestinationName
    )

    if ($DestinationName -ne (Split-Path $DestinationName -Leaf)) {
        throw "Artifact destination must be a filename, not a path: $DestinationName"
    }

    $destination = Join-Path $CollectionDirectory $DestinationName
    Copy-Item -LiteralPath $Path -Destination $destination -Force
    Write-Host "  Collected: $destination" -ForegroundColor DarkGray

    # Remove obsolete names created by earlier versions of this build script.
    $legacyDestination = Join-Path $CollectionDirectory (Split-Path $Path -Leaf)
    if ($legacyDestination -ne $destination -and
        (Test-Path -LiteralPath $legacyDestination -PathType Leaf)) {
        Remove-Item -LiteralPath $legacyDestination -Force
    }

    $destinationStem = [IO.Path]::GetFileNameWithoutExtension($DestinationName)
    $platformSeparator = $destinationStem.LastIndexOf('-')
    if ($platformSeparator -gt 0) {
        $productName = $destinationStem.Substring(0, $platformSeparator)
        $platformName = $destinationStem.Substring($platformSeparator + 1)
        $versionedPattern = "$productName-*-$platformName$([IO.Path]::GetExtension($DestinationName))"
        Get-ChildItem -LiteralPath $CollectionDirectory -Filter $versionedPattern -File |
            Where-Object { $_.FullName -ne $destination } |
            Remove-Item -Force
    }
}

$allTargets = @('PC', 'Switch', 'Vita')
$requestedTargets = @(
    foreach ($value in $Target) {
        foreach ($part in $value.Split(',')) {
            if (-not [string]::IsNullOrWhiteSpace($part)) {
                $part.Trim()
            }
        }
    }
)
$validTargets = @('All') + $allTargets
$invalidTargets = @($requestedTargets | Where-Object { $validTargets -notcontains $_ })
if ($requestedTargets.Count -eq 0 -or $invalidTargets.Count -gt 0) {
    $invalidText = if ($invalidTargets.Count -gt 0) {
        $invalidTargets -join ', '
    }
    else {
        '(empty)'
    }
    [Console]::Error.WriteLine(
        "Unknown target: $invalidText. Expected All, PC, Switch, or Vita."
    )
    exit 2
}

$selectedTargets = if ($requestedTargets -contains 'All') {
    $allTargets
}
else {
    @($allTargets | Where-Object { $requestedTargets -contains $_ })
}

$definitions = @{
    PC = [pscustomobject]@{
        Build = { Build-PC }
        Required = @([pscustomobject]@{
            Source = Join-Path $RepoRoot "$PcBaseName.exe"
            Name = "$PackageBaseName-$PcPackagePlatform.exe"
        })
        Optional = @([pscustomobject]@{
            Source = Join-Path $RepoRoot "$PcBaseName.pdb"
            Name = "$PackageBaseName-$PcPackagePlatform.pdb"
        })
    }
    Switch = [pscustomobject]@{
        Build = { Build-Switch }
        Required = @([pscustomobject]@{
            Source = Join-Path $RepoRoot 'switch\opentyrian2000.nro'
            Name = "$PackageBaseName-Switch.nro"
        })
        Optional = @()
    }
    Vita = [pscustomobject]@{
        Build = { Build-Vita }
        Required = @([pscustomobject]@{
            Source = Join-Path $RepoRoot 'vita\build\OpenTyrian2000.vpk'
            Name = "$PackageBaseName-Vita.vpk"
        })
        Optional = @()
    }
}

if (-not $NoCollect) {
    New-Item -ItemType Directory -Path $CollectionDirectory -Force | Out-Null
}

Write-Host ''
Write-Host 'OpenTyrian 2000 Engaged build' -ForegroundColor Cyan
Write-Host "  Targets:       $($selectedTargets -join ', ')"
Write-Host "  PC variant:    $Platform $Configuration"
Write-Host "  Clean rebuild: $([bool]$Clean)"
Write-Host "  Collect:       $(-not [bool]$NoCollect)"

$results = [System.Collections.Generic.List[object]]::new()
$skipRemaining = $false

foreach ($name in $selectedTargets) {
    if ($skipRemaining) {
        $results.Add([pscustomobject]@{
            Target = $name
            Status = 'SKIPPED'
            Duration = '-'
            Details = 'FailFast stopped the build.'
        })
        continue
    }

    Write-Host ''
    Write-Host "[$name]" -ForegroundColor Cyan
    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    $definition = $definitions[$name]

    try {
        & $definition.Build

        foreach ($artifact in $definition.Required) {
            Assert-Artifact $artifact.Source
        }

        if (-not $NoCollect) {
            foreach ($artifact in $definition.Required) {
                Copy-Artifact $artifact.Source $artifact.Name
            }
            foreach ($artifact in $definition.Optional) {
                if (Test-Path -LiteralPath $artifact.Source -PathType Leaf) {
                    Copy-Artifact $artifact.Source $artifact.Name
                }
            }
        }

        $timer.Stop()
        $details = if ($NoCollect) { 'Built and validated.' } else { 'Built and collected.' }
        $results.Add([pscustomobject]@{
            Target = $name
            Status = 'OK'
            Duration = $timer.Elapsed.ToString('mm\:ss')
            Details = $details
        })
    }
    catch {
        $timer.Stop()
        $message = $_.Exception.Message
        Write-Host "  FAILED: $message" -ForegroundColor Red
        $results.Add([pscustomobject]@{
            Target = $name
            Status = 'FAILED'
            Duration = $timer.Elapsed.ToString('mm\:ss')
            Details = $message
        })
        if ($FailFast) {
            $skipRemaining = $true
        }
    }
}

Write-Host ''
Write-Host 'Build summary' -ForegroundColor Cyan
$results | Format-Table Target, Status, Duration, Details -AutoSize | Out-Host

$failures = @($results | Where-Object { $_.Status -eq 'FAILED' })
if ($failures.Count -gt 0) {
    foreach ($failure in $failures) {
        Write-Host "$($failure.Target): $($failure.Details)" -ForegroundColor Red
    }
    exit 1
}

if (-not $NoCollect) {
    Write-Host "Artifacts: $CollectionDirectory" -ForegroundColor Green
}
exit 0

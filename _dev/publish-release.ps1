[CmdletBinding()]
param(
    [ValidateSet("Menu","Build","PackagePortable")]
    [string]$Action = "Menu",
    [string]$Arch = "x64",
    [string]$Configuration = "Release",
    [string]$VsVersion = "vs2026",
    [string]$ArtifactDir = "",
    [switch]$NoVersionBump,
    [switch]$NoPause
)

$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$SetVersionScript = Join-Path $PSScriptRoot "set-version.ps1"
$DefaultArtifactDir = Join-Path $PSScriptRoot "artifacts"

function Write-Step {
    param([string]$Message)
    Write-Host ""
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Get-Version {
    return (& $SetVersionScript -PrintVersion).Trim()
}

function Update-VersionForPackaging {
    if ($NoVersionBump) {
        $version = Get-Version
        Write-Host "Skipping version bump. Using version $version" -ForegroundColor Yellow
        return $version
    }

    $version = (& $SetVersionScript -IncrementBuild -PrintVersion).Trim()
    Write-Host "Version bumped to $version" -ForegroundColor Green
    return $version
}

function Find-MSBuild {
    $vsWherePath = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vsWherePath) {
        $installPath = & $vsWherePath -latest -requires Microsoft.Component.MSBuild -property installationPath
        if ($LASTEXITCODE -eq 0 -and $installPath) {
            $candidate = Join-Path $installPath "MSBuild\Current\Bin\MSBuild.exe"
            if (Test-Path $candidate) {
                return $candidate
            }
        }
    }

    $command = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    throw "MSBuild.exe not found. Please install Visual Studio 2022 Build Tools or Visual Studio 2022."
}

function Build-ReleaseBinary {
    if ($Arch -ne "x64") {
        throw "Only x64 packaging is implemented right now."
    }

    $msbuildPath = Find-MSBuild
    $projectPath = Join-Path $RepoRoot "$VsVersion/voidImageViewer.vcxproj"

    Write-Step "Building $Configuration|$Arch with $VsVersion"
    & $msbuildPath $projectPath "/p:Configuration=$Configuration" "/p:Platform=$Arch" /m /nologo
    if ($LASTEXITCODE -ne 0) {
        throw "MSBuild failed with exit code $LASTEXITCODE"
    }
}

function Get-BuildOutputPath {
    if ($Arch -eq "x64") {
        return Join-Path $RepoRoot "$VsVersion\x64\$Configuration\voidImageViewer.exe"
    }

    return Join-Path $RepoRoot "$VsVersion\$Configuration\voidImageViewer.exe"
}

function Get-ArtifactRoot {
    param([string]$Version)

    $baseDir = $ArtifactDir
    if ([string]::IsNullOrWhiteSpace($baseDir)) {
        $baseDir = $DefaultArtifactDir
    }

    $artifactRoot = Join-Path $baseDir $Version
    New-Item -ItemType Directory -Force -Path $artifactRoot | Out-Null
    return (Resolve-Path $artifactRoot).Path
}

function New-PortableZip {
    param(
        [string]$Version,
        [string]$ArtifactRoot
    )

    $exePath = Get-BuildOutputPath
    if (-not (Test-Path $exePath)) {
        throw "Built executable not found: $exePath"
    }

    $portableDirName = "voidImageViewer-$Version-$Arch-portable"
    $portableDir = Join-Path $ArtifactRoot $portableDirName
    $zipPath = Join-Path $ArtifactRoot "$portableDirName.zip"

    if (Test-Path $portableDir) {
        Remove-Item -Recurse -Force $portableDir
    }
    if (Test-Path $zipPath) {
        Remove-Item -Force $zipPath
    }

    New-Item -ItemType Directory -Force -Path $portableDir | Out-Null
    Copy-Item $exePath -Destination (Join-Path $portableDir "voidImageViewer.exe")
    Copy-Item (Join-Path $RepoRoot "README.md") -Destination (Join-Path $portableDir "README.md")
    Copy-Item (Join-Path $RepoRoot "LICENSE") -Destination (Join-Path $portableDir "LICENSE")
    Copy-Item (Join-Path $RepoRoot "Changes.txt") -Destination (Join-Path $portableDir "Changes.txt")

    Compress-Archive -Path (Join-Path $portableDir "*") -DestinationPath $zipPath -CompressionLevel Optimal
    Write-Host "Portable zip created: $zipPath" -ForegroundColor Green
}

function Invoke-PackagePortable {
    $version = Update-VersionForPackaging
    Build-ReleaseBinary
    $artifactRoot = Get-ArtifactRoot -Version $version
    New-PortableZip -Version $version -ArtifactRoot $artifactRoot
}

function Show-Menu {
    Write-Host ""
    Write-Host "voidImageViewer publish release menu" -ForegroundColor Cyan
    Write-Host "1. Build x64 Release only (safe check, no version bump)"
    Write-Host "2. Package x64 portable zip (for testing/share, auto build+1)"
    Write-Host "3. Show current version"
    Write-Host "0. Exit"
    Write-Host ""
}

function Pause-IfNeeded {
    if (-not $NoPause) {
        Write-Host ""
        Read-Host "Press Enter to exit"
    }
}

try {
    switch ($Action) {
        "Build" {
            Build-ReleaseBinary
        }
        "PackagePortable" {
            Invoke-PackagePortable
        }
        default {
            while ($true) {
                Show-Menu
                $choice = Read-Host "Select an option"
                switch ($choice) {
                    "1" {
                        Build-ReleaseBinary
                        return
                    }
                    "2" {
                        Invoke-PackagePortable
                        return
                    }
                    "3" {
                        Write-Host "Current version: $(Get-Version)" -ForegroundColor Green
                    }
                    "0" {
                        return
                    }
                    default {
                        Write-Host "Invalid selection." -ForegroundColor Yellow
                    }
                }
            }
        }
    }
}
catch {
    Write-Host ""
    Write-Host "Release flow failed: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
finally {
    Pause-IfNeeded
}
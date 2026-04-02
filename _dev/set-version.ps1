[CmdletBinding()]
param(
    [string]$Version,
    [switch]$IncrementBuild,
    [switch]$PrintVersion
)

$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$VersionHeaderPath = Join-Path $RepoRoot "src/version.h"
$ResourceScriptPath = Join-Path $RepoRoot "res/voidImageViewer.rc"
$NsisVersionPath = Join-Path $RepoRoot "nsis/version.nsh"
$RcEncoding = [System.Text.Encoding]::GetEncoding(1252)
$AsciiEncoding = [System.Text.Encoding]::ASCII

function Get-VersionInfo {
    $text = [System.IO.File]::ReadAllText($VersionHeaderPath, $AsciiEncoding)

    $year = [int][regex]::Match($text, '#define VERSION_YEAR\s+(\d+)').Groups[1].Value
    $major = [int][regex]::Match($text, '#define VERSION_MAJOR\s+(\d+)').Groups[1].Value
    $minor = [int][regex]::Match($text, '#define VERSION_MINOR\s+(\d+)').Groups[1].Value
    $revision = [int][regex]::Match($text, '#define VERSION_REVISION\s+(\d+)').Groups[1].Value
    $build = [int][regex]::Match($text, '#define VERSION_BUILD\s+(\d+)').Groups[1].Value

    return [pscustomobject]@{
        Year = $year
        Major = $major
        Minor = $minor
        Revision = $revision
        Build = $build
        VersionString = "$major.$minor.$revision.$build"
        VersionComma = "$major,$minor,$revision,$build"
    }
}

function New-VersionInfo {
    param(
        [int]$Major,
        [int]$Minor,
        [int]$Revision,
        [int]$Build
    )

    $year = (Get-Date).Year
    return [pscustomobject]@{
        Year = $year
        Major = $Major
        Minor = $Minor
        Revision = $Revision
        Build = $Build
        VersionString = "$Major.$Minor.$Revision.$Build"
        VersionComma = "$Major,$Minor,$Revision,$Build"
    }
}

function Set-VersionHeader {
    param([pscustomobject]$Info)

    $text = [System.IO.File]::ReadAllText($VersionHeaderPath, $AsciiEncoding)
    $text = [regex]::Replace($text, '(?m)^#define VERSION_YEAR\s+\d+\s*$', "#define VERSION_YEAR`t`t$($Info.Year)")
    $text = [regex]::Replace($text, '(?m)^#define VERSION_MAJOR\s+\d+\s*$', "#define VERSION_MAJOR`t`t$($Info.Major)")
    $text = [regex]::Replace($text, '(?m)^#define VERSION_MINOR\s+\d+\s*$', "#define VERSION_MINOR`t`t$($Info.Minor)")
    $text = [regex]::Replace($text, '(?m)^#define VERSION_REVISION\s+\d+\s*$', "#define VERSION_REVISION`t$($Info.Revision)")
    $text = [regex]::Replace($text, '(?m)^#define VERSION_BUILD\s+\d+\s*$', "#define VERSION_BUILD`t`t$($Info.Build)")
    [System.IO.File]::WriteAllText($VersionHeaderPath, $text, $AsciiEncoding)
}

function Set-ResourceScriptVersion {
    param([pscustomobject]$Info)

    $text = [System.IO.File]::ReadAllText($ResourceScriptPath, $RcEncoding)
    $text = [regex]::Replace($text, '(?m)^ FILEVERSION\s+.+$', " FILEVERSION $($Info.VersionComma)")
    $text = [regex]::Replace($text, '(?m)^ PRODUCTVERSION\s+.+$', " PRODUCTVERSION $($Info.VersionComma)")
    $text = [regex]::Replace($text, 'VALUE "FileVersion", "[^"]+"', "VALUE ""FileVersion"", ""$($Info.VersionString)""")
    $text = [regex]::Replace($text, 'VALUE "ProductVersion", "[^"]+"', "VALUE ""ProductVersion"", ""$($Info.VersionString)""")
    $text = [regex]::Replace($text, '(VALUE "LegalCopyright", ".*?)(20\d{2})([^"]*")', "`$1$($Info.Year)`$3")
    [System.IO.File]::WriteAllText($ResourceScriptPath, $text, $RcEncoding)
}

function Set-NsisVersion {
    param([pscustomobject]$Info)

    $text = [System.IO.File]::ReadAllText($NsisVersionPath, $AsciiEncoding)
    $text = [regex]::Replace($text, '(?m)^!define VERSION "[^"]*"\s*$', "!define VERSION ""$($Info.VersionString)""")
    $text = [regex]::Replace($text, '(?m)^!define VERSIONYEAR "[^"]*"\s*$', "!define VERSIONYEAR ""$($Info.Year)""")
    [System.IO.File]::WriteAllText($NsisVersionPath, $text, $AsciiEncoding)
}

function Sync-VersionFiles {
    param([pscustomobject]$Info)

    Set-VersionHeader -Info $Info
    Set-ResourceScriptVersion -Info $Info
    Set-NsisVersion -Info $Info
}

$info = Get-VersionInfo

if ($IncrementBuild) {
    $info = New-VersionInfo -Major $info.Major -Minor $info.Minor -Revision $info.Revision -Build ($info.Build + 1)
    Sync-VersionFiles -Info $info
}
elseif ($Version) {
    if ($Version -notmatch '^(\d+)\.(\d+)\.(\d+)\.(\d+)$') {
        throw "Version format must be major.minor.revision.build"
    }

    $info = New-VersionInfo -Major ([int]$Matches[1]) -Minor ([int]$Matches[2]) -Revision ([int]$Matches[3]) -Build ([int]$Matches[4])
    Sync-VersionFiles -Info $info
}

if ($PrintVersion -or $IncrementBuild -or $Version) {
    Write-Output $info.VersionString
}

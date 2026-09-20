<#
.SYNOPSIS
    Automated Stuttometer release and version management wrapper.
.EXAMPLE
    .\scripts\release.ps1 0.5.1 -Title "Audio Glitch Timing Polish"
    .\scripts\release.ps1 -Bump patch -DryRun
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$Version,

    [ValidateSet("patch", "minor", "major")]
    [string]$Bump,

    [string]$Title,
    [string]$Notes,
    [switch]$DryRun,
    [switch]$SkipBuild,
    [switch]$SkipPush
)

$argsList = @("scripts/release.py")
if ($Bump) { $argsList += @("--bump", $Bump) }
elseif ($Version) { $argsList += $Version }

if ($Title) { $argsList += @("--title", $Title) }
if ($Notes) { $argsList += @("--notes", $Notes) }
if ($DryRun) { $argsList += "--dry-run" }
if ($SkipBuild) { $argsList += "--skip-build" }
if ($SkipPush) { $argsList += "--skip-push" }

python @argsList
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

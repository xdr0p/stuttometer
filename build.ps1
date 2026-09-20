<#
.SYNOPSIS
    Builds Stuttometer and optionally runs the test suite.

.PARAMETER Config
    Build configuration ("Release" or "Debug"). Default is "Release".

.PARAMETER Test
    Runs the automated test suite after building.

.PARAMETER Clean
    Cleans previous build outputs before building.

.EXAMPLE
    .\build.ps1
    Builds Stuttometer in Release mode.

.EXAMPLE
    .\build.ps1 -Test
    Builds Stuttometer in Release mode and runs all unit tests.

.EXAMPLE
    .\build.ps1 -Config Debug -Clean
    Cleans and builds Stuttometer in Debug mode.
#>
[CmdletBinding()]
param (
    [ValidateSet("Release", "Debug")]
    [string]$Config = "Release",

    [switch]$Test,
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

# 1. Locate Visual Studio tools using vswhere
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    Write-Error "vswhere.exe not found at '$vswhere'. Ensure Visual Studio is installed."
    exit 1
}

$msbuild = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find "MSBuild\**\Bin\amd64\MSBuild.exe" | Select-Object -First 1
if (-not $msbuild -or -not (Test-Path $msbuild)) {
    # Fallback to 32-bit MSBuild if amd64 not found
    $msbuild = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
}

if (-not $msbuild -or -not (Test-Path $msbuild)) {
    Write-Error "MSBuild.exe could not be located via vswhere."
    exit 1
}

$buildDir = Join-Path $PSScriptRoot "build"
$guiProj = Join-Path $buildDir "stuttometer_gui.vcxproj"
$testProj = Join-Path $buildDir "RUN_TESTS.vcxproj"

if (-not (Test-Path $guiProj)) {
    Write-Error "Project file not found: '$guiProj'. Ensure the build directory is configured."
    exit 1
}

# 2. Clean if requested
if ($Clean) {
    Write-Host "Cleaning Stuttometer ($Config)..." -ForegroundColor Yellow
    & $msbuild $guiProj /p:Configuration=$Config /t:Clean /nologo
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

# 3. Build Stuttometer GUI
Write-Host "Building Stuttometer ($Config)..." -ForegroundColor Cyan
& $msbuild $guiProj /p:Configuration=$Config /nologo /m
if ($LASTEXITCODE -ne 0) {
    Write-Host "Build failed with exit code $LASTEXITCODE." -ForegroundColor Red
    exit $LASTEXITCODE
}
Write-Host "Build succeeded: build\$Config\stuttometer_gui.exe" -ForegroundColor Green

# 4. Run tests if requested
if ($Test) {
    Write-Host "`nRunning Test Suite ($Config)..." -ForegroundColor Cyan
    & $msbuild $testProj /p:Configuration=$Config /nologo
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Tests failed with exit code $LASTEXITCODE." -ForegroundColor Red
        exit $LASTEXITCODE
    }
    Write-Host "All tests passed successfully!" -ForegroundColor Green
}

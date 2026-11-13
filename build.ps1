<#
.SYNOPSIS
  Sets up the build tools, downloads the D2RLoader PluginSDK and builds the GrailTrackrr plugin.

.DESCRIPTION
  1. Installs Git, CMake, Ninja and Visual Studio 2022 Build Tools (C++ workload) with winget if missing.
  2. Downloads the D2RLoader PluginSDK into sdk\PluginSDK (or updates it).
  3. Builds d2rl-grailtrackrr.dll and lays out the release in dist: instructions.txt,
     d2rloader\plugins with the DLL, and mods with the GrailTrackrr and GrailTrackrrRace mods.
  4. Optionally copies both folders into your game folder.

.EXAMPLE
  .\build.ps1
  .\build.ps1 -GameDir "C:\Program Files (x86)\Diablo II Resurrected"
  .\build.ps1 -Config Debug -Clean
#>
[CmdletBinding()]
param(
	# Game folder (the one with D2R.exe). When set, dist\d2rloader and dist\mods are copied into it.
	[string]$GameDir = "",
	# PluginSDK branch, tag or commit. The plugin needs SDK 0.4.0 or newer (the v4 tag is too old).
	[string]$SdkRef = "master",
	[ValidateSet("Release", "Debug")]
	[string]$Config = "Release",
	# Do not install or check tools with winget.
	[switch]$SkipTools,
	# Delete the build folder first.
	[switch]$Clean
)

$ErrorActionPreference = "Stop"
$Root      = $PSScriptRoot
$SdkUrl    = "https://github.com/D2RLoader/PluginSDK.git"
$SdkDir    = Join-Path $Root "sdk\PluginSDK"
$BuildDir  = Join-Path $Root "build\$Config"
$DistDir   = Join-Path $Root "dist"
$MinCMake  = [version]"3.29"

function Step([string]$Message) { Write-Host "`n==> $Message" -ForegroundColor Cyan }
function Info([string]$Message) { Write-Host "    $Message" }
function Fail([string]$Message) { Write-Host "`nBuild failed: $Message" -ForegroundColor Red; exit 1 }

# Runs a native command and stops on a non-zero exit code.
function Run([string]$Exe, [string[]]$Arguments) {
	& $Exe @Arguments
	if ($LASTEXITCODE -ne 0) { Fail "$([System.IO.Path]::GetFileName($Exe)) exited with code $LASTEXITCODE" }
}

# Picks up PATH changes made by installers without reopening the terminal.
# -Append keeps the Visual Studio paths first once the build environment is loaded.
function Update-SessionPath([switch]$Append) {
	$machine = [Environment]::GetEnvironmentVariable("Path", "Machine")
	$user    = [Environment]::GetEnvironmentVariable("Path", "User")
	if ($Append) { $env:Path = "$env:Path;$machine;$user" } else { $env:Path = "$machine;$user;$env:Path" }
}

function Install-WithWinget([string]$Id, [string[]]$Extra = @()) {
	if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
		Fail "winget is not available. Install 'App Installer' from the Microsoft Store, or install $Id yourself and rerun with -SkipTools."
	}
	Info "Installing $Id with winget (a UAC prompt may appear)..."
	$wingetArgs = @("install", "--id", $Id, "-e", "--accept-source-agreements", "--accept-package-agreements") + $Extra
	& winget @wingetArgs
	# winget returns non-zero when the package is already installed; check for the tool afterwards instead.
	Update-SessionPath
}

function Get-CMakeVersion([string]$Exe) {
	try {
		$line = (& $Exe --version 2>$null | Select-Object -First 1)
		if ($line -match "(\d+\.\d+(\.\d+)?)") { return [version]$Matches[1] }
	} catch { }
	return [version]"0.0"
}

function Find-VsInstall {
	$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
	if (-not (Test-Path $vswhere)) { return $null }
	$path = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
	if ($path) { return ($path | Select-Object -First 1).Trim() }
	return $null
}

# ---------------------------------------------------------------------------
Step "Checking build tools"

if (-not $SkipTools) {
	if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Install-WithWinget "Git.Git" }

	$kitwareCMake = Join-Path $env:ProgramFiles "CMake\bin\cmake.exe"
	if (-not (Test-Path $kitwareCMake) -or (Get-CMakeVersion $kitwareCMake) -lt $MinCMake) {
		$existing = Get-Command cmake -ErrorAction SilentlyContinue
		if (-not $existing -or (Get-CMakeVersion $existing.Source) -lt $MinCMake) {
			Install-WithWinget "Kitware.CMake"
			if ((Test-Path $kitwareCMake) -and (Get-CMakeVersion $kitwareCMake) -lt $MinCMake) {
				& winget upgrade --id Kitware.CMake -e --accept-source-agreements --accept-package-agreements
			}
		}
	}

	if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) { Install-WithWinget "Ninja-build.Ninja" }

	if (-not (Find-VsInstall)) {
		Info "Visual Studio C++ build tools not found. Installing Build Tools 2022 with the C++ workload."
		Info "This is a large download (several GB) and can take a while."
		Install-WithWinget "Microsoft.VisualStudio.2022.BuildTools" @(
			"--override", "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
		)
	}
}

$git = Get-Command git -ErrorAction SilentlyContinue
if (-not $git) { Fail "Git not found. Install it (winget install Git.Git) or rerun without -SkipTools." }

$vs = Find-VsInstall
if (-not $vs) {
	Fail "Visual Studio C++ build tools not found. If Visual Studio is already installed, open the Visual Studio Installer and add 'Desktop development with C++'."
}
Info "Visual Studio: $vs"

# ---------------------------------------------------------------------------
Step "Loading the Visual Studio x64 build environment"

$devShell = Join-Path $vs "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
if (-not (Test-Path $devShell)) { Fail "Could not find $devShell" }
Import-Module $devShell
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
Update-SessionPath -Append
if (-not (Get-Command cl -ErrorAction SilentlyContinue)) { Fail "cl.exe is not on PATH after loading the Visual Studio environment." }

# Prefer a CMake that is new enough; Visual Studio ships its own, which may be older.
$cmake = $null
foreach ($candidate in @((Join-Path $env:ProgramFiles "CMake\bin\cmake.exe")) + @(Get-Command cmake -All -ErrorAction SilentlyContinue | ForEach-Object { $_.Source })) {
	if ($candidate -and (Test-Path $candidate) -and (Get-CMakeVersion $candidate) -ge $MinCMake) { $cmake = $candidate; break }
}
if (-not $cmake) { Fail "CMake $MinCMake or newer not found. Install it with: winget install Kitware.CMake" }
Info "CMake: $cmake ($(Get-CMakeVersion $cmake))"

$ninja = Get-Command ninja -ErrorAction SilentlyContinue
if (-not $ninja) { Fail "Ninja not found. Install it with: winget install Ninja-build.Ninja" }
Info "Ninja: $($ninja.Source)"

# ---------------------------------------------------------------------------
Step "Getting the D2RLoader PluginSDK ($SdkRef)"

if (-not (Test-Path (Join-Path $SdkDir ".git"))) {
	New-Item -ItemType Directory -Force -Path $SdkDir | Out-Null
	Run $git.Source @("-C", $SdkDir, "init", "-q")
	Run $git.Source @("-C", $SdkDir, "remote", "add", "origin", $SdkUrl)
}
# Fetching by ref works for branches, tags and commit hashes alike.
Run $git.Source @("-C", $SdkDir, "fetch", "-q", "--depth", "1", "origin", $SdkRef)
Run $git.Source @("-C", $SdkDir, "checkout", "-q", "--detach", "FETCH_HEAD")
$sdkCommit = (& $git.Source -C $SdkDir rev-parse --short HEAD).Trim()
$versionHeader = Get-Content (Join-Path $SdkDir "include\D2RLPlugin\version.h") -Raw
$sdkVersion = if ($versionHeader -match 'D2RL_SDK_VERSION\s+"([^"]+)"') { $Matches[1] } else { "unknown" }
Info "PluginSDK $sdkVersion at $sdkCommit"

# ---------------------------------------------------------------------------
Step "Building d2rl-grailtrackrr.dll ($Config)"

if ($Clean -and (Test-Path $BuildDir)) { Remove-Item -Recurse -Force $BuildDir }

Run $cmake @(
	"-S", $Root,
	"-B", $BuildDir,
	"-G", "Ninja",
	"-DCMAKE_BUILD_TYPE=$Config",
	"-DCMAKE_MAKE_PROGRAM=$($ninja.Source)",
	"-DCMAKE_CXX_COMPILER=cl",
	"-DD2RL_SDK_DIR=$SdkDir"
)
Run $cmake @("--build", $BuildDir)

$dll = Get-ChildItem -Path $BuildDir -Recurse -Filter "d2rl-grailtrackrr.dll" | Select-Object -First 1
if (-not $dll) { Fail "Build finished but d2rl-grailtrackrr.dll was not found in $BuildDir" }

# dist holds exactly what goes in the release zip.
if (Test-Path $DistDir) { Remove-Item -Recurse -Force $DistDir }
Copy-Item (Join-Path $Root "package") $DistDir -Recurse
$plugins = Join-Path $DistDir "d2rloader\plugins"
New-Item -ItemType Directory -Force -Path $plugins | Out-Null
Copy-Item $dll.FullName $plugins -Force
Copy-Item (Join-Path $Root "grailtrackrr.toml") $plugins -Force
Info "Output: $DistDir"

# ---------------------------------------------------------------------------
if ($GameDir) {
	Step "Installing into the game folder"
	if (-not (Test-Path $GameDir)) { Fail "Game folder not found: $GameDir" }
	if (-not (Test-Path (Join-Path $GameDir "D2R.exe"))) { Write-Warning "D2R.exe not found in $GameDir. Copying anyway." }
	Copy-Item (Join-Path $DistDir "d2rloader") $GameDir -Recurse -Force
	Copy-Item (Join-Path $DistDir "mods") $GameDir -Recurse -Force
	Info "Copied the plugin to $(Join-Path $GameDir 'd2rloader\plugins') and the mods to $(Join-Path $GameDir 'mods')"
	$oldDll = Join-Path $GameDir "d2rloader\plugins\d2rl-live-grail.dll"
	if (Test-Path $oldDll) { Remove-Item $oldDll -Force; Info "Removed the old d2rl-live-grail.dll" }
}

Write-Host "`nDone." -ForegroundColor Green

#Requires -Version 5.1
<#
.SYNOPSIS
    ClawSpan one-click installer

.DESCRIPTION
    Download and install ClawSpan (OpenClaw + GUI automation gateway).

    Installs:
      - ClawSpan daemon + UI + plugins (Windows side)
      - ClawSpan WSL2 VM (contains OpenClaw + MCP Server)

    Install latest version:
      irm https://github.com/carlos-Ng/ClawSpan/releases/latest/download/install.ps1 | iex

    Install specific version (recommended for pinning):
      irm https://github.com/carlos-Ng/ClawSpan/releases/latest/download/install.ps1 | iex -Version 0.1.0
      # Or download install.ps1 from the specific release (URL already pinned):
      irm https://github.com/carlos-Ng/ClawSpan/releases/download/v0.1.0/install.ps1 | iex

    Release assets:
      clawspan-windows-<ver>.zip  -- All Windows binaries (daemon, vmm, ui, dll)
      clawspan-rootfs.tar.gz      -- WSL2 VM image (downloaded on first install)
      install.ps1                  -- This installer script (included in each release)

.PARAMETER Version
    Version number to install (e.g. 0.1.0). Leave empty for latest.

.PARAMETER Uninstall
    Uninstall ClawSpan

.PARAMETER Upgrade
    Upgrade ClawSpan components (preserves user data)

.PARAMETER Offline
    Enable offline install mode (no network requests). In this mode, installer
    searches local assets near install.ps1 by default.

.PARAMETER WindowsZipPath
    Local path to clawspan-windows-<ver>.zip

.PARAMETER RootfsPath
    Local path to clawspan-rootfs.tar.gz (required for fresh install)

.PARAMETER AssetDir
    Local directory containing release assets
#>

[CmdletBinding()]
param(
    [switch]$Uninstall,
    [switch]$Upgrade,
    [switch]$Offline,
    [string]$Version = "",
    [string]$InstallDir = "",
    [string]$ApiKey = "",
    [string]$ApiProvider = "",
    [string]$ReleaseUrl = "",
    [string]$LocalModelUrl = "",
    [string]$LocalModelId = "",
    [string]$WindowsZipPath = "",
    [string]$RootfsPath = "",
    [string]$AssetDir = ""
)

# Version convention (unified):
#   - Git tag / URL path: with "v"  → .../releases/download/v0.1.0/...
#   - Package name / version string: no "v" → clawspan-windows-0.1.0.zip, config/version.txt = "0.1.0"
# So URL is: .../v0.1.0/clawspan-windows-0.1.0.zip

# Normalize: -Version may be "0.1.0" or "v0.1.0"; we keep version without "v" for zip name and display
if ($Version -match '^v(.+)$') { $Version = $Matches[1] }

# -- Config ----------------------------------------------------------------

$ErrorActionPreference = "Stop"

if ($Host.Name -eq 'ConsoleHost') {
    try {
        [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
        [Console]::InputEncoding  = [System.Text.Encoding]::UTF8
        $OutputEncoding = [System.Text.Encoding]::UTF8
    } catch {}
}

$AppName        = "ClawSpan"
$DistroName     = "ClawSpan"
$DefaultInstDir = Join-Path $env:LOCALAPPDATA $AppName
$StartupDir     = [Environment]::GetFolderPath("Startup")
$UninstallRegKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\$AppName"

# GitHub Release URL: path uses "v" (tag style), e.g. .../releases/download/v0.1.0
if (-not $ReleaseUrl) {
    if ($Version) {
        $DefaultReleaseBase = "https://github.com/carlos-Ng/ClawSpan/releases/download/v$Version"
    } else {
        $DefaultReleaseBase = "https://github.com/carlos-Ng/ClawSpan/releases/latest/download"
    }
}

# Release assets (zip contains all Windows binaries; rootfs packaged separately so upgrades can skip it)
$RootfsName = "clawspan-rootfs.tar.gz"

# Files expected inside the zip (used for install verification)
$WindowsPayloadExpectations = @{
    bin = @(
        "claw_span_service.exe"
        "claw_span_vmm.exe"
        "claw_span_ui.exe"
    )
    modules = @(
        "capability_ax.dll"
        "security_filter.dll"
    )
}

# -- Helper functions ------------------------------------------------------

function Write-Banner {
    param([string]$Text)
    $line = "=" * 50
    Write-Host ""
    Write-Host $line -ForegroundColor Cyan
    Write-Host "  $Text" -ForegroundColor Cyan
    Write-Host $line -ForegroundColor Cyan
    Write-Host ""
}

function Write-Step {
    param([string]$Text)
    Write-Host "  > $Text" -ForegroundColor White
}

function Write-Ok {
    param([string]$Text)
    Write-Host "  [OK] $Text" -ForegroundColor Green
}

function Write-Warn {
    param([string]$Text)
    Write-Host "  [WARN] $Text" -ForegroundColor Yellow
}

function Write-Err {
    param([string]$Text)
    Write-Host "  [ERR] $Text" -ForegroundColor Red
}

function Fail-Install {
    param(
        [string]$Message,
        [int]$ExitCode = 1
    )
    Write-Err $Message
    exit $ExitCode
}

function Confirm-Continue {
    param([string]$Prompt = "Continue?")
    $answer = Read-Host "$Prompt (Y/n)"
    if ($answer -and $answer -notmatch '^[Yy]') {
        Write-Host "Cancelled."
        exit 0
    }
}

function Stop-ClawSpanProcesses {
    param(
        [int]$TimeoutSeconds = 12
    )

    $names = @("claw_span_service", "claw_span_vmm", "claw_span_ui")
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $killedByTaskkill = $false

    while ((Get-Date) -lt $deadline) {
        $procs = Get-Process -Name $names -ErrorAction SilentlyContinue
        if (-not $procs) { break }

        # First try native PowerShell kill.
        $procs | Stop-Process -Force -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 500

        $stillRunning = Get-Process -Name $names -ErrorAction SilentlyContinue
        if (-not $stillRunning) { break }

        # Fallback: taskkill with /T to terminate child process tree.
        foreach ($n in $names) {
            & taskkill /F /T /IM "$n.exe" 2>$null | Out-Null
        }
        $killedByTaskkill = $true
        Start-Sleep -Milliseconds 800
    }

    $remaining = Get-Process -Name $names -ErrorAction SilentlyContinue
    if ($remaining) {
        Write-Warn "Some ClawSpan processes are still running:"
        $remaining |
            Select-Object ProcessName, Id |
            ForEach-Object { Write-Warn "  $($_.ProcessName) (PID: $($_.Id))" }

        $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).
            IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
        if (-not $isAdmin) {
            Write-Warn "Current shell is not elevated. Try uninstalling from an Administrator PowerShell."
        } else {
            Write-Warn "Please close related apps manually, then retry uninstall."
        }
        return $false
    }

    if ($killedByTaskkill) {
        Write-Ok "Processes stopped (with force-kill fallback)"
    } else {
        Write-Ok "Processes stopped"
    }
    return $true
}

function Get-InstallerDirectory {
    if ($PSCommandPath) {
        return Split-Path -Parent $PSCommandPath
    }
    if ($MyInvocation.MyCommand.Path) {
        return Split-Path -Parent $MyInvocation.MyCommand.Path
    }
    return (Get-Location).Path
}

function Test-ArchiveSignature {
    param(
        [string]$Path,
        [ValidateSet("zip", "gzip")]
        [string]$Type
    )
    if (-not (Test-Path $Path)) { return $false }
    if ((Get-Item $Path).Length -lt 2) { return $false }
    $headBytes = New-Object byte[] 2
    $fs = [System.IO.File]::OpenRead($Path)
    try { $fs.Read($headBytes, 0, 2) | Out-Null } finally { $fs.Close() }
    if ($Type -eq "zip") {
        return ($headBytes[0] -eq 0x50 -and $headBytes[1] -eq 0x4B)
    }
    return ($headBytes[0] -eq 0x1F -and $headBytes[1] -eq 0x8B)
}

function Assert-WindowsPayloadFiles {
    param(
        [string]$BinDir,
        [string]$ModuleDir
    )
    $requiredPaths = @()
    foreach ($file in $WindowsPayloadExpectations.bin) {
        $requiredPaths += (Join-Path $BinDir $file)
    }
    foreach ($file in $WindowsPayloadExpectations.modules) {
        $requiredPaths += (Join-Path $ModuleDir $file)
    }

    $missing = @($requiredPaths | Where-Object { -not (Test-Path $_) })
    if ($missing.Count -gt 0) {
        Write-Err "Windows package is incomplete after extraction."
        foreach ($m in $missing) {
            Write-Err "  Missing: $m"
        }
        exit 1
    }
    Write-Ok "Windows package integrity verified"
}

# Run wsl.exe with stdout/stderr/console completely detached from the parent console.
# This prevents WSL boot messages, systemd journal output, dbus noise, etc. from
# leaking into the installer's formatted output and breaking the layout.
function Invoke-WslSilent {
    param(
        [string]$WslArguments,
        [string]$InputText = $null
    )
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = "wsl.exe"
    $psi.Arguments = $WslArguments
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    if ($InputText -ne $null -and $InputText -ne '') {
        $psi.RedirectStandardInput = $true
    }
    $proc = [System.Diagnostics.Process]::Start($psi)
    if ($InputText -ne $null -and $InputText -ne '') {
        $proc.StandardInput.Write($InputText)
        $proc.StandardInput.Close()
    }
    $stdoutTask = $proc.StandardOutput.ReadToEndAsync()
    $stderrTask = $proc.StandardError.ReadToEndAsync()
    $proc.WaitForExit()
    return $proc.ExitCode
}

# Run a WSL command in the background with a visual spinner.
# Optionally monitors a directory for VHDX disk growth (useful for wsl --import).
function Invoke-WslWithSpinner {
    param(
        [string]$WslArguments,
        [string]$StatusPrefix = "Working",
        [string]$MonitorDir = $null
    )
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = "wsl.exe"
    $psi.Arguments = $WslArguments
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $proc = [System.Diagnostics.Process]::Start($psi)

    $spinChars = @('-', '\', '|', '/')
    $i = 0
    $sw = [System.Diagnostics.Stopwatch]::StartNew()

    while (-not $proc.HasExited) {
        $spin = $spinChars[$i % 4]
        $elapsed = [math]::Floor($sw.Elapsed.TotalSeconds)
        $extra = ""
        if ($MonitorDir -and (Test-Path $MonitorDir)) {
            $vhdx = Get-ChildItem $MonitorDir -Filter "*.vhdx" -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($vhdx) {
                $diskMB = [math]::Round($vhdx.Length / 1MB, 0)
                $extra = ", ${diskMB} MB written"
            }
        }
        Write-Host "`r  $spin $StatusPrefix (${elapsed}s$extra)              " -NoNewline
        Start-Sleep -Milliseconds 300
        $i++
    }
    $sw.Stop()
    Write-Host "`r                                                                              `r" -NoNewline

    $proc.StandardOutput.ReadToEnd() | Out-Null
    $proc.StandardError.ReadToEnd() | Out-Null

    return $proc.ExitCode
}

function Invoke-UninstallFlow {
    $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).
        IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $isAdmin) {
        Write-Warn "Uninstall requires Administrator privileges. Requesting elevation ..."
        try {
            $argList = @(
                "-ExecutionPolicy", "Bypass",
                "-NoProfile",
                "-File", "`"$PSCommandPath`"",
                "-Uninstall"
            )
            Start-Process -FilePath "powershell.exe" -Verb RunAs -ArgumentList $argList -WindowStyle Normal | Out-Null
            exit 0
        } catch {
            Write-Err "Elevation was cancelled or failed. Uninstall aborted."
            exit 1
        }
    }

    Write-Banner "Uninstall $AppName"
    $uninstallComplete = $true

    Write-Step "Stopping $AppName processes ..."
    $stopped = Stop-ClawSpanProcesses -TimeoutSeconds 15

    Write-Step "Shutting down WSL distro ..."
    wsl -t $DistroName 2>$null
    Start-Sleep -Milliseconds 500
    if (-not $stopped) {
        $uninstallComplete = $false
        Write-Warn "Uninstall will continue, but running processes may keep some files locked."
    }

    $removeVM = Read-Host "  Delete $DistroName VM and all its data? (y/N)"
    if ($removeVM -match '^[Yy]') {
        Write-Step "Unregistering WSL distro ..."
        wsl --unregister $DistroName 2>$null
        Write-Ok "VM deleted"
    } else {
        Write-Warn "VM kept (you can manually run: wsl --unregister $DistroName)"
    }

    $startupLink = Join-Path $StartupDir "$AppName.lnk"
    if (Test-Path $startupLink) {
        try {
            Remove-Item $startupLink -Force -ErrorAction Stop
            Write-Ok "Startup shortcut removed"
        } catch {
            $uninstallComplete = $false
            Write-Warn "Failed to remove startup shortcut: $startupLink"
            Write-Warn "Error: $($_.Exception.Message)"
        }
    }

    $instDir = $DefaultInstDir
    if (Test-Path $UninstallRegKey) {
        $regInstDir = (Get-ItemProperty $UninstallRegKey -ErrorAction SilentlyContinue).InstallLocation
        if ($regInstDir) { $instDir = $regInstDir }
    }

    if (Test-Path $instDir) {
        Write-Step "Removing install directory $instDir ..."
        try {
            Remove-Item $instDir -Recurse -Force -ErrorAction Stop
            Write-Ok "Install files removed"
        } catch {
            $uninstallComplete = $false
            Write-Warn "Some files could not be removed (possibly in use)."
            Write-Warn "Error: $($_.Exception.Message)"
            Write-Warn "Please close related processes and run uninstall again."
        }
        if (Test-Path $instDir) {
            $uninstallComplete = $false
            Write-Warn "Install directory still exists: $instDir"
        }
    }

    if (Test-Path $UninstallRegKey) {
        try {
            Remove-Item $UninstallRegKey -Force -ErrorAction Stop
            Write-Ok "Registry entries cleaned"
        } catch {
            $uninstallComplete = $false
            Write-Warn "Failed to clean uninstall registry entries."
            Write-Warn "Error: $($_.Exception.Message)"
        }
    }

    $remaining = Get-Process -Name "claw_span_service", "claw_span_vmm", "claw_span_ui" -ErrorAction SilentlyContinue
    if ($remaining) {
        $uninstallComplete = $false
        Write-Warn "Residual ClawSpan processes detected after uninstall:"
        $remaining |
            Select-Object ProcessName, Id |
            ForEach-Object { Write-Warn "  $($_.ProcessName) (PID: $($_.Id))" }
    }

    Write-Host ""
    if ($uninstallComplete) {
        Write-Ok "$AppName has been uninstalled"
        exit 0
    }
    Write-Warn "$AppName uninstall finished with warnings."
    Write-Warn "Some components are still present. Please fix the warnings above and retry."
    exit 1
}

# -- Uninstall -------------------------------------------------------------

if ($Uninstall) {
    Invoke-UninstallFlow
}

# -- Environment checks ----------------------------------------------------

function Assert-InstallPrerequisites {
    Write-Step "Checking system requirements ..."
    $osVersion = [System.Environment]::OSVersion.Version
    if ($osVersion.Build -lt 19041) {
        Fail-Install "Requires Windows 10 2004 (Build 19041) or later. Current version: $($osVersion.ToString())"
    }
    Write-Ok "Windows version: $($osVersion.ToString())"

    $CurlExe = $null
    $_curlCmd = Get-Command "curl.exe" -ErrorAction SilentlyContinue
    if ($_curlCmd) { $CurlExe = $_curlCmd.Source }
    if (-not $CurlExe) {
        Write-Host ""
        Write-Host "  curl.exe is built into Windows 10 (Build 17063) and later." -ForegroundColor Yellow
        Write-Host "  If your system doesn't have it, download from:" -ForegroundColor Yellow
        Write-Host "    https://curl.se/windows/" -ForegroundColor White
        Write-Host "  Then re-run this script." -ForegroundColor Yellow
        Write-Host ""
        Fail-Install "curl.exe not found"
    }
    Write-Ok "curl.exe: $CurlExe"

    $wslStatus = $null
    try { $wslStatus = wsl --status 2>&1 } catch {}
    $wslInstalled = $false
    if ($LASTEXITCODE -eq 0 -or ($wslStatus -match "Default Version")) { $wslInstalled = $true }
    if (-not $wslInstalled) {
        Write-Host ""
        Write-Host "  WSL2 must be installed first. Run in an admin PowerShell:" -ForegroundColor Yellow
        Write-Host "    wsl --install --no-distribution" -ForegroundColor White
        Write-Host "  Reboot after installation, then re-run this script." -ForegroundColor Yellow
        Write-Host ""
        Fail-Install "WSL2 is not installed or not enabled"
    }
    Write-Ok "WSL2 ready"
    return $CurlExe
}

function Resolve-InstallMode {
    $existingDistro = wsl -l -q 2>$null | Where-Object { $_ -match "^$DistroName$" }
    $isUpgrade = $false
    if ($existingDistro -and -not $Upgrade) {
        Write-Warn "$DistroName VM already exists"
        Write-Host ""
        Write-Host "  Options:" -ForegroundColor Yellow
        Write-Host "    1) Upgrade (keep data, update components)" -ForegroundColor White
        Write-Host "    2) Clean install (delete existing data)" -ForegroundColor White
        Write-Host "    3) Cancel" -ForegroundColor White
        $choice = Read-Host "  Choose (1/2/3)"
        switch ($choice) {
            "1" { $isUpgrade = $true }
            "2" {
                Write-Step "Unregistering existing distro ..."
                wsl -t $DistroName 2>$null
                wsl --unregister $DistroName 2>$null
                Write-Ok "Cleared"
            }
            default { Write-Host "Cancelled."; exit 0 }
        }
    }
    if ($Upgrade) { $isUpgrade = $true }
    if ($Offline -and $isUpgrade) {
        Fail-Install "Offline mode does not support upgrade yet. Please run fresh install with -Offline, or use online mode for upgrade."
    }
    return $isUpgrade
}

function Resolve-InstallPaths {
    if (-not $InstallDir) {
        if (Test-Path $UninstallRegKey) {
            $regInstDir = (Get-ItemProperty $UninstallRegKey -ErrorAction SilentlyContinue).InstallLocation
            if ($regInstDir) { $InstallDir = $regInstDir }
        }
    }
    if (-not $InstallDir) { $InstallDir = $DefaultInstDir }
    $DistroDir = Join-Path $InstallDir "vm"
    $BinDir = Join-Path $InstallDir "bin"
    $ModuleDir = Join-Path $InstallDir "modules"
    $ConfigDir = Join-Path $InstallDir "config"
    $DownloadDir = Join-Path $InstallDir "downloads"
    Write-Ok "Install path: $InstallDir"
    return @{
        InstallDir = $InstallDir
        DistroDir = $DistroDir
        BinDir = $BinDir
        ModuleDir = $ModuleDir
        ConfigDir = $ConfigDir
        DownloadDir = $DownloadDir
    }
}

Write-Banner "$AppName Installer v$((Get-Date).ToString('yyyy.M.d'))"
$CurlExe = Assert-InstallPrerequisites
$isUpgrade = Resolve-InstallMode
$paths = Resolve-InstallPaths
$InstallDir = $paths.InstallDir
$DistroDir = $paths.DistroDir
$BinDir = $paths.BinDir
$ModuleDir = $paths.ModuleDir
$ConfigDir = $paths.ConfigDir
$DownloadDir = $paths.DownloadDir

# -- Download --------------------------------------------------------------

if (-not $isUpgrade) {
    Write-Banner "Download Components"
} else {
    Write-Banner "Download Updates"
}

if (-not $ReleaseUrl) { $ReleaseUrl = $DefaultReleaseBase }

New-Item -ItemType Directory -Path $DownloadDir -Force | Out-Null

function Get-RemoteFileSize {
    param([string]$Url)
    $output = & $CurlExe -s --fail -I -L --connect-timeout 10 $Url 2>&1
    $expectedSize = [long]-1
    foreach ($line in $output) {
        if ($line -match "(?i)^content-length:\s*(\d+)") {
            $expectedSize = [long]$Matches[1]
        }
    }
    return $expectedSize
}

function Invoke-Download {
    param(
        [string]$Url,
        [string]$Dest,
        [string]$Desc,
        [ValidateSet("zip", "gzip")]
        [string]$ExpectedType
    )

    $expectedSize = Get-RemoteFileSize -Url $Url
    if ($expectedSize -gt 0) {
        $sizeMB = [math]::Round($expectedSize / 1MB, 1)
        Write-Step "Downloading $Desc ($sizeMB MB) ..."
    } else {
        Write-Step "Downloading $Desc ..."
    }
    Write-Host ""
    & $CurlExe -L --fail "-#" --connect-timeout 15 --retry 3 --retry-delay 2 -o $Dest $Url
    Write-Host ""
    if ($LASTEXITCODE -ne 0) {
        Write-Err "Download failed: $Desc (curl exit code: $LASTEXITCODE)"
        Write-Err "  URL: $Url"
        if ($LASTEXITCODE -eq 22) {
            Write-Err "  Server returned an error (file may not exist or URL is invalid)"
        } elseif ($LASTEXITCODE -eq 28) {
            Write-Err "  Connection timed out, please check your network"
        }
        Write-Host ""
        Write-Host "  If you cannot access GitHub, download these files manually:" -ForegroundColor Yellow
        Write-Host "    - clawspan-windows-<version>.zip" -ForegroundColor White
        Write-Host "    - clawspan-rootfs.tar.gz (first install)" -ForegroundColor White
        Write-Host "  Place them in $DownloadDir and re-run the script." -ForegroundColor Yellow
        Write-Host ""
        Fail-Install "Download failed: $Desc"
    }

    # File size validation
    if ($expectedSize -gt 0) {
        $actualSize = (Get-Item $Dest).Length
        if ($actualSize -ne $expectedSize) {
            Write-Err "File integrity check failed: $Desc"
            Write-Err "  Expected: $expectedSize bytes, Actual: $actualSize bytes"
            Write-Host ""
            Write-Host "  Download incomplete. Remove the partial file and re-run:" -ForegroundColor Yellow
            Write-Host "    Remove-Item '$Dest'" -ForegroundColor White
            Write-Host ""
            Fail-Install "File integrity check failed: $Desc"
        }
    }

    # Strict file type validation by expected asset kind.
    if ((Test-Path $Dest) -and (Get-Item $Dest).Length -ge 2) {
        if (-not (Test-ArchiveSignature -Path $Dest -Type $ExpectedType)) {
            Write-Err "Downloaded file has unexpected format: $Desc"
            Write-Err "  Expected type: $ExpectedType"
            Write-Err "  Please verify your network/proxy and retry."
            Remove-Item $Dest -Force -ErrorAction SilentlyContinue
            Fail-Install "Downloaded file has unexpected format: $Desc"
        }
    }

    Write-Ok $Desc
}

function Resolve-Assets {
    param(
        [string]$ResolvedVersion
    )

    $resolvedReleaseUrl = $ReleaseUrl
    if (-not $resolvedReleaseUrl) { $resolvedReleaseUrl = $DefaultReleaseBase }

    $windowsZipName = $null
    $zipDestPath = $null
    $rootfsDestPath = $null

    if ($Offline) {
        Write-Step "Offline mode enabled (network requests disabled)"
        if ($ReleaseUrl) {
            Write-Warn "-ReleaseUrl is ignored in offline mode"
        }

        $searchDirs = New-Object System.Collections.Generic.List[string]
        if ($AssetDir) {
            if (-not (Test-Path $AssetDir)) {
                Fail-Install "Asset directory not found: $AssetDir"
            }
            $searchDirs.Add((Resolve-Path $AssetDir).Path)
        }

        $installerDir = Get-InstallerDirectory
        if ($installerDir -and -not $searchDirs.Contains($installerDir)) {
            $searchDirs.Add($installerDir)
        }

        if ($WindowsZipPath -and -not (Test-Path $WindowsZipPath)) {
            Fail-Install "Windows package not found: $WindowsZipPath"
        }
        if ($RootfsPath -and -not (Test-Path $RootfsPath)) {
            Fail-Install "Rootfs package not found: $RootfsPath"
        }

        if (-not $WindowsZipPath) {
            $zipCandidates = @()
            foreach ($dir in $searchDirs) {
                $zipCandidates += Get-ChildItem -Path $dir -Filter "clawspan-windows-*.zip" -File -ErrorAction SilentlyContinue
            }
            if ($zipCandidates.Count -gt 1) {
                $zipCandidates = $zipCandidates | Sort-Object `
                    @{ Expression = {
                        if ($_.Name -match '^clawspan-windows-(.+)\.zip$') {
                            try { [version]$Matches[1] } catch { [version]"0.0.0.0" }
                        } else {
                            [version]"0.0.0.0"
                        }
                    }; Descending = $true }, `
                    @{ Expression = { $_.Name }; Descending = $true }
                Write-Warn "Multiple Windows packages found, auto-selecting: $($zipCandidates[0].Name)"
                Write-Warn "Use -WindowsZipPath to choose a specific file."
            }
            if ($zipCandidates.Count -gt 0) {
                $WindowsZipPath = $zipCandidates[0].FullName
            }
        }

        if (-not $RootfsPath -and -not $isUpgrade) {
            foreach ($dir in $searchDirs) {
                $candidate = Join-Path $dir $RootfsName
                if (Test-Path $candidate) {
                    $RootfsPath = $candidate
                    break
                }
            }
        }

        if (-not $WindowsZipPath -or (-not $isUpgrade -and -not $RootfsPath)) {
            Write-Err "Offline assets not found."
            Write-Host ""
            Write-Host "  You can provide local assets in one of these ways:" -ForegroundColor Yellow
            Write-Host "    1) -WindowsZipPath and -RootfsPath" -ForegroundColor White
            Write-Host "    2) -AssetDir (directory contains both files)" -ForegroundColor White
            Write-Host "    3) Put files next to install.ps1 and run with -Offline" -ForegroundColor White
            Write-Host ""
            Write-Host "  Examples:" -ForegroundColor Yellow
            Write-Host "    .\install.ps1 -Offline -WindowsZipPath `"D:\pkg\clawspan-windows-0.1.1.zip`" -RootfsPath `"D:\pkg\clawspan-rootfs.tar.gz`"" -ForegroundColor White
            Write-Host "    .\install.ps1 -Offline -AssetDir `"D:\pkg`"" -ForegroundColor White
            Write-Host ""
            Fail-Install "Offline assets not found"
        }

        $zipLeaf = [System.IO.Path]::GetFileName($WindowsZipPath)
        if ($zipLeaf -match '^clawspan-windows-(.+)\.zip$') {
            if (-not $ResolvedVersion) { $ResolvedVersion = $Matches[1] }
        }
        if (-not $ResolvedVersion) {
            Fail-Install "Cannot determine version from Windows package filename. Please pass -Version <x.y.z> or use clawspan-windows-<ver>.zip naming."
        }

        $windowsZipName = [System.IO.Path]::GetFileName($WindowsZipPath)
        $zipDestPath = Join-Path $DownloadDir $windowsZipName
        Write-Step "Using local Windows package: $WindowsZipPath"
        if (-not (Test-ArchiveSignature -Path $WindowsZipPath -Type "zip")) {
            Fail-Install "Invalid Windows package format: $WindowsZipPath (expected .zip)"
        }
        Copy-Item -Path $WindowsZipPath -Destination $zipDestPath -Force
        Write-Ok "Windows package ready"

        if (-not $isUpgrade) {
            $rootfsDestPath = Join-Path $DownloadDir $RootfsName
            Write-Step "Using local VM image: $RootfsPath"
            if (-not (Test-ArchiveSignature -Path $RootfsPath -Type "gzip")) {
                Fail-Install "Invalid VM image format: $RootfsPath (expected .tar.gz)"
            }
            Copy-Item -Path $RootfsPath -Destination $rootfsDestPath -Force
            Write-Ok "VM image ready"
        }
    } else {
        if ($resolvedReleaseUrl -match 'github\.com') {
            Write-Step "Checking GitHub connectivity ..."
            $null = & $CurlExe -s --fail --head -L --connect-timeout 10 "https://github.com" 2>&1
            if ($LASTEXITCODE -ne 0) {
                Write-Err "Cannot connect to GitHub, please check your network"
                Write-Host ""
                Write-Host "  Possible causes:" -ForegroundColor Yellow
                Write-Host "    - No network connection" -ForegroundColor White
                Write-Host "    - Firewall/proxy blocking GitHub" -ForegroundColor White
                Write-Host "    - GitHub is temporarily unavailable" -ForegroundColor White
                Write-Host ""
                Write-Host "  For offline install, download assets manually and run install.ps1 -Offline." -ForegroundColor Yellow
                Write-Host ""
                Fail-Install "Cannot connect to GitHub"
            }
            Write-Ok "Network connection OK"
        }

        if (-not $ResolvedVersion) {
            Write-Step "Querying latest version ..."
            try {
                $apiJson = & $CurlExe -s --fail --connect-timeout 10 "https://api.github.com/repos/carlos-Ng/ClawSpan/releases/latest" 2>&1
                if ($LASTEXITCODE -eq 0 -and $apiJson) {
                    $apiResp = $apiJson | ConvertFrom-Json
                    if ($apiResp.tag_name) {
                        $ResolvedVersion = ($apiResp.tag_name -replace '^v', '')
                        Write-Ok "Latest version: $ResolvedVersion"
                    }
                }
            } catch {}

            if (-not $ResolvedVersion) {
                Write-Warn "GitHub API unavailable, trying redirect-based version detection ..."
                try {
                    $effectiveUrl = & $CurlExe -s -L -o NUL -w "%{url_effective}" --connect-timeout 10 "https://github.com/carlos-Ng/ClawSpan/releases/latest" 2>&1
                    if ($LASTEXITCODE -eq 0 -and $effectiveUrl -match "/releases/tag/v([^/?#]+)") {
                        $ResolvedVersion = $Matches[1]
                        Write-Ok "Latest version (redirect): $ResolvedVersion"
                    }
                } catch {}
            }

            if (-not $ResolvedVersion) {
                Write-Warn "Cannot auto-resolve latest version from GitHub."
                $manualVersion = Read-Host "  Enter version manually (e.g. 0.1.1), or press Enter to cancel"
                if ($manualVersion -match '^v(.+)$') { $manualVersion = $Matches[1] }
                if (-not $manualVersion) {
                    Fail-Install "No version provided. Installation cancelled."
                }
                $ResolvedVersion = $manualVersion
            }
        }

        $windowsZipName = "clawspan-windows-$ResolvedVersion.zip"
        $zipDestPath = Join-Path $DownloadDir $windowsZipName
        Invoke-Download -Url "$resolvedReleaseUrl/$windowsZipName" -Dest $zipDestPath -Desc "Windows package ($windowsZipName)" -ExpectedType "zip"

        if (-not $isUpgrade) {
            $rootfsDestPath = Join-Path $DownloadDir $RootfsName
            Invoke-Download -Url "$resolvedReleaseUrl/$RootfsName" -Dest $rootfsDestPath -Desc "VM image ($RootfsName)" -ExpectedType "gzip"
        }
    }

    return @{
        ResolvedVersion = $ResolvedVersion
        WindowsZipName  = $windowsZipName
        ZipDest         = $zipDestPath
        RootfsDest      = $rootfsDestPath
        ReleaseUrl      = $resolvedReleaseUrl
    }
}

$assets = Resolve-Assets -ResolvedVersion $Version
$ResolvedVersion = $assets.ResolvedVersion
$WindowsZipName = $assets.WindowsZipName
$zipDest = $assets.ZipDest
$rootfsDest = $assets.RootfsDest
$ReleaseUrl = $assets.ReleaseUrl

function Invoke-ConfigurationWizard {
    param(
        [bool]$IsUpgrade,
        [string]$ApiProvider,
        [string]$ApiKey,
        [string]$LocalModelUrl,
        [string]$LocalModelId
    )
    if ($IsUpgrade) {
        return @{
            ApiProvider   = $ApiProvider
            ApiKey        = $ApiKey
            LocalModelUrl = $LocalModelUrl
            LocalModelId  = $LocalModelId
        }
    }

    Write-Banner "Configuration"

    if (-not $ApiProvider) {
        Write-Host "  Select AI model provider:" -ForegroundColor White
        Write-Host "    1) Anthropic (Claude)  -- Recommended" -ForegroundColor White
        Write-Host "    2) OpenAI (GPT)" -ForegroundColor White
        Write-Host "    3) Local model (llama.cpp / Ollama / OpenAI-compatible)" -ForegroundColor White
        Write-Host "    4) Other (configure later in OpenClaw WebUI)" -ForegroundColor White
        $providerChoice = Read-Host "  Choose (1/2/3/4)"
        switch ($providerChoice) {
            "1" { $ApiProvider = "anthropic" }
            "2" { $ApiProvider = "openai" }
            "3" { $ApiProvider = "local" }
            default { $ApiProvider = "skip" }
        }
    }

    if ($ApiProvider -eq "local") {
        Write-Host ""
        Write-Host "  +---------------------------------------------+" -ForegroundColor Cyan
        Write-Host "  |  Local Model Configuration                  |" -ForegroundColor Cyan
        Write-Host "  |                                             |" -ForegroundColor Cyan
        Write-Host "  |  Supports any OpenAI-compatible API:        |" -ForegroundColor Cyan
        Write-Host "  |    - llama.cpp (--host 0.0.0.0 -p 8080)    |" -ForegroundColor Cyan
        Write-Host "  |    - Ollama (http://host:11434)             |" -ForegroundColor Cyan
        Write-Host "  |    - vLLM / SGLang / LocalAI / LM Studio   |" -ForegroundColor Cyan
        Write-Host "  +---------------------------------------------+" -ForegroundColor Cyan
        Write-Host ""

        if (-not $LocalModelUrl) {
            Write-Host "  Enter model service URL" -ForegroundColor White
            Write-Host "  Example: http://192.168.1.100:8080  (llama.cpp)" -ForegroundColor DarkGray
            Write-Host "  Example: http://192.168.1.100:11434 (Ollama)" -ForegroundColor DarkGray
            $LocalModelUrl = Read-Host "  Model URL"
        }

        if (-not $LocalModelUrl) {
            Write-Warn "No model URL provided, you can configure it later in OpenClaw WebUI"
            $ApiProvider = "skip"
        } else {
            $isOllama = $LocalModelUrl -match ":11434"
            if (-not $LocalModelId) {
                Write-Host ""
                Write-Host "  Enter model ID (leave empty for default)" -ForegroundColor White
                if ($isOllama) {
                    Write-Host "  Example: qwen3.5:27b, glm-4.7-flash, deepseek-r1:32b" -ForegroundColor DarkGray
                    $defaultModelId = "glm-4.7-flash"
                } else {
                    Write-Host "  Example: qwen3.5, glm-4.7, llama-3.3-70b" -ForegroundColor DarkGray
                    $defaultModelId = "default"
                }
                $LocalModelId = Read-Host "  Model ID (default: $defaultModelId)"
                if (-not $LocalModelId) { $LocalModelId = $defaultModelId }
            }
            Write-Ok "Local model configured ($LocalModelUrl -> $LocalModelId)"
        }
    }

    if ($ApiProvider -in @("anthropic", "openai") -and -not $ApiKey) {
        Write-Host ""
        if ($ApiProvider -eq "anthropic") {
            Write-Host "  Enter your Anthropic API Key" -ForegroundColor White
            Write-Host "  (Get it from https://console.anthropic.com/account/keys)" -ForegroundColor DarkGray
        } else {
            Write-Host "  Enter your OpenAI API Key" -ForegroundColor White
            Write-Host "  (Get it from https://platform.openai.com/api-keys)" -ForegroundColor DarkGray
        }
        $ApiKey = Read-Host "  API Key"
        if (-not $ApiKey) {
            Write-Warn "No API Key provided, you can configure it later in OpenClaw WebUI"
            $ApiProvider = "skip"
        }
    }

    if ($ApiProvider -ne "skip") {
        Write-Ok "API configuration ready ($ApiProvider)"
    }

    return @{
        ApiProvider   = $ApiProvider
        ApiKey        = $ApiKey
        LocalModelUrl = $LocalModelUrl
        LocalModelId  = $LocalModelId
    }
}

$wizard = Invoke-ConfigurationWizard `
    -IsUpgrade $isUpgrade `
    -ApiProvider $ApiProvider `
    -ApiKey $ApiKey `
    -LocalModelUrl $LocalModelUrl `
    -LocalModelId $LocalModelId
$ApiProvider = $wizard.ApiProvider
$ApiKey = $wizard.ApiKey
$LocalModelUrl = $wizard.LocalModelUrl
$LocalModelId = $wizard.LocalModelId

# -- Install files ---------------------------------------------------------

Write-Banner "Install"

# Create directories
New-Item -ItemType Directory -Path $BinDir    -Force | Out-Null
New-Item -ItemType Directory -Path $ModuleDir -Force | Out-Null
New-Item -ItemType Directory -Path $ConfigDir -Force | Out-Null
New-Item -ItemType Directory -Path $DistroDir -Force | Out-Null

# Stop running processes
Write-Step "Stopping old processes ..."
Stop-ClawSpanProcesses -TimeoutSeconds 12 | Out-Null

# Extract Windows package
# zip 结构：bin/(exe), modules/(dll), config/(toml)
# 解压到 $InstallDir，使各文件落入对应子目录
Write-Step "Extracting Windows package ..."
$zipPath = $zipDest
Expand-Archive -Path $zipPath -DestinationPath $InstallDir -Force
Write-Ok "Program files installed"
Assert-WindowsPayloadFiles -BinDir $BinDir -ModuleDir $ModuleDir

# -- Import WSL Distro -----------------------------------------------------

if (-not $isUpgrade) {
    Write-Step "Importing WSL VM ..."
    $rootfsPath = $rootfsDest

    if (-not (Test-Path $rootfsPath)) {
        Fail-Install "rootfs file not found: $rootfsPath"
    }

    $rootfsSizeMB = [math]::Round((Get-Item $rootfsPath).Length / 1MB, 0)
    $exitCode = Invoke-WslWithSpinner `
        -WslArguments "--import $DistroName `"$DistroDir`" `"$rootfsPath`"" `
        -StatusPrefix "Importing VM image (${rootfsSizeMB} MB compressed)" `
        -MonitorDir $DistroDir
    if ($exitCode -ne 0) {
        Fail-Install "WSL import failed (exit code: $exitCode)"
    }
    Write-Ok "VM imported"

    # Warm up the distro (first boot emits systemd/dbus messages; run silently to absorb them)
    Write-Step "Initializing WSL VM (first boot) ..."
    Invoke-WslWithSpinner `
        -WslArguments "-d $DistroName -- bash -c `"sleep 2 && exit 0`"" `
        -StatusPrefix "First boot initialization" | Out-Null
    Write-Ok "WSL VM ready"
}

# -- Write config ----------------------------------------------------------

function Write-InstallConfiguration {
    param(
        [bool]$IsUpgrade,
        [string]$ConfigDir,
        [string]$ModuleDir,
        [string]$DistroName,
        [string]$ApiProvider,
        [string]$ApiKey,
        [string]$LocalModelUrl,
        [string]$LocalModelId
    )
    Write-Step "Writing configuration ..."

    $ModuleDirToml = $ModuleDir -replace '\\', '\\\\'
    $ConfigDirToml = $ConfigDir -replace '\\', '\\\\'
    $rulesFile     = "$ConfigDirToml\\security_filter_rules.toml"
    $nl = "`n"
    $daemonConfigLines = @(
        "[daemon]",
        "socket_path      = `"\\\\.\\pipe\\crew-shell-service`"",
        "thread_pool_size = 4",
        "log_level        = `"info`"",
        "module_dir       = `"$ModuleDirToml`"",
        "",
        "[ui]",
        "pipe_path    = `"\\\\.\\pipe\\crew-shell-service-ui`"",
        "timeout_mode = `"timeout_deny`"",
        "timeout_secs = 60",
        "",
        "[vsock]",
        "port    = 100",
        "enabled = true",
        "",
        "[vmm]",
        "distro_name = `"$DistroName`"",
        "auto_start  = true",
        "",
        "[[modules]]",
        "name = `"capability_ax`"",
        "",
        "[[modules]]",
        "name       = `"security_filter`"",
        "priority   = 10",
        "rules_file = `"$rulesFile`"",
        ""
    )
    $daemonConfigContent = $daemonConfigLines -join $nl
    [System.IO.File]::WriteAllText(
        (Join-Path $ConfigDir "daemon.toml"),
        $daemonConfigContent,
        [System.Text.UTF8Encoding]::new($false)
    )
    Write-Ok "daemon.toml"

    $tokenFilePath = Join-Path $ConfigDir "gateway-token.txt"
    $GatewayToken = $null
    $GatewayPort = 18789

    if (-not $IsUpgrade) {
        $GatewayToken = -join ((48..57) + (65..90) + (97..122) | Get-Random -Count 32 | ForEach-Object { [char]$_ })
        $openclawConfig = @{
            gateway = @{
                port = $GatewayPort
                mode = "local"
                bind = "lan"
                auth = @{ mode = "token"; token = $GatewayToken }
            }
            plugins = @{
                entries = @{
                    acpx = @{
                        enabled = $true
                        config = @{
                            mcpServers = @{
                                "clawspan-gui" = @{
                                    command = "python3"
                                    args = @("/opt/clawspan/mcp/launch_mcp_server.py")
                                }
                            }
                        }
                    }
                }
            }
            skills = @{ load = @{ extraDirs = @("/opt/clawspan/skills") } }
        }

        if ($ApiProvider -eq "local" -and $LocalModelUrl) {
            $isOllama = $LocalModelUrl -match ":11434"
            if ($isOllama) {
                $ollamaBaseUrl = $LocalModelUrl -replace '/v1$', ''
                $openclawConfig["models"] = @{
                    providers = @{
                        ollama = @{
                            baseUrl = $ollamaBaseUrl
                            apiKey  = "ollama-local"
                            api     = "ollama"
                            models  = @(@{
                                id            = $LocalModelId
                                name          = $LocalModelId
                                reasoning     = ([bool]($LocalModelId -match 'r1|reason|think'))
                                input         = @("text")
                                cost          = @{ input = 0; output = 0; cacheRead = 0; cacheWrite = 0 }
                                contextWindow = 131072
                                maxTokens     = 8192
                            })
                        }
                    }
                }
                $openclawConfig["agents"] = @{ defaults = @{ model = @{ primary = "ollama/$LocalModelId" } } }
            } else {
                $apiBaseUrl = $LocalModelUrl.TrimEnd('/')
                if ($apiBaseUrl -notmatch '/v1$') { $apiBaseUrl += "/v1" }
                $isQwen = $LocalModelId -match 'qwen'
                $modelCompat = @{}
                if ($isQwen) { $modelCompat["thinkingFormat"] = "qwen" }
                $modelDef = @{
                    id            = $LocalModelId
                    name          = $LocalModelId
                    reasoning     = ([bool]($LocalModelId -match 'r1|reason|think|qwen'))
                    input         = @("text")
                    cost          = @{ input = 0; output = 0; cacheRead = 0; cacheWrite = 0 }
                    contextWindow = 32768
                    maxTokens     = 8192
                }
                if ($modelCompat.Count -gt 0) { $modelDef["compat"] = $modelCompat }
                $openclawConfig["models"] = @{
                    providers = @{
                        local_llm = @{
                            baseUrl                      = $apiBaseUrl
                            apiKey                       = "no-key"
                            api                          = "openai-completions"
                            injectNumCtxForOpenAICompat = $false
                            models                       = @( $modelDef )
                        }
                    }
                }
                $openclawConfig["agents"] = @{ defaults = @{ model = @{ primary = "local_llm/$LocalModelId" } } }
            }
        }

        $configJson = $openclawConfig | ConvertTo-Json -Depth 10
        Invoke-WslSilent "-d $DistroName -- bash -c `"cat > /home/clawspan/.openclaw/openclaw.json`"" -InputText $configJson | Out-Null
        Write-Ok "OpenClaw config (with Gateway token)"
    } elseif (Test-Path $tokenFilePath) {
        $existingToken = Get-Content $tokenFilePath -ErrorAction SilentlyContinue |
            Where-Object { $_ -match '^[A-Za-z0-9]{16,}$' } |
            Select-Object -Last 1
        if ($existingToken) {
            $GatewayToken = $existingToken.Trim()
            Write-Ok "Reusing existing Gateway token"
        }
    }

    if (-not $IsUpgrade -and $ApiProvider -in @("anthropic", "openai")) {
        $envContent = if ($ApiProvider -eq "anthropic") {
            "export ANTHROPIC_API_KEY=`"$ApiKey`""
        } else {
            "export OPENAI_API_KEY=`"$ApiKey`""
        }
        Invoke-WslSilent "-d $DistroName -- bash -c `"cat > /home/clawspan/.clawspan-env`"" -InputText $envContent | Out-Null
        Invoke-WslSilent "-d $DistroName -- bash -c `"chown clawspan:clawspan /home/clawspan/.clawspan-env && chmod 600 /home/clawspan/.clawspan-env`"" | Out-Null
        $bashrcLine = '[[ -f ~/.clawspan-env ]] && source ~/.clawspan-env'
        Invoke-WslSilent "-d $DistroName -- bash -c `"grep -qF .clawspan-env /home/clawspan/.bashrc 2>/dev/null || echo '$bashrcLine' >> /home/clawspan/.bashrc`"" | Out-Null
        Write-Ok "OpenClaw API config"
    }

    if (-not $IsUpgrade -and $ApiProvider -eq "local" -and $LocalModelUrl -match ":11434") {
        $envContent = "export OLLAMA_API_KEY=`"ollama-local`""
        Invoke-WslSilent "-d $DistroName -- bash -c `"cat > /home/clawspan/.clawspan-env`"" -InputText $envContent | Out-Null
        Invoke-WslSilent "-d $DistroName -- bash -c `"chown clawspan:clawspan /home/clawspan/.clawspan-env && chmod 600 /home/clawspan/.clawspan-env`"" | Out-Null
        $bashrcLine = '[[ -f ~/.clawspan-env ]] && source ~/.clawspan-env'
        Invoke-WslSilent "-d $DistroName -- bash -c `"grep -qF .clawspan-env /home/clawspan/.bashrc 2>/dev/null || echo '$bashrcLine' >> /home/clawspan/.bashrc`"" | Out-Null
        Write-Ok "Ollama environment variables"
    }

    Write-Step "Installing OpenClaw Gateway service ..."
    $installExitCode = 1
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        if ($attempt -gt 1) {
            Write-Step "Retrying service registration ($attempt/3) ..."
            Start-Sleep -Seconds 2
        }
        $installExitCode = Invoke-WslSilent "-d $DistroName -u clawspan -- bash -lc `"openclaw daemon install >/dev/null 2>&1`""
        if ($installExitCode -eq 0) { break }
    }
    if ($installExitCode -eq 0) {
        Write-Ok "OpenClaw Gateway registered as systemd service"
    } else {
        Write-Warn "OpenClaw Gateway service registration skipped (environment not fully ready yet)."
        Write-Warn "It will be retried on next install/upgrade. Manual command: wsl -d $DistroName -u clawspan -- bash -lc 'openclaw daemon install'"
    }

    Write-Step "Checking systemd linger state ..."
    $lingerExistsExit = Invoke-WslSilent "-d $DistroName -- bash -lc `"test -f /var/lib/systemd/linger/clawspan`""
    if ($lingerExistsExit -eq 0) {
        Write-Ok "systemd linger already configured"
    } else {
        $lingerEnableExit = Invoke-WslSilent "-d $DistroName -- bash -lc `"loginctl enable-linger clawspan >/dev/null 2>&1`""
        if ($lingerEnableExit -eq 0) {
            Write-Ok "systemd linger enabled (OpenClaw will auto-start when distro boots)"
        } else {
            Write-Warn "systemd linger setup skipped; Windows daemon will start OpenClaw on boot."
        }
    }

    Write-Step "Starting OpenClaw Gateway ..."
    $startExitCode = 1
    for ($attempt = 1; $attempt -le 2; $attempt++) {
        if ($attempt -gt 1) { Start-Sleep -Seconds 1 }
        $startExitCode = Invoke-WslSilent "-d $DistroName -u clawspan -- bash -lc `"openclaw daemon start >/dev/null 2>&1`""
        if ($startExitCode -eq 0) { break }
    }
    if ($startExitCode -eq 0) {
        Write-Ok "OpenClaw Gateway started"
    } else {
        Write-Warn "OpenClaw Gateway start failed (exit code: $startExitCode)"
        Write-Warn "You can run manually later: wsl -d $DistroName -u clawspan -- bash -lc 'openclaw daemon start'"
    }

    if (-not $IsUpgrade -and $GatewayToken) {
@"
# ClawSpan OpenClaw Gateway Access Token
# Keep this safe, do not share
#
# WebUI URL: http://localhost:$GatewayPort
# Access token:
$GatewayToken
"@ | Set-Content -Path $tokenFilePath -Encoding UTF8
        Write-Ok "Gateway token saved to $tokenFilePath"
    }

    return @{
        GatewayToken   = $GatewayToken
        GatewayPort    = $GatewayPort
        TokenFilePath  = $tokenFilePath
        DaemonTomlPath = (Join-Path $ConfigDir "daemon.toml")
    }
}

$configResult = Write-InstallConfiguration `
    -IsUpgrade $isUpgrade `
    -ConfigDir $ConfigDir `
    -ModuleDir $ModuleDir `
    -DistroName $DistroName `
    -ApiProvider $ApiProvider `
    -ApiKey $ApiKey `
    -LocalModelUrl $LocalModelUrl `
    -LocalModelId $LocalModelId
$GatewayToken = $configResult.GatewayToken
$GatewayPort = $configResult.GatewayPort
$tokenFilePath = $configResult.TokenFilePath

# -- Register startup ------------------------------------------------------

function Register-StartupShortcut {
    param(
        [string]$AppName,
        [string]$BinDir,
        [string]$ConfigPath,
        [string]$StartupDir
    )
    Write-Step "Registering startup shortcut ..."
    $daemonExe = Join-Path $BinDir "claw_span_service.exe"
    $shortcutPath = Join-Path $StartupDir "$AppName.lnk"
    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $shell.CreateShortcut($shortcutPath)
    $shortcut.TargetPath = $daemonExe
    $shortcut.Arguments = "--config `"$ConfigPath`""
    $shortcut.WorkingDirectory = $BinDir
    $shortcut.Description = "$AppName Daemon"
    $shortcut.Save()
    Write-Ok "Startup shortcut created"
    return $daemonExe
}

# -- Register in Add/Remove Programs --------------------------------------

function Register-UninstallEntry {
    param(
        [string]$InstallDir,
        [string]$ReleaseUrl,
        [bool]$Offline,
        [string]$CurlExe,
        [string]$UninstallRegKey,
        [string]$AppName,
        [string]$ResolvedVersion
    )
    Write-Step "Saving uninstall script ..."
    $uninstScriptsDir = Join-Path $InstallDir "scripts"
    New-Item -ItemType Directory -Path $uninstScriptsDir -Force | Out-Null
    $localScriptPath = Join-Path $uninstScriptsDir "uninstall.ps1"
    $scriptSaved = $false

    if ($PSCommandPath -and (Test-Path $PSCommandPath -ErrorAction SilentlyContinue)) {
        try {
            Copy-Item $PSCommandPath $localScriptPath -Force
            $scriptSaved = $true
        } catch {}
    }

    if (-not $scriptSaved -and $ReleaseUrl -and -not $Offline) {
        try {
            $scriptUrl = "$ReleaseUrl/install.ps1"
            & $CurlExe -s --fail -L --connect-timeout 10 -o $localScriptPath $scriptUrl 2>&1 | Out-Null
            if ($LASTEXITCODE -eq 0 -and (Test-Path $localScriptPath) -and (Get-Item $localScriptPath).Length -gt 1000) {
                $scriptSaved = $true
            }
        } catch {}
    }

    if (-not $scriptSaved) {
        try {
            $scriptText = "#Requires -Version 5.1`r`n" + $MyInvocation.MyCommand.ScriptBlock.ToString()
            $utf8Bom = New-Object System.Text.UTF8Encoding $true
            [System.IO.File]::WriteAllText($localScriptPath, $scriptText, $utf8Bom)
            $scriptSaved = $true
        } catch {}
    }

    if ($scriptSaved) {
        Write-Ok "Uninstall script saved"
    } else {
        Write-Warn "Could not save uninstall script"
    }

    Write-Step "Registering uninstall info ..."
    New-Item -Path $UninstallRegKey -Force | Out-Null
    $uninstallCmd = "powershell.exe -ExecutionPolicy Bypass -NoProfile -File `"$localScriptPath`" -Uninstall"

    Set-ItemProperty -Path $UninstallRegKey -Name "DisplayName"     -Value $AppName
    Set-ItemProperty -Path $UninstallRegKey -Name "DisplayVersion"  -Value $ResolvedVersion
    Set-ItemProperty -Path $UninstallRegKey -Name "Publisher"       -Value "ClawSpan"
    Set-ItemProperty -Path $UninstallRegKey -Name "InstallLocation" -Value $InstallDir
    Set-ItemProperty -Path $UninstallRegKey -Name "UninstallString" -Value $uninstallCmd
    Set-ItemProperty -Path $UninstallRegKey -Name "QuietUninstallString" -Value $uninstallCmd
    Set-ItemProperty -Path $UninstallRegKey -Name "NoModify"        -Value 1 -Type DWord
    Set-ItemProperty -Path $UninstallRegKey -Name "NoRepair"        -Value 1 -Type DWord
    Write-Ok "Registered in Settings > Apps"
}

$configPath = $configResult.DaemonTomlPath
$daemonExe = Register-StartupShortcut -AppName $AppName -BinDir $BinDir -ConfigPath $configPath -StartupDir $StartupDir
Register-UninstallEntry `
    -InstallDir $InstallDir `
    -ReleaseUrl $ReleaseUrl `
    -Offline $Offline `
    -CurlExe $CurlExe `
    -UninstallRegKey $UninstallRegKey `
    -AppName $AppName `
    -ResolvedVersion $ResolvedVersion

# -- Clean up downloads ----------------------------------------------------

Write-Step "Cleaning download cache ..."
Remove-Item $DownloadDir -Recurse -Force -ErrorAction SilentlyContinue
Write-Ok "Download cache cleaned"

# -- Launch ----------------------------------------------------------------

Write-Banner "Starting $AppName"

Write-Step "Starting daemon ..."
Start-Process -FilePath $daemonExe -ArgumentList "--config", "`"$configPath`"" -WindowStyle Hidden
Write-Ok "Daemon started"

# UI
$uiExe = Join-Path $BinDir "claw_span_ui.exe"
if (Test-Path $uiExe) {
    Write-Step "Starting UI ..."
    Start-Process -FilePath $uiExe -WindowStyle Hidden
    Write-Ok "UI started (check system tray)"
}

# -- Wait for OpenClaw Gateway to be ready ---------------------------------

Write-Host ""
Write-Step "Waiting for OpenClaw Gateway to initialize ..."
$gatewayReady = $false
$spinChars = @('-', '\', '|', '/')
for ($waitIdx = 0; $waitIdx -lt 20; $waitIdx++) {
    Start-Sleep -Seconds 2
    $spin = $spinChars[$waitIdx % 4]
    $elapsed = ($waitIdx + 1) * 2
    Write-Host "`r  $spin Waiting for OpenClaw Gateway (${elapsed}s) ...          " -NoNewline
    $null = & $CurlExe -s --connect-timeout 2 "http://localhost:$GatewayPort" 2>&1
    if ($LASTEXITCODE -ne 7 -and $LASTEXITCODE -ne 28) {
        $gatewayReady = $true
        break
    }
}
Write-Host "`r                                                              `r" -NoNewline

if ($gatewayReady) {
    Write-Ok "OpenClaw Gateway is ready!"
} else {
    Write-Warn "OpenClaw Gateway is still starting up."
    Write-Warn "The system tray icon may appear grey for up to a minute."
    Write-Warn "This is normal -- services are initializing in the background."
}

# -- Done ------------------------------------------------------------------

$actionText = if ($isUpgrade) { "upgrade" } else { "installation" }

Write-Host ""
Write-Host "==================================================" -ForegroundColor Green
Write-Host "  [OK] $AppName $actionText complete!" -ForegroundColor Green
Write-Host "==================================================" -ForegroundColor Green
Write-Host ""
Write-Host "  Install path: $InstallDir" -ForegroundColor White
Write-Host "  WSL Distro:   $DistroName" -ForegroundColor White
Write-Host ""

# OpenClaw WebUI access info
$displayToken = if ($GatewayToken) { $GatewayToken } else { "(unchanged, see token file)" }
Write-Host "  +----------------------------------------------+" -ForegroundColor Cyan
Write-Host "  |  OpenClaw WebUI                              |" -ForegroundColor Cyan
Write-Host "  |                                              |" -ForegroundColor Cyan
Write-Host "  |  URL:   http://localhost:$GatewayPort            |" -ForegroundColor Cyan
Write-Host "  |  Token: $displayToken  |" -ForegroundColor Cyan
Write-Host "  |                                              |" -ForegroundColor Cyan
Write-Host "  |  Open the URL above and enter the token.     |" -ForegroundColor Cyan
Write-Host "  +----------------------------------------------+" -ForegroundColor Cyan
Write-Host ""
if (Test-Path $tokenFilePath) {
    Write-Host "  Token file: $tokenFilePath" -ForegroundColor DarkGray
}
Write-Host ""

if ($ApiProvider -eq "skip" -or $isUpgrade) {
    Write-Host "  API Key not configured. Set it up in the WebUI." -ForegroundColor Yellow
    Write-Host ""
}

if ($ApiProvider -eq "local" -and $LocalModelUrl) {
    Write-Host "  +----------------------------------------------+" -ForegroundColor Magenta
    Write-Host "  |  Local Model                                 |" -ForegroundColor Magenta
    Write-Host "  |                                              |" -ForegroundColor Magenta
    Write-Host "  |  URL:      $($LocalModelUrl.PadRight(35))|" -ForegroundColor Magenta
    Write-Host "  |  Model ID: $($LocalModelId.PadRight(35))|" -ForegroundColor Magenta
    Write-Host "  |                                              |" -ForegroundColor Magenta
    Write-Host "  |  Make sure the model service is running      |" -ForegroundColor Magenta
    Write-Host "  |  and accessible from WSL.                    |" -ForegroundColor Magenta
    Write-Host "  +----------------------------------------------+" -ForegroundColor Magenta
    Write-Host ""
}

Write-Host "  Management:" -ForegroundColor White
Write-Host "    - System tray icon  -- Status & quick actions" -ForegroundColor DarkGray
Write-Host "    - OpenClaw WebUI    -- AI chat & advanced settings" -ForegroundColor DarkGray
Write-Host "    - wsl -d $DistroName -- CLI (advanced users)" -ForegroundColor DarkGray
Write-Host ""
Write-Host "  Uninstall:" -ForegroundColor White
Write-Host "    Settings > Apps > $AppName > Uninstall" -ForegroundColor DarkGray
Write-Host ""

param(
    [switch]$AllUsers,
    [string]$BuildDirectory = ""
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $Root "native\build" }
if (-not [System.IO.Path]::IsPathRooted($BuildDirectory)) {
    $BuildDirectory = Join-Path (Get-Location).Path $BuildDirectory
}
$BuildDirectory = [System.IO.Path]::GetFullPath($BuildDirectory)
$Clsid = "{B3F8E8E4-1C65-4C10-9DB4-AD2B780A6401}"
$Scope = if ($AllUsers) { "all-users" } else { "current-user" }
$ProgramFiles = [Environment]::GetFolderPath(
    [Environment+SpecialFolder]::ProgramFiles)
$InstallDirectory = Join-Path $ProgramFiles "VIVIDCAM\VirtualCamera"
$InstalledServer = Join-Path $InstallDirectory "vividcam_virtual_camera_source.dll"
$InstalledDiagnostics = Join-Path $InstallDirectory "vividcam_diagnostics.exe"
$InstalledEngine = Join-Path $InstallDirectory "vividcam_engine.exe"
$ProducerIdentityKey = "HKLM:\Software\VIVIDCAM\VirtualCamera\ProducerIdentity"
if ($AllUsers) {
    if ([Environment]::Is64BitOperatingSystem -and
        -not [Environment]::Is64BitProcess) {
        throw "All-users COM removal requires 64-bit PowerShell"
    }
    $Identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $Principal = [Security.Principal.WindowsPrincipal]::new($Identity)
    if (-not $Principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "All-users COM removal requires an elevated PowerShell session"
    }
}

function Stop-VividCamFrameServer {
    $Service = Get-Service -Name "FrameServer" -ErrorAction SilentlyContinue
    if ($null -eq $Service -or $Service.Status -eq "Stopped") { return }
    if ($Service.Status -eq "StopPending") {
        $Service.WaitForStatus(
            [System.ServiceProcess.ServiceControllerStatus]::Stopped,
            [TimeSpan]::FromSeconds(15))
        return
    }
    if ($Service.Status -eq "StartPending") {
        $Service.WaitForStatus(
            [System.ServiceProcess.ServiceControllerStatus]::Running,
            [TimeSpan]::FromSeconds(15))
        $Service.Refresh()
    }
    if (-not $Service.CanStop) {
        throw ("FrameServer is running but does not accept stop controls. " +
               "Close applications using a camera and retry.")
    }
    Write-Host "[VIVIDCAM] Stopping FrameServer to remove the loaded camera source" `
        -ForegroundColor Yellow
    Stop-Service -InputObject $Service -ErrorAction Stop
    $Service.WaitForStatus(
        [System.ServiceProcess.ServiceControllerStatus]::Stopped,
        [TimeSpan]::FromSeconds(15))
}

function Assert-VividCamFrameServerCanStop {
    $Service = Get-Service -Name "FrameServer" -ErrorAction SilentlyContinue
    if ($null -eq $Service -or $Service.Status -eq "Stopped" -or
        $Service.Status -eq "StopPending") {
        return
    }
    if ($Service.Status -eq "StartPending") {
        $Service.WaitForStatus(
            [System.ServiceProcess.ServiceControllerStatus]::Running,
            [TimeSpan]::FromSeconds(15))
        $Service.Refresh()
    }
    if (-not $Service.CanStop) {
        throw ("FrameServer is running but does not accept stop controls. " +
               "Close applications using a camera and retry.")
    }
}

function Remove-VividCamActivationServerAfterUnload {
    param([string]$Path)

    $Deadline = [DateTime]::UtcNow.AddSeconds(15)
    $WaitMessageWritten = $false
    while ($true) {
        try {
            Remove-Item -LiteralPath $Path -Force
            return
        } catch {
            $RemovalFailure = $_.Exception.Message
            if ([DateTime]::UtcNow -ge $Deadline) {
                throw ("Activation server remains loaded after the virtual " +
                       "camera was removed. Close camera applications and " +
                       "retry; if it remains locked, restart Windows. " +
                       $RemovalFailure)
            }
            if (-not $WaitMessageWritten) {
                Write-Host ("[VIVIDCAM] Waiting for Windows camera services " +
                            "to release the activation server") `
                    -ForegroundColor Yellow
                $WaitMessageWritten = $true
            }
            Start-Sleep -Milliseconds 250
        }
    }
}

function Test-InstalledEngineRunning {
    param([string]$InstalledEnginePath)
    foreach ($Process in @(Get-Process -Name "vividcam_engine" `
                            -ErrorAction SilentlyContinue)) {
        try {
            if ($Process.Path -and [string]::Equals(
                    [System.IO.Path]::GetFullPath($Process.Path),
                    $InstalledEnginePath,
                    [StringComparison]::OrdinalIgnoreCase)) {
                return $true
            }
        } catch {
            if (-not $Process.HasExited) {
                throw ("Could not verify a running vividcam_engine process. " +
                       "Stop all VIVIDCAM engines and retry.")
            }
        }
    }
    return $false
}

$Key = if ($AllUsers) {
    "HKLM:\Software\Classes\CLSID\$Clsid"
} else {
    "HKCU:\Software\Classes\CLSID\$Clsid"
}

$AllUsersPackageArtifactsPresent = $false
if ($AllUsers) {
    $AllUsersPackageArtifactsPresent =
        (Test-Path -LiteralPath $InstalledServer) -or
        (Test-Path -LiteralPath $InstalledDiagnostics) -or
        (Test-Path -LiteralPath $InstalledEngine) -or
        (Test-Path -LiteralPath $ProducerIdentityKey) -or
        (Test-Path -LiteralPath $Key)
}

$BuildDiagnostics = Join-Path $BuildDirectory "Release\vividcam_diagnostics.exe"
$CameraManager = if (Test-Path -LiteralPath $InstalledDiagnostics `
                              -PathType Leaf) {
    $InstalledDiagnostics
} elseif (Test-Path -LiteralPath $BuildDiagnostics -PathType Leaf) {
    $BuildDiagnostics
} else {
    $null
}
if ($AllUsers -and -not $AllUsersPackageArtifactsPresent -and
    -not $CameraManager) {
    if (Test-Path -LiteralPath $InstallDirectory) {
        if (-not (Test-Path -LiteralPath $InstallDirectory `
                                 -PathType Container)) {
            throw "VIVIDCAM install directory path is not a directory: $InstallDirectory"
        }
        if ((Get-ChildItem -LiteralPath $InstallDirectory -Force).Count -ne 0) {
            throw ("Cannot verify or remove the persistent virtual camera " +
                   "because vividcam_diagnostics.exe is missing and the " +
                   "install directory is not empty: $InstallDirectory")
        }
        Remove-Item -LiteralPath $InstallDirectory -Force
    }
    Write-Host "[VIVIDCAM] $Scope activation server was not registered" `
        -ForegroundColor Yellow
    return
}
if (-not $CameraManager) {
    throw ("Cannot remove the persistent virtual camera because vividcam_diagnostics.exe " +
           "was not found in the install or build directory")
}

if ($AllUsers) {
    if ((Test-Path -LiteralPath $InstallDirectory) -and
        -not (Test-Path -LiteralPath $InstallDirectory -PathType Container)) {
        throw "VIVIDCAM install directory path is not a directory: $InstallDirectory"
    }
    foreach ($InstalledPath in @(
            $InstalledServer, $InstalledDiagnostics, $InstalledEngine)) {
        if ((Test-Path -LiteralPath $InstalledPath) -and
            -not (Test-Path -LiteralPath $InstalledPath -PathType Leaf)) {
            throw "VIVIDCAM installed component path is not a file: $InstalledPath"
        }
    }
    if ((Test-Path -LiteralPath $InstalledEngine -PathType Leaf) -and
        (Test-InstalledEngineRunning $InstalledEngine)) {
        throw ("Installed VIVIDCAM engine is running and cannot be removed. " +
               "Stop it and retry: $InstalledEngine")
    }
    if (Test-Path -LiteralPath $InstalledServer -PathType Leaf) {
        Assert-VividCamFrameServerCanStop
    }
}

& $CameraManager --remove-camera
$CameraRemovalExitCode = $LASTEXITCODE
if ($CameraRemovalExitCode -ne 0) {
    throw ("Persistent current-user virtual camera removal failed " +
           "with exit code $CameraRemovalExitCode")
}

if (-not $AllUsers) {
    if (Test-Path -LiteralPath $Key) {
        Remove-Item -LiteralPath $Key -Recurse -Force
        Write-Host "[VIVIDCAM] Removed $Scope activation server" `
            -ForegroundColor Green
    } else {
        Write-Host "[VIVIDCAM] $Scope activation server was not registered" `
            -ForegroundColor Yellow
    }
    return
}

if (Test-Path -LiteralPath $InstalledServer -PathType Leaf) {
    try {
        Stop-VividCamFrameServer
        Remove-VividCamActivationServerAfterUnload -Path $InstalledServer
    } catch {
        $SourceRemovalFailure = $_.Exception.Message
        $CameraRecoveryFailure = $null
        try {
            & $CameraManager --install-camera
            $CameraRecoveryExitCode = $LASTEXITCODE
            if ($CameraRecoveryExitCode -ne 0) {
                throw ("Persistent current-user virtual camera recovery failed " +
                       "with exit code $CameraRecoveryExitCode")
            }
            Write-Host ("[VIVIDCAM] Restored the persistent virtual camera " +
                        "after the uninstall was interrupted") `
                -ForegroundColor Yellow
        } catch {
            $CameraRecoveryFailure = $_.Exception.Message
        }
        $FailureMessage =
            "VIVIDCAM activation server removal failed: $SourceRemovalFailure; " +
            "installed diagnostics, engine, COM registration, and manifest were preserved for retry"
        if ($null -ne $CameraRecoveryFailure) {
            $FailureMessage += "; camera recovery failed: $CameraRecoveryFailure"
        }
        throw $FailureMessage
    }
}

$RemovalErrors = @()
if (Test-Path -LiteralPath $ProducerIdentityKey) {
    try {
        Remove-Item -LiteralPath $ProducerIdentityKey -Recurse -Force
    } catch {
        $RemovalErrors +=
            "Producer identity manifest removal failed: $($_.Exception.Message)"
    }
}

foreach ($InstalledFile in @($InstalledEngine)) {
    if (-not (Test-Path -LiteralPath $InstalledFile)) { continue }
    try {
        Remove-Item -LiteralPath $InstalledFile -Force
    } catch {
        $RemovalErrors +=
            "Installed file removal failed ($InstalledFile): $($_.Exception.Message)"
    }
}

if (Test-Path -LiteralPath $Key) {
    try {
        Remove-Item -LiteralPath $Key -Recurse -Force
    } catch {
        $RemovalErrors +=
            "COM registration removal failed ($Key): $($_.Exception.Message)"
    }
}

if ($RemovalErrors.Count -gt 0) {
    throw ("VIVIDCAM all-users removal was incomplete: " +
           ($RemovalErrors -join " | ") +
           "; installed diagnostics were preserved for retry")
}

if (Test-Path -LiteralPath $InstalledDiagnostics) {
    try {
        Remove-Item -LiteralPath $InstalledDiagnostics -Force
    } catch {
        throw ("VIVIDCAM all-users removal was incomplete: installed diagnostics " +
               "removal failed ($InstalledDiagnostics): $($_.Exception.Message)")
    }
}

try {
    if ((Test-Path -LiteralPath $InstallDirectory) -and
        (Get-ChildItem -LiteralPath $InstallDirectory -Force).Count -eq 0) {
        Remove-Item -LiteralPath $InstallDirectory -Force
    }
} catch {
    throw ("VIVIDCAM all-users removal was incomplete: " +
           "empty install directory removal failed: $($_.Exception.Message)")
}

if (-not (Test-Path -LiteralPath $Key)) {
    Write-Host "[VIVIDCAM] Removed $Scope activation server" -ForegroundColor Green
}
if (-not (Test-Path -LiteralPath $InstalledServer) -and
    -not (Test-Path -LiteralPath $InstalledDiagnostics) -and
    -not (Test-Path -LiteralPath $InstalledEngine) -and
    -not (Test-Path -LiteralPath $ProducerIdentityKey)) {
    Write-Host "[VIVIDCAM] Removed installed native components" `
        -ForegroundColor Green
}

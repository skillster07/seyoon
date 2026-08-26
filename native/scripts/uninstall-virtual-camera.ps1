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

function Stop-VividCamFrameServerServices {
    $StoppedServices = @()
    try {
        foreach ($ServiceName in @("FrameServerMonitor", "FrameServer")) {
            $Service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
            if ($null -eq $Service -or $Service.Status -eq "Stopped") { continue }
            Write-Host "[VIVIDCAM] Stopping $ServiceName to remove the loaded camera source" `
                -ForegroundColor Yellow
            $StoppedServices += $ServiceName
            Stop-Service -InputObject $Service -ErrorAction Stop
            $Service.WaitForStatus(
                [System.ServiceProcess.ServiceControllerStatus]::Stopped,
                [TimeSpan]::FromSeconds(15))
        }
    } catch {
        Restart-VividCamFrameServerServices $StoppedServices
        throw
    }
    return $StoppedServices
}

function Restart-VividCamFrameServerServices {
    param([string[]]$ServiceNames)
    foreach ($ServiceName in @("FrameServer", "FrameServerMonitor")) {
        if ($ServiceNames -notcontains $ServiceName) { continue }
        $Service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
        if ($null -eq $Service -or $Service.Status -eq "Running") { continue }
        Start-Service -InputObject $Service -ErrorAction Stop
        $Service.WaitForStatus(
            [System.ServiceProcess.ServiceControllerStatus]::Running,
            [TimeSpan]::FromSeconds(15))
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

$BuildDiagnostics = Join-Path $BuildDirectory "Release\vividcam_diagnostics.exe"
$CameraManager = if (Test-Path $InstalledDiagnostics) {
    $InstalledDiagnostics
} elseif (Test-Path $BuildDiagnostics) {
    $BuildDiagnostics
} else {
    $null
}
if (-not $CameraManager) {
    throw ("Cannot remove the persistent virtual camera because vividcam_diagnostics.exe " +
           "was not found in the install or build directory")
}
& $CameraManager --remove-camera
if ($LASTEXITCODE -ne 0) {
    throw "Persistent current-user virtual camera removal failed"
}

$Key = if ($AllUsers) {
    "HKLM:\Software\Classes\CLSID\$Clsid"
} else {
    "HKCU:\Software\Classes\CLSID\$Clsid"
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

$AllUsersArtifactsPresent =
    (Test-Path -LiteralPath $InstalledServer) -or
    (Test-Path -LiteralPath $InstalledDiagnostics) -or
    (Test-Path -LiteralPath $InstalledEngine) -or
    (Test-Path -LiteralPath $ProducerIdentityKey) -or
    (Test-Path -LiteralPath $Key)

if (-not $AllUsersArtifactsPresent) {
    Write-Host "[VIVIDCAM] $Scope activation server was not registered" `
        -ForegroundColor Yellow
    return
}

if ((Test-Path -LiteralPath $InstalledEngine) -and
    (Test-InstalledEngineRunning $InstalledEngine)) {
    throw ("Installed VIVIDCAM engine is running and cannot be removed. " +
           "Stop it and retry: $InstalledEngine")
}

$StoppedFrameServerServices = @(Stop-VividCamFrameServerServices)
$RemovalErrors = @()
try {
    if (Test-Path -LiteralPath $ProducerIdentityKey) {
        try {
            Remove-Item -LiteralPath $ProducerIdentityKey -Recurse -Force
        } catch {
            $RemovalErrors +=
                "Producer identity manifest removal failed: $($_.Exception.Message)"
        }
    }

    foreach ($InstalledFile in @(
            $InstalledServer, $InstalledDiagnostics, $InstalledEngine)) {
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

    try {
        if ((Test-Path -LiteralPath $InstallDirectory) -and
            (Get-ChildItem -LiteralPath $InstallDirectory -Force).Count -eq 0) {
            Remove-Item -LiteralPath $InstallDirectory -Force
        }
    } catch {
        $RemovalErrors +=
            "Empty install directory removal failed: $($_.Exception.Message)"
    }
} finally {
    try {
        Restart-VividCamFrameServerServices $StoppedFrameServerServices
    } catch {
        $RemovalErrors += "FrameServer restart failed: $($_.Exception.Message)"
    }
}

if ($RemovalErrors.Count -gt 0) {
    throw ("VIVIDCAM all-users removal was incomplete: " +
           ($RemovalErrors -join " | "))
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

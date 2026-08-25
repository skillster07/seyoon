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
if (Test-Path $Key) {
    Remove-Item -LiteralPath $Key -Recurse -Force
    Write-Host "[VIVIDCAM] Removed $Scope activation server" -ForegroundColor Green
} else {
    Write-Host "[VIVIDCAM] $Scope activation server was not registered" -ForegroundColor Yellow
}

if ($AllUsers -and ((Test-Path $InstalledServer) -or
                    (Test-Path $InstalledDiagnostics))) {
    $StoppedFrameServerServices = @()
    if (Test-Path $InstalledServer) {
        $StoppedFrameServerServices = @(Stop-VividCamFrameServerServices)
    }
    try {
        if (Test-Path $InstalledServer) {
            Remove-Item -LiteralPath $InstalledServer -Force
        }
        if (Test-Path $InstalledDiagnostics) {
            Remove-Item -LiteralPath $InstalledDiagnostics -Force
        }
        if ((Get-ChildItem -LiteralPath $InstallDirectory -Force).Count -eq 0) {
            Remove-Item -LiteralPath $InstallDirectory -Force
        }
    } catch {
        throw ("Activation server removal failed. Close applications using a camera " +
               "and retry. " + $_.Exception.Message)
    } finally {
        Restart-VividCamFrameServerServices $StoppedFrameServerServices
    }
    Write-Host "[VIVIDCAM] Removed activation server file: $InstalledServer" -ForegroundColor Green
}

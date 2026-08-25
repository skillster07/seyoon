param(
    [string]$BuildDirectory = "",
    [switch]$SkipBuild,
    [switch]$AllUsers
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $Root "native\build" }
if (-not [System.IO.Path]::IsPathRooted($BuildDirectory)) {
    $BuildDirectory = Join-Path (Get-Location).Path $BuildDirectory
}
$BuildDirectory = [System.IO.Path]::GetFullPath($BuildDirectory)
$Clsid = "{B3F8E8E4-1C65-4C10-9DB4-AD2B780A6401}"
$BuildServer = Join-Path $BuildDirectory "Release\vividcam_virtual_camera_source.dll"
$BuildDiagnostics = Join-Path $BuildDirectory "Release\vividcam_diagnostics.exe"

if (-not $SkipBuild) {
    cmake -S (Join-Path $Root "native") -B $BuildDirectory -G "Visual Studio 17 2022" -A x64
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
    cmake --build $BuildDirectory --config Release `
        --target vividcam_virtual_camera_source vividcam_diagnostics --parallel
    if ($LASTEXITCODE -ne 0) { throw "Activation server/diagnostics build failed" }
}
if (-not (Test-Path $BuildServer)) {
    throw "Activation server not found: $BuildServer"
}
if ($AllUsers -and -not (Test-Path $BuildDiagnostics)) {
    throw "Virtual camera diagnostics not found: $BuildDiagnostics"
}

if ($AllUsers) {
    if ([Environment]::Is64BitOperatingSystem -and
        -not [Environment]::Is64BitProcess) {
        throw "All-users COM installation requires 64-bit PowerShell"
    }
    $Identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $Principal = [Security.Principal.WindowsPrincipal]::new($Identity)
    if (-not $Principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "All-users COM installation requires an elevated PowerShell session"
    }
}

function Stop-VividCamFrameServerServices {
    $StoppedServices = @()
    try {
        foreach ($ServiceName in @("FrameServerMonitor", "FrameServer")) {
            $Service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
            if ($null -eq $Service -or $Service.Status -eq "Stopped") { continue }
            Write-Host "[VIVIDCAM] Stopping $ServiceName to update the loaded camera source" `
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

$Scope = if ($AllUsers) { "all-users" } else { "current-user" }
$Server = $BuildServer
$Diagnostics = $BuildDiagnostics
if ($AllUsers) {
    $ProgramFiles = [Environment]::GetFolderPath(
        [Environment+SpecialFolder]::ProgramFiles)
    $InstallDirectory = Join-Path $ProgramFiles "VIVIDCAM\VirtualCamera"
    $Server = Join-Path $InstallDirectory "vividcam_virtual_camera_source.dll"
    $Diagnostics = Join-Path $InstallDirectory "vividcam_diagnostics.exe"

    New-Item -ItemType Directory -Path $InstallDirectory -Force | Out-Null
    $BuildHash = (Get-FileHash -LiteralPath $BuildServer -Algorithm SHA256).Hash
    $ServerNeedsUpdate = -not (Test-Path $Server)
    if (-not $ServerNeedsUpdate) {
        $InstalledHash = (Get-FileHash -LiteralPath $Server -Algorithm SHA256).Hash
        $ServerNeedsUpdate = $BuildHash -ne $InstalledHash
    }
    $BuildDiagnosticsHash =
        (Get-FileHash -LiteralPath $BuildDiagnostics -Algorithm SHA256).Hash
    $DiagnosticsNeedsUpdate = -not (Test-Path $Diagnostics)
    if (-not $DiagnosticsNeedsUpdate) {
        $InstalledDiagnosticsHash =
            (Get-FileHash -LiteralPath $Diagnostics -Algorithm SHA256).Hash
        $DiagnosticsNeedsUpdate = $BuildDiagnosticsHash -ne $InstalledDiagnosticsHash
    }

    $StoppedFrameServerServices = @()
    if ($ServerNeedsUpdate -and (Test-Path $Server)) {
        $StoppedFrameServerServices = @(Stop-VividCamFrameServerServices)
    }
    try {
        if ($ServerNeedsUpdate -and
            -not [string]::Equals($BuildServer, $Server,
                                  [StringComparison]::OrdinalIgnoreCase)) {
            Copy-Item -LiteralPath $BuildServer -Destination $Server -Force
        }
        if ($DiagnosticsNeedsUpdate -and
            -not [string]::Equals($BuildDiagnostics, $Diagnostics,
                                  [StringComparison]::OrdinalIgnoreCase)) {
            Copy-Item -LiteralPath $BuildDiagnostics -Destination $Diagnostics -Force
        }
    } catch {
        throw ("Activation server deployment failed. Close applications using a camera " +
               "and retry. " + $_.Exception.Message)
    } finally {
        Restart-VividCamFrameServerServices $StoppedFrameServerServices
    }
    if (-not (Test-Path $Server)) {
        throw "Activation server deployment failed: $Server"
    }
    if (-not (Test-Path $Diagnostics)) {
        throw "Diagnostics deployment failed: $Diagnostics"
    }
    $InstalledHash = (Get-FileHash -LiteralPath $Server -Algorithm SHA256).Hash
    if ($BuildHash -ne $InstalledHash) {
        throw "Activation server deployment verification failed: $Server"
    }
    $InstalledDiagnosticsHash =
        (Get-FileHash -LiteralPath $Diagnostics -Algorithm SHA256).Hash
    if ($BuildDiagnosticsHash -ne $InstalledDiagnosticsHash) {
        throw "Diagnostics deployment verification failed: $Diagnostics"
    }
}

$Key = if ($AllUsers) {
    "HKLM:\Software\Classes\CLSID\$Clsid"
} else {
    "HKCU:\Software\Classes\CLSID\$Clsid"
}
New-Item -Path "$Key\InprocServer32" -Force | Out-Null
Set-Item -Path $Key -Value "VIVIDCAM Virtual Camera Source"
Set-Item -Path "$Key\InprocServer32" -Value $Server
Set-ItemProperty -Path "$Key\InprocServer32" -Name "ThreadingModel" -Value "Both"
Write-Host "[VIVIDCAM] Installed $Scope activation server: $Server" -ForegroundColor Green

if ($AllUsers) {
    & $Diagnostics --install-camera
    if ($LASTEXITCODE -ne 0) {
        throw "Persistent current-user virtual camera installation failed"
    }
    Write-Host "[VIVIDCAM] Installed persistent current-user virtual camera" `
        -ForegroundColor Green
}

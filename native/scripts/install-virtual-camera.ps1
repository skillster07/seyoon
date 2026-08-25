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

if (-not $SkipBuild) {
    cmake -S (Join-Path $Root "native") -B $BuildDirectory -G "Visual Studio 17 2022" -A x64
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
    cmake --build $BuildDirectory --config Release --target vividcam_virtual_camera_source --parallel
    if ($LASTEXITCODE -ne 0) { throw "Activation server build failed" }
}
if (-not (Test-Path $BuildServer)) {
    throw "Activation server not found: $BuildServer"
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

$Scope = if ($AllUsers) { "all-users" } else { "current-user" }
$Server = $BuildServer
if ($AllUsers) {
    $ProgramFiles = [Environment]::GetFolderPath(
        [Environment+SpecialFolder]::ProgramFiles)
    $InstallDirectory = Join-Path $ProgramFiles "VIVIDCAM\VirtualCamera"
    $Server = Join-Path $InstallDirectory "vividcam_virtual_camera_source.dll"

    New-Item -ItemType Directory -Path $InstallDirectory -Force | Out-Null
    if (-not [string]::Equals($BuildServer, $Server,
                              [StringComparison]::OrdinalIgnoreCase)) {
        Copy-Item -LiteralPath $BuildServer -Destination $Server -Force
    }
    if (-not (Test-Path $Server)) {
        throw "Activation server deployment failed: $Server"
    }
    $BuildHash = (Get-FileHash -LiteralPath $BuildServer -Algorithm SHA256).Hash
    $InstalledHash = (Get-FileHash -LiteralPath $Server -Algorithm SHA256).Hash
    if ($BuildHash -ne $InstalledHash) {
        throw "Activation server deployment verification failed: $Server"
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

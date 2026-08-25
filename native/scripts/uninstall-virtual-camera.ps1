param([switch]$AllUsers)

$ErrorActionPreference = "Stop"
$Clsid = "{B3F8E8E4-1C65-4C10-9DB4-AD2B780A6401}"
$Scope = if ($AllUsers) { "all-users" } else { "current-user" }
$ProgramFiles = [Environment]::GetFolderPath(
    [Environment+SpecialFolder]::ProgramFiles)
$InstallDirectory = Join-Path $ProgramFiles "VIVIDCAM\VirtualCamera"
$InstalledServer = Join-Path $InstallDirectory "vividcam_virtual_camera_source.dll"
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

if ($AllUsers -and (Test-Path $InstalledServer)) {
    Remove-Item -LiteralPath $InstalledServer -Force
    if ((Get-ChildItem -LiteralPath $InstallDirectory -Force).Count -eq 0) {
        Remove-Item -LiteralPath $InstallDirectory -Force
    }
    Write-Host "[VIVIDCAM] Removed activation server file: $InstalledServer" -ForegroundColor Green
}

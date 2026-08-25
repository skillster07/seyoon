$ErrorActionPreference = "Stop"

if ([Environment]::Is64BitOperatingSystem -and
    -not [Environment]::Is64BitProcess) {
    throw "Windows validation requires 64-bit PowerShell"
}

$Root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$Build = Join-Path $Root "native\build"

Write-Host "[VIVIDCAM] Configure Windows x64 native build" -ForegroundColor Cyan
cmake -S (Join-Path $Root "native") -B $Build -G "Visual Studio 17 2022" -A x64
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

Write-Host "[VIVIDCAM] Build Release" -ForegroundColor Cyan
cmake --build $Build --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw "Native build failed" }

Write-Host "[VIVIDCAM] Run unit tests" -ForegroundColor Cyan
ctest --test-dir $Build -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "Native tests failed" }

$Diagnostics = Join-Path $Build "Release\vividcam_diagnostics.exe"
Write-Host "[VIVIDCAM] Inspect D3D11 and camera capabilities" -ForegroundColor Cyan
& $Diagnostics
if ($LASTEXITCODE -ne 0) { throw "Diagnostics failed" }

$Clsid = "{B3F8E8E4-1C65-4C10-9DB4-AD2B780A6401}"
$BuildServer = (Resolve-Path (Join-Path $Build "Release\vividcam_virtual_camera_source.dll")).Path
$ProgramFiles = [Environment]::GetFolderPath(
    [Environment+SpecialFolder]::ProgramFiles)
$InstallDirectory = Join-Path $ProgramFiles "VIVIDCAM\VirtualCamera"
$MachineServer = Join-Path $InstallDirectory "vividcam_virtual_camera_source.dll"
$MachineServerKey = "HKLM:\Software\Classes\CLSID\$Clsid\InprocServer32"
$RegisteredServer = if (Test-Path $MachineServerKey) {
    (Get-Item -LiteralPath $MachineServerKey).GetValue("")
} else {
    $null
}
$MachineInstallIsCurrent =
    [string]::Equals($RegisteredServer, $MachineServer,
                     [StringComparison]::OrdinalIgnoreCase) -and
    (Test-Path $MachineServer)
if ($MachineInstallIsCurrent) {
    $BuildHash = (Get-FileHash -LiteralPath $BuildServer -Algorithm SHA256).Hash
    $MachineHash = (Get-FileHash -LiteralPath $MachineServer -Algorithm SHA256).Hash
    $MachineInstallIsCurrent = $BuildHash -eq $MachineHash
}
if (-not $MachineInstallIsCurrent) {
    $Identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $Principal = [Security.Principal.WindowsPrincipal]::new($Identity)
    if (-not $Principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw ("Frame Server requires machine-wide COM registration. " +
               "Run elevated PowerShell once: " +
               ".\native\scripts\install-virtual-camera.ps1 -BuildDirectory " +
               "`"$Build`" -SkipBuild -AllUsers")
    }
    Write-Host "[VIVIDCAM] Install all-users COM activation server" -ForegroundColor Cyan
    & (Join-Path $Root "native\scripts\install-virtual-camera.ps1") `
        -BuildDirectory $Build -SkipBuild -AllUsers
    if ($LASTEXITCODE -ne 0) { throw "Activation server installation failed" }
}

Write-Host "[VIVIDCAM] Validate IMFActivate and Frame Server media source contract" -ForegroundColor Cyan
& $Diagnostics --activation-test
if ($LASTEXITCODE -ne 0) { throw "COM activation or media source contract gate failed" }

Write-Host "[VIVIDCAM] Run W4a virtual camera registration lifecycle" -ForegroundColor Cyan
& $Diagnostics --register-test
if ($LASTEXITCODE -ne 0) { throw "Virtual camera registration lifecycle gate failed" }

Write-Host "[VIVIDCAM] Install or re-enable the persistent current-user camera" -ForegroundColor Cyan
& $Diagnostics --install-camera
if ($LASTEXITCODE -ne 0) {
    throw "Persistent current-user virtual camera installation gate failed"
}

Write-Host "[VIVIDCAM] Open the registered device and validate W4b-0 sample delivery" -ForegroundColor Cyan
& $Diagnostics --registered-source-test
if ($LASTEXITCODE -ne 0) {
    throw ("Registered-device sample delivery gate failed. " +
           "VIVIDCAM must enumerate as a camera and deliver changing 1920x1080 NV12 60p samples.")
}

Write-Host "[VIVIDCAM] Windows W4a activation and W4b-0 registered-device delivery passed" -ForegroundColor Green

Write-Host "[VIVIDCAM] Run 3-second hardware capture gate" -ForegroundColor Cyan
& $Diagnostics --capture-test
if ($LASTEXITCODE -ne 0) { throw "Hardware capture gate failed" }

Write-Host "[VIVIDCAM] Run D3D11 compositor and BGRA-to-NV12 conversion gate" -ForegroundColor Cyan
& $Diagnostics --render-test
if ($LASTEXITCODE -ne 0) { throw "D3D11 compositor or NV12 conversion gate failed" }

Write-Host "[VIVIDCAM] Windows W1/W2/W3 validation passed" -ForegroundColor Green

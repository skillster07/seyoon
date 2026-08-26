$ErrorActionPreference = "Stop"

if ([Environment]::Is64BitOperatingSystem -and
    -not [Environment]::Is64BitProcess) {
    throw "Windows validation requires 64-bit PowerShell"
}
$ValidationIdentity = [Security.Principal.WindowsIdentity]::GetCurrent()
$ValidationPrincipal =
    [Security.Principal.WindowsPrincipal]::new($ValidationIdentity)
if (-not $ValidationPrincipal.IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw ("Windows validation now verifies the protected machine producer " +
           "identity. Run this script from an elevated PowerShell session.")
}

function Test-ByteArraysEqual {
    param([byte[]]$Left, [byte[]]$Right)
    if ($null -eq $Left -or $null -eq $Right -or
        $Left.Length -ne $Right.Length) {
        return $false
    }
    for ($Index = 0; $Index -lt $Left.Length; ++$Index) {
        if ($Left[$Index] -ne $Right[$Index]) { return $false }
    }
    return $true
}

function Convert-HexToBytes {
    param([string]$Hex)
    if ($Hex.Length -ne 64 -or $Hex -notmatch '^[0-9A-Fa-f]{64}$') {
        return $null
    }
    [byte[]]$Bytes = [byte[]]::new(32)
    for ($Index = 0; $Index -lt $Bytes.Length; ++$Index) {
        $Bytes[$Index] = [Convert]::ToByte($Hex.Substring($Index * 2, 2), 16)
    }
    return $Bytes
}

function Test-ProducerIdentityManifestSecurity {
    param([string]$ManifestPath)
    try {
        $SystemSid = [Security.Principal.SecurityIdentifier]::new("S-1-5-18")
        $AdministratorsSid =
            [Security.Principal.SecurityIdentifier]::new("S-1-5-32-544")
        $FrameServerSid = ([Security.Principal.NTAccount]::new(
            "NT SERVICE", "FrameServer")).Translate(
                [Security.Principal.SecurityIdentifier])
        $FullControl = [Security.AccessControl.RegistryRights]::FullControl
        $FrameServerRead =
            [Security.AccessControl.RegistryRights]::QueryValues -bor
            [Security.AccessControl.RegistryRights]::ReadPermissions
        $Allow = [Security.AccessControl.AccessControlType]::Allow
        $NoInheritance = [Security.AccessControl.InheritanceFlags]::None
        $NoPropagation = [Security.AccessControl.PropagationFlags]::None
        $Security = Get-Acl -LiteralPath $ManifestPath
        if (-not $Security.AreAccessRulesProtected) { return $false }
        $OwnerSid = ([Security.Principal.NTAccount]$Security.Owner).Translate(
            [Security.Principal.SecurityIdentifier])
        if ($OwnerSid -ne $SystemSid -and $OwnerSid -ne $AdministratorsSid) {
            return $false
        }
        $ExpectedRights = @{
            $SystemSid.Value = [Int64]$FullControl
            $AdministratorsSid.Value = [Int64]$FullControl
            $FrameServerSid.Value = [Int64]$FrameServerRead
        }
        $Rules = @($Security.GetAccessRules(
            $true, $true, [Security.Principal.SecurityIdentifier]))
        if ($Rules.Count -ne $ExpectedRights.Count) { return $false }
        $SeenSids = @{}
        foreach ($Rule in $Rules) {
            $SidValue = $Rule.IdentityReference.Value
            if ($Rule.IsInherited -or
                $Rule.AccessControlType -ne $Allow -or
                $Rule.InheritanceFlags -ne $NoInheritance -or
                $Rule.PropagationFlags -ne $NoPropagation -or
                -not $ExpectedRights.ContainsKey($SidValue) -or
                $SeenSids.ContainsKey($SidValue) -or
                [Int64]$Rule.RegistryRights -ne $ExpectedRights[$SidValue]) {
                return $false
            }
            $SeenSids[$SidValue] = $true
        }
        return $SeenSids.Count -eq $ExpectedRights.Count
    } catch {
        return $false
    }
}

function Test-MachineProducerIdentityInstall {
    param(
        [string]$RegisteredServer,
        [string]$MachineServer,
        [string]$MachineDiagnostics,
        [string]$MachineEngine,
        [string]$BuildServer,
        [string]$BuildDiagnostics,
        [string]$BuildEngine,
        [string]$ManifestPath,
        [string]$ExpectedUserSid
    )
    try {
        if (-not [string]::Equals(
                $RegisteredServer, $MachineServer,
                [StringComparison]::OrdinalIgnoreCase)) {
            return $false
        }
        foreach ($Path in @(
                $MachineServer, $MachineDiagnostics, $MachineEngine,
                $BuildServer, $BuildDiagnostics, $BuildEngine)) {
            if (-not (Test-Path -LiteralPath $Path)) { return $false }
        }
        foreach ($Pair in @(
                @($BuildServer, $MachineServer),
                @($BuildDiagnostics, $MachineDiagnostics),
                @($BuildEngine, $MachineEngine))) {
            $BuildHash =
                (Get-FileHash -LiteralPath $Pair[0] -Algorithm SHA256).Hash
            $MachineHash =
                (Get-FileHash -LiteralPath $Pair[1] -Algorithm SHA256).Hash
            if ($BuildHash -ne $MachineHash) { return $false }
        }
        if (-not (Test-Path -LiteralPath $ManifestPath) -or
            -not (Test-ProducerIdentityManifestSecurity $ManifestPath)) {
            return $false
        }
        $Manifest = Get-Item -LiteralPath $ManifestPath
        $ExpectedValueNames = @(
            "SchemaVersion", "Generation", "EnginePath", "EngineSha256",
            "EngineUserSid")
        $ActualValueNames = @($Manifest.GetValueNames())
        if ($ActualValueNames.Count -ne $ExpectedValueNames.Count) {
            return $false
        }
        foreach ($ValueName in $ExpectedValueNames) {
            if ($ActualValueNames -notcontains $ValueName) { return $false }
        }
        if ($Manifest.GetValueKind("SchemaVersion") -ne
                [Microsoft.Win32.RegistryValueKind]::DWord -or
            $Manifest.GetValueKind("Generation") -ne
                [Microsoft.Win32.RegistryValueKind]::QWord -or
            $Manifest.GetValueKind("EnginePath") -ne
                [Microsoft.Win32.RegistryValueKind]::String -or
            $Manifest.GetValueKind("EngineSha256") -ne
                [Microsoft.Win32.RegistryValueKind]::Binary -or
            $Manifest.GetValueKind("EngineUserSid") -ne
                [Microsoft.Win32.RegistryValueKind]::String) {
            return $false
        }
        if ([Int32]$Manifest.GetValue("SchemaVersion") -ne 1 -or
            [Int64]$Manifest.GetValue("Generation") -lt 1 -or
            -not [string]::Equals(
                [string]$Manifest.GetValue("EnginePath"), $MachineEngine,
                [StringComparison]::OrdinalIgnoreCase) -or
            -not [string]::Equals(
                [string]$Manifest.GetValue("EngineUserSid"),
                $ExpectedUserSid, [StringComparison]::Ordinal)) {
            return $false
        }
        [byte[]]$ManifestHash = $Manifest.GetValue("EngineSha256")
        [byte[]]$MachineEngineHash = Convert-HexToBytes (
            Get-FileHash -LiteralPath $MachineEngine -Algorithm SHA256).Hash
        return $ManifestHash.Length -eq 32 -and
            (Test-ByteArraysEqual $ManifestHash $MachineEngineHash)
    } catch {
        return $false
    }
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
$BuildDiagnostics =
    (Resolve-Path (Join-Path $Build "Release\vividcam_diagnostics.exe")).Path
$BuildEngine =
    (Resolve-Path (Join-Path $Build "Release\vividcam_engine.exe")).Path
$ProgramFiles = [Environment]::GetFolderPath(
    [Environment+SpecialFolder]::ProgramFiles)
$InstallDirectory = Join-Path $ProgramFiles "VIVIDCAM\VirtualCamera"
$MachineServer = Join-Path $InstallDirectory "vividcam_virtual_camera_source.dll"
$MachineDiagnostics = Join-Path $InstallDirectory "vividcam_diagnostics.exe"
$MachineEngine = Join-Path $InstallDirectory "vividcam_engine.exe"
$ProducerIdentityKey =
    "HKLM:\Software\VIVIDCAM\VirtualCamera\ProducerIdentity"
$MachineServerKey = "HKLM:\Software\Classes\CLSID\$Clsid\InprocServer32"
$RegisteredServer = if (Test-Path $MachineServerKey) {
    (Get-Item -LiteralPath $MachineServerKey).GetValue("")
} else {
    $null
}
$MachineInstallIsCurrent = Test-MachineProducerIdentityInstall `
    -RegisteredServer $RegisteredServer `
    -MachineServer $MachineServer `
    -MachineDiagnostics $MachineDiagnostics `
    -MachineEngine $MachineEngine `
    -BuildServer $BuildServer `
    -BuildDiagnostics $BuildDiagnostics `
    -BuildEngine $BuildEngine `
    -ManifestPath $ProducerIdentityKey `
    -ExpectedUserSid $ValidationIdentity.User.Value
if (-not $MachineInstallIsCurrent) {
    Write-Host "[VIVIDCAM] Install all-users producer identity package" `
        -ForegroundColor Cyan
    & (Join-Path $Root "native\scripts\install-virtual-camera.ps1") `
        -BuildDirectory $Build -SkipBuild -AllUsers
    if ($LASTEXITCODE -ne 0) { throw "Producer identity installation failed" }
    $RegisteredServer = if (Test-Path $MachineServerKey) {
        (Get-Item -LiteralPath $MachineServerKey).GetValue("")
    } else {
        $null
    }
    if (-not (Test-MachineProducerIdentityInstall `
            -RegisteredServer $RegisteredServer `
            -MachineServer $MachineServer `
            -MachineDiagnostics $MachineDiagnostics `
            -MachineEngine $MachineEngine `
            -BuildServer $BuildServer `
            -BuildDiagnostics $BuildDiagnostics `
            -BuildEngine $BuildEngine `
            -ManifestPath $ProducerIdentityKey `
            -ExpectedUserSid $ValidationIdentity.User.Value)) {
        throw "Installed producer identity package verification failed"
    }
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

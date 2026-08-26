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
$BuildEngine = Join-Path $BuildDirectory "Release\vividcam_engine.exe"
$ProducerIdentityKey = "HKLM:\Software\VIVIDCAM\VirtualCamera\ProducerIdentity"

if (-not $SkipBuild) {
    cmake -S (Join-Path $Root "native") -B $BuildDirectory -G "Visual Studio 17 2022" -A x64
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
    $BuildTargets = @("vividcam_virtual_camera_source", "vividcam_diagnostics")
    if ($AllUsers) { $BuildTargets += "vividcam_engine" }
    cmake --build $BuildDirectory --config Release `
        --target $BuildTargets --parallel
    if ($LASTEXITCODE -ne 0) { throw "VIVIDCAM native component build failed" }
}
if (-not (Test-Path $BuildServer)) {
    throw "Activation server not found: $BuildServer"
}
if ($AllUsers -and -not (Test-Path $BuildDiagnostics)) {
    throw "Virtual camera diagnostics not found: $BuildDiagnostics"
}
if ($AllUsers -and -not (Test-Path $BuildEngine)) {
    throw "VIVIDCAM engine not found: $BuildEngine"
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
    $InstallerUserSid = $Identity.User.Value
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

function Convert-HexToBytes {
    param([string]$Hex)
    if ($Hex.Length -ne 64 -or $Hex -notmatch '^[0-9A-Fa-f]{64}$') {
        throw "Expected a 32-byte SHA-256 hexadecimal value"
    }
    [byte[]]$Bytes = [byte[]]::new(32)
    for ($Index = 0; $Index -lt $Bytes.Length; ++$Index) {
        $Bytes[$Index] = [Convert]::ToByte($Hex.Substring($Index * 2, 2), 16)
    }
    return $Bytes
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

function Test-RegistryValueDataEqual {
    param($Left, $Right)
    if ($null -eq $Left -or $null -eq $Right) {
        return $null -eq $Left -and $null -eq $Right
    }
    if ($Left -is [byte[]] -and $Right -is [byte[]]) {
        return Test-ByteArraysEqual -Left $Left -Right $Right
    }
    if ($Left -is [string[]] -and $Right -is [string[]]) {
        if ($Left.Length -ne $Right.Length) { return $false }
        for ($Index = 0; $Index -lt $Left.Length; ++$Index) {
            if (-not [string]::Equals(
                    $Left[$Index], $Right[$Index],
                    [StringComparison]::Ordinal)) {
                return $false
            }
        }
        return $true
    }
    return [object]::Equals($Left, $Right)
}

function New-RegistryKeyPath {
    param(
        [string]$RegistryPath,
        [System.Collections.Generic.List[string]]$CreatedPaths
    )

    $Qualifier = Split-Path -Path $RegistryPath -Qualifier
    $RelativePath = Split-Path -Path $RegistryPath -NoQualifier
    if ([string]::IsNullOrWhiteSpace($Qualifier) -or
        [string]::IsNullOrWhiteSpace($RelativePath)) {
        throw "Invalid registry path: $RegistryPath"
    }

    $CurrentPath = $Qualifier
    foreach ($Component in $RelativePath.TrimStart('\').Split('\')) {
        if ([string]::IsNullOrWhiteSpace($Component)) { continue }
        $CurrentPath = Join-Path $CurrentPath $Component
        if (Test-Path -LiteralPath $CurrentPath) { continue }

        New-Item -Path $CurrentPath -Force | Out-Null
        if (-not (Test-Path -LiteralPath $CurrentPath)) {
            throw "Could not create registry key: $CurrentPath"
        }
        [void]$CreatedPaths.Add($CurrentPath)
    }
}

function Remove-EmptyCreatedRegistryKeyPaths {
    param([System.Collections.Generic.List[string]]$CreatedPaths)

    for ($Index = $CreatedPaths.Count - 1; $Index -ge 0; --$Index) {
        $RegistryPath = $CreatedPaths[$Index]
        if (-not (Test-Path -LiteralPath $RegistryPath)) { continue }

        $RegistryKey = Get-Item -LiteralPath $RegistryPath
        if (@($RegistryKey.GetValueNames()).Count -ne 0 -or
            @($RegistryKey.GetSubKeyNames()).Count -ne 0) {
            continue
        }
        Remove-Item -LiteralPath $RegistryPath -Force
        if (Test-Path -LiteralPath $RegistryPath) {
            throw "Could not remove newly created empty registry key: $RegistryPath"
        }
    }
}

function Open-RegistryKeyWritable {
    param([string]$RegistryPath)

    $Hive = $null
    $RelativePath = $null
    if ($RegistryPath.StartsWith(
            "HKLM:\", [StringComparison]::OrdinalIgnoreCase)) {
        $Hive = [Microsoft.Win32.RegistryHive]::LocalMachine
        $RelativePath = $RegistryPath.Substring("HKLM:\".Length)
    } elseif ($RegistryPath.StartsWith(
            "HKCU:\", [StringComparison]::OrdinalIgnoreCase)) {
        $Hive = [Microsoft.Win32.RegistryHive]::CurrentUser
        $RelativePath = $RegistryPath.Substring("HKCU:\".Length)
    } else {
        throw "Unsupported writable registry path: $RegistryPath"
    }
    if ([string]::IsNullOrWhiteSpace($RelativePath)) {
        throw "Writable registry path must name a subkey: $RegistryPath"
    }

    $BaseKey = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
        $Hive, [Microsoft.Win32.RegistryView]::Registry64)
    try {
        $WriteRights = [Security.AccessControl.RegistryRights]::QueryValues -bor
            [Security.AccessControl.RegistryRights]::SetValue
        $RegistryKey = $BaseKey.OpenSubKey(
            $RelativePath,
            [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,
            $WriteRights)
    } finally {
        $BaseKey.Dispose()
    }
    if ($null -eq $RegistryKey) {
        throw "Could not open registry key for writing: $RegistryPath"
    }
    return $RegistryKey
}

function Get-ProducerIdentityManifestSnapshot {
    param([string]$ManifestPath)

    $Snapshot = [pscustomobject]@{
        Exists = $false
        Values = @()
        SecurityDescriptor = $null
        SecurityDescriptorSddl = $null
    }
    if (-not (Test-Path -LiteralPath $ManifestPath)) {
        return $Snapshot
    }

    $Manifest = Get-Item -LiteralPath $ManifestPath
    $Values = @()
    foreach ($ValueName in $Manifest.GetValueNames()) {
        $Value = $Manifest.GetValue(
            $ValueName, $null,
            [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        if ($Value -is [byte[]]) {
            [byte[]]$Value = [byte[]]$Value.Clone()
        } elseif ($Value -is [string[]]) {
            [string[]]$Value = [string[]]$Value.Clone()
        }
        $Values += [pscustomobject]@{
            Name = $ValueName
            Kind = $Manifest.GetValueKind($ValueName)
            Value = $Value
        }
    }
    $Acl = Get-Acl -Path $ManifestPath
    [byte[]]$SecurityDescriptor = $Acl.GetSecurityDescriptorBinaryForm()
    $SecuritySections = [Security.AccessControl.AccessControlSections]::Owner -bor
        [Security.AccessControl.AccessControlSections]::Group -bor
        [Security.AccessControl.AccessControlSections]::Access
    $SecurityDescriptorSddl =
        $Acl.GetSecurityDescriptorSddlForm($SecuritySections)
    return [pscustomobject]@{
        Exists = $true
        Values = $Values
        SecurityDescriptor = $SecurityDescriptor
        SecurityDescriptorSddl = $SecurityDescriptorSddl
    }
}

function Test-ProducerIdentityManifestSnapshot {
    param(
        [string]$ManifestPath,
        $ExpectedSnapshot
    )

    $ActualSnapshot = Get-ProducerIdentityManifestSnapshot $ManifestPath
    if ($ActualSnapshot.Exists -ne $ExpectedSnapshot.Exists) { return $false }
    if (-not $ExpectedSnapshot.Exists) { return $true }
    if ($ActualSnapshot.Values.Count -ne $ExpectedSnapshot.Values.Count) {
        return $false
    }
    foreach ($ExpectedValue in $ExpectedSnapshot.Values) {
        $Matches = @($ActualSnapshot.Values | Where-Object {
            $_.Name -ceq $ExpectedValue.Name
        })
        if ($Matches.Count -ne 1 -or
            $Matches[0].Kind -ne $ExpectedValue.Kind -or
            -not (Test-RegistryValueDataEqual `
                -Left $Matches[0].Value -Right $ExpectedValue.Value)) {
            return $false
        }
    }
    return [string]::Equals(
        $ActualSnapshot.SecurityDescriptorSddl,
        $ExpectedSnapshot.SecurityDescriptorSddl,
        [StringComparison]::Ordinal)
}

function Restore-ProducerIdentityManifestSnapshot {
    param(
        [string]$ManifestPath,
        $Snapshot
    )

    if (-not $Snapshot.Exists) {
        if (Test-Path -LiteralPath $ManifestPath) {
            Remove-Item -LiteralPath $ManifestPath -Recurse -Force
        }
        if (Test-Path -LiteralPath $ManifestPath) {
            throw "Could not remove the newly created producer identity manifest"
        }
        return
    }

    New-Item -Path $ManifestPath -Force | Out-Null
    $Manifest = Open-RegistryKeyWritable -RegistryPath $ManifestPath
    try {
        $Manifest.SetValue("Generation", [Int64]0,
            [Microsoft.Win32.RegistryValueKind]::QWord)
        $Manifest.Flush()
        foreach ($ValueName in $Manifest.GetValueNames()) {
            $Manifest.DeleteValue($ValueName, $false)
        }
        foreach ($Entry in @($Snapshot.Values | Where-Object {
                $_.Name -ine "Generation"
            })) {
            $Manifest.SetValue($Entry.Name, $Entry.Value, $Entry.Kind)
        }
        $Manifest.Flush()
        foreach ($Entry in @($Snapshot.Values | Where-Object {
                $_.Name -ieq "Generation"
            })) {
            $Manifest.SetValue($Entry.Name, $Entry.Value, $Entry.Kind)
        }
        $Manifest.Flush()
    } finally {
        $Manifest.Dispose()
    }

    $SecuritySections = [Security.AccessControl.AccessControlSections]::Owner -bor
        [Security.AccessControl.AccessControlSections]::Group -bor
        [Security.AccessControl.AccessControlSections]::Access
    $SnapshotSecurity = [Security.AccessControl.RegistrySecurity]::new()
    $SnapshotSecurity.SetSecurityDescriptorBinaryForm(
        $Snapshot.SecurityDescriptor, $SecuritySections)
    $ExpectedSecuritySddl =
        $SnapshotSecurity.GetSecurityDescriptorSddlForm($SecuritySections)
    $CurrentSecurity = Get-Acl -Path $ManifestPath
    $CurrentSecuritySddl =
        $CurrentSecurity.GetSecurityDescriptorSddlForm($SecuritySections)
    if (-not [string]::Equals(
            $CurrentSecuritySddl, $ExpectedSecuritySddl,
            [StringComparison]::Ordinal)) {
        $RestoreSections =
            [Security.AccessControl.AccessControlSections]::Owner -bor
            [Security.AccessControl.AccessControlSections]::Access
        $GroupSection =
            [Security.AccessControl.AccessControlSections]::Group
        $ExpectedGroupSddl =
            $SnapshotSecurity.GetSecurityDescriptorSddlForm($GroupSection)
        $CurrentGroupSddl =
            $CurrentSecurity.GetSecurityDescriptorSddlForm($GroupSection)
        if (-not [string]::Equals(
                $CurrentGroupSddl, $ExpectedGroupSddl,
                [StringComparison]::Ordinal)) {
            $RestoreSections = $RestoreSections -bor $GroupSection
        }
        $Security = [Security.AccessControl.RegistrySecurity]::new()
        $Security.SetSecurityDescriptorBinaryForm(
            $Snapshot.SecurityDescriptor, $RestoreSections)
        Set-Acl -LiteralPath $ManifestPath -AclObject $Security
    }
    if (-not (Test-ProducerIdentityManifestSnapshot `
            -ManifestPath $ManifestPath -ExpectedSnapshot $Snapshot)) {
        throw "Restored producer identity manifest does not match its snapshot"
    }
}

function Test-ProducerIdentityManifestSecurity {
    param([string]$ManifestPath)

    $SystemSid = [Security.Principal.SecurityIdentifier]::new("S-1-5-18")
    $AdministratorsSid =
        [Security.Principal.SecurityIdentifier]::new("S-1-5-32-544")
    $FrameServerSid = ([Security.Principal.NTAccount]::new(
        "NT SERVICE", "FrameServer")).Translate(
            [Security.Principal.SecurityIdentifier])
    $FullControl = [Security.AccessControl.RegistryRights]::FullControl
    $FrameServerRead = [Security.AccessControl.RegistryRights]::QueryValues -bor
        [Security.AccessControl.RegistryRights]::ReadPermissions
    $Allow = [Security.AccessControl.AccessControlType]::Allow

    $AppliedSecurity = Get-Acl -Path $ManifestPath
    if (-not $AppliedSecurity.AreAccessRulesProtected) { return $false }
    $OwnerSid = ([Security.Principal.NTAccount]$AppliedSecurity.Owner).Translate(
        [Security.Principal.SecurityIdentifier])
    if ($OwnerSid -ne $SystemSid -and $OwnerSid -ne $AdministratorsSid) {
        return $false
    }
    $ExpectedRights = @{
        $SystemSid.Value = [Int64]$FullControl
        $AdministratorsSid.Value = [Int64]$FullControl
        $FrameServerSid.Value = [Int64]$FrameServerRead
    }
    $Rules = @($AppliedSecurity.GetAccessRules(
        $true, $true, [Security.Principal.SecurityIdentifier]))
    if ($Rules.Count -ne $ExpectedRights.Count) { return $false }
    $SeenSids = @{}
    foreach ($Rule in $Rules) {
        $SidValue = $Rule.IdentityReference.Value
        if ($Rule.IsInherited -or
            $Rule.AccessControlType -ne $Allow -or
            $Rule.InheritanceFlags -ne
                [Security.AccessControl.InheritanceFlags]::None -or
            $Rule.PropagationFlags -ne
                [Security.AccessControl.PropagationFlags]::None -or
            -not $ExpectedRights.ContainsKey($SidValue) -or
            $SeenSids.ContainsKey($SidValue) -or
            [Int64]$Rule.RegistryRights -ne $ExpectedRights[$SidValue]) {
            return $false
        }
        $SeenSids[$SidValue] = $true
    }
    return $SeenSids.Count -eq $ExpectedRights.Count
}

function Get-NextProducerIdentityGeneration {
    param([string]$ManifestPath)
    if (-not (Test-Path -LiteralPath $ManifestPath)) {
        return [Int64]1
    }
    try {
        $Manifest = Get-Item -LiteralPath $ManifestPath
        $ExpectedValueNames = @(
            "SchemaVersion", "Generation", "EnginePath", "EngineSha256",
            "EngineUserSid")
        $ActualValueNames = @($Manifest.GetValueNames())
        if ($ActualValueNames.Count -ne $ExpectedValueNames.Count) {
            return [Int64]1
        }
        foreach ($ValueName in $ExpectedValueNames) {
            if ($ActualValueNames -notcontains $ValueName) {
                return [Int64]1
            }
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
            return [Int64]1
        }
        if ([Int32]$Manifest.GetValue("SchemaVersion") -ne 1 -or
            -not (Test-ProducerIdentityManifestSecurity $ManifestPath)) {
            return [Int64]1
        }
        $Generation = [Int64]$Manifest.GetValue("Generation")
        if ($Generation -lt 1) { return [Int64]1 }
        $ManifestEnginePath = [string]$Manifest.GetValue("EnginePath")
        if ([string]::IsNullOrWhiteSpace($ManifestEnginePath) -or
            $ManifestEnginePath.Length -lt 3 -or
            $ManifestEnginePath[1] -ne ':' -or
            $ManifestEnginePath[2] -ne '\') {
            return [Int64]1
        }
        $CanonicalEnginePath =
            [System.IO.Path]::GetFullPath($ManifestEnginePath)
        if (-not [string]::Equals(
                $CanonicalEnginePath, $ManifestEnginePath,
                [StringComparison]::OrdinalIgnoreCase)) {
            return [Int64]1
        }
        $ManifestUserSid = [string]$Manifest.GetValue("EngineUserSid")
        $ParsedUserSid =
            [Security.Principal.SecurityIdentifier]::new($ManifestUserSid)
        if (-not [string]::Equals(
                $ParsedUserSid.Value, $ManifestUserSid,
                [StringComparison]::Ordinal)) {
            return [Int64]1
        }
        [byte[]]$ManifestHash = $Manifest.GetValue("EngineSha256")
        $HashHasNonzeroByte = $false
        foreach ($Byte in $ManifestHash) {
            if ($Byte -ne 0) { $HashHasNonzeroByte = $true }
        }
        if ($ManifestHash.Length -ne 32 -or -not $HashHasNonzeroByte) {
            return [Int64]1
        }
        if ($Generation -eq [Int64]::MaxValue) {
            throw "Producer identity generation is exhausted"
        }
        return $Generation + 1
    } catch {
        if ($_.Exception.Message -eq "Producer identity generation is exhausted") {
            throw
        }
        return [Int64]1
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

function Set-ProducerIdentityManifest {
    param(
        [string]$ManifestPath,
        [Int64]$Generation,
        [string]$EnginePath,
        [byte[]]$EngineSha256,
        [string]$EngineUserSid
    )
    $SystemSid = [Security.Principal.SecurityIdentifier]::new("S-1-5-18")
    $AdministratorsSid =
        [Security.Principal.SecurityIdentifier]::new("S-1-5-32-544")
    $FrameServerSid = ([Security.Principal.NTAccount]::new(
        "NT SERVICE", "FrameServer")).Translate(
            [Security.Principal.SecurityIdentifier])

    $Security = [Security.AccessControl.RegistrySecurity]::new()
    $Security.SetAccessRuleProtection($true, $false)
    $NoInheritance = [Security.AccessControl.InheritanceFlags]::None
    $NoPropagation = [Security.AccessControl.PropagationFlags]::None
    $Allow = [Security.AccessControl.AccessControlType]::Allow
    $FullControl = [Security.AccessControl.RegistryRights]::FullControl
    $FrameServerRead = [Security.AccessControl.RegistryRights]::QueryValues -bor
        [Security.AccessControl.RegistryRights]::ReadPermissions
    foreach ($Sid in @($SystemSid, $AdministratorsSid)) {
        $Security.AddAccessRule([Security.AccessControl.RegistryAccessRule]::new(
            $Sid, $FullControl, $NoInheritance, $NoPropagation, $Allow))
    }
    $Security.AddAccessRule([Security.AccessControl.RegistryAccessRule]::new(
        $FrameServerSid, $FrameServerRead, $NoInheritance, $NoPropagation,
        $Allow))

    New-Item -Path $ManifestPath -Force | Out-Null
    $Security.SetOwner($SystemSid)
    $SystemOwnerApplied = $false
    try {
        Set-Acl -LiteralPath $ManifestPath -AclObject $Security
        $SystemOwnerAcl = Get-Acl -Path $ManifestPath
        $SystemOwnerActual =
            ([Security.Principal.NTAccount]$SystemOwnerAcl.Owner).Translate(
                [Security.Principal.SecurityIdentifier])
        $SystemOwnerApplied = $SystemOwnerActual -eq $SystemSid
    } catch {}
    if (-not $SystemOwnerApplied) {
        $Security.SetOwner($AdministratorsSid)
        Set-Acl -LiteralPath $ManifestPath -AclObject $Security
    }

    if (-not (Test-ProducerIdentityManifestSecurity $ManifestPath)) {
        throw "Producer identity manifest security verification failed"
    }

    $Manifest = Open-RegistryKeyWritable -RegistryPath $ManifestPath
    try {
        $ExpectedValueNames = @(
            "SchemaVersion", "Generation", "EnginePath", "EngineSha256",
            "EngineUserSid")
        $Manifest.SetValue("Generation", [Int64]0,
            [Microsoft.Win32.RegistryValueKind]::QWord)
        $Manifest.Flush()
        foreach ($ValueName in $Manifest.GetValueNames()) {
            if ($ExpectedValueNames -notcontains $ValueName) {
                $Manifest.DeleteValue($ValueName, $false)
            }
        }
        $Manifest.SetValue("SchemaVersion", [Int32]1,
            [Microsoft.Win32.RegistryValueKind]::DWord)
        $Manifest.SetValue("EnginePath", $EnginePath,
            [Microsoft.Win32.RegistryValueKind]::String)
        $Manifest.SetValue("EngineSha256", $EngineSha256,
            [Microsoft.Win32.RegistryValueKind]::Binary)
        $Manifest.SetValue("EngineUserSid", $EngineUserSid,
            [Microsoft.Win32.RegistryValueKind]::String)
        $Manifest.Flush()
        $Manifest.SetValue("Generation", $Generation,
            [Microsoft.Win32.RegistryValueKind]::QWord)
        $Manifest.Flush()
        $StoredHashMatches = Test-ByteArraysEqual `
            -Left ([byte[]]$Manifest.GetValue("EngineSha256")) `
            -Right $EngineSha256
        $StoredValueNames = @($Manifest.GetValueNames())
        $StoredValueNamesValid = $StoredValueNames.Count -eq
            $ExpectedValueNames.Count
        foreach ($ValueName in $ExpectedValueNames) {
            if ($StoredValueNames -notcontains $ValueName) {
                $StoredValueNamesValid = $false
            }
        }

        if (-not $StoredValueNamesValid -or
            $Manifest.GetValueKind("SchemaVersion") -ne
                [Microsoft.Win32.RegistryValueKind]::DWord -or
            $Manifest.GetValueKind("Generation") -ne
                [Microsoft.Win32.RegistryValueKind]::QWord -or
            $Manifest.GetValueKind("EnginePath") -ne
                [Microsoft.Win32.RegistryValueKind]::String -or
            $Manifest.GetValueKind("EngineSha256") -ne
                [Microsoft.Win32.RegistryValueKind]::Binary -or
            $Manifest.GetValueKind("EngineUserSid") -ne
                [Microsoft.Win32.RegistryValueKind]::String -or
            [Int32]$Manifest.GetValue("SchemaVersion") -ne 1 -or
            [Int64]$Manifest.GetValue("Generation") -ne $Generation -or
            -not [string]::Equals(
                [string]$Manifest.GetValue("EnginePath"), $EnginePath,
                [StringComparison]::OrdinalIgnoreCase) -or
            -not [string]::Equals(
                [string]$Manifest.GetValue("EngineUserSid"), $EngineUserSid,
                [StringComparison]::Ordinal) -or
            -not $StoredHashMatches) {
            throw "Producer identity manifest verification failed"
        }
    } finally {
        $Manifest.Dispose()
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
    $Engine = Join-Path $InstallDirectory "vividcam_engine.exe"

    $InstallDirectoryExisted = Test-Path -LiteralPath $InstallDirectory
    $BuildHash = (Get-FileHash -LiteralPath $BuildServer -Algorithm SHA256).Hash
    $ServerNeedsUpdate = -not (Test-Path -LiteralPath $Server)
    $PriorServerHash = $null
    if (-not $ServerNeedsUpdate) {
        $PriorServerHash =
            (Get-FileHash -LiteralPath $Server -Algorithm SHA256).Hash
        $ServerNeedsUpdate = $BuildHash -ne $PriorServerHash
    }
    $BuildDiagnosticsHash =
        (Get-FileHash -LiteralPath $BuildDiagnostics -Algorithm SHA256).Hash
    $DiagnosticsNeedsUpdate = -not (Test-Path -LiteralPath $Diagnostics)
    $PriorDiagnosticsHash = $null
    if (-not $DiagnosticsNeedsUpdate) {
        $PriorDiagnosticsHash =
            (Get-FileHash -LiteralPath $Diagnostics -Algorithm SHA256).Hash
        $DiagnosticsNeedsUpdate =
            $BuildDiagnosticsHash -ne $PriorDiagnosticsHash
    }
    $BuildEngineHash =
        (Get-FileHash -LiteralPath $BuildEngine -Algorithm SHA256).Hash
    $EngineNeedsUpdate = -not (Test-Path -LiteralPath $Engine)
    $PriorEngineHash = $null
    if (-not $EngineNeedsUpdate) {
        $PriorEngineHash =
            (Get-FileHash -LiteralPath $Engine -Algorithm SHA256).Hash
        $EngineNeedsUpdate = $BuildEngineHash -ne $PriorEngineHash
    }
    if ((Test-Path -LiteralPath $Engine) -and
        (Test-InstalledEngineRunning $Engine)) {
        throw ("Installed VIVIDCAM engine is running and the all-users " +
               "identity deployment cannot be updated. " +
               "Stop it and retry: $Engine")
    }

    $ManifestSnapshot =
        Get-ProducerIdentityManifestSnapshot $ProducerIdentityKey
    $ManifestGeneration = Get-NextProducerIdentityGeneration `
        -ManifestPath $ProducerIdentityKey

    New-Item -ItemType Directory -Path $InstallDirectory -Force | Out-Null
    $OperationId = [Guid]::NewGuid().ToString("N")
    $Deployments = @(
        [pscustomobject]@{
            Label = "activation server"
            Source = $BuildServer
            Target = $Server
            ExpectedHash = $BuildHash
            NeedsUpdate = $ServerNeedsUpdate
            HadTarget = $null -ne $PriorServerHash
            PriorHash = $PriorServerHash
            Stage = Join-Path $InstallDirectory `
                (".vividcam_virtual_camera_source.dll.{0}.stage" -f $OperationId)
            Backup = Join-Path $InstallDirectory `
                (".vividcam_virtual_camera_source.dll.{0}.backup" -f $OperationId)
            BackupCreated = $false
            NewTargetInstalled = $false
        },
        [pscustomobject]@{
            Label = "diagnostics"
            Source = $BuildDiagnostics
            Target = $Diagnostics
            ExpectedHash = $BuildDiagnosticsHash
            NeedsUpdate = $DiagnosticsNeedsUpdate
            HadTarget = $null -ne $PriorDiagnosticsHash
            PriorHash = $PriorDiagnosticsHash
            Stage = Join-Path $InstallDirectory `
                (".vividcam_diagnostics.exe.{0}.stage" -f $OperationId)
            Backup = Join-Path $InstallDirectory `
                (".vividcam_diagnostics.exe.{0}.backup" -f $OperationId)
            BackupCreated = $false
            NewTargetInstalled = $false
        },
        [pscustomobject]@{
            Label = "engine"
            Source = $BuildEngine
            Target = $Engine
            ExpectedHash = $BuildEngineHash
            NeedsUpdate = $EngineNeedsUpdate
            HadTarget = $null -ne $PriorEngineHash
            PriorHash = $PriorEngineHash
            Stage = Join-Path $InstallDirectory `
                (".vividcam_engine.exe.{0}.stage" -f $OperationId)
            Backup = Join-Path $InstallDirectory `
                (".vividcam_engine.exe.{0}.backup" -f $OperationId)
            BackupCreated = $false
            NewTargetInstalled = $false
        }
    )

    try {
        foreach ($Deployment in $Deployments) {
            if (-not $Deployment.NeedsUpdate) { continue }
            Copy-Item -LiteralPath $Deployment.Source `
                -Destination $Deployment.Stage
            $StageHash =
                (Get-FileHash -LiteralPath $Deployment.Stage -Algorithm SHA256).Hash
            if ($StageHash -ne $Deployment.ExpectedHash) {
                throw ("Staged {0} hash verification failed: {1}" -f
                       $Deployment.Label, $Deployment.Stage)
            }
        }
    } catch {
        $StageFailure = $_.Exception.Message
        $StageCleanupErrors = @()
        foreach ($Deployment in $Deployments) {
            if (-not (Test-Path -LiteralPath $Deployment.Stage)) { continue }
            try {
                Remove-Item -LiteralPath $Deployment.Stage -Force
            } catch {
                $StageCleanupErrors += $_.Exception.Message
            }
        }
        if (-not $InstallDirectoryExisted -and
            (Test-Path -LiteralPath $InstallDirectory) -and
            (Get-ChildItem -LiteralPath $InstallDirectory -Force).Count -eq 0) {
            try {
                Remove-Item -LiteralPath $InstallDirectory -Force
            } catch {
                $StageCleanupErrors += $_.Exception.Message
            }
        }
        $StageMessage = "VIVIDCAM deployment staging failed: $StageFailure"
        if ($StageCleanupErrors.Count -gt 0) {
            $StageMessage += ("; staging cleanup incomplete: " +
                              ($StageCleanupErrors -join " | "))
        }
        throw $StageMessage
    }

    $StoppedFrameServerServices = @()
    try {
        $StoppedFrameServerServices = @(Stop-VividCamFrameServerServices)
    } catch {
        $StopFailure = $_.Exception.Message
        $StopCleanupErrors = @()
        foreach ($Deployment in $Deployments) {
            if (-not (Test-Path -LiteralPath $Deployment.Stage)) { continue }
            try {
                Remove-Item -LiteralPath $Deployment.Stage -Force
            } catch {
                $StopCleanupErrors += $_.Exception.Message
            }
        }
        if (-not $InstallDirectoryExisted -and
            (Test-Path -LiteralPath $InstallDirectory) -and
            (Get-ChildItem -LiteralPath $InstallDirectory -Force).Count -eq 0) {
            try {
                Remove-Item -LiteralPath $InstallDirectory -Force
            } catch {
                $StopCleanupErrors += $_.Exception.Message
            }
        }
        $StopMessage = "Could not stop FrameServer for deployment: $StopFailure"
        if ($StopCleanupErrors.Count -gt 0) {
            $StopMessage += ("; staging cleanup incomplete: " +
                             ($StopCleanupErrors -join " | "))
        }
        throw $StopMessage
    }

    $DeploymentFailure = $null
    $RollbackErrors = @()
    $CleanupErrors = @()
    $RestartFailure = $null
    $TransactionCommitted = $false
    $CreatedProducerIdentityRegistryKeys =
        [System.Collections.Generic.List[string]]::new()
    try {
        foreach ($Deployment in $Deployments) {
            if (-not $Deployment.NeedsUpdate) { continue }
            if ($Deployment.HadTarget) {
                Move-Item -LiteralPath $Deployment.Target `
                    -Destination $Deployment.Backup
                $Deployment.BackupCreated = $true
            }
            Move-Item -LiteralPath $Deployment.Stage `
                -Destination $Deployment.Target
            $Deployment.NewTargetInstalled = $true
        }

        foreach ($Deployment in $Deployments) {
            if (-not (Test-Path -LiteralPath $Deployment.Target)) {
                throw ("{0} deployment did not produce its target: {1}" -f
                       $Deployment.Label, $Deployment.Target)
            }
            $InstalledHash = (Get-FileHash -LiteralPath $Deployment.Target `
                -Algorithm SHA256).Hash
            if ($InstalledHash -ne $Deployment.ExpectedHash) {
                throw ("{0} deployment verification failed: {1}" -f
                       $Deployment.Label, $Deployment.Target)
            }
        }

        [byte[]]$EngineSha256 = Convert-HexToBytes $BuildEngineHash
        New-RegistryKeyPath `
            -RegistryPath $ProducerIdentityKey `
            -CreatedPaths $CreatedProducerIdentityRegistryKeys
        Set-ProducerIdentityManifest `
            -ManifestPath $ProducerIdentityKey `
            -Generation $ManifestGeneration `
            -EnginePath $Engine `
            -EngineSha256 $EngineSha256 `
            -EngineUserSid $InstallerUserSid
        $TransactionCommitted = $true
    } catch {
        $DeploymentFailure = $_.Exception.Message
        for ($Index = $Deployments.Count - 1; $Index -ge 0; --$Index) {
            $Deployment = $Deployments[$Index]
            try {
                if ($Deployment.BackupCreated) {
                    if (Test-Path -LiteralPath $Deployment.Target) {
                        Remove-Item -LiteralPath $Deployment.Target -Force
                    }
                    if (-not (Test-Path -LiteralPath $Deployment.Backup)) {
                        throw "Backup is missing: $($Deployment.Backup)"
                    }
                    Move-Item -LiteralPath $Deployment.Backup `
                        -Destination $Deployment.Target
                    $Deployment.BackupCreated = $false
                } elseif ($Deployment.NewTargetInstalled -and
                          -not $Deployment.HadTarget) {
                    if (Test-Path -LiteralPath $Deployment.Target) {
                        Remove-Item -LiteralPath $Deployment.Target -Force
                    }
                }
            } catch {
                $RollbackErrors += ("{0} rollback failed: {1}" -f
                                    $Deployment.Label, $_.Exception.Message)
            }
        }
        try {
            Restore-ProducerIdentityManifestSnapshot `
                -ManifestPath $ProducerIdentityKey `
                -Snapshot $ManifestSnapshot
        } catch {
            $RollbackErrors +=
                "Producer identity manifest rollback failed: $($_.Exception.Message)"
        }
        try {
            Remove-EmptyCreatedRegistryKeyPaths `
                -CreatedPaths $CreatedProducerIdentityRegistryKeys
        } catch {
            $RollbackErrors +=
                "Producer identity registry cleanup failed: $($_.Exception.Message)"
        }
        foreach ($Deployment in $Deployments) {
            try {
                if ($Deployment.HadTarget) {
                    if (-not (Test-Path -LiteralPath $Deployment.Target)) {
                        throw "Restored target is missing"
                    }
                    $RestoredHash = (Get-FileHash `
                        -LiteralPath $Deployment.Target -Algorithm SHA256).Hash
                    if ($RestoredHash -ne $Deployment.PriorHash) {
                        throw "Restored target hash does not match"
                    }
                } elseif (Test-Path -LiteralPath $Deployment.Target) {
                    throw "A target that did not previously exist remains"
                }
            } catch {
                $RollbackErrors += ("{0} rollback verification failed: {1}" -f
                                    $Deployment.Label, $_.Exception.Message)
            }
        }
    }

    $RemoveBackups = $TransactionCommitted -or $RollbackErrors.Count -eq 0
    foreach ($Deployment in $Deployments) {
        if (Test-Path -LiteralPath $Deployment.Stage) {
            try {
                Remove-Item -LiteralPath $Deployment.Stage -Force
            } catch {
                $CleanupErrors += ("Could not remove staging file {0}: {1}" -f
                                   $Deployment.Stage, $_.Exception.Message)
            }
        }
        if ($RemoveBackups -and
            (Test-Path -LiteralPath $Deployment.Backup)) {
            try {
                Remove-Item -LiteralPath $Deployment.Backup -Force
            } catch {
                $CleanupErrors += ("Could not remove backup file {0}: {1}" -f
                                   $Deployment.Backup, $_.Exception.Message)
            }
        }
    }
    if (-not $TransactionCommitted -and $RollbackErrors.Count -eq 0 -and
        -not $InstallDirectoryExisted -and
        (Test-Path -LiteralPath $InstallDirectory) -and
        (Get-ChildItem -LiteralPath $InstallDirectory -Force).Count -eq 0) {
        try {
            Remove-Item -LiteralPath $InstallDirectory -Force
        } catch {
            $CleanupErrors +=
                "Could not remove the empty install directory: $($_.Exception.Message)"
        }
    }
    try {
        Restart-VividCamFrameServerServices $StoppedFrameServerServices
    } catch {
        $RestartFailure = $_.Exception.Message
    }

    if ($null -ne $DeploymentFailure) {
        $FailureMessage = "VIVIDCAM native deployment failed: $DeploymentFailure"
        if ($RollbackErrors.Count -eq 0) {
            $FailureMessage += "; prior files and manifest were restored"
        } else {
            $FailureMessage += ("; ROLLBACK INCOMPLETE: " +
                                ($RollbackErrors -join " | "))
            $PreservedBackups = @($Deployments | ForEach-Object {
                if (Test-Path -LiteralPath $_.Backup) { $_.Backup }
            })
            if ($PreservedBackups.Count -gt 0) {
                $FailureMessage += ("; preserved backups: " +
                                    ($PreservedBackups -join ", "))
            }
        }
        if ($CleanupErrors.Count -gt 0) {
            $FailureMessage += ("; cleanup incomplete: " +
                                ($CleanupErrors -join " | "))
        }
        if ($null -ne $RestartFailure) {
            $FailureMessage += "; FrameServer restart failed: $RestartFailure"
        }
        throw $FailureMessage
    }
    if ($CleanupErrors.Count -gt 0 -or $null -ne $RestartFailure) {
        $PostCommitErrors = @($CleanupErrors)
        if ($null -ne $RestartFailure) {
            $PostCommitErrors += "FrameServer restart failed: $RestartFailure"
        }
        throw ("VIVIDCAM deployment committed, but post-commit work was incomplete: " +
               ($PostCommitErrors -join " | "))
    }
    Write-Host "[VIVIDCAM] Installed producer identity generation $ManifestGeneration" `
        -ForegroundColor Green
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

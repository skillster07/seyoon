$ErrorActionPreference = "Stop"
if ($PSVersionTable.PSEdition -eq "Desktop") {
    $SystemDirectory = [Environment]::GetFolderPath(
        [Environment+SpecialFolder]::System)
    $env:PSModulePath = Join-Path $SystemDirectory `
        "WindowsPowerShell\v1.0\Modules"
}
Import-Module Microsoft.PowerShell.Security -ErrorAction Stop

$InstallerPath = Join-Path $PSScriptRoot "install-virtual-camera.ps1"
$Tokens = $null
$ParserErrors = $null
$InstallerAst = [System.Management.Automation.Language.Parser]::ParseFile(
    $InstallerPath, [ref]$Tokens, [ref]$ParserErrors)
if ($ParserErrors.Count -ne 0) {
    throw "Installer script did not parse: $($ParserErrors[0].Message)"
}

$ValidatorPath = Join-Path $PSScriptRoot "validate-windows.ps1"
$ValidatorTokens = $null
$ValidatorParserErrors = $null
$ValidatorAst = [System.Management.Automation.Language.Parser]::ParseFile(
    $ValidatorPath, [ref]$ValidatorTokens, [ref]$ValidatorParserErrors)
if ($ValidatorParserErrors.Count -ne 0) {
    throw "Validator script did not parse: $($ValidatorParserErrors[0].Message)"
}

foreach ($FunctionName in @(
        "New-RegistryKeyPath", "Remove-EmptyCreatedRegistryKeyPaths",
        "Open-RegistryKeyWritable", "Test-ByteArraysEqual",
        "Test-RegistryValueDataEqual", "Get-RegistryValueSnapshot",
        "Test-RegistryValueSnapshot", "Restore-RegistryValueSnapshot",
        "Get-VividCamComRegistrationSnapshot",
        "Test-VividCamComRegistrationSnapshot",
        "Restore-VividCamComRegistrationSnapshot",
        "Get-ProducerIdentityManifestSnapshot",
        "Test-ProducerIdentityManifestSnapshot",
        "Restore-ProducerIdentityManifestSnapshot")) {
    $FunctionAst = $InstallerAst.Find({
        param($Node)
        $Node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
            $Node.Name -eq $FunctionName
    }, $true)
    if ($null -eq $FunctionAst) {
        throw "Installer helper is missing: $FunctionName"
    }
    . ([scriptblock]::Create($FunctionAst.Extent.Text))
}

function Assert-Condition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Assert-NoLiteralRegistryGetAcl {
    param($ScriptAst, [string]$ScriptLabel)

    $InvalidCommands = @($ScriptAst.FindAll({
        param($Node)
        if ($Node -isnot [System.Management.Automation.Language.CommandAst] -or
            $Node.GetCommandName() -ne "Get-Acl") {
            return $false
        }
        foreach ($Element in $Node.CommandElements) {
            if ($Element -is
                    [System.Management.Automation.Language.CommandParameterAst] -and
                $Element.ParameterName -eq "LiteralPath") {
                return $true
            }
        }
        return $false
    }, $true))
    Assert-Condition -Condition ($InvalidCommands.Count -eq 0) `
        -Message ("$ScriptLabel uses Get-Acl -LiteralPath, which reports " +
                  "existing registry keys as missing in Windows PowerShell 5.1")
}

Assert-NoLiteralRegistryGetAcl -ScriptAst $InstallerAst `
    -ScriptLabel "Installer"
Assert-NoLiteralRegistryGetAcl -ScriptAst $ValidatorAst `
    -ScriptLabel "Validator"

function Test-ElevatedMachineRegistryWrite {
    $Identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $Principal = [Security.Principal.WindowsPrincipal]::new($Identity)
    if (-not $Principal.IsInRole(
            [Security.Principal.WindowsBuiltInRole]::Administrator)) {
        return
    }
    Assert-Condition -Condition ([Environment]::Is64BitProcess) `
        -Message "Elevated machine registry test requires 64-bit PowerShell"

    $MachineTestRoot =
        "HKLM:\Software\VIVIDCAMInstallerRegistryTest_$((
            [Guid]::NewGuid()).ToString('N'))"
    $CreatedMachinePaths = [System.Collections.Generic.List[string]]::new()
    try {
        New-RegistryKeyPath -RegistryPath $MachineTestRoot `
            -CreatedPaths $CreatedMachinePaths

        $SystemSid =
            [Security.Principal.SecurityIdentifier]::new("S-1-5-18")
        $AdministratorsSid =
            [Security.Principal.SecurityIdentifier]::new("S-1-5-32-544")
        $LocalServiceSid =
            [Security.Principal.SecurityIdentifier]::new("S-1-5-19")
        $ProtectedSecurity =
            [Security.AccessControl.RegistrySecurity]::new()
        $ProtectedSecurity.SetAccessRuleProtection($true, $false)
        $ProtectedSecurity.SetOwner($AdministratorsSid)
        $NoInheritance = [Security.AccessControl.InheritanceFlags]::None
        $NoPropagation = [Security.AccessControl.PropagationFlags]::None
        $Allow = [Security.AccessControl.AccessControlType]::Allow
        foreach ($Sid in @($SystemSid, $AdministratorsSid)) {
            $ProtectedSecurity.AddAccessRule(
                [Security.AccessControl.RegistryAccessRule]::new(
                    $Sid,
                    [Security.AccessControl.RegistryRights]::FullControl,
                    $NoInheritance, $NoPropagation, $Allow))
        }
        $ProtectedSecurity.AddAccessRule(
            [Security.AccessControl.RegistryAccessRule]::new(
                $LocalServiceSid,
                [Security.AccessControl.RegistryRights]::QueryValues -bor
                    [Security.AccessControl.RegistryRights]::ReadPermissions,
                $NoInheritance, $NoPropagation, $Allow))
        Set-Acl -LiteralPath $MachineTestRoot -AclObject $ProtectedSecurity
        $AppliedMachineAcl = Get-Acl -Path $MachineTestRoot
        Assert-Condition -Condition $AppliedMachineAcl.AreAccessRulesProtected `
            -Message "Machine registry test ACL was not protected"

        $MachineKey = Open-RegistryKeyWritable `
            -RegistryPath $MachineTestRoot
        try {
            Assert-Condition -Condition (
                $MachineKey.View -eq
                    [Microsoft.Win32.RegistryView]::Registry64) `
                -Message "Machine registry handle did not use Registry64"
            $MachineKey.SetValue(
                "WriteProbe", [Int32]1,
                [Microsoft.Win32.RegistryValueKind]::DWord)
            $MachineKey.Flush()
            Assert-Condition -Condition (
                [Int32]$MachineKey.GetValue("WriteProbe") -eq 1) `
                -Message "Machine registry handle did not persist its value"
            $MachineKey.DeleteValue("WriteProbe", $false)
            $MachineKey.Flush()
        } finally {
            $MachineKey.Dispose()
        }
        Remove-EmptyCreatedRegistryKeyPaths `
            -CreatedPaths $CreatedMachinePaths
        Assert-Condition -Condition (
            -not (Test-Path -LiteralPath $MachineTestRoot)) `
            -Message "Machine registry test did not clean up its key"
    } finally {
        if (Test-Path -LiteralPath $MachineTestRoot) {
            Remove-Item -LiteralPath $MachineTestRoot -Recurse -Force
        }
    }
}

Test-ElevatedMachineRegistryWrite

$TestRoot = "HKCU:\Software\VIVIDCAMInstallerRegistryTest_$((
    [Guid]::NewGuid()).ToString('N'))"
$VirtualCameraKey = Join-Path $TestRoot "VirtualCamera"
$ManifestKey = Join-Path $VirtualCameraKey "ProducerIdentity"
$ComRegistrationKey = Join-Path $TestRoot "Classes\CLSID\VIVIDCAM-Test"
$ComInprocServerKey = Join-Path $ComRegistrationKey "InprocServer32"

function Remove-TestRoot {
    if (Test-Path -LiteralPath $TestRoot) {
        Remove-Item -LiteralPath $TestRoot -Recurse -Force
    }
}

function Set-TestRegistryValue {
    param(
        [string]$RegistryPath,
        [string]$Name,
        $Value,
        [Microsoft.Win32.RegistryValueKind]$Kind
    )

    $RegistryKey = Open-RegistryKeyWritable -RegistryPath $RegistryPath
    try {
        $RegistryKey.SetValue($Name, $Value, $Kind)
        $RegistryKey.Flush()
    } finally {
        $RegistryKey.Dispose()
    }
}

function Assert-TestRegistryValue {
    param(
        [string]$RegistryPath,
        [string]$Name,
        $ExpectedValue,
        [Microsoft.Win32.RegistryValueKind]$ExpectedKind,
        [string]$Label
    )

    $Snapshot = Get-RegistryValueSnapshot `
        -RegistryPath $RegistryPath -ValueName $Name
    Assert-Condition -Condition $Snapshot.Exists `
        -Message "$Label was removed"
    Assert-Condition -Condition ($Snapshot.Kind -eq $ExpectedKind) `
        -Message "$Label registry kind changed"
    Assert-Condition -Condition (Test-RegistryValueDataEqual `
            -Left $Snapshot.Value -Right $ExpectedValue) `
        -Message "$Label registry data changed"
}

function Test-ComRegistrationSnapshotRoundTrip {
    Remove-TestRoot

    $AbsentSnapshot = Get-VividCamComRegistrationSnapshot `
        -RegistrationPath $ComRegistrationKey
    New-Item -Path $ComInprocServerKey -Force | Out-Null
    Set-TestRegistryValue -RegistryPath $ComRegistrationKey -Name "" `
        -Value "VIVIDCAM new registration" `
        -Kind ([Microsoft.Win32.RegistryValueKind]::String)
    Set-TestRegistryValue -RegistryPath $ComInprocServerKey -Name "" `
        -Value "C:\new\vividcam.dll" `
        -Kind ([Microsoft.Win32.RegistryValueKind]::String)
    Set-TestRegistryValue -RegistryPath $ComInprocServerKey `
        -Name "ThreadingModel" -Value "Both" `
        -Kind ([Microsoft.Win32.RegistryValueKind]::String)
    Restore-VividCamComRegistrationSnapshot `
        -RegistrationPath $ComRegistrationKey -Snapshot $AbsentSnapshot
    Assert-Condition -Condition (
        -not (Test-Path -LiteralPath $ComRegistrationKey)) `
        -Message "First-install COM rollback left its registration root"

    Remove-TestRoot
    New-Item -Path $ComInprocServerKey -Force | Out-Null
    $PriorRootDefault = "%TEMP%\VIVIDCAM prior registration"
    [byte[]]$PriorRootExtra = @(1, 3, 5, 7)
    $PriorServerDefault = "%SystemRoot%\VIVIDCAM-prior.dll"
    [string[]]$PriorInprocExtra = @("preserve-one", "preserve-two")
    Set-TestRegistryValue -RegistryPath $ComRegistrationKey -Name "" `
        -Value $PriorRootDefault `
        -Kind ([Microsoft.Win32.RegistryValueKind]::ExpandString)
    Set-TestRegistryValue -RegistryPath $ComRegistrationKey -Name "ExtraBinary" `
        -Value $PriorRootExtra `
        -Kind ([Microsoft.Win32.RegistryValueKind]::Binary)
    Set-TestRegistryValue -RegistryPath $ComInprocServerKey -Name "" `
        -Value $PriorServerDefault `
        -Kind ([Microsoft.Win32.RegistryValueKind]::ExpandString)
    Set-TestRegistryValue -RegistryPath $ComInprocServerKey `
        -Name "ThreadingModel" -Value ([Int32]7) `
        -Kind ([Microsoft.Win32.RegistryValueKind]::DWord)
    Set-TestRegistryValue -RegistryPath $ComInprocServerKey -Name "ExtraMulti" `
        -Value $PriorInprocExtra `
        -Kind ([Microsoft.Win32.RegistryValueKind]::MultiString)
    $ExtraSubkey = Join-Path $ComRegistrationKey "ExternalChild"
    New-Item -Path $ExtraSubkey -Force | Out-Null
    Set-TestRegistryValue -RegistryPath $ExtraSubkey -Name "Sentinel" `
        -Value "preserve" `
        -Kind ([Microsoft.Win32.RegistryValueKind]::String)

    $ExistingSnapshot = Get-VividCamComRegistrationSnapshot `
        -RegistrationPath $ComRegistrationKey
    Set-TestRegistryValue -RegistryPath $ComRegistrationKey -Name "" `
        -Value "VIVIDCAM replacement" `
        -Kind ([Microsoft.Win32.RegistryValueKind]::String)
    Set-TestRegistryValue -RegistryPath $ComInprocServerKey -Name "" `
        -Value "C:\replacement\vividcam.dll" `
        -Kind ([Microsoft.Win32.RegistryValueKind]::String)
    Set-TestRegistryValue -RegistryPath $ComInprocServerKey `
        -Name "ThreadingModel" -Value "Both" `
        -Kind ([Microsoft.Win32.RegistryValueKind]::String)
    Restore-VividCamComRegistrationSnapshot `
        -RegistrationPath $ComRegistrationKey -Snapshot $ExistingSnapshot
    Assert-Condition -Condition (Test-VividCamComRegistrationSnapshot `
            -RegistrationPath $ComRegistrationKey `
            -ExpectedSnapshot $ExistingSnapshot) `
        -Message "Existing COM registration did not round-trip exactly"
    Assert-TestRegistryValue -RegistryPath $ComRegistrationKey `
        -Name "ExtraBinary" -ExpectedValue $PriorRootExtra `
        -ExpectedKind ([Microsoft.Win32.RegistryValueKind]::Binary) `
        -Label "Existing COM root extra value"
    Assert-TestRegistryValue -RegistryPath $ComInprocServerKey `
        -Name "ExtraMulti" -ExpectedValue $PriorInprocExtra `
        -ExpectedKind ([Microsoft.Win32.RegistryValueKind]::MultiString) `
        -Label "Existing COM inproc extra value"
    Assert-TestRegistryValue -RegistryPath $ExtraSubkey -Name "Sentinel" `
        -ExpectedValue "preserve" `
        -ExpectedKind ([Microsoft.Win32.RegistryValueKind]::String) `
        -Label "Existing COM extra subkey"

    Remove-TestRoot
    New-Item -Path $ComRegistrationKey -Force | Out-Null
    Set-TestRegistryValue -RegistryPath $ComRegistrationKey -Name "Sentinel" `
        -Value "preserve" `
        -Kind ([Microsoft.Win32.RegistryValueKind]::String)
    $MissingInprocSnapshot = Get-VividCamComRegistrationSnapshot `
        -RegistrationPath $ComRegistrationKey
    New-Item -Path $ComInprocServerKey -Force | Out-Null
    Set-TestRegistryValue -RegistryPath $ComInprocServerKey -Name "" `
        -Value "C:\new\vividcam.dll" `
        -Kind ([Microsoft.Win32.RegistryValueKind]::String)
    Restore-VividCamComRegistrationSnapshot `
        -RegistrationPath $ComRegistrationKey `
        -Snapshot $MissingInprocSnapshot
    Assert-Condition -Condition (
        -not (Test-Path -LiteralPath $ComInprocServerKey)) `
        -Message "COM rollback retained a newly created InprocServer32 key"
    Assert-TestRegistryValue -RegistryPath $ComRegistrationKey `
        -Name "Sentinel" -ExpectedValue "preserve" `
        -ExpectedKind ([Microsoft.Win32.RegistryValueKind]::String) `
        -Label "Existing COM root sentinel"
}

try {
    Assert-Condition -Condition (-not (Test-Path -LiteralPath $TestRoot)) `
        -Message "Unique registry test root already exists"

    Test-ComRegistrationSnapshotRoundTrip
    Remove-TestRoot

    $CreatedPaths = [System.Collections.Generic.List[string]]::new()
    New-RegistryKeyPath -RegistryPath $ManifestKey -CreatedPaths $CreatedPaths
    Assert-Condition -Condition (Test-Path -LiteralPath $ManifestKey) `
        -Message "First-install hierarchy did not reach the manifest key"
    $ManifestAcl = Get-Acl -Path $ManifestKey
    Assert-Condition -Condition ($null -ne $ManifestAcl) `
        -Message "Windows PowerShell 5.1 could not read the registry key ACL"
    $WritableManifest = Open-RegistryKeyWritable -RegistryPath $ManifestKey
    [byte[]]$PriorEngineHash = 0..31
    $PriorEngineSid = "S-1-5-21-1366100792-3461352843-3611251064-1001"
    [string[]]$PriorMultiString = @("preserve-one", "preserve-two")
    try {
        Assert-Condition -Condition (
            $WritableManifest.View -eq
                [Microsoft.Win32.RegistryView]::Registry64) `
            -Message "Writable registry handle did not use Registry64"
        $WritableManifest.SetValue(
            "WriteProbe", [Int32]1,
            [Microsoft.Win32.RegistryValueKind]::DWord)
        $WritableManifest.Flush()
        Assert-Condition -Condition (
            [Int32]$WritableManifest.GetValue("WriteProbe") -eq 1) `
            -Message "Writable registry handle did not persist its value"
        $WritableManifest.DeleteValue("WriteProbe", $false)
        $WritableManifest.SetValue(
            "PriorString", "original",
            [Microsoft.Win32.RegistryValueKind]::String)
        $WritableManifest.SetValue(
            "PriorQword", [Int64]42,
            [Microsoft.Win32.RegistryValueKind]::QWord)
        $WritableManifest.SetValue(
            "EngineSha256", $PriorEngineHash,
            [Microsoft.Win32.RegistryValueKind]::Binary)
        $WritableManifest.SetValue(
            "EngineUserSid", $PriorEngineSid,
            [Microsoft.Win32.RegistryValueKind]::String)
        $WritableManifest.SetValue(
            "PriorMultiString", $PriorMultiString,
            [Microsoft.Win32.RegistryValueKind]::MultiString)
        $WritableManifest.SetValue(
            "TrailingDword", [Int32]7,
            [Microsoft.Win32.RegistryValueKind]::DWord)
        $WritableManifest.Flush()
    } finally {
        $WritableManifest.Dispose()
    }
    $ProtectedSnapshotAcl = Get-Acl -Path $ManifestKey
    $ProtectedSnapshotAcl.SetAccessRuleProtection($true, $true)
    Set-Acl -LiteralPath $ManifestKey -AclObject $ProtectedSnapshotAcl
    $PriorSnapshot = Get-ProducerIdentityManifestSnapshot $ManifestKey
    $WritableManifest = Open-RegistryKeyWritable -RegistryPath $ManifestKey
    try {
        $WritableManifest.DeleteValue("PriorString", $false)
        $WritableManifest.SetValue(
            "PriorQword", [Int64]99,
            [Microsoft.Win32.RegistryValueKind]::QWord)
        $WritableManifest.DeleteValue("EngineSha256", $false)
        $WritableManifest.SetValue(
            "EngineUserSid", "S-1-5-18",
            [Microsoft.Win32.RegistryValueKind]::String)
        $WritableManifest.DeleteValue("PriorMultiString", $false)
        $WritableManifest.SetValue(
            "TrailingDword", [Int32]99,
            [Microsoft.Win32.RegistryValueKind]::DWord)
        $WritableManifest.SetValue(
            "Unexpected", [Int32]1,
            [Microsoft.Win32.RegistryValueKind]::DWord)
        $WritableManifest.Flush()
    } finally {
        $WritableManifest.Dispose()
    }
    $MutatedAcl = Get-Acl -Path $ManifestKey
    $MutatedAcl.SetAccessRuleProtection($false, $true)
    Set-Acl -LiteralPath $ManifestKey -AclObject $MutatedAcl
    $MutatedSnapshot = Get-ProducerIdentityManifestSnapshot $ManifestKey
    Assert-Condition -Condition (-not [string]::Equals(
            $MutatedSnapshot.SecurityDescriptorSddl,
            $PriorSnapshot.SecurityDescriptorSddl,
            [StringComparison]::Ordinal)) `
        -Message "ACL mutation did not change the snapshot fixture"
    Restore-ProducerIdentityManifestSnapshot `
        -ManifestPath $ManifestKey -Snapshot $PriorSnapshot
    Assert-Condition -Condition (Test-ProducerIdentityManifestSnapshot `
            -ManifestPath $ManifestKey -ExpectedSnapshot $PriorSnapshot) `
        -Message "Manifest snapshot restore did not reproduce the prior state"
    Assert-TestRegistryValue -RegistryPath $ManifestKey `
        -Name "EngineSha256" -ExpectedValue $PriorEngineHash `
        -ExpectedKind ([Microsoft.Win32.RegistryValueKind]::Binary) `
        -Label "Existing manifest engine hash"
    Assert-TestRegistryValue -RegistryPath $ManifestKey `
        -Name "EngineUserSid" -ExpectedValue $PriorEngineSid `
        -ExpectedKind ([Microsoft.Win32.RegistryValueKind]::String) `
        -Label "Existing manifest engine user SID"
    Assert-TestRegistryValue -RegistryPath $ManifestKey `
        -Name "PriorMultiString" -ExpectedValue $PriorMultiString `
        -ExpectedKind ([Microsoft.Win32.RegistryValueKind]::MultiString) `
        -Label "Existing manifest multi-string"
    Assert-TestRegistryValue -RegistryPath $ManifestKey `
        -Name "TrailingDword" -ExpectedValue ([Int32]7) `
        -ExpectedKind ([Microsoft.Win32.RegistryValueKind]::DWord) `
        -Label "Existing manifest value after array data"
    Assert-Condition -Condition ($CreatedPaths.Count -eq 3) `
        -Message "First-install hierarchy did not track exactly three new keys"
    Remove-Item -LiteralPath $ManifestKey -Force
    Remove-EmptyCreatedRegistryKeyPaths -CreatedPaths $CreatedPaths
    Assert-Condition -Condition (-not (Test-Path -LiteralPath $TestRoot)) `
        -Message "First-install rollback left an empty registry hierarchy"

    New-Item -Path $TestRoot -Force | Out-Null
    New-ItemProperty -LiteralPath $TestRoot -Name "Sentinel" `
        -PropertyType String -Value "preserve" | Out-Null
    $SentinelChild = Join-Path $TestRoot "SentinelChild"
    New-Item -Path $SentinelChild -Force | Out-Null
    $CreatedPaths = [System.Collections.Generic.List[string]]::new()
    New-RegistryKeyPath -RegistryPath $ManifestKey -CreatedPaths $CreatedPaths
    Assert-Condition -Condition ($CreatedPaths.Count -eq 2) `
        -Message "Existing registry parent was incorrectly tracked as new"
    Remove-Item -LiteralPath $ManifestKey -Force
    Remove-EmptyCreatedRegistryKeyPaths -CreatedPaths $CreatedPaths
    $ExistingRoot = Get-Item -LiteralPath $TestRoot
    Assert-Condition -Condition (
        [string]$ExistingRoot.GetValue("Sentinel") -eq "preserve") `
        -Message "Rollback changed a value on an existing registry parent"
    Assert-Condition -Condition (Test-Path -LiteralPath $SentinelChild) `
        -Message "Rollback removed an existing registry child"

    Remove-TestRoot
    $CreatedPaths = [System.Collections.Generic.List[string]]::new()
    New-RegistryKeyPath -RegistryPath $ManifestKey -CreatedPaths $CreatedPaths
    New-ItemProperty -LiteralPath $VirtualCameraKey -Name "ExternalMarker" `
        -PropertyType String -Value "preserve" | Out-Null
    Remove-Item -LiteralPath $ManifestKey -Force
    Remove-EmptyCreatedRegistryKeyPaths -CreatedPaths $CreatedPaths
    Assert-Condition -Condition (Test-Path -LiteralPath $VirtualCameraKey) `
        -Message "Rollback removed a newly created key that gained external data"
    $RetainedParent = Get-Item -LiteralPath $VirtualCameraKey
    Assert-Condition -Condition (
        [string]$RetainedParent.GetValue("ExternalMarker") -eq "preserve") `
        -Message "Rollback changed external data on a newly created key"

    Write-Host "VIVIDCAM installer registry helper tests passed"
} finally {
    Remove-TestRoot
}

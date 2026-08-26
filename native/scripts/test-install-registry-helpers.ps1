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
        "Test-RegistryValueDataEqual", "Get-ProducerIdentityManifestSnapshot",
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

function Remove-TestRoot {
    if (Test-Path -LiteralPath $TestRoot) {
        Remove-Item -LiteralPath $TestRoot -Recurse -Force
    }
}

try {
    Assert-Condition -Condition (-not (Test-Path -LiteralPath $TestRoot)) `
        -Message "Unique registry test root already exists"

    $CreatedPaths = [System.Collections.Generic.List[string]]::new()
    New-RegistryKeyPath -RegistryPath $ManifestKey -CreatedPaths $CreatedPaths
    Assert-Condition -Condition (Test-Path -LiteralPath $ManifestKey) `
        -Message "First-install hierarchy did not reach the manifest key"
    $ManifestAcl = Get-Acl -Path $ManifestKey
    Assert-Condition -Condition ($null -ne $ManifestAcl) `
        -Message "Windows PowerShell 5.1 could not read the registry key ACL"
    $WritableManifest = Open-RegistryKeyWritable -RegistryPath $ManifestKey
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

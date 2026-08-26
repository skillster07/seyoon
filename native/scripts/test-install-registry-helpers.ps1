$ErrorActionPreference = "Stop"

$InstallerPath = Join-Path $PSScriptRoot "install-virtual-camera.ps1"
$Tokens = $null
$ParserErrors = $null
$InstallerAst = [System.Management.Automation.Language.Parser]::ParseFile(
    $InstallerPath, [ref]$Tokens, [ref]$ParserErrors)
if ($ParserErrors.Count -ne 0) {
    throw "Installer script did not parse: $($ParserErrors[0].Message)"
}

foreach ($FunctionName in @(
        "New-RegistryKeyPath", "Remove-EmptyCreatedRegistryKeyPaths")) {
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

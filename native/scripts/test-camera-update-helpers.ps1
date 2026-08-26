$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.ServiceProcess

function Assert-Condition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Get-ParsedScriptAst {
    param([string]$Path, [string]$Label)

    $Tokens = $null
    $ParserErrors = $null
    $Ast = [System.Management.Automation.Language.Parser]::ParseFile(
        $Path, [ref]$Tokens, [ref]$ParserErrors)
    if ($ParserErrors.Count -ne 0) {
        throw "$Label did not parse: $($ParserErrors[0].Message)"
    }
    return $Ast
}

function Get-FunctionAst {
    param($ScriptAst, [string]$FunctionName, [string]$ScriptLabel)

    $Matches = @($ScriptAst.FindAll({
        param($Node)
        $Node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
            $Node.Name -eq $FunctionName
    }, $true))
    if ($Matches.Count -ne 1) {
        throw ("$ScriptLabel must define exactly one $FunctionName helper; " +
               "found $($Matches.Count)")
    }
    return $Matches[0]
}

function Assert-Events {
    param([string[]]$Expected, [string]$Label)

    [string[]]$Actual = $script:ServiceEvents.ToArray()
    Assert-Condition -Condition ($Actual.Count -eq $Expected.Count) `
        -Message ("$Label event count mismatch: expected [{0}], actual [{1}]" -f
                  ($Expected -join ", "), ($Actual -join ", "))
    for ($Index = 0; $Index -lt $Expected.Count; ++$Index) {
        Assert-Condition -Condition ([string]::Equals(
                $Actual[$Index], $Expected[$Index],
                [StringComparison]::Ordinal)) `
            -Message ("$Label event mismatch at index $Index`: expected " +
                      "'$($Expected[$Index])', actual '$($Actual[$Index])'")
    }
}

$script:ServiceEvents = [System.Collections.Generic.List[string]]::new()
$script:FakeFrameServer = $null
$script:StopFailuresRemaining = 0
$script:StopFailureLeavesStopped = $false
$script:StopAttempts = 0
$script:WaitFailuresRemaining = 0
$script:WaitFailureNextStatus = $null
$script:GetServiceFailuresRemaining = 0

function New-FakeFrameServer {
    param(
        [System.ServiceProcess.ServiceControllerStatus]$Status,
        [bool]$CanStop
    )

    $Service = [pscustomobject]@{
        Name = "FrameServer"
        Status = $Status
        CanStop = $CanStop
    }
    Add-Member -InputObject $Service -MemberType ScriptMethod `
        -Name Refresh -Value {
            [void]$script:ServiceEvents.Add("refresh:$($this.Name)")
        }
    Add-Member -InputObject $Service -MemberType ScriptMethod `
        -Name WaitForStatus -Value {
            param($DesiredStatus, $Timeout)

            [void]$script:ServiceEvents.Add(
                "wait:$($this.Status)->$DesiredStatus")
            if ($Timeout -le [TimeSpan]::Zero -or
                $Timeout -gt [TimeSpan]::FromMilliseconds(500)) {
                throw "Unexpected FrameServer wait timeout: $Timeout"
            }
            if ($script:WaitFailuresRemaining -gt 0) {
                --$script:WaitFailuresRemaining
                if ($null -ne $script:WaitFailureNextStatus) {
                    $this.Status = $script:WaitFailureNextStatus
                }
                throw "Transient fake FrameServer wait timeout"
            }
            if ($this.Status -eq
                    [System.ServiceProcess.ServiceControllerStatus]::StopPending -and
                $DesiredStatus -eq
                    [System.ServiceProcess.ServiceControllerStatus]::Stopped) {
                $this.Status = $DesiredStatus
                return
            }
            if ($this.Status -eq
                    [System.ServiceProcess.ServiceControllerStatus]::StartPending -and
                $DesiredStatus -eq
                    [System.ServiceProcess.ServiceControllerStatus]::Running) {
                $this.Status = $DesiredStatus
                return
            }
            if ($this.Status -ne $DesiredStatus) {
                throw ("Fake FrameServer cannot wait from $($this.Status) " +
                       "to $DesiredStatus")
            }
        }
    return $Service
}

function Get-Service {
    param([string]$Name, $ErrorAction)

    [void]$script:ServiceEvents.Add("get:$Name")
    if (-not [string]::Equals(
            $Name, "FrameServer", [StringComparison]::Ordinal)) {
        throw "Camera update helper requested an unexpected service: $Name"
    }
    if ($script:GetServiceFailuresRemaining -gt 0) {
        --$script:GetServiceFailuresRemaining
        throw "Transient fake FrameServer query failure"
    }
    return $script:FakeFrameServer
}

function Stop-Service {
    param($InputObject, $ErrorAction)

    [void]$script:ServiceEvents.Add("stop:$($InputObject.Name)")
    ++$script:StopAttempts
    if ($script:StopFailuresRemaining -gt 0) {
        --$script:StopFailuresRemaining
        if ($script:StopFailureLeavesStopped) {
            $InputObject.Status =
                [System.ServiceProcess.ServiceControllerStatus]::Stopped
        }
        throw "Transient fake FrameServer stop failure"
    }
    if (-not $InputObject.CanStop) {
        throw "Fake FrameServer does not accept stop controls"
    }
    $InputObject.Status =
        [System.ServiceProcess.ServiceControllerStatus]::StopPending
}

function Test-FrameServerStopHelper {
    param($ScriptAst, [string]$ScriptLabel)

    $FunctionAst = Get-FunctionAst -ScriptAst $ScriptAst `
        -FunctionName "Stop-VividCamFrameServer" -ScriptLabel $ScriptLabel
    $FunctionText = $FunctionAst.Extent.Text
    Assert-Condition -Condition (
        $FunctionText.IndexOf(
            "FrameServerMonitor", [StringComparison]::OrdinalIgnoreCase) -lt 0) `
        -Message "$ScriptLabel stop helper still references FrameServerMonitor"
    Assert-Condition -Condition (
        $FunctionText.IndexOf(
            "Start-Service", [StringComparison]::OrdinalIgnoreCase) -lt 0) `
        -Message "$ScriptLabel stop helper text still invokes Start-Service"
    $StartServiceCalls = @($FunctionAst.FindAll({
        param($Node)
        $Node -is [System.Management.Automation.Language.CommandAst] -and
            $Node.GetCommandName() -eq "Start-Service"
    }, $true))
    Assert-Condition -Condition ($StartServiceCalls.Count -eq 0) `
        -Message "$ScriptLabel stop helper AST still invokes Start-Service"

    . ([scriptblock]::Create($FunctionText))

    $script:ServiceEvents.Clear()
    $script:StopFailuresRemaining = 0
    $script:StopFailureLeavesStopped = $false
    $script:StopAttempts = 0
    $script:WaitFailuresRemaining = 0
    $script:WaitFailureNextStatus = $null
    $script:GetServiceFailuresRemaining = 0
    $script:FakeFrameServer = New-FakeFrameServer `
        -Status ([System.ServiceProcess.ServiceControllerStatus]::Running) `
        -CanStop $true
    Stop-VividCamFrameServer
    Assert-Condition -Condition (
        $script:FakeFrameServer.Status -eq
            [System.ServiceProcess.ServiceControllerStatus]::Stopped) `
        -Message "$ScriptLabel did not stop a running FrameServer"
    Assert-Events -Label "$ScriptLabel running stop" -Expected @(
        "get:FrameServer", "stop:FrameServer", "wait:StopPending->Stopped")

    $script:ServiceEvents.Clear()
    $script:StopAttempts = 0
    $script:FakeFrameServer = New-FakeFrameServer `
        -Status ([System.ServiceProcess.ServiceControllerStatus]::StopPending) `
        -CanStop $false
    Stop-VividCamFrameServer
    Assert-Condition -Condition (
        $script:FakeFrameServer.Status -eq
            [System.ServiceProcess.ServiceControllerStatus]::Stopped) `
        -Message "$ScriptLabel did not finish an existing FrameServer stop"
    Assert-Events -Label "$ScriptLabel pending stop" -Expected @(
        "get:FrameServer", "wait:StopPending->Stopped")

    $script:ServiceEvents.Clear()
    $script:StopAttempts = 0
    $script:FakeFrameServer = New-FakeFrameServer `
        -Status ([System.ServiceProcess.ServiceControllerStatus]::StartPending) `
        -CanStop $true
    Stop-VividCamFrameServer
    Assert-Condition -Condition (
        $script:FakeFrameServer.Status -eq
            [System.ServiceProcess.ServiceControllerStatus]::Stopped) `
        -Message "$ScriptLabel did not stop after StartPending completed"
    Assert-Events -Label "$ScriptLabel start-pending stop" -Expected @(
        "get:FrameServer", "wait:StartPending->Running",
        "get:FrameServer", "stop:FrameServer", "wait:StopPending->Stopped")

    $script:ServiceEvents.Clear()
    $script:StopAttempts = 0
    $script:FakeFrameServer = New-FakeFrameServer `
        -Status ([System.ServiceProcess.ServiceControllerStatus]::Running) `
        -CanStop $false
    $Rejected = $false
    $RejectionMessage = $null
    try {
        Stop-VividCamFrameServer
    } catch {
        $Rejected = $true
        $RejectionMessage = $_.Exception.Message
    }
    Assert-Condition -Condition $Rejected `
        -Message "$ScriptLabel accepted a non-stoppable running FrameServer"
    Assert-Condition -Condition (
        $RejectionMessage.IndexOf(
            "does not accept stop controls",
            [StringComparison]::OrdinalIgnoreCase) -ge 0) `
        -Message "$ScriptLabel did not report a clear non-stoppable error"
    Assert-Events -Label "$ScriptLabel rejected stop" -Expected @(
        "get:FrameServer")

    $script:ServiceEvents.Clear()
    $script:GetServiceFailuresRemaining = 1
    $script:FakeFrameServer = New-FakeFrameServer `
        -Status ([System.ServiceProcess.ServiceControllerStatus]::Running) `
        -CanStop $true
    $QueryFailureRejected = $false
    $QueryFailureMessage = $null
    try {
        Stop-VividCamFrameServer
    } catch {
        $QueryFailureRejected = $true
        $QueryFailureMessage = $_.Exception.Message
    }
    Assert-Condition -Condition $QueryFailureRejected `
        -Message "$ScriptLabel treated a service query failure as stopped"
    Assert-Condition -Condition (
        $QueryFailureMessage.IndexOf(
            "Could not query FrameServer state",
            [StringComparison]::OrdinalIgnoreCase) -ge 0 -and
        $QueryFailureMessage.IndexOf(
            "Transient fake FrameServer query failure",
            [StringComparison]::OrdinalIgnoreCase) -ge 0) `
        -Message "$ScriptLabel did not preserve its service query failure"
    $script:GetServiceFailuresRemaining = 0

    $script:ServiceEvents.Clear()
    $script:StopFailuresRemaining = 1
    $script:StopFailureLeavesStopped = $true
    $script:StopAttempts = 0
    $script:FakeFrameServer = New-FakeFrameServer `
        -Status ([System.ServiceProcess.ServiceControllerStatus]::Running) `
        -CanStop $true
    Stop-VividCamFrameServer
    Assert-Condition -Condition (
        $script:FakeFrameServer.Status -eq
            [System.ServiceProcess.ServiceControllerStatus]::Stopped) `
        -Message ("$ScriptLabel did not accept a transient service-control " +
                  "error after FrameServer had already stopped")
    Assert-Condition -Condition ($script:StopAttempts -eq 1) `
        -Message "$ScriptLabel retried an already stopped FrameServer"

    $script:ServiceEvents.Clear()
    $script:StopFailuresRemaining = 1
    $script:StopFailureLeavesStopped = $false
    $script:StopAttempts = 0
    $script:FakeFrameServer = New-FakeFrameServer `
        -Status ([System.ServiceProcess.ServiceControllerStatus]::Running) `
        -CanStop $true
    Stop-VividCamFrameServer
    Assert-Condition -Condition (
        $script:FakeFrameServer.Status -eq
            [System.ServiceProcess.ServiceControllerStatus]::Stopped) `
        -Message "$ScriptLabel did not recover from a transient stop failure"
    Assert-Condition -Condition ($script:StopAttempts -eq 2) `
        -Message "$ScriptLabel did not retry FrameServer exactly once"

    $script:ServiceEvents.Clear()
    $script:StopFailuresRemaining = 0
    $script:WaitFailuresRemaining = 1
    $script:WaitFailureNextStatus =
        [System.ServiceProcess.ServiceControllerStatus]::Running
    $script:StopAttempts = 0
    $script:FakeFrameServer = New-FakeFrameServer `
        -Status ([System.ServiceProcess.ServiceControllerStatus]::Running) `
        -CanStop $true
    Stop-VividCamFrameServer
    Assert-Condition -Condition (
        $script:FakeFrameServer.Status -eq
            [System.ServiceProcess.ServiceControllerStatus]::Stopped) `
        -Message ("$ScriptLabel did not recover when a StopPending service " +
                  "returned to Running")
    Assert-Condition -Condition ($script:StopAttempts -eq 2) `
        -Message ("$ScriptLabel did not reissue stop after the service " +
                  "returned to Running")
    $script:WaitFailuresRemaining = 0
    $script:WaitFailureNextStatus = $null

    $script:ServiceEvents.Clear()
    $script:StopFailuresRemaining = 1000
    $script:StopFailureLeavesStopped = $false
    $script:StopAttempts = 0
    $script:FakeFrameServer = New-FakeFrameServer `
        -Status ([System.ServiceProcess.ServiceControllerStatus]::Running) `
        -CanStop $true
    $PersistentFailureRejected = $false
    $PersistentFailureMessage = $null
    try {
        Stop-VividCamFrameServer `
            -Timeout ([TimeSpan]::FromMilliseconds(50))
    } catch {
        $PersistentFailureRejected = $true
        $PersistentFailureMessage = $_.Exception.Message
    }
    Assert-Condition -Condition $PersistentFailureRejected `
        -Message "$ScriptLabel did not bound persistent stop failures"
    Assert-Condition -Condition (
        $PersistentFailureMessage.IndexOf(
            "Last service-control error",
            [StringComparison]::OrdinalIgnoreCase) -ge 0 -and
        $PersistentFailureMessage.IndexOf(
            "Transient fake FrameServer stop failure",
            [StringComparison]::OrdinalIgnoreCase) -ge 0) `
        -Message "$ScriptLabel did not preserve its final stop failure"
    Assert-Condition -Condition (
        $script:FakeFrameServer.Status -eq
            [System.ServiceProcess.ServiceControllerStatus]::Running) `
        -Message "$ScriptLabel persistent failure fixture stopped unexpectedly"

    $script:StopFailuresRemaining = 0
    $script:StopFailureLeavesStopped = $false
    $script:WaitFailuresRemaining = 0
    $script:WaitFailureNextStatus = $null
    $script:GetServiceFailuresRemaining = 0
}

$InstallerPath = Join-Path $PSScriptRoot "install-virtual-camera.ps1"
$UninstallerPath = Join-Path $PSScriptRoot "uninstall-virtual-camera.ps1"
$InstallerAst = Get-ParsedScriptAst -Path $InstallerPath -Label "Installer"
$UninstallerAst = Get-ParsedScriptAst -Path $UninstallerPath -Label "Uninstaller"

Test-FrameServerStopHelper -ScriptAst $InstallerAst -ScriptLabel "Installer"
Test-FrameServerStopHelper -ScriptAst $UninstallerAst -ScriptLabel "Uninstaller"

foreach ($FunctionName in @(
        "Invoke-VividCamDiagnosticsLifecycle", "Get-VividCamLifecycleLink",
        "Stop-VividCamPersistentCameraForUpdate",
        "Start-VividCamPersistentCamera",
        "Remove-VividCamPersistentCameraForRollback",
        "Assert-VividCamCameraIdentity")) {
    $FunctionAst = Get-FunctionAst -ScriptAst $InstallerAst `
        -FunctionName $FunctionName -ScriptLabel "Installer"
    . ([scriptblock]::Create($FunctionAst.Extent.Text))
}

$TemporaryDirectory = Join-Path ([System.IO.Path]::GetTempPath()) `
    ("vividcam-camera-update-tests-{0}" -f [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $TemporaryDirectory | Out-Null
try {
    $StopSuccessDiagnostics = Join-Path $TemporaryDirectory "stop-success.cmd"
    Set-Content -LiteralPath $StopSuccessDiagnostics -Encoding Ascii -Value @(
        "@echo off",
        "if /I not `"%~1`"==`"--stop-camera`" exit /b 9",
        "echo diagnostics prelude",
        "echo [persistent-camera] stopped link=VIVIDCAM-LINK-001",
        "exit /b 0")
    $CameraWasStopped = $false
    $StoppedLink = Stop-VividCamPersistentCameraForUpdate `
        -DiagnosticsPath $StopSuccessDiagnostics `
        -WasStopped ([ref]$CameraWasStopped)
    Assert-Condition -Condition ([string]::Equals(
            $StoppedLink, "VIVIDCAM-LINK-001", [StringComparison]::Ordinal)) `
        -Message "Persistent camera stop did not parse its symbolic link"
    Assert-Condition -Condition $CameraWasStopped `
        -Message "Successful persistent camera stop did not set its recovery flag"

    $StopMissingDiagnostics = Join-Path $TemporaryDirectory "stop-missing.cmd"
    Set-Content -LiteralPath $StopMissingDiagnostics -Encoding Ascii -Value @(
        "@echo off",
        "if /I not `"%~1`"==`"--stop-camera`" exit /b 9",
        "echo [persistent-camera] not-installed",
        "exit /b 3")
    $CameraWasStopped = $true
    $MissingLink = Stop-VividCamPersistentCameraForUpdate `
        -DiagnosticsPath $StopMissingDiagnostics `
        -WasStopped ([ref]$CameraWasStopped)
    Assert-Condition -Condition ($null -eq $MissingLink) `
        -Message "Exit code 3 did not report an absent persistent camera"
    Assert-Condition -Condition (-not $CameraWasStopped) `
        -Message "Absent persistent camera incorrectly set its recovery flag"

    $StopMalformedDiagnostics = Join-Path $TemporaryDirectory `
        "stop-malformed.cmd"
    Set-Content -LiteralPath $StopMalformedDiagnostics -Encoding Ascii -Value @(
        "@echo off",
        "if /I not `"%~1`"==`"--stop-camera`" exit /b 9",
        "echo [persistent-camera] stopped without a link",
        "exit /b 0")
    $CameraWasStopped = $false
    $MalformedStopRejected = $false
    try {
        $null = Stop-VividCamPersistentCameraForUpdate `
            -DiagnosticsPath $StopMalformedDiagnostics `
            -WasStopped ([ref]$CameraWasStopped)
    } catch {
        $MalformedStopRejected = $true
    }
    Assert-Condition -Condition $MalformedStopRejected `
        -Message "Malformed successful camera stop output was accepted"
    Assert-Condition -Condition $CameraWasStopped `
        -Message ("Successful native camera stop did not retain its recovery " +
                  "flag when output parsing failed")

    $StopFailedDiagnostics = Join-Path $TemporaryDirectory "stop-failed.cmd"
    Set-Content -LiteralPath $StopFailedDiagnostics -Encoding Ascii -Value @(
        "@echo off",
        "exit /b 4")
    $CameraWasStopped = $true
    $NativeStopFailureRejected = $false
    try {
        $null = Stop-VividCamPersistentCameraForUpdate `
            -DiagnosticsPath $StopFailedDiagnostics `
            -WasStopped ([ref]$CameraWasStopped)
    } catch {
        $NativeStopFailureRejected = $true
    }
    Assert-Condition -Condition $NativeStopFailureRejected `
        -Message "Failed native camera stop was accepted"
    Assert-Condition -Condition (-not $CameraWasStopped) `
        -Message "Failed native camera stop incorrectly set its recovery flag"

    $StartSuccessDiagnostics = Join-Path $TemporaryDirectory "start-success.cmd"
    Set-Content -LiteralPath $StartSuccessDiagnostics -Encoding Ascii -Value @(
        "@echo off",
        "if /I not `"%~1`"==`"--install-camera`" exit /b 9",
        "echo diagnostics prelude",
        "echo [persistent-camera] installed/started link=VIVIDCAM-LINK-001",
        "exit /b 0")
    $StartedLink = Start-VividCamPersistentCamera `
        -DiagnosticsPath $StartSuccessDiagnostics
    Assert-Condition -Condition ([string]::Equals(
            $StartedLink, "VIVIDCAM-LINK-001", [StringComparison]::Ordinal)) `
        -Message "Persistent camera start did not parse its symbolic link"

    $RemoveSuccessDiagnostics = Join-Path $TemporaryDirectory `
        "remove-success.cmd"
    Set-Content -LiteralPath $RemoveSuccessDiagnostics -Encoding Ascii -Value @(
        "@echo off",
        "if /I not `"%~1`"==`"--remove-camera`" exit /b 9",
        "echo [persistent-camera] removed",
        "exit /b 0")
    Remove-VividCamPersistentCameraForRollback `
        -DiagnosticsPath $RemoveSuccessDiagnostics

    $RemoveFailedDiagnostics = Join-Path $TemporaryDirectory `
        "remove-failed.cmd"
    Set-Content -LiteralPath $RemoveFailedDiagnostics -Encoding Ascii -Value @(
        "@echo off",
        "exit /b 4")
    $NativeRemoveFailureRejected = $false
    try {
        Remove-VividCamPersistentCameraForRollback `
            -DiagnosticsPath $RemoveFailedDiagnostics
    } catch {
        $NativeRemoveFailureRejected = $true
    }
    Assert-Condition -Condition $NativeRemoveFailureRejected `
        -Message "Failed native camera rollback removal was accepted"

    Assert-VividCamCameraIdentity `
        -ExpectedLink "VIVIDCAM-LINK-001" -ActualLink "VIVIDCAM-LINK-001"
    Assert-VividCamCameraIdentity `
        -ExpectedLink "\\?\swd#vcamdevapi#VIVIDCAM-LINK-AbC" `
        -ActualLink "\\?\SWD#VCAMDEVAPI#vividcam-link-aBc"
    $IdentityMismatchRejected = $false
    $IdentityMismatchMessage = $null
    try {
        Assert-VividCamCameraIdentity `
            -ExpectedLink "VIVIDCAM-LINK-001" `
            -ActualLink "VIVIDCAM-LINK-002"
    } catch {
        $IdentityMismatchRejected = $true
        $IdentityMismatchMessage = $_.Exception.Message
    }
    Assert-Condition -Condition $IdentityMismatchRejected `
        -Message "Persistent camera identity mismatch was accepted"
    Assert-Condition -Condition (
        $IdentityMismatchMessage.IndexOf(
            "identity changed", [StringComparison]::OrdinalIgnoreCase) -ge 0) `
        -Message "Persistent camera identity mismatch error was not clear"
} finally {
    if (Test-Path -LiteralPath $TemporaryDirectory) {
        Remove-Item -LiteralPath $TemporaryDirectory -Recurse -Force
    }
}

Write-Host "VIVIDCAM camera update helper tests passed"

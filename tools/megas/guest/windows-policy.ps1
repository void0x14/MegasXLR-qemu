# SPDX-License-Identifier: GPL-2.0-or-later
# Run INSIDE the intended Windows VM as administrator. Never modifies the host.
[CmdletBinding()]
param(
    [switch]$Apply,
    [ValidatePattern('^[a-zA-Z0-9_-]{1,32}$')][string]$User = 'megas',
    [ValidateSet('Passwordless','Password','Restore')][string]$Mode = 'Passwordless',
    [switch]$ReadPasswordFromStdin,
    [switch]$PasswordAlreadySet,
    [switch]$SelfDelete
)
$ErrorActionPreference = 'Stop'
$stage = 'guest identity'
$secret = $null
$plainPassword = $null
$bytes = $null
function Write-GuestLog([string]$Message) {
    try { Add-Content -LiteralPath $log -Value "[$([DateTime]::UtcNow.ToString('o'))] $Message" } catch { }
}
function Read-GuestPassword {
    $stream = [Console]::OpenStandardInput()
    $memory = New-Object IO.MemoryStream
    $stream.CopyTo($memory)
    $script:bytes = $memory.ToArray()
    $memory.Dispose()
    try { $value = ([Text.UTF8Encoding]::new($false, $true)).GetString($script:bytes) }
    catch { throw 'Password input was not valid UTF-8' }
    if ($value.EndsWith("`n")) { $value = $value.Substring(0, $value.Length - 1) }
    if ($value.EndsWith("`r")) { $value = $value.Substring(0, $value.Length - 1) }
    if ([string]::IsNullOrEmpty($value) -or $value.Contains("`n") -or $value.Contains("`r")) {
        throw 'Password input must be one nonempty UTF-8 line'
    }
    return $value
}
function Save-PolicySnapshot([string]$StatePath, [string]$SnapshotPath) {
    if (Test-Path -LiteralPath $SnapshotPath) { return }
    $registry = @(
        @{ Path = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon'; Name = 'AutoAdminLogon' },
        @{ Path = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon'; Name = 'DefaultUserName' },
        @{ Path = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon'; Name = 'DefaultDomainName' },
        @{ Path = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon'; Name = 'DefaultPassword' },
        @{ Path = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System'; Name = 'InactivityTimeoutSecs' },
        @{ Path = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\Personalization'; Name = 'NoLockScreen' }
    )
    $saved = @()
    foreach ($entry in $registry) {
        New-Item -Path $entry.Path -Force | Out-Null
        $key = Get-Item -LiteralPath $entry.Path
        $exists = $key.GetValueNames() -contains $entry.Name
        if ($exists) {
            $saved += [pscustomobject]@{
                Path = $entry.Path; Name = $entry.Name; Exists = $true
                Kind = $key.GetValueKind($entry.Name).ToString()
                Value = $key.GetValue($entry.Name, $null, 'DoNotExpandEnvironmentNames')
            }
        } else {
            $saved += [pscustomobject]@{ Path = $entry.Path; Name = $entry.Name; Exists = $false; Kind = ''; Value = $null }
        }
    }
    # Winlogon may contain DefaultPassword in plain text. Keep its saved value
    # in a directory readable only by SYSTEM and local administrators.
    $acl = New-Object Security.AccessControl.DirectorySecurity
    $acl.SetAccessRuleProtection($true, $false)
    foreach ($sidText in @('S-1-5-18', 'S-1-5-32-544')) {
        $sid = New-Object Security.Principal.SecurityIdentifier($sidText)
        $rule = New-Object Security.AccessControl.FileSystemAccessRule(
            $sid, 'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow')
        $acl.AddAccessRule($rule)
    }
    Set-Acl -LiteralPath $StatePath -AclObject $acl
    $saved | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $SnapshotPath -Encoding UTF8
}
function Restore-PolicySnapshot([string]$SnapshotPath) {
    if (-not (Test-Path -LiteralPath $SnapshotPath)) { throw 'No saved Megas guest policy exists' }
    $saved = Get-Content -Raw -LiteralPath $SnapshotPath | ConvertFrom-Json
    foreach ($entry in $saved) {
        if ($entry.Exists) {
            New-Item -Path $entry.Path -Force | Out-Null
            New-ItemProperty -LiteralPath $entry.Path -Name $entry.Name -Value $entry.Value -PropertyType $entry.Kind -Force | Out-Null
        } else {
            Remove-ItemProperty -LiteralPath $entry.Path -Name $entry.Name -ErrorAction SilentlyContinue
        }
    }
}
try {
    if (-not $Apply) { throw 'Explicit -Apply is required' }
    if ($User -eq 'Administrator' -or $User -eq 'Guest') { throw 'Use a dedicated local guest account' }
    if ($PasswordAlreadySet -and ($Mode -ne 'Password' -or $ReadPasswordFromStdin)) {
        throw '-PasswordAlreadySet is valid only with -Mode Password and cannot be combined with stdin password input'
    }
    if ($ReadPasswordFromStdin -and $Mode -ne 'Password') { throw '-ReadPasswordFromStdin is valid only with -Mode Password' }
    if ($Mode -eq 'Password' -and -not ($ReadPasswordFromStdin -or $PasswordAlreadySet)) {
        throw 'Password mode requires -ReadPasswordFromStdin or -PasswordAlreadySet'
    }
    $bios = Get-CimInstance Win32_BIOS
    if ($bios.SerialNumber.Trim() -ne 'MEGAS-GUEST') { throw 'Megas guest identity marker missing; no changes made' }
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Guest administrator access is required' }

    $state = Join-Path $env:ProgramData 'Megas\policy'
    New-Item -ItemType Directory -Force -Path $state | Out-Null
    $log = Join-Path $state 'events.log'
    $snapshot = Join-Path $state "${User}-policy.json"
    Write-GuestLog "policy.begin user=$User mode=$Mode pid=$PID"
    $stage = 'password input'
    if ($ReadPasswordFromStdin) {
        $plainPassword = Read-GuestPassword
        $secret = ConvertTo-SecureString -String $plainPassword -AsPlainText -Force
    }
    $stage = 'account'
    $existing = Get-LocalUser -Name $User -ErrorAction SilentlyContinue
    if ($PasswordAlreadySet -and -not $existing) { throw 'Password was marked set, but the selected local guest account does not exist' }
    if ($Mode -eq 'Password' -and $ReadPasswordFromStdin -and -not $existing) {
        New-LocalUser -Name $User -Password $secret -AccountNeverExpires | Out-Null
        $existing = Get-LocalUser -Name $User
    } elseif ($Mode -eq 'Passwordless' -and -not $existing) {
        New-LocalUser -Name $User -NoPassword -AccountNeverExpires | Out-Null
        $existing = Get-LocalUser -Name $User
    }
    if ($Mode -ne 'Restore' -and -not $existing) { throw 'The selected local guest account does not exist' }
    Save-PolicySnapshot -StatePath $state -SnapshotPath $snapshot

    if ($Mode -eq 'Passwordless') {
        $stage = 'passwordless account'
        if (-not $existing) { throw 'The local guest account could not be created' }
        $localAccount = [ADSI]("WinNT://" + $env:COMPUTERNAME + "/" + $User + ',user')
        $localAccount.SetPassword('')
        $localAccount.SetInfo()
        Set-LocalUser -Name $User -PasswordNeverExpires $true
        $stage = 'passwordless sign-in policy'
        $winlogon = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon'
        New-ItemProperty -Path $winlogon -Name AutoAdminLogon -Value '1' -PropertyType String -Force | Out-Null
        New-ItemProperty -Path $winlogon -Name DefaultUserName -Value $User -PropertyType String -Force | Out-Null
        New-ItemProperty -Path $winlogon -Name DefaultDomainName -Value $env:COMPUTERNAME -PropertyType String -Force | Out-Null
        New-ItemProperty -Path $winlogon -Name DefaultPassword -Value '' -PropertyType String -Force | Out-Null
        $system = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System'
        New-ItemProperty -Path $system -Name InactivityTimeoutSecs -Value 0 -PropertyType DWord -Force | Out-Null
        $personalization = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\Personalization'
        New-Item -Path $personalization -Force | Out-Null
        New-ItemProperty -Path $personalization -Name NoLockScreen -Value 1 -PropertyType DWord -Force | Out-Null
    } elseif ($Mode -eq 'Password') {
        $stage = 'guest password'
        if (-not $PasswordAlreadySet) {
            if (-not $secret) { throw 'Password input is unavailable' }
            Set-LocalUser -Name $User -Password $secret
        }
        $stage = 'interactive sign-in policy'
        Restore-PolicySnapshot -SnapshotPath $snapshot
        $winlogon = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon'
        New-ItemProperty -Path $winlogon -Name AutoAdminLogon -Value '0' -PropertyType String -Force | Out-Null
        Remove-ItemProperty -Path $winlogon -Name DefaultPassword -ErrorAction SilentlyContinue
        New-ItemProperty -Path $winlogon -Name DefaultUserName -Value $User -PropertyType String -Force | Out-Null
        New-ItemProperty -Path $winlogon -Name DefaultDomainName -Value $env:COMPUTERNAME -PropertyType String -Force | Out-Null
    } else {
        $stage = 'restore saved sign-in and lock policy'
        Restore-PolicySnapshot -SnapshotPath $snapshot
    }
    Write-GuestLog "policy.complete user=$User mode=$Mode"
    Write-Progress -Activity 'Preparing the Megas guest' -Completed
    if ($Mode -eq 'Restore') {
        Write-Host "Saved Windows sign-in and lock settings restored. The previous local account password cannot be retrieved; set a new password inside the guest if that account was made passwordless. Journal: $log"
    } else {
        Write-Host "Guest account policy applied. Journal: $log. Domain policy and third-party lock software can override local settings."
    }
} catch {
    Write-Progress -Activity 'Preparing the Megas guest' -Completed
    if ($log) { Write-GuestLog "policy.failed user=$User mode=$Mode stage=$stage error=$($_.Exception.Message)" }
    $prompt = "Repair the Megas Windows guest policy inside the intended VM. Stage: $stage. User: $User. Mode: $Mode. Diagnostic: $($_.Exception.Message). Read guest/windows-policy.ps1 and the guest policy journal. Validate the MEGAS-GUEST BIOS serial before changes. Preserve the host. Do not put passwords in logs or prompts."
    Write-Host "Copy this to your agent:`n$prompt"
    exit 1
} finally {
    if ($secret) { $secret.Dispose() }
    if ($bytes) { [Array]::Clear($bytes, 0, $bytes.Length) }
    $plainPassword = $null
    $value = $null
    if ($SelfDelete -and $PSCommandPath -match '^C:\\Windows\\Temp\\megas-windows-policy-[0-9a-f]{32}\.ps1$') {
        try { Remove-Item -LiteralPath $PSCommandPath -Force } catch { }
    }
}

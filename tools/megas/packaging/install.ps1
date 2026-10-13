# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param(
    [ValidateSet('Ask','Yes','No')][string]$DesktopIcon = 'Ask',
    [switch]$Agent,
    [string]$Destination = (Join-Path $env:LOCALAPPDATA 'Programs\Megas')
)
$ErrorActionPreference = 'Stop'
$bundle = Split-Path -Parent $PSScriptRoot
$stage = 'checking'
$staging = $null
$backup = $null
$diagnosticDir = Join-Path $env:LOCALAPPDATA 'Megas\installation'
$diagnostic = Join-Path $diagnosticDir 'last-install.log'
try {
    New-Item -ItemType Directory -Force -Path $diagnosticDir | Out-Null
    Add-Content -LiteralPath $diagnostic -Value "`n[$([DateTime]::UtcNow.ToString('o'))] installer.begin pid=$PID"
} catch {
    $diagnosticDir = $env:TEMP
    $diagnostic = Join-Path $diagnosticDir 'megas-install-last.log'
    try { Add-Content -LiteralPath $diagnostic -Value "`n[$([DateTime]::UtcNow.ToString('o'))] installer.begin pid=$PID" } catch { }
}
function Write-InstallLog([string]$Message) {
    try { Add-Content -LiteralPath $diagnostic -Value "[$([DateTime]::UtcNow.ToString('o'))] $Message" } catch { }
}
try {
    Write-Host "Installer process ID: $PID"
    Write-InstallLog "stage=$stage bundle=$bundle destination=$Destination"
    Write-Progress -Activity 'Installing Megas' -Status 'Checking the bundled application and dependencies' -PercentComplete 10
    if (-not (Test-Path (Join-Path $bundle 'SHA256SUMS'))) { throw 'Bundle manifest missing' }
    foreach ($line in Get-Content (Join-Path $bundle 'SHA256SUMS')) {
        if ($line -notmatch '^([a-f0-9]{64})  (.+)$') { throw 'Invalid bundle manifest' }
        $hash, $relative = $Matches[1], $Matches[2]
        $path = [IO.Path]::GetFullPath((Join-Path $bundle $relative))
        if (-not $path.StartsWith(([IO.Path]::GetFullPath($bundle) + [IO.Path]::DirectorySeparatorChar), [StringComparison]::OrdinalIgnoreCase)) { throw 'Manifest path escaped the bundle' }
        if ((Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant() -ne $hash) { throw 'Bundle checksum mismatch' }
    }
    if (-not (Test-Path (Join-Path $bundle 'megas.exe')) -or -not (Test-Path (Join-Path $bundle 'qemu\bin\qemu-system-x86_64.exe'))) { throw 'Bundle incomplete' }
    $Destination = [IO.Path]::GetFullPath($Destination)
    if ($Destination -eq [IO.Path]::GetPathRoot($Destination) -or $Destination -eq $env:USERPROFILE) { throw 'Unsafe destination' }
    if ($DesktopIcon -eq 'Ask') {
        if ($Agent) { throw 'The installing agent must ask its user and pass -DesktopIcon Yes or No' }
        Add-Type -AssemblyName System.Windows.Forms
        $answer = [System.Windows.Forms.MessageBox]::Show('Add a Megas icon to your desktop?', 'Megas installation', 'YesNo', 'Question')
        $DesktopIcon = if ($answer -eq 'Yes') { 'Yes' } else { 'No' }
    }
    $stage = 'copying'
    Write-InstallLog "stage=$stage"
    Write-Progress -Activity 'Installing Megas' -Status 'Installing in your personal application folder' -PercentComplete 40
    $parent = Split-Path -Parent $Destination
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    $staging = Join-Path $parent ('.megas-install-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $staging | Out-Null
    Get-ChildItem -Force -LiteralPath $bundle | Copy-Item -Destination $staging -Recurse -Force
    Set-Content -LiteralPath (Join-Path $staging '.megas-install') -Value 'Megas managed application directory'
    if (Test-Path -LiteralPath $Destination) {
        if (-not (Test-Path (Join-Path $Destination '.megas-install'))) { throw 'Destination belongs to another application' }
        $backup = Join-Path $parent ('.megas-old-' + [Guid]::NewGuid().ToString('N'))
        Move-Item -LiteralPath $Destination -Destination $backup
    }
    Move-Item -LiteralPath $staging -Destination $Destination
    $staging = $null
    $stage = 'shortcuts'
    Write-InstallLog "stage=$stage desktop=$DesktopIcon"
    Write-Progress -Activity 'Installing Megas' -Status 'Adding Megas to Windows search' -PercentComplete 80
    $shell = New-Object -ComObject WScript.Shell
    $folders = @([Environment]::GetFolderPath('Programs'))
    if ($DesktopIcon -eq 'Yes') { $folders += [Environment]::GetFolderPath('DesktopDirectory') }
    foreach ($folder in $folders) {
        $shortcut = $shell.CreateShortcut((Join-Path $folder 'Megas.lnk'))
        $shortcut.TargetPath = Join-Path $Destination 'megas.exe'
        $shortcut.Arguments = 'gui'
        $shortcut.WorkingDirectory = $Destination
        $shortcut.Description = 'See and control your virtual computer'
        $shortcut.Save()
        [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shortcut)
    }
    [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shell)
    if ($backup) { Remove-Item -LiteralPath $backup -Recurse -Force; $backup = $null }
    Write-Progress -Activity 'Installing Megas' -Completed
    Write-InstallLog 'installer.complete'
    Write-Host 'Ready. Press the Windows key and search for Megas.'
} catch {
    Write-Progress -Activity 'Installing Megas' -Completed
    Write-InstallLog "installer.failed stage=$stage error=$($_.Exception.Message)"
    $prompt = "Repair the Megas per-user installation. Stage: $stage. Bundle: $bundle. Destination: $Destination. Diagnostic: $($_.Exception.Message). Diagnostic log: $diagnostic. Read packaging/install.ps1 and SHA256SUMS, preserve my VM disks and other applications, and retry. Ask my desktop icon preference before passing -DesktopIcon Yes or No. Do not install global dependencies."
    Write-Host "`nCopy this to your agent:`n$prompt"
    try { Set-Clipboard -Value $prompt } catch { }
    try { $prompt | Set-Content -LiteralPath (Join-Path $env:TEMP 'megas-install-repair.txt') } catch { }
    if ($backup -and (Test-Path -LiteralPath $backup)) {
        if ((Test-Path -LiteralPath $Destination) -and (Test-Path -LiteralPath (Join-Path $Destination '.megas-install'))) {
            Remove-Item -LiteralPath $Destination -Recurse -Force
        }
        if (-not (Test-Path -LiteralPath $Destination)) { Move-Item -LiteralPath $backup -Destination $Destination }
    }
    # Keep the failure catchable by bootstrap.ps1; a child `exit` would bypass
    # its repair prompt and temporary-file cleanup.
    throw
} finally {
    if ($staging -and (Test-Path -LiteralPath $staging)) { Remove-Item -LiteralPath $staging -Recurse -Force }
}

# Installs the bash relay for Claude Code on Windows, or removes it with -Uninstall.
#
# Install copies dist\bash.exe (building it first when it is missing) to the
# install folder and edits Claude Code's user settings.json:
#   env.CLAUDE_CODE_GIT_BASH_PATH   points at the relay
#   env.CLAUDE_BASH_SHIM_TARGET     set only when a custom bash was configured before
#   hooks.SessionStart              gains a check that reports when the relay is not in effect
# The settings file as it was before the first install is kept beside it.
param(
    [switch]$Uninstall,
    [string]$InstallDir,
    [string]$Settings
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$config = if ($env:CLAUDE_CONFIG_DIR) { $env:CLAUDE_CONFIG_DIR } else { Join-Path $env:USERPROFILE '.claude' }
if (-not $InstallDir) { $InstallDir = Join-Path $config 'tools\bash-shim' }
if (-not $Settings) { $Settings = Join-Path $config 'settings.json' }
$relay = Join-Path $InstallDir 'bash.exe'

$marker = 'Bash launch check FAILED'
$check = @'
x='\\'; [ ${#x} = 2 ] || echo "Bash launch check FAILED in this session: a backslash pair in a Bash command reaches bash as ${#x} backslash(es), and commands over 8000 characters may be cut. Until fixed, put any content with backslashes and any long script in a file with the Write tool and run the file; do not inline it in a Bash command or heredoc. Tell the user: the bash relay (claude-bash-shim) is not in effect or no longer matches Claude Code."
'@

function Get-Field($object, $name) {
    if ($null -ne $object -and $object.PSObject.Properties[$name]) { return $object.PSObject.Properties[$name].Value }
    return $null
}
function Set-Field($object, $name, $value) {
    if ($object.PSObject.Properties[$name]) { $object.PSObject.Properties[$name].Value = $value }
    else { $object | Add-Member -NotePropertyName $name -NotePropertyValue $value }
}
function Read-Settings {
    if (-not (Test-Path $Settings)) { return [pscustomobject]@{} }
    $text = [System.IO.File]::ReadAllText($Settings)
    if (-not $text.Trim()) { return [pscustomobject]@{} }
    return $text | ConvertFrom-Json
}
function Write-Settings($value) {
    $folder = Split-Path -Parent $Settings
    New-Item -ItemType Directory -Force $folder | Out-Null
    $backup = "$Settings.before-bash-shim"
    if ((Test-Path $Settings) -and -not (Test-Path $backup)) { Copy-Item $Settings $backup }
    $json = ($value | ConvertTo-Json -Depth 64) + "`n"
    [System.IO.File]::WriteAllText($Settings, $json, (New-Object System.Text.UTF8Encoding $false))
}
# The SessionStart groups without this relay's check; groups left empty are dropped.
function Remove-Check($groups) {
    $kept = @()
    foreach ($group in @($groups)) {
        if ($null -eq $group) { continue }
        $inner = @(@(Get-Field $group 'hooks') | Where-Object { $null -ne $_ })
        $others = @($inner | Where-Object { -not ([string](Get-Field $_ 'command')).Contains($marker) })
        if ($others.Count -eq $inner.Count) { $kept += $group; continue }
        if ($others.Count -gt 0) { Set-Field $group 'hooks' $others; $kept += $group }
    }
    return ,$kept
}

$settingsValue = Read-Settings
$envValue = Get-Field $settingsValue 'env'
$hooksValue = Get-Field $settingsValue 'hooks'

if ($Uninstall) {
    if ($envValue) {
        $previous = Get-Field $envValue 'CLAUDE_BASH_SHIM_TARGET'
        $envValue.PSObject.Properties.Remove('CLAUDE_BASH_SHIM_TARGET')
        if ($previous) { Set-Field $envValue 'CLAUDE_CODE_GIT_BASH_PATH' $previous }
        elseif ((Get-Field $envValue 'CLAUDE_CODE_GIT_BASH_PATH') -eq $relay) { $envValue.PSObject.Properties.Remove('CLAUDE_CODE_GIT_BASH_PATH') }
        if (@($envValue.PSObject.Properties).Count -eq 0) { $settingsValue.PSObject.Properties.Remove('env') }
    }
    if ($hooksValue) {
        $groups = Remove-Check (Get-Field $hooksValue 'SessionStart')
        if ($groups.Count -gt 0) { Set-Field $hooksValue 'SessionStart' $groups } else { $hooksValue.PSObject.Properties.Remove('SessionStart') }
        if (@($hooksValue.PSObject.Properties).Count -eq 0) { $settingsValue.PSObject.Properties.Remove('hooks') }
    }
    Write-Settings $settingsValue
    Write-Host "Removed the relay from $Settings."
    Write-Host "Sessions already running keep using $relay; delete $InstallDir once they have ended."
    return
}

$built = Join-Path $root 'dist\bash.exe'
if (-not (Test-Path $built)) { & (Join-Path $root 'build.ps1') }
New-Item -ItemType Directory -Force $InstallDir | Out-Null
# A running session may hold the installed relay open: a file in use can be renamed but not
# replaced or deleted. A displaced copy still in use is removed by a later install.
if (Test-Path $relay) { Rename-Item $relay ("bash.exe.replaced-" + [DateTime]::UtcNow.Ticks) }
Copy-Item $built $relay
Get-ChildItem $InstallDir -Filter 'bash.exe.replaced-*' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue

if (-not $envValue) { $envValue = [pscustomobject]@{}; Set-Field $settingsValue 'env' $envValue }
$configured = Get-Field $envValue 'CLAUDE_CODE_GIT_BASH_PATH'
if ($configured -and $configured -ne $relay) { Set-Field $envValue 'CLAUDE_BASH_SHIM_TARGET' $configured }
Set-Field $envValue 'CLAUDE_CODE_GIT_BASH_PATH' $relay

if (-not $hooksValue) { $hooksValue = [pscustomobject]@{}; Set-Field $settingsValue 'hooks' $hooksValue }
$groups = Remove-Check (Get-Field $hooksValue 'SessionStart')
$groups += [pscustomobject]@{ hooks = @([pscustomobject]@{ type = 'command'; command = $check.Trim(); timeout = 10 }) }
Set-Field $hooksValue 'SessionStart' $groups

Write-Settings $settingsValue
Write-Host "Installed $relay"
Write-Host "Updated $Settings"
Write-Host 'New Claude Code sessions use the relay; sessions already running keep their bash until restarted.'

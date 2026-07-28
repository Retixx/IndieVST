# Installs the built VST3 where FL Studio looks by default.
#
# The build writes to the per-user VST3 folder so a compiler never has to run
# elevated. FL Studio does not scan that folder unless you add it by hand, so
# for FL specifically the bundle has to go to the shared location - and that
# needs Administrator.
#
#   Right-click this file -> "Run with PowerShell" as Administrator
#   (or: from an elevated PowerShell, .\install-vst3.ps1)

$ErrorActionPreference = 'Stop'

$source = Join-Path $env:LOCALAPPDATA 'Programs\Common\VST3\IndieVST.vst3'
$shared = 'C:\Program Files\Common Files\VST3'

if (-not (Test-Path $source)) {
    Write-Host "Not built yet - no bundle at $source" -ForegroundColor Red
    Write-Host 'Run: cmake --build build --config Release'
    exit 1
}

$elevated = ([Security.Principal.WindowsPrincipal] `
    [Security.Principal.WindowsIdentity]::GetCurrent()
).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $elevated) {
    Write-Host 'This needs Administrator - C:\Program Files is not user-writable.' -ForegroundColor Yellow
    Write-Host 'Re-launching elevated...'
    Start-Process powershell -Verb RunAs -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    exit
}

# A stale Forge.vst3 from before the rename loads perfectly well, so every
# change since would look like it silently failed to land.
foreach ($old in @("$shared\Forge.vst3", (Join-Path $env:LOCALAPPDATA 'Programs\Common\VST3\Forge.vst3'))) {
    if (Test-Path $old) {
        Write-Host "Removing stale $old" -ForegroundColor Yellow
        Remove-Item -Recurse -Force $old
    }
}

$dest = Join-Path $shared 'IndieVST.vst3'
if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
Copy-Item -Recurse -Force $source $shared

Write-Host ''
Write-Host "Installed to $dest" -ForegroundColor Green
Write-Host ''
Write-Host 'In FL Studio: Options -> Manage plugins -> Find installed plugins'
Write-Host 'It appears under Generators as "IndieVST".'
Read-Host 'Press Enter to close'

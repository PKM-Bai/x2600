[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$driverInf = Join-Path $workspace 'tools\cloner-windows\cloner-2.5.54-windows_alpha\cloner-win32-driver\usb_cloner_device.inf'
if (-not (Test-Path -LiteralPath $driverInf)) { throw "Driver INF missing: $driverInf" }
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $arguments = '-NoProfile -File "' + $PSCommandPath + '"'
    $installer = Start-Process -FilePath 'powershell.exe' -ArgumentList $arguments -Verb RunAs -WindowStyle Hidden -PassThru
    if (-not $installer.WaitForExit(20000)) {
        Write-Host 'Administrator driver installer is still running. Check build\driver-install.log after accepting Windows UAC.'
        return
    }
    if ($installer.ExitCode -ne 0) { throw 'Driver installation failed. See build\driver-install.log.' }
    Get-Content -LiteralPath (Join-Path $workspace 'build\driver-install.log')
    return
}
New-Item -ItemType Directory -Force -Path (Join-Path $workspace 'build') | Out-Null
& pnputil.exe /add-driver $driverInf /install 2>&1 | Tee-Object -FilePath (Join-Path $workspace 'build\driver-install.log')
$driverExitCode = $LASTEXITCODE
exit $driverExitCode

[CmdletBinding()]
param([string]$Port, [int]$BaudRate = 115200)
$ErrorActionPreference = 'Stop'
$python = Join-Path $PSScriptRoot '..\.venv\Scripts\python.exe'
if (-not (Test-Path -LiteralPath $python)) {
    throw 'Run python -m venv .venv and .\.venv\Scripts\python.exe -m pip install -r requirements.txt first.'
}
if (-not $Port) {
    & $python -m serial.tools.list_ports -v
    if ($LASTEXITCODE -ne 0) { throw 'Serial port enumeration failed.' }
    Write-Host 'After connecting the board, run .\scripts\serial-monitor.ps1 -Port COM<number>.'
    return
}
& $python -m serial.tools.miniterm $Port $BaudRate --raw
if ($LASTEXITCODE -ne 0) { throw 'Serial monitor failed.' }

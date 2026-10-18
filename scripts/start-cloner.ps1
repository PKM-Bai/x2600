[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$workspaceRoot = Split-Path -Parent $PSScriptRoot
$installRoot = Join-Path $workspaceRoot 'tools\cloner-windows'
if (-not (Test-Path -LiteralPath $installRoot -PathType Container)) {
    throw 'Windows Cloner is not installed. Run scripts\install-cloner.ps1 with the complete Windows ZIP package first.'
}

$executables = @(Get-ChildItem -LiteralPath $installRoot -Recurse -File -Filter 'cloner.exe')
if ($executables.Count -ne 1) {
    throw "Expected one cloner.exe under $installRoot; found $($executables.Count)."
}

$cloner = $executables[0]
Start-Process -FilePath $cloner.FullName -WorkingDirectory $cloner.DirectoryName -WindowStyle Normal

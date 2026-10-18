[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$ArchivePath)
$ErrorActionPreference = 'Stop'
$archive = (Resolve-Path -LiteralPath $ArchivePath).Path
if ([IO.Path]::GetFileName($archive) -notmatch '^mips-ingenic-xburst2-elf-tools.*\.zip$') {
    throw 'Select the official XBurst2 ELF Windows ZIP. Linux, XBurst1 and Linux-target toolchains use different packages.'
}
$destination = Join-Path $PSScriptRoot '..\toolchains\xburst2-windows'
if (Test-Path -LiteralPath $destination) {
    throw "Destination exists: $destination. Keep the installed toolchain, or use a separate directory."
}
Expand-Archive -LiteralPath $archive -DestinationPath $destination
Get-ChildItem -LiteralPath $destination -Recurse -File -Filter '*gcc.exe' |
    Where-Object { $_.Directory.Name -eq 'bin' } | Select-Object FullName
Write-Host 'Installed. Run .\scripts\build.ps1 to compile the HALLEY7-M example.'

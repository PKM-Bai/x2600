[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ArchivePath
)

$ErrorActionPreference = 'Stop'
$workspaceRoot = Split-Path -Parent $PSScriptRoot
$installRoot = Join-Path $workspaceRoot 'tools\cloner-windows'
$archive = Get-Item -LiteralPath $ArchivePath

if ($archive.PSIsContainer -or $archive.Extension -ine '.zip') {
    throw 'Provide the complete Windows Cloner ZIP package, not the source TAR archive.'
}
if (Test-Path -LiteralPath $installRoot) {
    throw "Installation directory already exists; nothing was overwritten: $installRoot"
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($archive.FullName)
try {
    $destinationPrefix = [System.IO.Path]::GetFullPath($installRoot) + [System.IO.Path]::DirectorySeparatorChar
    foreach ($entry in $zip.Entries) {
        $entryPath = [System.IO.Path]::GetFullPath([System.IO.Path]::Combine($installRoot, $entry.FullName))
        if (-not $entryPath.StartsWith($destinationPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "ZIP entry is outside the installation directory: $($entry.FullName)"
        }
    }
    $clonerEntries = @($zip.Entries | Where-Object { $_.Name -ieq 'cloner.exe' })
    if ($clonerEntries.Count -ne 1) {
        throw "Expected one cloner.exe in the complete Windows package; found $($clonerEntries.Count)."
    }
}
finally {
    $zip.Dispose()
}

Expand-Archive -LiteralPath $archive.FullName -DestinationPath $installRoot
$files = @(Get-ChildItem -LiteralPath $installRoot -Recurse -File)
$cloner = $files | Where-Object { $_.Name -ieq 'cloner.exe' }
$driverFiles = @($files | Where-Object {
    $_.Extension -in '.inf', '.sys', '.cat' -or
    ($_.Extension -ieq '.exe' -and $_.FullName -match '(?i)driver|dpinst|驱动')
})
$configFiles = @($files | Where-Object { $_.Extension -ieq '.cfg' })

[PSCustomObject]@{
    InstallRoot = $installRoot
    ClonerExe = $cloner.FullName
    DriverFiles = @($driverFiles.FullName)
    ConfigFolders = @($configFiles.DirectoryName | Sort-Object -Unique)
    X2600ConfigFiles = @($configFiles | Where-Object { $_.Name -match '(?i)x2600' } | ForEach-Object { $_.FullName })
}

Write-Host 'Extracted only. Install the matching bundled USB driver and check the X2600M DDR2 NOR configuration before burning.'

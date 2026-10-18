[CmdletBinding()]
param(
    [string]$ToolchainRoot = (Join-Path $PSScriptRoot '..\toolchains\xburst2-windows'),
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release'
)
$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if (-not (Test-Path -LiteralPath $ToolchainRoot)) {
    throw 'XBurst2 toolchain missing. Run scripts\install-toolchain.ps1 with the official Windows ZIP.'
}
$compilers = @(Get-ChildItem -LiteralPath $ToolchainRoot -Recurse -File -Filter '*gcc.exe' |
    Where-Object { $_.Directory.Name -eq 'bin' -and $_.Name -match '^mips.*-gcc\.exe$' })
if ($compilers.Count -ne 1) {
    throw "Expected one MIPS GCC in $ToolchainRoot; found $($compilers.Count)."
}
$compiler = $compilers[0].FullName
$version = (& $compiler --version 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0 -or $version -notmatch '(?i)xburst2') {
    throw "An Ingenic XBurst2 toolchain is required. Detected: $version"
}
Write-Host $version.Trim()
$prefix = ($compiler -replace 'gcc\.exe$', '') -replace '\\', '/'
$buildDir = Join-Path $workspace "build\windows-$Configuration"
& cmake -S (Join-Path $workspace 'projects\halley7m-hello') -B $buildDir -G 'MinGW Makefiles' "-DCMAKE_TOOLCHAIN_FILE=$workspace/cmake/xburst2-elf.cmake" "-DCROSS_COMPILE=$prefix" "-DCMAKE_BUILD_TYPE=$Configuration"
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& cmake --build $buildDir --parallel ([Environment]::ProcessorCount)
if ($LASTEXITCODE -ne 0) { throw 'Compilation failed.' }
$readelf = $compiler -replace 'gcc\.exe$', 'readelf.exe'
& $readelf -h (Join-Path $buildDir 'halley7m_hello.elf')
if ($LASTEXITCODE -ne 0) { throw 'ELF inspection failed.' }
Get-Item -LiteralPath (Join-Path $buildDir 'halley7m_hello.bin') | Format-List FullName,Length

[CmdletBinding()]
param(
    [string]$ToolchainRoot,
    [ValidateSet('Debug','Release')][string]$Configuration = 'Release'
)
$ErrorActionPreference = 'Stop'
if (-not $ToolchainRoot) { $ToolchainRoot = Join-Path $PSScriptRoot '..\toolchains\xburst2-windows' }
$workspace = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$compilers = @(Get-ChildItem -LiteralPath $ToolchainRoot -Recurse -File -Filter '*gcc.exe' |
    Where-Object { $_.Directory.Name -eq 'bin' -and $_.Name -match '^mips.*-gcc\.exe$' })
if ($compilers.Count -ne 1) { throw 'Expected one official XBurst2 GCC; install toolchain first.' }
$compiler = $compilers[0].FullName
$prefix = ($compiler -replace 'gcc\.exe$', '') -replace '\\', '/'
$buildDir = Join-Path $workspace "build\regba-$Configuration"
& cmake -S (Join-Path $workspace 'projects\halley7m-regba') -B $buildDir -G Ninja "-DCMAKE_TOOLCHAIN_FILE=$workspace/cmake/xburst2-elf.cmake" "-DCROSS_COMPILE=$prefix" "-DCMAKE_BUILD_TYPE=$Configuration"
if ($LASTEXITCODE) { throw 'ReGBA CMake configuration failed.' }
& cmake --build $buildDir --parallel 4
if ($LASTEXITCODE) { throw 'ReGBA compilation failed.' }
& python (Join-Path $workspace 'scripts\verify-regba.py') --build $buildDir --prefix $prefix
if ($LASTEXITCODE) { throw 'ReGBA artifact verification failed.' }
$sdRoot = Join-Path $workspace 'build\sdcard\GBA'
foreach ($dir in @('ROMS','SAVES','BIOS','CONFIG')) {
    New-Item -ItemType Directory -Force -Path (Join-Path $sdRoot $dir) | Out-Null
}
Copy-Item -LiteralPath (Join-Path $workspace 'third_party\ReGBA\game_config.txt') -Destination (Join-Path $sdRoot 'CONFIG\game_config.txt')
Copy-Item -LiteralPath (Join-Path $buildDir 'regba-core\x2600-selftest.gba') -Destination (Join-Path $sdRoot 'ROMS\x2600-selftest.gba')
Get-Item -LiteralPath (Join-Path $buildDir 'halley7m_regba.bin') | Select-Object FullName,Length
$hash = Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $buildDir 'halley7m_regba.bin')
Write-Host "BIN SHA256: $($hash.Hash)"
Write-Host "TF resource staging only: $sdRoot (no physical card or NOR was written)."

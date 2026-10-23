[CmdletBinding()]
param([ValidateSet('Debug','Release')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$compilers = @(Get-ChildItem -LiteralPath (Join-Path $workspace 'toolchains/xburst2-windows') -Recurse -File -Filter '*gcc.exe' |
    Where-Object Name -Match '^mips.*-gcc.exe$')
if ($compilers.Count -ne 1) { throw 'Expected one installed XBurst2 ELF compiler' }
$prefix = ($compilers[0].FullName -replace 'gcc\.exe$', '') -replace '\\', '/'
$build = Join-Path $workspace "build/ai-$Configuration"
& cmake -S (Join-Path $workspace 'projects/halley7m-ai') -B $build -G Ninja `
    "-DCMAKE_TOOLCHAIN_FILE=$workspace/cmake/xburst2-elf.cmake" "-DCROSS_COMPILE=$prefix" "-DCMAKE_BUILD_TYPE=$Configuration"
if ($LASTEXITCODE) { throw 'AI configure failed' }
& cmake --build $build --parallel 4
if ($LASTEXITCODE) { throw 'AI build failed' }
& python (Join-Path $workspace 'scripts/verify-ai.py') --build $build
if ($LASTEXITCODE) { throw 'AI artifact verification failed' }

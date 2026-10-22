[CmdletBinding()]
param(
    [string]$ToolchainRoot = (Join-Path $PSScriptRoot '..\toolchains\xburst2-windows'),
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [ValidateSet('Ninja', 'MinGW Makefiles')][string]$Generator = 'Ninja',
    [string]$BuildDirectory
)
$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if (-not $BuildDirectory) { $BuildDirectory = "build\windows-$Configuration" }
$buildDir = if ([IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory } else { Join-Path $workspace $BuildDirectory }
$cache = Join-Path $buildDir 'CMakeCache.txt'
if (Test-Path -LiteralPath $cache -PathType Leaf) {
    $cachedGenerator = @(Get-Content -LiteralPath $cache | Where-Object { $_ -match '^CMAKE_GENERATOR:INTERNAL=' })
    if ($cachedGenerator.Count -eq 1) {
        $previousGenerator = $cachedGenerator[0] -replace '^CMAKE_GENERATOR:INTERNAL=', ''
        if ($previousGenerator -ne $Generator) {
            throw "Build directory $buildDir uses generator '$previousGenerator'. Run with -Generator '$previousGenerator', or choose a new -BuildDirectory. The existing cache was not changed."
        }
    }
}
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
& cmake -S (Join-Path $workspace 'projects\halley7m-hello') -B $buildDir -G $Generator "-DCMAKE_TOOLCHAIN_FILE=$workspace/cmake/xburst2-elf.cmake" "-DCROSS_COMPILE=$prefix" "-DCMAKE_BUILD_TYPE=$Configuration"
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& cmake --build $buildDir --parallel ([Environment]::ProcessorCount)
if ($LASTEXITCODE -ne 0) { throw 'Compilation failed.' }
$readelf = $compiler -replace 'gcc\.exe$', 'readelf.exe'
& $readelf -h (Join-Path $buildDir 'halley7m_hello.elf')
if ($LASTEXITCODE -ne 0) { throw 'ELF inspection failed.' }
Get-Item -LiteralPath (Join-Path $buildDir 'halley7m_hello.bin') | Format-List FullName,Length

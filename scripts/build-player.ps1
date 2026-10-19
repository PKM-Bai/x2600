[CmdletBinding()]
param(
    [string]$MipsToolchainRoot,
    [string]$RiscvToolchainRoot,
    [string]$RiscvArchive,
    [string]$WslDistribution = 'Ubuntu-22.04',
    [string]$ProbeMedia,
    [ValidateSet('Debug','Release')][string]$Configuration = 'Release',
    [ValidateSet('mips','riscv')][string]$AudioOwner = 'riscv',
    [ValidateRange(1000000,1000000000)][long]$RiscvClockHz = 576000000,
    [switch]$MipsOnly
)
$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if (-not $MipsToolchainRoot) { $MipsToolchainRoot = Join-Path $workspace 'toolchains\xburst2-windows' }
function Find-Compiler([string]$Root, [string]$Pattern) {
    if (-not (Test-Path -LiteralPath $Root)) { throw "Toolchain directory missing: $Root" }
    $found = @(Get-ChildItem -LiteralPath $Root -Recurse -File -Filter '*gcc.exe' |
        Where-Object { $_.Directory.Name -eq 'bin' -and $_.Name -match $Pattern })
    if ($found.Count -ne 1) { throw "Expected one matching compiler in $Root; found $($found.Count)." }
    return (($found[0].FullName -replace 'gcc\.exe$', '') -replace '\\', '/')
}
function ConvertTo-WslPath([string]$Path) {
    $converted = & wsl.exe -d $WslDistribution --exec wslpath -a $Path
    if ($LASTEXITCODE) { throw "WSL path conversion failed: $Path" }
    return ($converted -join "`n").Trim()
}
$mipsPrefix = Find-Compiler $MipsToolchainRoot '^mips.*-gcc\.exe$'
if ($MipsOnly -and -not $PSBoundParameters.ContainsKey('AudioOwner')) { $AudioOwner = 'mips' }
if ($MipsOnly -and $AudioOwner -ne 'mips') { throw '-MipsOnly requires -AudioOwner mips.' }
$buildRoot = Join-Path $workspace "build\player-$AudioOwner-$Configuration"
if ($MipsOnly) { $buildRoot = Join-Path $workspace "build\player-mips-only-$Configuration" }
$riscvBuild = Join-Path $buildRoot 'riscv'
$mipsBuild = Join-Path $buildRoot 'mips'
$source = Join-Path $workspace 'projects\halley7m-player'
$probeMediaPath = ''
if ($ProbeMedia) { $probeMediaPath = (Resolve-Path -LiteralPath $ProbeMedia).Path -replace '\\', '/' }
if ($MipsOnly) {
    & cmake -S $source -B $mipsBuild -G Ninja "-DCMAKE_TOOLCHAIN_FILE=$workspace/cmake/xburst2-elf.cmake" "-DCROSS_COMPILE=$mipsPrefix" "-DCMAKE_BUILD_TYPE=$Configuration" '-DPLAYER_CORE=mips' '-DPLAYER_AUDIO_OWNER=mips' '-DPLAYER_MIPS_ONLY=ON' "-DPLAYER_RISCV_CLOCK_HZ=$RiscvClockHz" "-DPLAYER_TEST_AVI=$probeMediaPath"
    if ($LASTEXITCODE) { throw 'MIPS-only configuration failed.' }
    & cmake --build $mipsBuild --parallel 4
    if ($LASTEXITCODE) { throw 'MIPS-only compilation failed.' }
    & python (Join-Path $PSScriptRoot 'verify-player.py') --mips-only --mips-build $mipsBuild --audio-owner mips --manifest (Join-Path $buildRoot 'manifest.json')
    if ($LASTEXITCODE) { throw 'MIPS-only artifact verification failed.' }
    Write-Host "MIPS-only diagnostics: $mipsBuild (RISC-V missing; no AMP validation; no hardware writes)."
    exit 0
}
if ($RiscvArchive) {
    & (Join-Path $PSScriptRoot 'prepare-player-tools.ps1') -RiscvArchive $RiscvArchive -WslDistribution $WslDistribution
    if (-not $?) { throw 'Victory0 toolchain preparation failed.' }
    if (-not $RiscvToolchainRoot) {
        $preparedTools = Get-Content -LiteralPath (Join-Path $workspace 'build\player-tools\dependencies.json') -Raw | ConvertFrom-Json
        $RiscvToolchainRoot = $preparedTools.riscv.toolchain_root
    }
}
if (-not $RiscvToolchainRoot) {
    $linuxRoot = Join-Path $workspace 'toolchains\victory0-linux'
    if (Test-Path -LiteralPath (Join-Path $linuxRoot 'bin\riscv32-ingenicv0-elf-gcc') -PathType Leaf) {
        $RiscvToolchainRoot = $linuxRoot
    } else { $RiscvToolchainRoot = Join-Path $workspace 'toolchains\victory0-windows' }
}
$linuxRiscv = Test-Path -LiteralPath (Join-Path $RiscvToolchainRoot 'bin\riscv32-ingenicv0-elf-gcc') -PathType Leaf
if ($linuxRiscv) {
    # Keep Linux CMake state separate from the Windows native-toolchain cache.
    $riscvBuild = Join-Path $buildRoot 'riscv-wsl'
    $wslSource = ConvertTo-WslPath $source
    $wslBuild = ConvertTo-WslPath $riscvBuild
    $wslToolchain = ConvertTo-WslPath (Join-Path $workspace 'cmake\victory0-elf.cmake')
    $riscvPrefix = (ConvertTo-WslPath (Join-Path $RiscvToolchainRoot 'bin\riscv32-ingenicv0-elf-gcc')) -replace 'gcc$', ''
    & wsl.exe -d $WslDistribution --exec cmake -S $wslSource -B $wslBuild -G Ninja "-DCMAKE_TOOLCHAIN_FILE=$wslToolchain" "-DCROSS_COMPILE=$riscvPrefix" "-DCMAKE_BUILD_TYPE=$Configuration" '-DPLAYER_CORE=riscv' "-DPLAYER_AUDIO_OWNER=$AudioOwner" "-DPLAYER_RISCV_CLOCK_HZ=$RiscvClockHz"
    if ($LASTEXITCODE) { throw 'WSL RISC-V configuration failed.' }
    & wsl.exe -d $WslDistribution --exec cmake --build $wslBuild --parallel 4
    if ($LASTEXITCODE) { throw 'WSL RISC-V compilation failed.' }
} else {
    $riscvPrefix = Find-Compiler $RiscvToolchainRoot '^riscv32-ingenicv0-elf-gcc\.exe$'
    & cmake -S $source -B $riscvBuild -G Ninja "-DCMAKE_TOOLCHAIN_FILE=$workspace/cmake/victory0-elf.cmake" "-DCROSS_COMPILE=$riscvPrefix" "-DCMAKE_BUILD_TYPE=$Configuration" '-DPLAYER_CORE=riscv' "-DPLAYER_AUDIO_OWNER=$AudioOwner" "-DPLAYER_RISCV_CLOCK_HZ=$RiscvClockHz"
    if ($LASTEXITCODE) { throw 'RISC-V configuration failed.' }
    & cmake --build $riscvBuild --parallel 4
    if ($LASTEXITCODE) { throw 'RISC-V compilation failed.' }
}
$embedHeader = Join-Path $riscvBuild 'player_embed.h'
& python (Join-Path $PSScriptRoot 'verify-player.py') --riscv-build $riscvBuild --emit-embed-header $embedHeader
if ($LASTEXITCODE) { throw 'RISC-V artifact verification failed.' }
& cmake -S $source -B $mipsBuild -G Ninja "-DCMAKE_TOOLCHAIN_FILE=$workspace/cmake/xburst2-elf.cmake" "-DCROSS_COMPILE=$mipsPrefix" "-DCMAKE_BUILD_TYPE=$Configuration" '-DPLAYER_CORE=mips' "-DPLAYER_AUDIO_OWNER=$AudioOwner" "-DPLAYER_RISCV_CLOCK_HZ=$RiscvClockHz" "-DPLAYER_RISCV_BIN=$riscvBuild/player_riscv.bin" "-DPLAYER_EMBED_HEADER=$embedHeader" "-DPLAYER_TEST_AVI=$probeMediaPath"
if ($LASTEXITCODE) { throw 'MIPS configuration failed.' }
& cmake --build $mipsBuild --parallel 4
if ($LASTEXITCODE) { throw 'MIPS compilation failed.' }
& python (Join-Path $PSScriptRoot 'verify-player.py') --riscv-build $riscvBuild --mips-build $mipsBuild --audio-owner $AudioOwner --riscv-clock-hz $RiscvClockHz --manifest (Join-Path $buildRoot 'manifest.json') --wsl-distribution $WslDistribution
if ($LASTEXITCODE) { throw 'Player artifact verification failed.' }
Get-Item -LiteralPath (Join-Path $mipsBuild 'halley7m_player.bin') | Select-Object FullName,Length
Write-Host "Player artifacts: $buildRoot (build only; no board or NOR writes)."

[CmdletBinding()]
param([string]$RiscvArchive, [string]$FfmpegArchive,
      [string]$WslDistribution = 'Ubuntu-22.04')
$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$output = Join-Path $workspace 'build\player-tools'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$reportPath = Join-Path $output 'dependencies.json'
$previousReport = $null
if (Test-Path -LiteralPath $reportPath) {
    $previousReport = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
}

function Install-Archive([string]$Archive, [string]$Target, [string]$Executable) {
    $existing = @(Get-ChildItem -LiteralPath $Target -Recurse -File -Filter $Executable -ErrorAction SilentlyContinue)
    if ($existing.Count -gt 1) { throw "Expected one installed $Executable in $Target" }
    if (-not $Archive) {
        if ($existing.Count -eq 1) { return $existing[0].FullName }
        return $null
    }
    $package = (Resolve-Path -LiteralPath $Archive).Path
    if (-not $existing.Count -and (Test-Path -LiteralPath $Target)) {
        if (@(Get-ChildItem -LiteralPath $Target -Force).Count) { throw "Install target is not empty: $Target" }
    }
    $staging = Join-Path $output ([guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $staging | Out-Null
    & tar.exe -xf $package -C $staging
    if ($LASTEXITCODE) { throw "Archive extraction failed: $package" }
    $found = @(Get-ChildItem -LiteralPath $staging -Recurse -File -Filter $Executable)
    if ($found.Count -ne 1 -or $found[0].Directory.Name -ne 'bin') {
        throw "Expected one bin/$Executable in $package"
    }
    if ($existing.Count -eq 1) {
        $archiveBinaryHash = (Get-FileHash -LiteralPath $found[0].FullName -Algorithm SHA256).Hash
        $installedBinaryHash = (Get-FileHash -LiteralPath $existing[0].FullName -Algorithm SHA256).Hash
        if ($archiveBinaryHash -ne $installedBinaryHash) {
            throw "Archive $Executable differs from installed binary: $package"
        }
        if ($Executable -eq 'ffmpeg.exe') {
            $archiveProbe = Join-Path $found[0].Directory.FullName 'ffprobe.exe'
            $installedProbe = Join-Path $existing[0].Directory.FullName 'ffprobe.exe'
            if (-not (Test-Path -LiteralPath $archiveProbe) -or
                (Get-FileHash -LiteralPath $archiveProbe -Algorithm SHA256).Hash -ne
                (Get-FileHash -LiteralPath $installedProbe -Algorithm SHA256).Hash) {
                throw 'Archive FFprobe differs from installed binary or is missing.'
            }
        }
        return $existing[0].FullName
    }
    $packageRoot = $found[0].Directory.Parent.FullName
    New-Item -ItemType Directory -Force -Path $Target | Out-Null
    Get-ChildItem -LiteralPath $packageRoot -Force | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $Target -Recurse
    }
    return (Join-Path $Target "bin\$Executable")
}

function ConvertTo-WslPath([string]$Path) {
    $converted = & wsl.exe -d $WslDistribution --exec wslpath -a $Path
    if ($LASTEXITCODE) { throw "WSL path conversion failed: $Path" }
    return ($converted -join "`n").Trim()
}

function Install-LinuxVictory0([string]$Archive, [string]$Target) {
    $compiler = Join-Path $Target 'bin\riscv32-ingenicv0-elf-gcc'
    $installed = Test-Path -LiteralPath $compiler -PathType Leaf
    if (-not $Archive) {
        if ($installed) { return $compiler }
        return $null
    }
    $package = (Resolve-Path -LiteralPath $Archive).Path
    if (-not $installed -and (Test-Path -LiteralPath $Target) -and
        @(Get-ChildItem -LiteralPath $Target -Force).Count) {
        throw "Linux toolchain target is not empty: $Target"
    }
    $staging = Join-Path $output ([guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $staging | Out-Null
    $extractor = Join-Path $output 'install-victory0.sh'
    $script = @'
set -eu
archive=$1
target=$2
mkdir -p -- "$target"
tar -xf "$archive" -C "$target" --strip-components=1
test -x "$target/bin/riscv32-ingenicv0-elf-gcc"
'@
    [IO.File]::WriteAllText($extractor, $script.Replace("`r`n", "`n") + "`n", [Text.UTF8Encoding]::new($false))
    & wsl.exe -d $WslDistribution --exec sh (ConvertTo-WslPath $extractor) (ConvertTo-WslPath $package) (ConvertTo-WslPath $staging)
    if ($LASTEXITCODE) { throw 'Official Linux Victory0 toolchain extraction failed.' }
    $archiveCompiler = Join-Path $staging 'bin\riscv32-ingenicv0-elf-gcc'
    if ($installed) {
        if ((Get-FileHash -LiteralPath $archiveCompiler -Algorithm SHA256).Hash -ne
            (Get-FileHash -LiteralPath $compiler -Algorithm SHA256).Hash) {
            throw 'Archive Victory0 compiler differs from installed binary.'
        }
    } else {
        New-Item -ItemType Directory -Force -Path $Target | Out-Null
        & wsl.exe -d $WslDistribution --exec cp -a -- ((ConvertTo-WslPath $staging) + '/.') (ConvertTo-WslPath $Target)
        if ($LASTEXITCODE) { throw 'Official Linux Victory0 toolchain installation failed.' }
    }
    return $compiler
}

$report = [ordered]@{ generated_at = (Get-Date).ToString('o'); hardware_validated = $false }
$linuxRiscv = Test-Path -LiteralPath (Join-Path $workspace 'toolchains\victory0-linux\bin\riscv32-ingenicv0-elf-gcc') -PathType Leaf
if ($RiscvArchive) {
    $archiveEntries = @(& tar.exe -tf (Resolve-Path -LiteralPath $RiscvArchive).Path)
    if ($LASTEXITCODE) { throw 'Could not inspect Victory0 archive.' }
    $linuxRiscv = @($archiveEntries | Where-Object { $_ -match '/bin/riscv32-ingenicv0-elf-gcc$' }).Count -eq 1
}
if ($linuxRiscv) {
    $riscvRoot = Join-Path $workspace 'toolchains\victory0-linux'
    $riscv = Install-LinuxVictory0 $RiscvArchive $riscvRoot
} else {
    $riscvRoot = Join-Path $workspace 'toolchains\victory0-windows'
    $riscv = Install-Archive $RiscvArchive $riscvRoot 'riscv32-ingenicv0-elf-gcc.exe'
}
if ($riscv) {
    if ($linuxRiscv) {
        $wslCompiler = ConvertTo-WslPath $riscv
        $version = @(& wsl.exe -d $WslDistribution --exec $wslCompiler --version)
    } else { $version = @(& $riscv --version) }
    if ($LASTEXITCODE) { throw 'Victory0 compiler could not run.' }
    if ($linuxRiscv) { $machine = & wsl.exe -d $WslDistribution --exec $wslCompiler -dumpmachine }
    else { $machine = & $riscv -dumpmachine }
    $machine = ($machine -join "`n").Trim()
    if ($LASTEXITCODE -or $machine -ne 'riscv32-ingenicv0-elf') { throw 'Expected the official Victory0 compiler target.' }
    $source = Join-Path $output 'victory0-smoke.c'
    $smoke = 'int entry(int a, int b) { __asm__ volatile ("add a6,%0,%1" : : "r"(a), "r"(b) : "a6"); return a*b; }'
    [IO.File]::WriteAllText($source, $smoke, [Text.UTF8Encoding]::new($false))
    $smokeElf = Join-Path $output 'victory0-smoke.elf'
    if ($linuxRiscv) {
        & wsl.exe -d $WslDistribution --exec $wslCompiler -march=rv32imc -mabi=ilp32 -ffreestanding -nostdlib '-Wl,-Ttext=0x03c00080,-e,entry' (ConvertTo-WslPath $source) -o (ConvertTo-WslPath $smokeElf)
    } else {
        & $riscv -march=rv32imc -mabi=ilp32 -ffreestanding -nostdlib '-Wl,-Ttext=0x03c00080,-e,entry' $source -o $smokeElf
    }
    if ($LASTEXITCODE) { throw 'Victory0 RV32IMC/ILP32 ELF smoke test failed.' }
    $report.riscv = @{ status = 'compiler-smoke-passed'; compiler = $riscv; version = $version[0]; target = $machine;
        compiler_sha256 = (Get-FileHash -LiteralPath $riscv -Algorithm SHA256).Hash;
        toolchain_root = $riscvRoot; march = 'rv32imc'; abi = 'ilp32' }
    if ($linuxRiscv) { $report.riscv.host = 'wsl-linux'; $report.riscv.wsl_distribution = $WslDistribution; $report.riscv.wsl_compiler = $wslCompiler }
    else { $report.riscv.host = 'windows' }
    if ($RiscvArchive) {
        $report.riscv.archive_sha256 = (Get-FileHash -LiteralPath $RiscvArchive -Algorithm SHA256).Hash
        $report.riscv.archive_file = (Resolve-Path -LiteralPath $RiscvArchive).Path
        $report.riscv.archive_verified = $true
    } elseif ($previousReport.riscv.compiler_sha256 -eq $report.riscv.compiler_sha256 -and
              $previousReport.riscv.archive_sha256 -and $previousReport.riscv.archive_verified) {
        $report.riscv.archive_sha256 = $previousReport.riscv.archive_sha256
        $report.riscv.archive_file = $previousReport.riscv.archive_file
        $report.riscv.archive_verified = $true
    }
} else {
    $report.riscv = @{ status = 'missing'; expected_archive = 'riscv-ingenic-victory0-series-toolchain-win64-r1.0.0.tar.bz2' }
}

$ffmpegRoot = Join-Path $workspace 'tools\ffmpeg'
$ffmpeg = Install-Archive $FfmpegArchive $ffmpegRoot 'ffmpeg.exe'
if ($ffmpeg) {
    $ffprobe = Join-Path (Split-Path -Parent $ffmpeg) 'ffprobe.exe'
    $version = @(& $ffmpeg -version)
    if ($LASTEXITCODE -or $version[0] -notmatch '^ffmpeg version 9\.0\.2(?:-|\s)') { throw 'Expected FFmpeg 9.0.2.' }
    $probeVersion = @(& $ffprobe -version)
    if ($LASTEXITCODE -or $probeVersion[0] -notmatch '^ffprobe version 9\.0\.2(?:-|\s)') { throw 'Expected FFprobe 9.0.2.' }
    $report.ffmpeg = @{ status = 'version-verified'; executable = $ffmpeg; version = $version[0]; ffprobe = $probeVersion[0];
        binary_sha256 = (Get-FileHash -LiteralPath $ffmpeg -Algorithm SHA256).Hash;
        ffprobe_binary_sha256 = (Get-FileHash -LiteralPath $ffprobe -Algorithm SHA256).Hash;
        version_output = $version; ffprobe_version_output = $probeVersion;
        build_configuration = (@($version | Where-Object { $_ -match '^configuration:' }) -join "`n") }
    if ($FfmpegArchive) {
        $report.ffmpeg.archive_sha256 = (Get-FileHash -LiteralPath $FfmpegArchive -Algorithm SHA256).Hash
        $report.ffmpeg.archive_file = (Resolve-Path -LiteralPath $FfmpegArchive).Path
        $report.ffmpeg.archive_verified = $true
    } elseif ($previousReport.ffmpeg.binary_sha256 -eq $report.ffmpeg.binary_sha256 -and
              $previousReport.ffmpeg.archive_sha256 -and $previousReport.ffmpeg.archive_verified) {
        $report.ffmpeg.archive_sha256 = $previousReport.ffmpeg.archive_sha256
        $report.ffmpeg.archive_file = $previousReport.ffmpeg.archive_file
        $report.ffmpeg.archive_verified = $true
    }
} else {
    $report.ffmpeg = @{ status = 'missing'; expected_version = '9.0.2 essentials' }
}
$report | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $reportPath -Encoding utf8
$report | ConvertTo-Json -Depth 5 | Write-Output
if (-not $riscv -or -not $ffmpeg) { throw 'Player dependencies are incomplete; see build/player-tools/dependencies.json.' }

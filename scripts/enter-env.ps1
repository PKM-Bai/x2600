[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$venvRoot = Join-Path $workspace '.venv'
$venvPython = Join-Path $venvRoot 'Scripts\python.exe'
$activate = Join-Path $venvRoot 'Scripts\Activate.ps1'
foreach ($required in @((Join-Path $venvRoot 'pyvenv.cfg'), $venvPython, $activate)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Repository virtual environment is missing or incomplete: $venvRoot. Create it with python -m venv .venv, then install requirements.txt."
    }
}
$pythonVersion = (& $venvPython --version 2>&1 | Out-String).Trim()
if ($LASTEXITCODE -ne 0) { throw "Virtual environment Python could not start: $venvPython" }

. $activate
$env:PYTHONUTF8 = '1'
$ffmpegBin = Join-Path $workspace 'tools\ffmpeg\bin'
if (Test-Path -LiteralPath $ffmpegBin -PathType Container) {
    $pathEntries = @($env:PATH -split [IO.Path]::PathSeparator | Where-Object { $_ -and $_ -ne $ffmpegBin })
    $env:PATH = (@($ffmpegBin) + $pathEntries) -join [IO.Path]::PathSeparator
    Write-Host "Repository FFmpeg: $ffmpegBin"
}

Write-Host "Repository: $workspace"
Write-Host "Python: $pythonVersion ($venvPython); PYTHONUTF8=$env:PYTHONUTF8"
function Show-ToolStatus([string]$Name) {
    $command = Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $command) {
        Write-Warning "$Name is missing from PATH."
        return
    }
    try {
        $version = @(& $command.Source --version 2>&1)
        if ($LASTEXITCODE -ne 0) { throw "exit code $LASTEXITCODE" }
        Write-Host "${Name}: $($version[0]) ($($command.Source))"
    } catch {
        Write-Warning "$Name was found at $($command.Source), but could not run: $_"
    }
}
Show-ToolStatus 'cmake'
Show-ToolStatus 'ninja'

$mipsRoot = Join-Path $workspace 'toolchains\xburst2-windows'
$mipsCompilers = @()
if (Test-Path -LiteralPath $mipsRoot -PathType Container) {
    $mipsCompilers = @(Get-ChildItem -LiteralPath $mipsRoot -Recurse -File -Filter 'mips*-gcc.exe' |
        Where-Object { $_.Directory.Name -eq 'bin' })
}
if ($mipsCompilers.Count -eq 1) {
    Write-Host "XBurst2 compiler found (not validated by this entry point): $($mipsCompilers[0].FullName)"
} else {
    Write-Warning "Expected one XBurst2 compiler under $mipsRoot; found $($mipsCompilers.Count). MIPS cross-compilation is not ready."
}
$riscvCompilers = @()
foreach ($name in @('victory0-windows', 'victory0-linux')) {
    $root = Join-Path $workspace "toolchains\$name"
    if (Test-Path -LiteralPath $root -PathType Container) {
        $riscvCompilers += @(Get-ChildItem -LiteralPath $root -Recurse -File -Filter '*gcc*' |
            Where-Object { $_.Directory.Name -eq 'bin' -and $_.Name -match '^riscv.*-gcc(\.exe)?$' })
    }
}
if ($riscvCompilers.Count -gt 0) {
    Write-Host "Victory0 compiler found (not validated by this entry point): $($riscvCompilers.FullName -join ', ')"
} else {
    Write-Warning 'Victory0 compiler is missing from the default toolchain directories. AMP player cross-compilation is not ready.'
}

[CmdletBinding()]
param([string]$VisualStudioPath, [switch]$InputOnly, [switch]$VideoOnly)
$ErrorActionPreference = 'Stop'
if ($InputOnly -and $VideoOnly) { throw 'Choose only one of -InputOnly or -VideoOnly.' }
$workspace = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if (-not $VisualStudioPath) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $VisualStudioPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($LASTEXITCODE) { throw 'vswhere failed to locate MSVC.' }
    }
    if (-not $VisualStudioPath) {
        $knownPath = 'C:\Program Files\Microsoft Visual Studio\18\Community'
        if (Test-Path -LiteralPath (Join-Path $knownPath 'VC\Auxiliary\Build\vcvars64.bat')) {
            $VisualStudioPath = $knownPath
        }
    }
}
if (-not $VisualStudioPath) { throw 'MSVC x64 build tools are required; pass -VisualStudioPath if not registered.' }
$vcvars = Join-Path $VisualStudioPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path -LiteralPath $vcvars)) { throw "Missing MSVC environment setup: $vcvars" }
$output = Join-Path $workspace 'build\player-host\input'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$common = Join-Path $workspace 'projects\halley7m-player\common'
$executable = Join-Path $output 'player_input_test.exe'
$videoExecutable = Join-Path $output 'player_video_test.exe'
$runner = Join-Path $output 'run-tests.cmd'
$testCommands = [Collections.Generic.List[string]]::new()
if (-not $VideoOnly) {
    $testCommands.Add("cl /nologo /std:c11 /utf-8 /O2 /W4 /WX /I`"$common`" `"$workspace\tests\player_input_test.c`" `"$common\player_input.c`" /Fe`"$executable`"")
    $testCommands.Add('if errorlevel 1 exit /b 1')
    $testCommands.Add("`"$executable`"")
    $testCommands.Add('if errorlevel 1 exit /b 1')
}
if (-not $InputOnly) {
    $testCommands.Add("cl /nologo /std:c11 /utf-8 /O2 /W4 /WX /I`"$common`" `"$workspace\tests\player_video_test.c`" `"$common\player_video.c`" /Fe`"$videoExecutable`"")
    $testCommands.Add('if errorlevel 1 exit /b 1')
    $testCommands.Add("`"$videoExecutable`"")
    $testCommands.Add('if errorlevel 1 exit /b 1')
}
$commands = @"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b 1
cd /d "$output"
$($testCommands -join "`n")
exit /b 0
"@
[IO.File]::WriteAllText($runner, $commands.Replace("`r`n", "`n").Replace("`n", "`r`n"), [Text.UTF8Encoding]::new($false))
& $env:ComSpec /d /s /c "`"$runner`""
if ($LASTEXITCODE) { throw 'Player native host regression failed; see output above.' }
if (-not $InputOnly -and -not $VideoOnly) {
    $mediaTest = Join-Path $PSScriptRoot 'test-player-media.py'
    if (Test-Path -LiteralPath $mediaTest) {
        $python = Join-Path $workspace '.venv\Scripts\python.exe'
        if (-not (Test-Path -LiteralPath $python)) { $python = 'python' }
        & $python $mediaTest
        if ($LASTEXITCODE) { throw 'Player media host regression failed.' }
    }
    foreach ($relative in @('scripts/test-player-amp.py', 'scripts/test-player-amp-control.py',
                            'scripts/test-player-udivdi3.py',
                            'projects/halley7m-player/storage/test-storage.py')) {
        & $python (Join-Path $workspace $relative)
        if ($LASTEXITCODE) { throw "Player host regression failed: $relative" }
    }
}
Write-Host "Player host checks passed. Artifacts: $output"
Write-Host 'Input/video checks use production C with virtual time and RAM buffers; no board or physical TF access.'

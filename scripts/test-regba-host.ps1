[CmdletBinding()]
param([string]$VisualStudioPath)
$ErrorActionPreference = 'Stop'
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
if (-not $VisualStudioPath) { throw 'MSVC x64 build tools are required; pass -VisualStudioPath if not registered with vswhere.' }
$vcvars = Join-Path $VisualStudioPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path -LiteralPath $vcvars)) { throw "Missing MSVC environment setup: $vcvars" }

$output = Join-Path $workspace 'build\regba-host'
$storageOutput = Join-Path $output 'storage'
$audioOutput = Join-Path $output 'audio'
$videoOutput = Join-Path $output 'video'
$buttonsOutput = Join-Path $output 'buttons'
$btpadOutput = Join-Path $output 'bt_gamepad'
$inputOutput = Join-Path $output 'input'
$batteryOutput = Join-Path $output 'battery'
$uiOutput = Join-Path $output 'ui'
$gameConfigOutput = Join-Path $output 'game_config'
$statsOutput = Join-Path $output 'game_stats'
foreach ($directory in @($output, $storageOutput, $audioOutput, $videoOutput, $buttonsOutput, $btpadOutput, $inputOutput, $batteryOutput, $uiOutput, $gameConfigOutput, $statsOutput)) {
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
}

# Inject the current production resampler, not a maintained copy. The template
# supplies an independent absolute-time oracle and a bounded output queue.
$platform = Join-Path $workspace 'projects\halley7m-regba\platform'
$core = Join-Path $workspace 'projects\halley7m-regba\core'
$regbaAdapter = Join-Path $workspace 'projects\halley7m-regba\engines\regba\regba_adapter.c'
$session = Join-Path $core 'emu_session.c'
function Get-ProductionFunction([string]$source, [string]$name) {
    # Hide braces in comments/strings while retaining source offsets.
    $masked = [regex]::Replace($source, '(?s)/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|''(?:\\.|[^''\\])*''', {
        param($match)
        [regex]::Replace($match.Value, '[^\r\n]', ' ')
    })
    $pattern = '(?m)^(?:static\s+)?[A-Za-z_][A-Za-z0-9_ \t*]*\b' + [regex]::Escape($name) + '\s*\([^;{]*\)\s*\{'
    $matches = [regex]::Matches($masked, $pattern)
    if ($matches.Count -ne 1) { throw "Expected exactly one production $name, found $($matches.Count)." }
    $match = $matches[0]
    $depth = 1
    for ($offset = $match.Index + $match.Length; $offset -lt $masked.Length; ++$offset) {
        if ($masked[$offset] -eq '{') { ++$depth }
        if ($masked[$offset] -eq '}') { --$depth }
        if (-not $depth) { return $source.Substring($match.Index, $offset - $match.Index + 1) }
    }
    throw "Unclosed production function $name."
}
# Compile the real sampler with only its time/Idle/critical-section boundary
# replaced. The fixture owns independent observations and expected rates.
$statsStub = @'
#ifndef GAME_STATS_HOST_FREERTOS_H
#define GAME_STATS_HOST_FREERTOS_H
#include <stdint.h>
void game_stats_test_enter(void);
void game_stats_test_exit(void);
uint64_t platform_time_us(void);
uint64_t ulTaskGetIdleRunTimeCounter(void);
#define taskENTER_CRITICAL() game_stats_test_enter()
#define taskEXIT_CRITICAL() game_stats_test_exit()
#endif
'@
[IO.File]::WriteAllText((Join-Path $statsOutput 'FreeRTOS.h'), $statsStub, [Text.UTF8Encoding]::new($false))
foreach ($header in @('task.h', 'platform_runtime.h')) {
    [IO.File]::WriteAllText((Join-Path $statsOutput $header), "#include `"FreeRTOS.h`"`n", [Text.UTF8Encoding]::new($false))
}
& (Join-Path $workspace '.venv\Scripts\python.exe') (Join-Path $workspace 'scripts\prepare-ui-font.py') --source (Join-Path $workspace 'third_party\ReGBA\source\tools\unifont.hex.gz') --output (Join-Path $uiOutput 'ui_font.h') --text (Join-Path $core 'handheld_ui.c') --text $session --text $regbaAdapter --text (Join-Path $platform 'ui_draw.c')
if ($LASTEXITCODE) { throw 'Could not prepare the native UI/HUD font.' }
$audioSource = Get-Content -LiteralPath (Join-Path $platform 'audio.c') -Raw -Encoding UTF8
$match = [regex]::Match($audioSource, '(?s)(size_t audio_submit_stereo\(.*?)(?=\r?\nvoid audio_get_stats\()')
if (-not $match.Success) { throw 'Could not extract audio_submit_stereo from production source.' }
$template = Get-Content -LiteralPath (Join-Path $workspace 'tests\audio_resampler_test.c.in') -Raw -Encoding UTF8
if (([regex]::Matches($template, '@AUDIO_SUBMIT_FUNCTION@')).Count -ne 1) { throw 'Audio test template must contain exactly one function marker.' }
$generatedAudio = Join-Path $audioOutput 'audio_resampler_test.c'
[IO.File]::WriteAllText($generatedAudio, $template.Replace('@AUDIO_SUBMIT_FUNCTION@', $match.Groups[1].Value), [Text.UTF8Encoding]::new($false))
$lifecycle = Get-Content -LiteralPath (Join-Path $workspace 'tests\audio_lifecycle_test.c.in') -Raw -Encoding UTF8
foreach ($part in @(
    @('(?s)(int audio_start\(.*?)(?=\r?\nvoid audio_stop\()', '@AUDIO_START_FUNCTION@'),
    @('(?s)(void audio_stop\(.*?)(?=\r?\nvoid audio_set_muted\()', '@AUDIO_STOP_FUNCTION@'),
    @('(?s)(static int queue_sample\(.*?)(?=\r?\nsize_t audio_submit_stereo\()', '@AUDIO_QUEUE_FUNCTION@'),
    @('(?s)(static void audio_service\(.*?)(?=\r?\nint audio_init\()', '@AUDIO_SERVICE_FUNCTION@')
)) {
    $function = [regex]::Match($audioSource, $part[0])
    if (-not $function.Success) { throw "Could not extract lifecycle function: $($part[1])" }
    $lifecycle = $lifecycle.Replace($part[1], $function.Groups[1].Value)
}
$generatedLifecycle = Join-Path $audioOutput 'audio_lifecycle_test.c'
[IO.File]::WriteAllText($generatedLifecycle, $lifecycle, [Text.UTF8Encoding]::new($false))
$startTemplate = Get-Content -LiteralPath (Join-Path $workspace 'tests\audio_start_test.c.in') -Raw -Encoding UTF8
foreach ($part in @(
    @('(?s)(static void count_fifo_underrun\(.*?)(?=\r?\nstatic void dma_irq\()', '@AUDIO_FIFO_COUNT_FUNCTION@'),
    @('(?s)(static void start_period\(.*?)(?=\r?\nstatic void audio_service\()', '@AUDIO_PERIOD_START_FUNCTION@')
)) {
    $function = [regex]::Match($audioSource, $part[0])
    if (-not $function.Success) { throw "Could not extract audio startup function: $($part[1])" }
    $startTemplate = $startTemplate.Replace($part[1], $function.Groups[1].Value)
}
$generatedStart = Join-Path $audioOutput 'audio_start_test.c'
[IO.File]::WriteAllText($generatedStart, $startTemplate, [Text.UTF8Encoding]::new($false))

# Prepare a fresh pinned core so the parser and packaged bytes cannot be stale
# relative to this checkout. This generates host fixtures, not firmware.
$gameConfigCore = Join-Path $gameConfigOutput 'regba-core'
& (Join-Path $workspace '.venv\Scripts\python.exe') (Join-Path $workspace 'scripts\prepare-regba.py') --source (Join-Path $workspace 'third_party\ReGBA') --output $gameConfigCore
if ($LASTEXITCODE) { throw 'Could not prepare the native game-config fixture.' }
$memorySource = [IO.File]::ReadAllText((Join-Path $gameConfigCore 'memory.c'), [Text.Encoding]::GetEncoding(28591))
$configFunctions = [regex]::Match($memorySource, '(?s)(char \*skip_spaces\(char \*line_ptr\)\r?\n\{.*?)(?=\r?\n#define LOAD_ON_MEMORY)')
if (-not $configFunctions.Success) { throw 'Could not extract the generated native game-config functions.' }
$configTemplate = Get-Content -LiteralPath (Join-Path $workspace 'tests\game_config_test.c.in') -Raw -Encoding UTF8
if (([regex]::Matches($configTemplate, '@GAME_CONFIG_FUNCTIONS@')).Count -ne 1) { throw 'Game-config template must contain exactly one function marker.' }
$generatedGameConfig = Join-Path $gameConfigOutput 'game_config_test.c'
[IO.File]::WriteAllText($generatedGameConfig, $configTemplate.Replace('@GAME_CONFIG_FUNCTIONS@', $configFunctions.Groups[1].Value), [Text.UTF8Encoding]::new($false))

# Exercise the production aggregator with independent input-source stubs.
$adapterSource = Get-Content -LiteralPath $regbaAdapter -Raw -Encoding UTF8
$sessionSource = Get-Content -LiteralPath $session -Raw -Encoding UTF8
$uiLifecycle = Get-Content -LiteralPath (Join-Path $workspace 'tests\handheld_lifecycle_test.c.in') -Raw -Encoding UTF8
foreach ($part in @(
    @($adapterSource, 'settle_backup', '@SETTLE_BACKUP_FUNCTION@'),
    @($adapterSource, 'core_reload_save', '@RELOAD_SAVE_FUNCTION@'),
    @($adapterSource, 'backend_unload', '@BACKEND_UNLOAD_FUNCTION@'),
    @($adapterSource, 'backend_request_save', '@BACKEND_REQUEST_SAVE_FUNCTION@'),
    @($sessionSource, 'backend_loaded', '@BACKEND_LOADED_FUNCTION@'),
    @($sessionSource, 'backend_path', '@BACKEND_PATH_FUNCTION@'),
    @($sessionSource, 'gui_run_game', '@GUI_RUN_FUNCTION@'),
    @($sessionSource, 'gui_unload_game', '@GUI_UNLOAD_FUNCTION@'),
    @($sessionSource, 'gui_service', '@GUI_SERVICE_FUNCTION@')
)) {
    if (([regex]::Matches($uiLifecycle, [regex]::Escape($part[2]))).Count -ne 1) { throw "Lifecycle template must contain exactly one $($part[2])." }
    $uiLifecycle = $uiLifecycle.Replace($part[2], (Get-ProductionFunction $part[0] $part[1]))
}
$generatedUiLifecycle = Join-Path $uiOutput 'handheld_lifecycle_test.c'
[IO.File]::WriteAllText($generatedUiLifecycle, $uiLifecycle, [Text.UTF8Encoding]::new($false))
$inputFunction = Get-ProductionFunction $sessionSource 'host_keys'
$inputTemplate = @'
#include <stdint.h>
#include <stdio.h>

static volatile uint32_t requested_keys;
static volatile uint32_t gui_input_gate;
static uint16_t board_keys, bt_keys;
static unsigned failures;
static uint16_t buttons_get_keys(void) { return board_keys; }
static uint16_t bt_gamepad_get_keys(void) { return bt_keys; }

@REGBA_GET_PRESSED_BUTTONS@

static void expect(const char *name, uint16_t expected)
{
    uint16_t actual = host_keys(NULL);
    if (actual != expected) {
        printf("FAIL %s expected=%03x actual=%03x\n", name,
               (unsigned)expected, (unsigned)actual);
        ++failures;
    }
}

int main(void)
{
    expect("all released", 0);
    requested_keys = 0x001; board_keys = 0x008; bt_keys = 0x3F6;
    expect("three sources combined", 0x3FF);
    requested_keys = board_keys = bt_keys = 0x001;
    expect("same key held by all sources", 0x001);
    bt_keys = 0;
    expect("bluetooth release preserves other sources", 0x001);
    board_keys = 0;
    expect("board release preserves serial source", 0x001);
    requested_keys = 0;
    expect("all holders released", 0);
    bt_keys = 0x3FF;
    expect("bluetooth provides all ten keys", 0x3FF);
    requested_keys = 0x1000; board_keys = 0x400; bt_keys = 0x800;
    expect("unrelated bits filtered", 0);
    requested_keys = 1; board_keys = bt_keys = 0; gui_input_gate = 1;
    expect("menu confirmation is consumed", 0);
    bt_keys = 0x10;
    expect("held inputs remain consumed", 0);
    requested_keys = bt_keys = 0;
    expect("release opens input gate", 0);
    bt_keys = 1;
    expect("next game press is delivered", 1);
    if (failures) return 1;
    puts("input aggregation regression PASS: three sources, release isolation, ten keys, mask");
    return 0;
}
'@
$generatedInput = Join-Path $inputOutput 'input_aggregation_test.c'
[IO.File]::WriteAllText($generatedInput, $inputTemplate.Replace('@REGBA_GET_PRESSED_BUTTONS@', $inputFunction), [Text.UTF8Encoding]::new($false))

$storageExe = Join-Path $storageOutput 'storage_host_test.exe'
$encodingExe = Join-Path $storageOutput 'path_encoding_test.exe'
$audioExe = Join-Path $audioOutput 'audio_resampler_test.exe'
$videoExe = Join-Path $videoOutput 'video_blit_test.exe'
$buttonsExe = Join-Path $buttonsOutput 'buttons_test.exe'
$btpadExe = Join-Path $btpadOutput 'bt_gamepad_test.exe'
$btpadTimeoutExe = Join-Path $btpadOutput 'bt_gamepad_timeout_test.exe'
$btpadInvertedExe = Join-Path $btpadOutput 'bt_gamepad_inverted_test.exe'
$inputExe = Join-Path $inputOutput 'input_aggregation_test.exe'
$lifecycleExe = Join-Path $audioOutput 'audio_lifecycle_test.exe'
$startExe = Join-Path $audioOutput 'audio_start_test.exe'
$batteryExe = Join-Path $batteryOutput 'battery_save_test.exe'
$resourcesExe = Join-Path $uiOutput 'resources_test.exe'
$uiExe = Join-Path $uiOutput 'handheld_ui_test.exe'
$uiLifecycleExe = Join-Path $uiOutput 'handheld_lifecycle_test.exe'
$gameConfigExe = Join-Path $gameConfigOutput 'game_config_test.exe'
$statsExe = Join-Path $statsOutput 'game_stats_test.exe'
$runner = Join-Path $output 'run-tests.cmd'
$commands = @"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b 1
cd /d "$workspace"
cl /nologo /std:c11 /utf-8 /O2 /W4 /WX /I"$platform" "$workspace\tests\path_encoding_test.c" "$platform\path_encoding.c" /Fo"$storageOutput\\" /Fe"$encodingExe"
if errorlevel 1 exit /b 1
"$encodingExe"
if errorlevel 1 exit /b 1
cl /nologo /std:c11 /utf-8 /O2 /W3 /DSDSPI_HOST_TEST /DSTORAGE_SCAN_HOST_TEST /I"$platform" "$platform\storage_host_test.c" "$platform\storage.c" "$platform\path_encoding.c" "$platform\sdspi.c" "$platform\fatfs\ff.c" "$platform\fatfs\unicode.c" /Fo"$storageOutput\\" /Fe"$storageExe"
if errorlevel 1 exit /b 1
"$storageExe"
if errorlevel 1 exit /b 1
cl /nologo /std:c11 /utf-8 /O2 /W4 /WX "$generatedAudio" /Fo"$audioOutput\\" /Fe"$audioExe"
if errorlevel 1 exit /b 1
"$audioExe"
if errorlevel 1 exit /b 1
cl /nologo /std:c11 /utf-8 /O2 /W4 /WX /I"$platform" "$generatedLifecycle" /Fo"$audioOutput\\" /Fe"$lifecycleExe"
if errorlevel 1 exit /b 1
"$lifecycleExe"
if errorlevel 1 exit /b 1
cl /nologo /std:c11 /utf-8 /O2 /W4 /WX /I"$platform" "$generatedStart" /Fo"$audioOutput\\" /Fe"$startExe"
if errorlevel 1 exit /b 1
"$startExe"
if errorlevel 1 exit /b 1
cl /nologo /std:c11 /utf-8 /O2 /W4 /WX /I"$platform" /I"$uiOutput" "$workspace\tests\video_blit_test.c" "$platform\video_blit.c" "$platform\ui_draw.c" /Fo"$videoOutput\\" /Fe"$videoExe"
if errorlevel 1 exit /b 1
"$videoExe" "$videoOutput"
if errorlevel 1 exit /b 1
cl /nologo /std:c11 /utf-8 /O2 /W4 /WX /I"$statsOutput" /I"$platform" "$workspace\tests\game_stats_test.c" "$platform\game_stats.c" /Fo"$statsOutput\\" /Fe"$statsExe"
if errorlevel 1 exit /b 1
"$statsExe"
if errorlevel 1 exit /b 1
cl /nologo /std:c11 /utf-8 /O2 /W4 /WX /DBUTTONS_HOST_TEST /I"$platform" "$workspace\tests\buttons_test.c" "$platform\buttons.c" /Fo"$buttonsOutput\\" /Fe"$buttonsExe"
if errorlevel 1 exit /b 1
"$buttonsExe"
if errorlevel 1 exit /b 1
cl /nologo /std:c11 /utf-8 /O2 /W4 /WX /DBT_GAMEPAD_HOST_TEST /I"$platform" "$workspace\tests\bt_gamepad_test.c" "$platform\bt_gamepad.c" /Fo"$btpadOutput\\" /Fe"$btpadExe"
if errorlevel 1 exit /b 1
"$btpadExe"
if errorlevel 1 exit /b 1
cl /nologo /std:c11 /utf-8 /O2 /W4 /WX /DBT_GAMEPAD_HOST_TEST /DBT_GAMEPAD_TIMEOUT_MS=500 /I"$platform" "$workspace\tests\bt_gamepad_test.c" "$platform\bt_gamepad.c" /Fo"$btpadOutput\\" /Fe"$btpadTimeoutExe"
if errorlevel 1 exit /b 1
"$btpadTimeoutExe"
if errorlevel 1 exit /b 1
cl /nologo /std:c11 /utf-8 /O2 /W4 /WX /DBT_GAMEPAD_HOST_TEST /DBT_GAMEPAD_X_RIGHT_SIGN=-1 /DBT_GAMEPAD_Y_UP_SIGN=-1 /I"$platform" "$workspace\tests\bt_gamepad_test.c" "$platform\bt_gamepad.c" /Fo"$btpadOutput\\" /Fe"$btpadInvertedExe"
if errorlevel 1 exit /b 1
"$btpadInvertedExe"
if errorlevel 1 exit /b 1
cl /nologo /std:c11 /utf-8 /O2 /W4 /WX "$generatedInput" /Fo"$inputOutput\\" /Fe"$inputExe"
if errorlevel 1 exit /b 1
"$inputExe"
if errorlevel 1 exit /b 1
cl /nologo /std:c11 /utf-8 /O2 /W4 /WX /I"$core" "$workspace\tests\resources_test.c" "$core\resources.c" /Fo"$uiOutput\\" /Fe"$resourcesExe"
if errorlevel 1 exit /b 1
"$resourcesExe"
if errorlevel 1 exit /b 1
cl /nologo /std:c11 /utf-8 /O2 /W4 /WX /I"$core" /I"$gameConfigCore" "$generatedGameConfig" /Fo"$gameConfigOutput\\" /Fe"$gameConfigExe"
if errorlevel 1 exit /b 1
"$gameConfigExe"
if errorlevel 1 exit /b 1
cl /nologo /std:c11 /utf-8 /O2 /W4 /WX /I"$core" /I"$platform" "$workspace\tests\handheld_ui_test.c" "$core\handheld_ui.c" "$core\resources.c" /Fo"$uiOutput\\" /Fe"$uiExe"
if errorlevel 1 exit /b 1
"$uiExe"
if errorlevel 1 exit /b 1
cl /nologo /std:c11 /utf-8 /O2 /W4 /WX /I"$core" /I"$platform" "$generatedUiLifecycle" /Fo"$uiOutput\\" /Fe"$uiLifecycleExe"
if errorlevel 1 exit /b 1
"$uiLifecycleExe"
if errorlevel 1 exit /b 1
cl /nologo /std:c11 /utf-8 /O2 /W4 /WX /I"$platform" "$workspace\tests\battery_save_test.c" /Fo"$batteryOutput\\" /Fe"$batteryExe"
if errorlevel 1 exit /b 1
cd /d "$batteryOutput"
"$batteryExe"
if errorlevel 1 exit /b 1
exit /b 0
"@
[IO.File]::WriteAllText($runner, $commands.Replace("`n", "`r`n"), [Text.UTF8Encoding]::new($false))
& $env:ComSpec /d /s /c "`"$runner`""
if ($LASTEXITCODE) { throw 'ReGBA native regression failed; see compiler/test output above.' }
& (Join-Path $workspace '.venv\Scripts\python.exe') (Join-Path $workspace 'scripts\test-ssi-end.py') --vcvars $vcvars
if ($LASTEXITCODE) { throw 'SSI END ordering host regression failed.' }
& (Join-Path $workspace '.venv\Scripts\python.exe') (Join-Path $workspace 'scripts\test-sdspi-read.py') --vcvars $vcvars
if ($LASTEXITCODE) { throw 'SD SPI command/read/stop host regression failed.' }
& (Join-Path $workspace '.venv\Scripts\python.exe') (Join-Path $workspace 'scripts\test-native-reload.py') --vcvars $vcvars
if ($LASTEXITCODE) { throw 'Native save reload host regression failed.' }
& (Join-Path $workspace '.venv\Scripts\python.exe') (Join-Path $workspace 'scripts\test-audio-volume.py') --vcvars $vcvars
if ($LASTEXITCODE) { throw 'Audio volume host regression failed.' }
& (Join-Path $workspace '.venv\Scripts\python.exe') (Join-Path $workspace 'scripts\test-playback.py') --vcvars $vcvars
if ($LASTEXITCODE) { throw 'Playback pacing/frameskip host regression failed.' }
& (Join-Path $workspace '.venv\Scripts\python.exe') (Join-Path $workspace 'tests\console_utf8_test.py')
if ($LASTEXITCODE) { throw 'Console UTF-8 host regression failed.' }
& (Join-Path $workspace '.venv\Scripts\python.exe') (Join-Path $workspace 'scripts\flash-regba-nor.py') --self-test
if ($LASTEXITCODE) { throw 'NOR install workflow host regression failed.' }
Write-Host "ReGBA host checks passed. Artifacts: $output"
Write-Host 'Storage/PCM/framebuffer/buttons/Bluetooth/input use host RAM; battery file tests use temporary files under build/regba-host/battery; SSI uses a register model and SD uses an in-memory card. No board or physical TF access.'

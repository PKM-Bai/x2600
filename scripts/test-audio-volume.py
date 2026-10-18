"""Run production audio-volume functions with codec/RTOS register stubs only."""

import argparse
import os
from pathlib import Path
import re
import subprocess


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=root / "build/regba-host/audio-volume")
    parser.add_argument("--vcvars", type=Path,
                        default=Path("C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat"))
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    source = (root / "projects/halley7m-regba/platform/audio.c").read_text(encoding="utf-8")
    template = (root / "tests/audio_volume_test.c.in").read_text(encoding="utf-8")

    def one(pattern):
        matches = list(re.finditer(pattern, source, re.MULTILINE | re.DOTALL))
        if len(matches) != 1:
            raise RuntimeError("Expected one production audio match: " + pattern)
        return matches[0][0]

    names = ("volume_gain", "apply_output_state", "audio_set_volume", "audio_get_volume",
             "audio_set_muted", "audio_start", "audio_stop", "audio_get_stats")
    replacements = {
        "@AUDIO_VOLUME_STATE@": one(r"^static unsigned volume_percent = [0-9]+u;$"),
        "@AUDIO_VOLUME_FUNCTIONS@": "\n\n".join(one(
            r"^(?:static )?(?:unsigned|int|void) " + name + r"\([^\n]*\)\n\{.*?^}") for name in names),
        "@AUDIO_INIT_GAIN@": one(r"^    codec\.Playback\.dgain = volume_gain\(audio_get_volume\(\)\);$"),
        "@AUDIO_INIT_PUBLISH@": one(r"^    /\* A task may change volume.*?^    taskEXIT_CRITICAL\(\);"),
    }
    for marker, code in replacements.items():
        if template.count(marker) != 1:
            raise RuntimeError("Expected one fixture marker: " + marker)
        template = template.replace(marker, code)
    generated = output / "audio_volume_test.c"
    generated.write_text(template, encoding="utf-8")
    if not args.vcvars.is_file():
        raise RuntimeError("MSVC environment script missing: " + str(args.vcvars))
    platform = root / "projects/halley7m-regba/platform"
    executable = output / "audio_volume_test.exe"
    commands = ["@echo off", f'call "{args.vcvars}" >nul', "if errorlevel 1 exit /b 1",
                f'cl /nologo /std:c11 /utf-8 /O2 /W4 /WX /I"{platform}" "{generated}" '
                f'/Fo"{output / "audio_volume_test.obj"}" /Fe"{executable}"',
                "if errorlevel 1 exit /b 1", f'"{executable}"', "exit /b %errorlevel%"]
    runner = output / "run.cmd"
    runner.write_text("\r\n".join(commands) + "\r\n", encoding="utf-8")
    subprocess.run([os.environ.get("COMSPEC", "cmd.exe"), "/d", "/s", "/c", str(runner)],
                   check=True, cwd=root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

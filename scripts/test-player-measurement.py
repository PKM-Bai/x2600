#!/usr/bin/env python3
"""Test actual RAM recorder with virtual source snapshots/time/UART only."""
import importlib.util
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    spec = importlib.util.spec_from_file_location("amp_test_support", ROOT / "scripts/test-player-amp.py")
    support = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(support)
    environment = support.msvc_environment()
    compiler = shutil.which("cl", path=environment.get("Path", environment.get("PATH"))) or "cl"
    output = ROOT / "build/player-host/measurement"
    output.mkdir(parents=True, exist_ok=True)
    source = ROOT / "projects/halley7m-player"
    for capacity in (None, 3):
        executable = output / ("measurement-full.exe" if capacity is None else "measurement-small.exe")
        command = [compiler, "/nologo", "/std:c11", "/O2", "/W4", "/WX", "/wd4324", "/wd4505", "/utf-8",
            "/D_CRT_SECURE_NO_WARNINGS", "/FI" + str(ROOT / "tests/player_measurement_stubs/measurement_output.h"),
            "/I" + str(ROOT / "tests/player_measurement_stubs"), "/I" + str(source / "common"),
            "/I" + str(source / "mips"), str(ROOT / "tests/player_measurement_test.c"),
            str(source / "mips/player_measurement.c"), "/Fe:" + str(executable)]
        if capacity is not None:
            command.append(f"/DPLAYER_MEASUREMENT_TEST_CAPACITY={capacity}")
        subprocess.run(command, cwd=output, env=environment, check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()

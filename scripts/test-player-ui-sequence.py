"""Exercise deterministic validation keys through the real input recognizer."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    output = ROOT / "build/player-host/ui-sequence"
    output.mkdir(parents=True, exist_ok=True)
    includes = [ROOT / "projects/halley7m-player" / folder for folder in ("mips", "common", "gui")]
    sources = [ROOT / "tests/player_ui_sequence_test.c", includes[0] / "player_ui_sequence.c",
               includes[1] / "player_input.c"]
    executable = output / ("ui_sequence_test.exe" if os.name == "nt" else "ui_sequence_test")
    if os.name == "nt":
        spec = importlib.util.spec_from_file_location("player_amp_test", ROOT / "scripts/test-player-amp.py")
        helper = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(helper)
        environment = helper.msvc_environment()
        compiler = shutil.which("cl", path=environment.get("Path", environment.get("PATH"))) or "cl"
        command = [compiler, "/nologo", "/std:c11", "/W4", "/WX", "/O2", "/utf-8",
                   *["/I" + str(path) for path in includes], *map(str, sources), "/Fe:" + str(executable)]
    else:
        environment = None
        command = ["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                   *["-I" + str(path) for path in includes], *map(str, sources), "-o", str(executable)]
    subprocess.run(command, cwd=output, env=environment, check=True)
    subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()

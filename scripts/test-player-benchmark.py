"""Run the real TF benchmark with fake read-only storage/OST, never hardware."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    spec = importlib.util.spec_from_file_location("player_amp_test", ROOT / "scripts/test-player-amp.py")
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    folder = ROOT / "build/player-host/benchmark"
    folder.mkdir(parents=True, exist_ok=True)
    production = (ROOT / "projects/halley7m-player/mips/player_probe.c").read_text(encoding="utf-8")
    begin = production.index("/* TF_BENCHMARK_BEGIN:")
    end = production.index("/* TF_BENCHMARK_END */", begin)
    test = (ROOT / "tests/player_benchmark_test.c").read_text(encoding="utf-8")
    source = folder / "benchmark_test.c"
    source.write_text(test.replace("/* BENCHMARK_PRODUCTION */", production[begin:end]), encoding="utf-8")
    executable = folder / ("benchmark_test.exe" if os.name == "nt" else "benchmark_test")
    includes = [ROOT / "projects/halley7m-player/mips", ROOT / "projects/halley7m-player/common"]
    if os.name == "nt":
        environment = helper.msvc_environment()
        compiler = shutil.which("cl", path=environment.get("Path", environment.get("PATH"))) or "cl"
        command = [compiler, "/nologo", "/std:c11", "/W4", "/WX", "/O2", "/utf-8",
                   "/D_CRT_SECURE_NO_WARNINGS", *["/I" + str(path) for path in includes],
                   str(source), "/Fe:" + str(executable)]
    else:
        environment = None
        command = ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O2",
                   *["-I" + str(path) for path in includes], str(source), "-o", str(executable)]
    subprocess.run(command, cwd=folder, env=environment, check=True)
    subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()

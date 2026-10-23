"""Test the board entropy service with mocks, optionally compile the MIPS driver."""
import argparse
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[4]
NETWORK = ROOT / "projects/halley7m-ai/network"


def host_test():
    output = ROOT / "build/ai-entropy-host"
    output.mkdir(parents=True, exist_ok=True)
    executable = output / ("board_entropy_test.exe" if os.name == "nt" else "board_entropy_test")
    sources = [NETWORK / "board_entropy.c", NETWORK / "tests/board_entropy_test.c"]
    includes = [NETWORK / "tests/entropy-stubs", NETWORK]
    env = None
    if os.name == "nt":
        spec = importlib.util.spec_from_file_location("test_support", ROOT / "scripts/test-player-amp.py")
        support = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(support)
        env = support.msvc_environment()
        compiler = shutil.which("cl", path=env.get("Path", env.get("PATH")))
        command = [compiler, "/nologo", "/std:c11", "/O2", "/W4", "/WX", "/utf-8",
                   "/DAI_BOARD_ENTROPY_TEST=1", *["/I" + str(p) for p in includes],
                   *map(str, sources), "/Fe:" + str(executable)]
    else:
        command = ["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                   "-DAI_BOARD_ENTROPY_TEST=1", *["-I" + str(p) for p in includes],
                   *map(str, sources), "-o", str(executable)]
    subprocess.run(command, cwd=output, env=env, check=True)
    # Each case is a fresh process: a latched hardware fault has no test-only reset API.
    for case in ["startup", "cadence", "repeat", "partial", "timeout", "startup-timeout",
                 "startup-repeat", "busy", "lock-timeout", "unavailable", "tls"]:
        subprocess.run([str(executable), case], check=True)


def mips_compile():
    compiler = next((ROOT / "toolchains/xburst2-windows").rglob("mips-sde-elf-gcc.exe"))
    output = ROOT / "build/ai-entropy-mips"
    output.mkdir(parents=True, exist_ok=True)
    includes = [NETWORK, ROOT / "projects/halley7m-player/mips/include",
                ROOT / "projects/halley7m-player/mips/port",
                ROOT / "libbare-cpu/Middlewares/FreeRTOS-Kernel/include",
                ROOT / "libbare-cpu/cpu/core/include",
                ROOT / "libbare-cpu/cpu/soc-x2600/include",
                ROOT / "libbare-cpu/drivers/drivers-x2600/include"]
    flags = ["-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
             "-mips32r2", "-mabi=32", "-mfp32", "-mhard-float", "-EL", "-mno-abicalls",
             "-fno-pic", "-G0", "-ffunction-sections", "-fdata-sections",
             "-DUSE_NEWLIB=1", "-D__SDE_MIPS_=1"]
    include_flags = ["-I" + str(p) for p in includes[:3]]
    for path in includes[3:]:
        include_flags.extend(["-isystem", str(path)])
    subprocess.run([str(compiler), *flags, *include_flags, "-c", str(NETWORK / "board_entropy.c"),
                    "-o", str(output / "board_entropy.o")], check=True)
    print("MIPS compile PASS: board_entropy.c (real MMIO/FreeRTOS headers, not hardware execution)")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mips", action="store_true")
    args = parser.parse_args()
    host_test()
    if args.mips:
        mips_compile()

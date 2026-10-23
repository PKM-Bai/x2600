"""Host network policy tests and compile-only native FreeRTOS/lwIP checks."""
import argparse
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[4]
NETWORK = ROOT / "projects/halley7m-ai/network"


def host_test(name, sources):
    output = ROOT / "build/ai-network-host"
    output.mkdir(parents=True, exist_ok=True)
    executable = output / (name + (".exe" if os.name == "nt" else ""))
    env = None
    if os.name == "nt":
        spec = importlib.util.spec_from_file_location("test_support", ROOT / "scripts/test-player-amp.py")
        support = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(support)
        env = support.msvc_environment()
        compiler = shutil.which("cl", path=env.get("Path", env.get("PATH")))
        command = [compiler, "/nologo", "/std:c11", "/O2", "/W4", "/WX", "/utf-8",
                   "/I" + str(NETWORK), *map(str, sources), "/Fe:" + str(executable)]
    else:
        command = ["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                   "-I" + str(NETWORK), *map(str, sources), "-o", str(executable)]
    subprocess.run(command, cwd=output, env=env, check=True)
    subprocess.run([str(executable)], check=True)


def mips_compile(dependencies):
    compiler = next((ROOT / "toolchains/xburst2-windows").rglob("*gcc.exe"))
    output = ROOT / "build/ai-network-mips"
    output.mkdir(parents=True, exist_ok=True)
    lwip = dependencies.resolve() / "lwip/src"
    includes = [NETWORK, lwip / "include",
                ROOT / "projects/halley7m-player/mips/include",
                ROOT / "projects/halley7m-player/mips/port",
                ROOT / "libbare-cpu/Middlewares/FreeRTOS-Kernel/include",
                ROOT / "libbare-cpu/cpu/core/include",
                ROOT / "libbare-cpu/cpu/soc-x2600/include",
                ROOT / "libbare-cpu/drivers/drivers-x2600/include"]
    sources = [NETWORK / "sys_arch.c", NETWORK / "ai_network.c",
               NETWORK / "board_rng.c", NETWORK / "board_gmac.c",
               *sorted((lwip / "core").glob("*.c")),
               *sorted((lwip / "core/ipv4").glob("*.c")),
               *sorted((lwip / "api").glob("*.c")),
               lwip / "netif/ethernet.c", lwip / "apps/sntp/sntp.c"]
    flags = ["-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
             "-Wno-unused-parameter", "-mips32r2", "-mabi=32", "-mfp32",
             "-mhard-float", "-EL", "-mno-abicalls", "-fno-pic", "-G0",
             "-ffunction-sections", "-fdata-sections", "-DUSE_NEWLIB=1", "-D__SDE_MIPS_=1"]
    for index, source in enumerate(sources):
        include_flags = ["-I" + str(p) for p in includes[:4]]
        for path in includes[4:]:
            include_flags.extend(["-isystem", str(path)])
        subprocess.run([str(compiler), *flags, *include_flags,
                        "-c", str(source), "-o", str(output / f"{index:02d}-{source.stem}.o")], check=True)
    print(f"MIPS compile PASS: {len(sources)} sources (lwIP IPv4/API/SNTP, native FreeRTOS port, board probes)")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mips", action="store_true")
    parser.add_argument("--dependencies", type=Path, default=ROOT / "build/ai-network-deps")
    args = parser.parse_args()
    host_test("network_host_test", [NETWORK / "ai_network.c", NETWORK / "tests/network_host_test.c"])
    host_test("ping_packet_test", [NETWORK / "tests/ping_packet_test.c"])
    if args.mips:
        mips_compile(args.dependencies)

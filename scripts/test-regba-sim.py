"""Build and run the generated ReGBA MIPS core without target hardware.

On Windows the bundled simulator may crash with host 0xC0000005; run this
script inside WSL with the Linux toolchain in that case. No flash is accessed.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import time


def main():
    root = Path(__file__).resolve().parents[1]
    windows = os.name == 'nt'
    default_tools = root / 'toolchains' / ('xburst2-windows' if windows else 'xburst2-linux')
    default_tools /= ('mips-ingenic-xburst2-elf-tools-x86_64-w64.r5.1.6' if windows
                      else 'mips-ingenic-xburst2-elf-tools-r5.1.6')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--core-dir', type=Path, default=root / 'build/regba-Release/regba-core')
    parser.add_argument('--output', type=Path, default=root / 'build/regba-sim')
    parser.add_argument('--toolchain', type=Path, default=default_tools)
    parser.add_argument('--timeout', type=int, default=45)
    parser.add_argument('--depth-regression', action='store_true',
                        help='Prepend 32 ARM branch blocks and require the translation depth limit')
    parser.add_argument('--build-only', action='store_true')
    args = parser.parse_args()
    core = args.core_dir.resolve()
    out = args.output.resolve()
    tools = args.toolchain.resolve()
    suffix = '.exe' if windows else ''
    gcc = tools / 'bin' / ('mips-sde-elf-gcc' + suffix)
    runner = tools / 'bin' / ('mips-sde-elf-run' + suffix)
    if not (core / 'test_resources.h').exists():
        parser.error('Build the firmware first to generate the patched core and diagnostic ROM')
    out.mkdir(parents=True, exist_ok=True)
    # Old monitor script predates .MIPS.abiflags; discard non-runtime metadata
    # so its LMA does not overlap .data. The toolchain installation stays intact.
    original = (tools / 'mips-sde-elf/lib/sde32.ld').read_text()
    script = original.replace('SECTIONS\n{',
                              'SECTIONS\n{\n  /DISCARD/ : { *(.MIPS.abiflags) }', 1)
    if script == original:
        raise RuntimeError('Unexpected sde32.ld structure')
    linker = out / 'sim.ld'
    linker.write_text(script)
    elf = out / 'regba_sim.elf'
    sources = [core / (name + '.c') for name in
               ['engine', 'video', 'input', 'bios', 'sound', 'stats', 'memory',
                'cpu_common', 'cpu_asm', 'sha1']]
    sources += [core / 'mips/stub.S', root / 'tests/regba_sim.c']
    command = [str(gcc), '-std=gnu11', '-O2', '-mips32r2', '-mabi=32',
               '-mfp32', '-mhard-float', '-EL', '-mno-abicalls', '-fno-pic',
               '-G0', '-fno-common', '-ffunction-sections', '-fdata-sections',
               '-DGCW_ZERO', '-DMIPS_XBURST', '-DLOAD_ALL_ROM', '-DMIPS_32R2',
               '-I' + str(root / 'projects/halley7m-regba/core'),
               '-I' + str(root / 'projects/halley7m-regba/platform'),
               '-I' + str(core), '-I' + str(core / 'mips'),
               '-I' + str(root / 'third_party/ReGBA/source/opendingux')]
    if args.depth_regression:
        command += ['-DREGBA_DEPTH_REGRESSION']
    command += [str(source) for source in sources]
    command += ['-T' + str(linker), '-Wl,--gc-sections', '-lm', '-o', str(elf)]
    print('Building generated ReGBA core + software-only harness', flush=True)
    subprocess.run(command, check=True, cwd=root)
    print('Linked ' + str(elf), flush=True)
    if args.build_only:
        return 0
    command = [str(runner), '--firmware', 'idt', '--memory-size', '64M',
               '--watch-clock-int', str(args.timeout * 1000), str(elf)]
    try:
        started = time.monotonic()
        result = subprocess.run(command, cwd=root, timeout=args.timeout + 5,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    except subprocess.TimeoutExpired as error:
        print((error.stdout or b'').decode(errors='replace'))
        print('FAIL: software diagnostic exceeded time limit')
        return 1
    output = result.stdout.decode(errors='replace')
    (out / 'simulator.log').write_text(output, encoding='utf-8')
    print(output, end='')
    print(f'Simulator return code: {result.returncode} '
          f'(0x{result.returncode & 0xffffffff:08x})')
    print(f'Simulator wall time: {time.monotonic() - started:.3f}s')
    passed = result.returncode == 0 and 'PASS: ARM/Thumb/SWI/IRQ/' in output
    return 0 if passed else 1


if __name__ == '__main__':
    sys.exit(main())

"""执行生产 mGBA 适配层的纯软件 MIPS 诊断，不连接开发板。

先用 -Engine mgba 构建 Release 静态库和测试资源。Windows 使用已有 WSL
内的官方 Linux 模拟器，避开 Windows run.exe 的已知宿主崩溃；无需联网。
模拟器耗时不能用于推算实板 FPS。
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time


def wsl_path(path):
    value = path.resolve().as_posix()
    if len(value) < 3 or value[1:3] != ':/':
        raise RuntimeError('WSL runner 需要本机盘符绝对路径: ' + value)
    return '/mnt/' + value[0].lower() + value[2:]


def main():
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    root = Path(__file__).resolve().parents[1]
    windows = os.name == 'nt'
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=root / 'build/mgba-Release')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--mode', choices=('all', 'cpu', 'sram', 'flash', 'eeprom', 'mirror'), default='all')
    parser.add_argument('--timeout', type=int, default=300)
    parser.add_argument('--wsl-distro', default='Ubuntu-22.04')
    parser.add_argument('--build-only', action='store_true')
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error('--timeout 必须大于 0')
    build = args.build.resolve()
    out = (args.output or root / 'build' / ('mgba-adapter-sim-' + args.mode)).resolve()
    out.mkdir(parents=True, exist_ok=True)
    linux_tools = root / 'toolchains/xburst2-linux/mips-ingenic-xburst2-elf-tools-r5.1.6'
    tools = (root / 'toolchains/xburst2-windows/mips-ingenic-xburst2-elf-tools-x86_64-w64.r5.1.6') if windows else linux_tools
    library = build / 'mgba-core/libmgba.a'
    inputs = {'library': library,
              'adapter': root / 'projects/halley7m-regba/engines/mgba/mgba_adapter.c',
              'harness': root / 'tests/mgba_sim.c',
              'test_resources': build / 'regba-core/test_resources.h'}
    input_hashes = {name + '_sha256': hashlib.sha256(path.read_bytes()).hexdigest()
                    for name, path in inputs.items()}
    entries = json.loads((build / 'compile_commands.json').read_text(encoding='utf-8'))
    entry = next(e for e in entries if e['file'].replace('\\', '/').endswith('/src/gba/core.c'))
    defines = re.findall(r'(?:^|\s)(-D\S+)', entry['command'])
    shim = out / 'include'
    shim.mkdir(exist_ok=True)
    (shim / 'FreeRTOS.h').write_text('#define taskENTER_CRITICAL() do { } while (0)\n#define taskEXIT_CRITICAL() do { } while (0)\n')
    (shim / 'task.h').write_text('/* No RTOS task is created by this software fixture. */\n')
    original = (tools / 'mips-sde-elf/lib/sde32.ld').read_text()
    linker_text = original.replace('SECTIONS\n{', 'SECTIONS\n{\n  /DISCARD/ : { *(.MIPS.abiflags) }', 1)
    if linker_text == original:
        raise RuntimeError('不识别官方 monitor 链接脚本')
    linker = out / 'sim.ld'
    linker.write_text(linker_text)
    elf = out / 'mgba_sim.elf'
    command = [str(tools / ('bin/mips-sde-elf-gcc.exe' if windows else 'bin/mips-sde-elf-gcc')),
               '-std=gnu11', '-O2', '-mips32r2', '-mabi=32', '-mfp32', '-mhard-float', '-EL',
               '-mno-abicalls', '-fno-pic', '-G0', '-fno-common', '-ffunction-sections', '-fdata-sections'] + defines
    command += ['-DMGBA_TEST_MODE=' + str(('all', 'cpu', 'sram', 'flash', 'eeprom', 'mirror').index(args.mode))]
    includes = [shim, root / 'projects/halley7m-regba/include', root / 'projects/halley7m-regba/platform',
                root / 'projects/halley7m-regba/core', root / 'third_party/mGBA/include', root / 'third_party/mGBA/src',
                build / 'mgba-core/include', build / 'regba-core']
    command += ['-I' + str(p) for p in includes]
    command += [str(root / 'tests/mgba_sim.c'), str(root / 'projects/halley7m-regba/core/resources.c'),
                str(library), '-T' + str(linker), '-Wl,--gc-sections', '-lm', '-o', str(elf)]
    compiled = subprocess.run(command, cwd=root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out / 'build.log').write_bytes(compiled.stdout)
    print(compiled.stdout.decode(errors='replace'), end='', flush=True)
    if compiled.returncode:
        return 1
    print('已链接真实 mGBA adapter + 软件 host: ' + str(elf), flush=True)
    if args.build_only:
        return 0
    runner = linux_tools / 'bin/mips-sde-elf-run'
    run = ['--firmware', 'idt', '--memory-size', '64M', '--watch-clock-int', str(args.timeout * 1000)]
    if windows:
        run = ['wsl', '-d', args.wsl_distro, '--', wsl_path(runner)] + run + [wsl_path(elf)]
    else:
        run = [str(runner)] + run + [str(elf)]
    log_path = out / 'simulator.log'
    print('正在执行 ' + args.mode + '；输出记录 ' + str(log_path), flush=True)
    started = time.monotonic()
    with log_path.open('wb', buffering=0) as log:
        try:
            result = subprocess.run(run, cwd=root, stdout=log, stderr=subprocess.STDOUT, timeout=args.timeout + 5)
            code = result.returncode
        except subprocess.TimeoutExpired:
            code = -1
            log.write(b'\nFAIL simulator timeout\n')
    output = log_path.read_bytes()
    print(output.decode(errors='replace'), end='', flush=True)
    changed = [name for name, path in inputs.items()
               if hashlib.sha256(path.read_bytes()).hexdigest() != input_hashes[name + '_sha256']]
    report = {'pass': code == 0 and b'PASS mGBA production adapter diagnostic:' in output and not changed,
              'mode': args.mode, 'return_code': code, 'host_wall_seconds': time.monotonic() - started,
              'mGBA_commit': '26b7884bc25a5933960f3cdcd98bac1ae14d42e2',
              **input_hashes, 'inputs_changed': changed,
              'elf_sha256': hashlib.sha256(elf.read_bytes()).hexdigest(),
              'log': str(log_path), 'scope': '生产适配层、软件 CPU / 原生存档；无实板 FPS 结论'}
    (out / 'result.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, ensure_ascii=False), flush=True)
    return 0 if report['pass'] else 1


if __name__ == '__main__':
    sys.exit(main())

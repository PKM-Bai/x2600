"""Test production MIPS AMP command handling with RAM/virtual CCU/time peers.

This checks production MIPS control and RISC-V PCM CRC/range logic, not real CCU/PMA,
RISC-V execution, timer calibration, serial transport or physical hardware.
"""
import importlib.util
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def replace_once(text, before, after):
    if text.count(before) != 1:
        raise RuntimeError(f'Production hardware boundary changed: {before[:80]}')
    return text.replace(before, after, 1)


def main():
    source = (ROOT / 'projects/halley7m-player/mips/amp_boot.c').read_text(encoding='utf-8')
    source = re.sub(r'^#include[^\n]*\n', '', source, flags=re.M)
    source = replace_once(source,
        'extern const unsigned char player_riscv_image_start[], player_riscv_image_end[];',
        '#define player_riscv_image_start test_payload\n#define player_riscv_image_end (test_payload + sizeof(test_payload))')
    source = replace_once(source,
        'static volatile struct player_amp_shared *const shared =\n'
        '    (volatile struct player_amp_shared *)(uintptr_t)PLAYER_SHARED_UNCACHED;',
        'static volatile struct player_amp_shared *const shared = &test_shared;')
    source = replace_once(source,
        '#define HOST_CFCR (*(volatile uint32_t *)UINT32_C(0xb2200fe0))', '#define HOST_CFCR test_cfcr')
    source = replace_once(source,
        '#define PMA_ADDR(i) (*(volatile uint32_t *)(UINT32_C(0xb2a00040) + 4u * (i)))',
        '#define PMA_ADDR(i) test_pma_address[i]')
    source = replace_once(source,
        '#define PMA_CFG(i)  (*(volatile uint32_t *)(UINT32_C(0xb2a00060) + 4u * (i)))',
        '#define PMA_CFG(i) test_pma_config[i]')
    source = replace_once(source,
        'volatile unsigned char *dst = (volatile unsigned char *)(uintptr_t)(PLAYER_RISCV_PHYS | 0xa0000000u);',
        'volatile unsigned char *dst = test_remote_image;')
    source = replace_once(source,
        'volatile unsigned char *pcm = (volatile unsigned char *)(uintptr_t)(PLAYER_SHARED_UNCACHED + PLAYER_PCM_OFFSET);',
        'volatile unsigned char *pcm = test_pcm;')
    riscv = (ROOT / 'projects/halley7m-player/riscv/main.c').read_text(encoding='utf-8')
    helpers = re.findall(r'^static int checksum_pcm\([^\n]+\n\{.*?^\}', riscv, re.M | re.S)
    if len(helpers) != 1:
        raise RuntimeError('Production RISC-V checksum helper changed')
    checksum = replace_once(helpers[0],
        'const void *bytes = (const void *)(uintptr_t)(PLAYER_SHARED_PHYS + offset);',
        'const void *bytes = test_pcm + (offset - PLAYER_PCM_OFFSET);')
    template = (ROOT / 'tests/player_amp_control_test.c').read_text(encoding='utf-8')
    text = replace_once(template, '/* @PRODUCTION_AMP_BOOT@ */', source)
    text = replace_once(text, '/* @PRODUCTION_RISCV_CHECKSUM@ */', checksum)
    output = ROOT / 'build/player-host/amp-control'
    output.mkdir(parents=True, exist_ok=True)
    generated = output / 'amp_control.c'
    generated.write_text(text, encoding='utf-8')
    spec = importlib.util.spec_from_file_location('amp_test_support', ROOT / 'scripts/test-player-amp.py')
    support = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(support)
    environment = support.msvc_environment()
    compiler = shutil.which('cl', path=environment.get('Path', environment.get('PATH'))) or 'cl'
    common = ROOT / 'projects/halley7m-player/common'
    for owner in (1, 2):
        executable = output / f'amp_control_{owner}.exe'
        subprocess.run([compiler, '/nologo', '/std:c11', '/O2', '/W4', '/WX', '/wd4324', '/utf-8',
                        '/D_CRT_SECURE_NO_WARNINGS', f'/DPLAYER_AUDIO_OWNER={owner}',
                        '/I' + str(common), str(generated), str(common / 'player_amp.c'),
                        '/Fe:' + str(executable)], cwd=output, env=environment, check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()

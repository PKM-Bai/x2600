"""Test production audio client and RV service against the real private PCM queue.

Only uncached DDR pointers, FreeRTOS time and audio hardware are replaced.
This verifies protocol/ownership logic; it does not validate DMA, IRQ or sound.
"""
import importlib.util
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def replace_once(text, before, after):
    if text.count(before) != 1:
        raise RuntimeError(f'Production audio boundary changed: {before[:80]}')
    return text.replace(before, after, 1)


def main():
    source = ROOT / 'projects/halley7m-player'
    client = (source / 'mips/player_audio_client.c').read_text(encoding='utf-8')
    client = re.sub(r'^#include[^\n]*\n', '', client, flags=re.M)
    client = replace_once(client,
        '(volatile int16_t *)(uintptr_t)(PLAYER_SHARED_UNCACHED + PLAYER_PCM_OFFSET)', 'test_pcm')
    riscv = (source / 'riscv/main.c').read_text(encoding='utf-8')
    begin, end = '/* @PRODUCTION_AUDIO_REMOTE_BEGIN@ */', '/* @PRODUCTION_AUDIO_REMOTE_END@ */'
    if riscv.count(begin) != 1 or riscv.count(end) != 1:
        raise RuntimeError('Production RV audio service markers changed')
    remote = riscv.split(begin, 1)[1].split(end, 1)[0]
    remote = replace_once(remote,
        '(const int16_t *)(uintptr_t)(PLAYER_SHARED_PHYS + offset)',
        'test_pcm + (offset - PLAYER_PCM_OFFSET) / 2u')
    remote = '#if PLAYER_AUDIO_OWNER == PLAYER_AUDIO_OWNER_RISCV\n' + remote + '\n#endif\n'
    template = (ROOT / 'tests/player_audio_client_test.c').read_text(encoding='utf-8')
    text = replace_once(template, '/* @PRODUCTION_AUDIO_REMOTE@ */', remote)
    text = replace_once(text, '/* @PRODUCTION_AUDIO_CLIENT@ */', client)
    output = ROOT / 'build/player-host/audio-client'
    output.mkdir(parents=True, exist_ok=True)
    generated = output / 'audio_client.c'
    generated.write_text(text, encoding='utf-8')
    spec = importlib.util.spec_from_file_location('amp_test_support', ROOT / 'scripts/test-player-amp.py')
    support = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(support)
    environment = support.msvc_environment()
    compiler = shutil.which('cl', path=environment.get('Path', environment.get('PATH'))) or 'cl'
    for owner in (1, 2):
        executable = output / f'audio_client_{owner}.exe'
        subprocess.run([compiler, '/nologo', '/std:c11', '/O2', '/W4', '/WX', '/wd4324', '/utf-8',
            f'/DPLAYER_AUDIO_OWNER={owner}', '/I' + str(source / 'common'), '/I' + str(source / 'audio'),
            '/I' + str(source / 'mips'), str(generated), str(source / 'common/player_amp.c'),
            str(source / 'audio/player_pcm_queue.c'), '/Fe:' + str(executable)],
            cwd=output, env=environment, check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()

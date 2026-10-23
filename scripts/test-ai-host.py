"""Build actual AI C modules and LVGL with a native compiler, then run CTest."""
import importlib.util
import os
from pathlib import Path
import subprocess
ROOT = Path(__file__).resolve().parents[1]
environment = None
if os.name == 'nt':
    spec = importlib.util.spec_from_file_location('ai_test_support', ROOT / 'scripts/test-player-amp.py')
    support = importlib.util.module_from_spec(spec); spec.loader.exec_module(support)
    environment = support.msvc_environment()
build = ROOT / 'build/ai-host'
build.mkdir(parents=True, exist_ok=True)
subprocess.run(['cmake', '-S', str(ROOT / 'projects/halley7m-ai'), '-B', str(build), '-G', 'Ninja',
                '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_TRY_COMPILE_CONFIGURATION=Release',
                '-DCMAKE_C_FLAGS_DEBUG=/Od /Z7' if os.name == 'nt' else '-DCMAKE_C_FLAGS_DEBUG=-g'], env=environment, check=True)
build_log = build / 'build.log'
with build_log.open('w', encoding='utf-8') as log:
    result = subprocess.run(['cmake', '--build', str(build), '--parallel', '4'], env=environment,
                            stdout=log, stderr=subprocess.STDOUT)
if result.returncode:
    print('\n'.join(build_log.read_text('utf-8', errors='replace').splitlines()[-60:]))
    raise SystemExit(result.returncode)
print('Build log:', build_log)
subprocess.run(['ctest', '--test-dir', str(build), '--output-on-failure'], env=environment, check=True)

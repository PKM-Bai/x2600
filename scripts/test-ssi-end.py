"""Run SSI completion fixtures on original (RED) and derived (GREEN) code."""

import argparse
import importlib.util
import os
from pathlib import Path
import subprocess
import sys


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=root / "build/regba-host/ssi")
    parser.add_argument("--vcvars", type=Path,
                        default=Path("C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat"))
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    sdk = root / "libbare-cpu"
    generated = output / "ssi-sdk"
    subprocess.run([sys.executable, str(root / "scripts/prepare-ssi.py"),
                    "--sdk", str(sdk), "--output", str(generated)], check=True)
    spec = importlib.util.spec_from_file_location("ssi_end_prepare", root / "scripts/prepare-ssi.py")
    prepare = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(prepare)
    template = (root / "tests/ssi_end_test.c.in").read_text(encoding="utf-8")
    if template.count("@SSI_FUNCTION@") != 1:
        raise RuntimeError("SSI fixture must contain exactly one function marker")
    sources = {"original": sdk / "drivers/drivers-x2600/src/x2600_hal_ssi.c",
               "generated": generated / "x2600_hal_ssi.c"}
    fixtures = {}
    for name, source in sources.items():
        functions = list(prepare.FUNCTION.finditer(source.read_text(encoding="utf-8")))
        if len(functions) != 1:
            raise RuntimeError("Expected exactly one polling SSI function in " + str(source))
        fixtures[name] = functions[0][0]
    if fixtures["generated"].count(prepare.FINAL_UNDERRUN_CLEAR) != 1:
        raise RuntimeError("Expected exactly one final underrun clear in generated function")
    # This is the G production function: all earlier fixes remain, while only
    # H's final underrun clear is removed as the targeted negative control.
    fixtures["g_without_final_clear"] = fixtures["generated"].replace(
        prepare.FINAL_UNDERRUN_CLEAR, "", 1)
    if fixtures["generated"].count(prepare.TX_WINDOW) != 1:
        raise RuntimeError("Expected exactly one TX window in generated function")
    fixtures["without_tx_window"] = fixtures["generated"].replace(
        prepare.TX_WINDOW, prepare.TX_POLL, 1)
    if fixtures["generated"].count("regba_ssi_poll_tick()") != 3:
        raise RuntimeError("Expected exactly three SSI task-clock reads")
    fixtures["without_task_tick"] = fixtures["generated"].replace(
        "regba_ssi_poll_tick()", "HAL_GetTick()")
    for name, function in fixtures.items():
        (output / (name + ".c")).write_text(
            template.replace("@SSI_FUNCTION@", function), encoding="utf-8")
    if not args.vcvars.is_file():
        raise RuntimeError("MSVC environment script missing: " + str(args.vcvars))
    commands = ["@echo off", f'call "{args.vcvars}" >nul', "if errorlevel 1 exit /b 1"]
    for name in fixtures:
        # These two negative controls retain the SDK's uint64 clock assigned
        # to uint32 tickstart. Keep that exact production conversion; derived
        # task-clock fixtures compile without this legacy-warning exception.
        legacy_warning = " /wd4244" if name in ("original", "without_task_tick") else ""
        commands += [f'cl /nologo /std:c11 /utf-8 /O2 /W4 /WX{legacy_warning} "{output / (name + ".c")}" '
                     f'/Fo"{output / (name + ".obj")}" /Fe"{output / (name + ".exe")}"',
                     "if errorlevel 1 exit /b 1"]
    runner = output / "compile.cmd"
    runner.write_text("\r\n".join(commands) + "\r\n", encoding="utf-8")
    subprocess.run([os.environ.get("COMSPEC", "cmd.exe"), "/d", "/s", "/c", str(runner)],
                   check=True, cwd=root)
    before = subprocess.run([str(output / "original.exe")], text=True, capture_output=True)
    print("Original SDK (expected RED): " + before.stdout.strip(), flush=True)
    if before.returncode != 1 or "normal cleanup preceded END" not in before.stdout:
        raise RuntimeError("Original SDK did not fail the expected completion-ordering assertion")
    after = subprocess.run([str(output / "generated.exe")], text=True, capture_output=True)
    print("Generated SDK (expected GREEN): " + after.stdout.strip(), flush=True)
    if after.returncode != 0 or "PASS: generated SSI polling function" not in after.stdout:
        raise RuntimeError("Generated SSI fixture failed: " + after.stderr)
    before_deadline = subprocess.run([str(output / "original.exe"), "deadline"],
                                     text=True, capture_output=True)
    print("Original deadline (expected RED): " + before_deadline.stdout.strip(), flush=True)
    if (before_deadline.returncode != 1 or
            "completed transfer rejected at expired deadline" not in before_deadline.stdout):
        raise RuntimeError("Original SDK did not reproduce the completed-transfer timeout")
    after_deadline = subprocess.run([str(output / "generated.exe"), "deadline"],
                                    text=True, capture_output=True)
    print("Generated deadline (expected GREEN): " + after_deadline.stdout.strip(), flush=True)
    if (after_deadline.returncode != 0 or
            "PASS: SSI deadline" not in after_deadline.stdout):
        raise RuntimeError("Generated SSI deadline fixture failed: " + after_deadline.stderr)
    before_finish = subprocess.run([str(output / "original.exe"), "finish"],
                                   text=True, capture_output=True)
    print("Original FINISH (expected RED): " + before_finish.stdout.strip(), flush=True)
    if before_finish.returncode != 1 or "FINISH preceded final RX" not in before_finish.stdout:
        raise RuntimeError("Original SDK did not show FINISH preceding final RX")
    after_finish = subprocess.run([str(output / "generated.exe"), "finish"],
                                  text=True, capture_output=True)
    print("Generated FINISH (expected GREEN): " + after_finish.stdout.strip(), flush=True)
    if after_finish.returncode != 0 or "PASS: SSI deferred FINISH" not in after_finish.stdout:
        raise RuntimeError("Generated SSI FINISH fixture failed: " + after_finish.stderr)
    before_underrun = subprocess.run([str(output / "g_without_final_clear.exe"), "underrun"],
                                     text=True, capture_output=True)
    print("G final underrun (expected RED): " + before_underrun.stdout.strip(), flush=True)
    if before_underrun.returncode != 1 or "final underrun prevented END" not in before_underrun.stdout:
        raise RuntimeError("G negative control did not reproduce final underrun END timeout")
    after_underrun = subprocess.run([str(output / "generated.exe"), "underrun"],
                                    text=True, capture_output=True)
    print("H final underrun (expected GREEN): " + after_underrun.stdout.strip(), flush=True)
    if after_underrun.returncode != 0 or "PASS: SSI final underrun" not in after_underrun.stdout:
        raise RuntimeError("H final underrun fixture failed: " + after_underrun.stderr)
    before_window = subprocess.run([str(output / "without_tx_window.exe"), "window"],
                                   text=True, capture_output=True)
    print("Unbounded TX window (expected RED): " + before_window.stdout.strip(), flush=True)
    if (before_window.returncode != 1 or
            "RX FIFO overrun under task preemption" not in before_window.stdout):
        raise RuntimeError("Unbounded polling did not reproduce the 512-byte RX FIFO overrun")
    after_window = subprocess.run([str(output / "generated.exe"), "window"],
                                  text=True, capture_output=True)
    print("Bounded TX window (expected GREEN): " + after_window.stdout.strip(), flush=True)
    if after_window.returncode != 0 or "PASS: SSI 512-byte window" not in after_window.stdout:
        raise RuntimeError("Bounded polling FIFO fixture failed: " + after_window.stderr)
    before_wrap = subprocess.run([str(output / "without_task_tick.exe"), "wrap"],
                                 text=True, capture_output=True)
    print("64-bit SDK clock (expected RED): " + before_wrap.stdout.strip(), flush=True)
    if before_wrap.returncode != 1 or "wrapped END completion failed" not in before_wrap.stdout:
        raise RuntimeError("SDK clock negative control did not reject the wrapped uptime")
    after_wrap = subprocess.run([str(output / "generated.exe"), "wrap"],
                                text=True, capture_output=True)
    print("32-bit task clock (expected GREEN): " + after_wrap.stdout.strip(), flush=True)
    if after_wrap.returncode != 0 or "PASS: SSI uint32 task tick" not in after_wrap.stdout:
        raise RuntimeError("SSI task-clock wrap fixture failed: " + after_wrap.stderr)
    # The only patched function must also reject ambiguous source matches.
    original = sources["original"].read_text(encoding="utf-8")
    for changed in (original.replace("HAL_SPI_TransmitReceive(", "HAL_SPI_Unexpected(", 1),
                    original.replace(fixtures["original"], fixtures["original"].replace(
                        prepare.TX_POLL, "        if (0) {\n", 1), 1),
                    original + "\n" + prepare.FUNCTION.search(original)[0]):
        try:
            prepare.derive_ssi(changed)
        except RuntimeError:
            continue
        raise RuntimeError("SSI generator accepted an absent or duplicate function")
    print("PASS: SSI generator rejects absent/ambiguous patches; RED/GREEN use the same production-function fixture", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

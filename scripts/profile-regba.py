"""Measure per-task CPU use while ReGBA runs with LCD and muted PCM/DMA."""
import argparse
from datetime import datetime
import importlib.util
import json
from pathlib import Path
import re
import sys
import time

import serial


_spec = importlib.util.spec_from_file_location(
    "regba_soak_support", Path(__file__).with_name("soak-regba.py"))
_soak = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_soak)
Console, require, check_running = _soak.Console, _soak.require, _soak.check_running


def read_profile(console, duration):
    console.send(f"cpu {duration}")
    console.wait(rf"^cpu sampling seconds={duration}$", 5)
    started = time.monotonic()
    summary, tasks, host_seconds = None, [], None
    for line in console.lines(duration + 10):
        match = re.fullmatch(
            r"cpu window_us=(\d+) busy_pct=([\d.]+) idle_pct=([\d.]+) "
            r"accounted_pct=([\d.]+) tasks=(\d+)", line)
        if match:
            require(summary is None, "duplicate CPU window summary")
            host_seconds = time.monotonic() - started
            summary = dict(window_us=int(match[1]), busy_pct=float(match[2]),
                           idle_pct=float(match[3]), accounted_pct=float(match[4]),
                           task_count=int(match[5]))
            print(line, flush=True)
        match = re.fullmatch(
            r"cpu_task name=(.*?) priority=(\d+) runtime_us=(\d+) "
            r"cpu_pct=([\d.]+) stack_free_bytes=(\d+)", line)
        if match:
            tasks.append(dict(name=match[1], priority=int(match[2]), runtime_us=int(match[3]),
                              cpu_pct=float(match[4]), stack_free_bytes=int(match[5])))
            print(line, flush=True)
        if line == "cpu done":
            require(summary is not None, "CPU report ended without summary")
            summary["tasks"] = tasks
            summary["host_seconds"] = host_seconds
            validate_profile(summary, duration)
            return summary
    raise RuntimeError("CPU report incomplete or timed out")


def validate_profile(profile, duration):
    board_seconds = profile["window_us"] / 1_000_000
    require(abs(board_seconds - duration) <= duration * 0.02 + 0.1,
            f"CPU window differs from requested duration: {board_seconds:.6f}s")
    require(abs(board_seconds - profile["host_seconds"]) <= duration * 0.02 + 0.2,
            f"CPU board/host time mismatch: board={board_seconds:.6f}s "
            f"host={profile['host_seconds']:.6f}s")
    tasks = profile["tasks"]
    require(len(tasks) == profile["task_count"] and len(tasks) > 0,
            "CPU task count does not match report")
    require(len({task["name"] for task in tasks}) == len(tasks), "duplicate CPU task names")
    require(abs(profile["busy_pct"] + profile["idle_pct"] - 100) <= 0.02,
            "busy and idle percentages do not sum to 100")
    require(98 <= profile["accounted_pct"] <= 102, "CPU accounting is outside 98..102 percent")
    idle = [task for task in tasks if task["name"].lower() == "idle"]
    require(len(idle) == 1, "CPU report must include one Idle task")
    require(abs(idle[0]["cpu_pct"] - profile["idle_pct"]) <= 0.02,
            "Idle task and summary percentages differ")
    for task in tasks:
        expected = task["runtime_us"] * 100 / profile["window_us"]
        require(abs(expected - task["cpu_pct"]) <= 0.011,
                f"task {task['name']} percentage does not match its runtime")
        require(task["stack_free_bytes"] > 0, f"task {task['name']} has no unused stack")
    task_sum = sum(task["cpu_pct"] for task in tasks)
    require(abs(task_sum - 100) <= 2.0 + len(tasks) * 0.01,
            f"task CPU percentages do not approximately sum to 100: {task_sum:.2f}")
    accounted = sum(task["runtime_us"] for task in tasks) * 100 / profile["window_us"]
    require(abs(accounted - profile["accounted_pct"]) <= 0.011,
            "accounted percentage does not match task runtimes")


def run_profile(console, args):
    state = console.status()
    require(state["core"].get("running") == 0 and state["audio"].get("running") == 0,
            "start from stopped emulator and audio")
    console.send("audio silent")
    state = console.status()
    require(state["audio"].get("muted") == 1 and state["audio"].get("running") == 0,
            "muted output not confirmed before ROM load")
    if args.skip_load:
        print("Reusing stopped ROM; caller must confirm its identity", flush=True)
    else:
        console.send("load " + args.rom)
        loaded = console.wait(r"load result=(-?\d+)", args.load_timeout)
        require(int(loaded[1]) == 0, "ROM load failed")
    console.send("run 0")
    # Allow the ROM's opening screens and audio initialization to settle.
    console.idle(10)
    if args.press_start:
        console.send("keys 008")
        console.idle(0.1)
        console.send("keys 000")
        console.idle(2.5)
    before = console.status()
    check_running(before, require_display=True)
    profile = read_profile(console, args.duration)
    after = console.status()
    check_running(after, before, require_display=True)
    seconds = (after["clock"]["time_us"] - before["clock"]["time_us"]) / 1_000_000
    profile["observation_seconds"] = seconds
    profile["gba_fps"] = (after["core"]["frames"] - before["core"]["frames"]) / seconds
    profile["lcd_hz"] = (after["lcd"]["vblank"] - before["lcd"]["vblank"]) / seconds
    profile["display_present_fps"] = (after["lcd"]["presented"] - before["lcd"]["presented"]) / seconds
    profile["before"] = before
    profile["after"] = after
    print(f"GBA={profile['gba_fps']:.5f}fps LCD={profile['lcd_hz']:.5f}Hz "
          f"heap_free={after['clock']['heap_free']} bytes", flush=True)
    return profile


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM52")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--duration", type=int, default=60, help="CPU sample seconds (1..300)")
    parser.add_argument("--rom", default="/gba_game/test4.gba")
    parser.add_argument("--load-timeout", type=float, default=600)
    parser.add_argument("--skip-load", action="store_true", help="Reuse the stopped, already loaded ROM")
    parser.add_argument("--press-start", action="store_true", help="Press Start for 100ms before sampling")
    parser.add_argument("--log", type=Path)
    parser.add_argument("--json", type=Path, help="JSON results path (default: log path with .json suffix)")
    args = parser.parse_args()
    if not 1 <= args.duration <= 300:
        parser.error("--duration must be 1..300 seconds")
    if not 0 < args.load_timeout < float("inf"):
        parser.error("--load-timeout must be finite and positive")
    try:
        rom_bytes = args.rom.encode("utf-8")
    except UnicodeEncodeError:
        parser.error("--rom must be a valid UTF-8 console path")
    if not args.rom.strip() or "\r" in args.rom or "\n" in args.rom or len(rom_bytes) > 500:
        parser.error("--rom must be a nonempty single-line UTF-8 path of at most 500 bytes")
    root = Path(__file__).resolve().parent.parent
    path = args.log or root / "build" / ("regba-cpu-" + datetime.now().strftime("%Y%m%d-%H%M%S") + ".log")
    json_path = args.json or path.with_suffix(".json")
    path.parent.mkdir(parents=True, exist_ok=True)
    json_path.parent.mkdir(parents=True, exist_ok=True)
    uart = serial.Serial()
    uart.port, uart.baudrate, uart.timeout = args.port, args.baud, 0.1
    uart.dtr = uart.rts = False
    result = {"timestamp": datetime.now().astimezone().isoformat(), "rom": args.rom,
              "reused_rom": args.skip_load, "requested_seconds": args.duration, "pass": False,
              "accounting": "FreeRTOS task runtime; ISR time is charged to the interrupted task"}
    with path.open("ab") as output:
        console = Console(uart, output)
        try:
            uart.open()
            print(f"UART {args.port} {args.baud}; raw log={path}", flush=True)
            console.idle(0.2)
            result["profile"] = run_profile(console, args)
            result["pass"] = True
        except (Exception, KeyboardInterrupt) as error:
            result["error"] = f"{type(error).__name__}: {error}"
        finally:
            if uart.is_open:
                console.ignore_failures = True
                try:
                    console.send("stop")
                    console.send("audio off")
                    if args.press_start:
                        console.send("keys 000")
                    console.idle(1)
                    stopped = console.status()
                    result["stopped"] = stopped
                    require(stopped["core"].get("running") == 0 and
                            stopped["audio"].get("running") == 0 and
                            stopped["audio"].get("muted") == 1,
                            "core/audio stop and mute were not confirmed")
                except Exception as error:
                    result["pass"] = False
                    result["stop_error"] = f"{type(error).__name__}: {error}"
                finally:
                    uart.close()
        message = "CPU PROFILE " + ("PASS" if result["pass"] else "FAIL")
        console.record(("\n[host " + message + "]\n").encode())
        print(message + f"; JSON={json_path}", flush=True)
        for key in ("error", "stop_error"):
            if key in result:
                print(result[key], flush=True)
    json_path.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

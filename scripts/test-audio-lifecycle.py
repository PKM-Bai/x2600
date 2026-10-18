"""Verify silent audio cancellation, off/resume, and rapid off/silent switching.

Start from an already mounted TF card, active LCD, and stopped core/audio.
Only audio silent/off are sent: no audio on, tone, reset, or NOR commands.
The selected game's normal battery-save policy remains active. All exits try
to stop the emulator, flush its native save, and leave audio stopped/muted.
"""

import argparse
from datetime import datetime
import importlib.util
import json
from pathlib import Path
import sys
import time

import serial


_spec = importlib.util.spec_from_file_location(
    "audio_lifecycle_save_support", Path(__file__).with_name("test-save-roundtrip.py"))
_save = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_save)
Console, require = _save.Console, _save.require


def check_common(state):
    require(state["audio"].get("muted") == 1, "audio output is not muted")
    require(state["lcd"].get("active") == 1, "LCD must remain enabled")
    require(state["lcd"].get("underrun") == 0,
            f"LCD underrun: {state['lcd']}")
    require(state["save"].get("errors") == 0,
            f"battery-save error: {state['save']}")
    for counter in ("queue_under", "FIFO_under", "errors", "timeouts"):
        require(state["audio"].get(counter) == 0,
                f"audio {counter}={state['audio'].get(counter)}; "
                f"timing={state.get('audio_timing')}")


def check_playing(state, previous=None):
    check_common(state)
    require(state["core"].get("running") == 1 and
            state["core"].get("audio_requested") == 1 and
            state["audio"].get("running") == 1,
            f"requested audio did not resume: core={state['core']} "
            f"audio={state['audio']} timing={state.get('audio_timing')}")
    for counter in ("in", "out", "DMA_done"):
        require(state["audio"].get(counter, 0) > 0,
                f"resumed audio has no {counter} activity")
    if previous:
        for group, counter in (("clock", "time_us"), ("core", "frames"),
                               ("audio", "in"), ("audio", "out"),
                               ("audio", "DMA_done"), ("lcd", "vblank"),
                               ("lcd", "presented")):
            require(state[group].get(counter, 0) > previous[group].get(counter, 0),
                    f"{group}.{counter} did not advance after resume")
        require(state["audio"].get("nonzero", 0) >= previous["audio"].get("nonzero", 0),
                "nonzero PCM counter went backwards within a running stream")


def check_audio_off(state, previous=None):
    check_common(state)
    require(state["core"].get("running") == 1 and
            state["core"].get("audio_requested") == 0 and
            state["audio"].get("running") == 0 and
            state["audio"].get("queued") == 0,
            f"audio off/cancellation did not persist: core={state['core']} "
            f"audio={state['audio']} timing={state.get('audio_timing')}")
    if previous:
        require(state["core"].get("frames", 0) > previous["core"].get("frames", 0),
                "audio off also stopped guest-frame progress")


def send_off_silent(console):
    # One write puts both commands back-to-back on the wire. Whether a frame
    # boundary falls between their handling is determined by board scheduling.
    console.record(("\n[host " + datetime.now().isoformat(timespec="milliseconds") +
                    ": audio off + audio silent, one UART write]\n").encode())
    console.uart.write(b"audio off\raudio silent\r")
    console.uart.flush()


def run_tests(console, args, report):
    started = time.monotonic()
    observations = report["observations"] = []

    def sample(label):
        state = console.status()
        observations.append({"label": label, "host_elapsed_s": time.monotonic() - started,
                             "state": state})
        # Store the complete state before validation, preserving timing on FAIL.
        return state

    def resumed(label, delay=0.7):
        console.idle(delay)
        before = sample(label)
        check_playing(before)
        console.idle(0.15)
        after = sample(label + "_progress")
        check_playing(after, before)
        print(f"AUDIO {label} PASS DMA={after['audio']['DMA_done']} "
              f"nonzero={after['audio'].get('nonzero', 0)} "
              f"timing={after['audio_timing']}", flush=True)
        return after

    initial = sample("initial")
    require(initial["core"].get("running") == 0 and
            initial["audio"].get("running") == 0 and _save.idle_backup(initial),
            "start from stopped core/audio and a flushed, idle native save")
    require(initial["lcd"].get("active") == 1, "LCD must already be enabled")
    console.send("keys 000")
    console.send("stop")
    console.send("audio silent")
    console.send("audio off")
    # Stop/before-run one-off diagnostics share the UART with console output.
    console.idle(0.3)
    ready = sample("stopped_muted")
    check_common(ready)
    require(ready["core"].get("running") == 0 and
            ready["audio"].get("running") == 0 and _save.idle_backup(ready),
            "stopped and muted precondition changed")
    if args.skip_load:
        print("Reusing the stopped ROM; its identity is supplied by the caller", flush=True)
    else:
        print(f"Loading {args.rom}; timeout={args.load_timeout:g}s", flush=True)
        console.send("load " + args.rom)
        loaded = console.wait(r"load result=(-?\d+)", args.load_timeout)
        require(int(loaded[1]) == 0, "ROM load failed")

    # Cancel the first startup before its 300 ms settling interval can end.
    # A first-use codec initialization may still be in progress at this point;
    # the later initialized-codec scenario separately exercises settling only.
    console.send("audio silent")
    console.send("run 0")
    console.idle(0.1)
    console.send("audio off")
    console.idle(0.5)
    cancelled = sample("initial_start_cancelled")
    check_audio_off(cancelled)
    require(cancelled["core"].get("frames", 0) > 0, "guest did not start")
    console.send("audio silent")
    current = resumed("after_initial_cancel", 0.6)

    for number in range(1, args.rounds + 1):
        console.send("audio off")
        console.idle(0.1)
        stopped = sample(f"off_{number}")
        check_audio_off(stopped, current)
        console.send("audio silent")
        current = resumed(f"normal_resume_{number}")

        send_off_silent(console)
        current = resumed(f"back_to_back_resume_{number}")

    # Cancel a known initialized-codec start, then immediately request silent
    # playback again. The cancelled start must not clear that newer request.
    console.send("audio off")
    console.idle(0.1)
    stopped = sample("before_settling_latest_request")
    check_audio_off(stopped, current)
    console.send("audio silent")
    console.idle(0.1)
    send_off_silent(console)
    current = resumed("settling_latest_request", 0.8)

    stopped = _save.stop_core(console, args.operation_timeout)
    observations.append({"label": "stop_with_save_flush",
                         "host_elapsed_s": time.monotonic() - started, "state": stopped})
    check_common(stopped)
    require(stopped["audio"].get("running") == 0 and _save.idle_backup(stopped),
            "stop did not leave native saves flushed and audio stopped")
    console.send("audio silent")
    console.send("run 0")
    current = resumed("run_after_stop", 0.8)
    report["nonzero_pcm_observed"] = any(
        item["state"]["audio"].get("nonzero", 0) > 0 for item in observations)
    report["last_running"] = current


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM52")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--rom", default="/gba_game/test4.gba")
    parser.add_argument("--skip-load", action="store_true",
                        help="Reuse a stopped, already loaded ROM")
    parser.add_argument("--rounds", type=int, default=5,
                        help="Normal off/resume and back-to-back off/silent pairs (minimum 5)")
    parser.add_argument("--load-timeout", type=float, default=120)
    parser.add_argument("--operation-timeout", type=float, default=30)
    parser.add_argument("--log", type=Path)
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    if args.rounds < 5:
        parser.error("--rounds must be at least 5")
    if not 0 < args.load_timeout <= 120:
        parser.error("--load-timeout must be positive and at most 120 seconds")
    if not 0 < args.operation_timeout < float("inf"):
        parser.error("--operation-timeout must be finite and positive")
    try:
        rom_bytes = args.rom.encode("utf-8")
    except UnicodeEncodeError:
        parser.error("--rom must be a valid UTF-8 console path")
    if not args.rom.strip() or "\r" in args.rom or "\n" in args.rom or len(rom_bytes) > 500:
        parser.error("--rom must be a nonempty single-line UTF-8 path of at most 500 bytes")
    root = Path(__file__).resolve().parent.parent
    path = args.log or root / "build" / (
        "regba-audio-lifecycle-" + datetime.now().strftime("%Y%m%d-%H%M%S") + ".log")
    json_path = args.json or path.with_suffix(".json")
    path.parent.mkdir(parents=True, exist_ok=True)
    json_path.parent.mkdir(parents=True, exist_ok=True)
    uart = serial.Serial()
    uart.port, uart.baudrate, uart.timeout = args.port, args.baud, 0.02
    uart.dtr = uart.rts = False
    report = {"timestamp": datetime.now().astimezone().isoformat(), "port": args.port,
              "rom": args.rom, "reused_rom": args.skip_load, "rounds": args.rounds,
              "pass": False, "log": str(path)}
    with path.open("ab") as output:
        console = Console(uart, output)
        try:
            uart.open()
            print(f"UART {args.port} {args.baud}; raw log={path}", flush=True)
            console.idle(0.3)
            run_tests(console, args, report)
            report["pass"] = True
        except (Exception, KeyboardInterrupt) as error:
            report["error"] = f"{type(error).__name__}: {error}"
            if uart.is_open:
                console.ignore_failures = True
                try:
                    report["failure_status"] = console.status()
                except Exception as diagnostic_error:
                    report["failure_status_error"] = str(diagnostic_error)
        finally:
            if uart.is_open:
                console.ignore_failures = True
                try:
                    console.send("keys 000")
                    console.send("stop")
                    console.send("audio silent")
                    console.send("audio off")
                    console.idle(0.5)
                    stopped = _save.wait_state(
                        console, lambda state: state["core"].get("running") == 0 and
                        state["audio"].get("running") == 0 and _save.idle_backup(state),
                        args.operation_timeout, "final stopped, muted, flushed state")
                    _save.quiet(stopped)
                    report["stopped"] = stopped
                except Exception as error:
                    report["pass"] = False
                    report["stop_error"] = f"{type(error).__name__}: {error}"
                finally:
                    uart.close()
        message = "AUDIO LIFECYCLE " + ("PASS" if report["pass"] else "FAIL")
        console.record(("\n[host " + message + "]\n").encode())
        print(message + f"; JSON={json_path}", flush=True)
        for key in ("error", "stop_error"):
            if key in report:
                print(report[key], flush=True)
    json_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return 0 if report["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

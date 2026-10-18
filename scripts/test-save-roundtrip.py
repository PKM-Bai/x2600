"""Verify native SRAM/FLASH/EEPROM saves on already booted, mounted firmware.

Only the embedded /__save_*.gba diagnostics and their /GBA/SAVES/__save_*.sav
files are used. No reset, boot-mode change, NOR write, or audible output is
requested. --verify-only checks persisted diagnostic saves without changing them.
"""

import argparse
from datetime import datetime
import importlib.util
import json
from pathlib import Path
import re
import sys
import time
import zlib

import serial


def _module(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


_soak = _module("save_roundtrip_soak", "soak-regba.py")
_roms = _module("save_roundtrip_roms", "make-save-test-roms.py")
require = _soak.require


class Console(_soak.Console):
    """Consume the complete current status, including lines after audio."""

    @staticmethod
    def parse_fields(line, hexadecimal=()):
        fields = {}
        for key, value in re.findall(r"([A-Za-z_][A-Za-z_0-9]*)=([^\s]+)", line):
            if key in hexadecimal:
                fields[key] = int(value, 16)
            elif re.fullmatch(r"-?\d+", value):
                fields[key] = int(value)
            else:
                fields[key] = value
        return fields

    def status(self):
        self.send("status")
        state = {}
        for line in self.lines(8):
            if line.startswith("time_us="):
                state = {"clock": self.parse_fields(line)}
            elif "clock" not in state:
                continue
            elif line.startswith("core running="):
                state["core"] = self.parse_fields(
                    line, ("crc", "keys", "serial_keys", "board_keys"))
            elif line.startswith("backup dirty="):
                state["backup"] = self.parse_fields(line)
            elif line.startswith("save_test magic="):
                state["save_test"] = self.parse_fields(
                    line, ("magic", "marker", "expected_marker", "mismatch", "keys", "boot_marker"))
            elif line.startswith("SAVE queued="):
                state["save"] = self.parse_fields(line, ("crc32",))
            elif line.startswith("lcd active="):
                state["lcd"] = self.parse_fields(line)
            elif line.startswith("audio running="):
                state["audio"] = self.parse_fields(line)
            elif line.startswith("audio_timing "):
                state["audio_timing"] = self.parse_fields(line)
                require(all(name in state for name in ("core", "backup", "save", "lcd", "audio")),
                        "incomplete status before audio_timing")
                return state
        raise RuntimeError("incomplete status response: audio_timing was not received")


def quiet(state):
    require(state["audio"].get("muted") == 1, "audio output is not muted")


def idle_backup(state):
    return (state["save"].get("busy") == 0 and
            state["backup"].get("dirty") == 0 and
            state["backup"].get("in_flight") == 0 and
            state["backup"].get("requested") == 0)


def wait_state(console, predicate, timeout, description):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        last = console.status()
        quiet(last)
        require(last["save"].get("errors") == 0,
                f"battery save error: {last['save']}")
        if last["core"].get("running") == 1 and last["audio"].get("running") == 1:
            for counter in ("queue_under", "FIFO_under", "errors", "timeouts"):
                require(last["audio"].get(counter) == 0,
                        f"audio during save test {counter}={last['audio'].get(counter)}; "
                        f"timing={last.get('audio_timing')}")
        if predicate(last):
            return last
        console.idle(min(0.15, max(0, deadline - time.monotonic())))
    raise RuntimeError(f"timeout waiting for {description}; last status={last}")


def stop_core(console, timeout):
    console.send("keys 000")
    console.send("stop")
    # The core's one-off stop diagnostics share the UART with the console.
    # Let that response finish before asking for a structured status snapshot.
    console.idle(0.3)
    return wait_state(console,
                      lambda state: state["core"].get("running") == 0 and idle_backup(state),
                      timeout, "stopped core and flushed battery save")


def boot_rom(console, kind, args):
    console.send("load /__save_" + kind + ".gba")
    loaded = console.wait(r"load result=(-?\d+)", args.load_timeout)
    require(int(loaded[1]) == 0, f"{kind} diagnostic ROM load failed")
    console.send("keys 000")
    console.send("audio silent")
    console.send("run 0")
    # Avoid overlapping the one-off before-run diagnostic with status; also
    # let the initial codec settling complete before the first observation.
    console.idle(0.5)
    kind_id, capacity = _roms.KINDS[kind]

    def started(state):
        mailbox = state.get("save_test", {})
        return (state["core"].get("running") == 1 and
                mailbox.get("magic") == _roms.MAILBOX_MAGIC and
                mailbox.get("kind") == kind_id and mailbox.get("bytes") == capacity and
                mailbox.get("checks", 0) >= 1 and mailbox.get("frames", 0) >= 1)

    state = wait_state(console, started, args.operation_timeout, kind + " read-only boot")
    mailbox = state["save_test"]
    require(mailbox.get("writes") == 0, f"{kind} changed backup at boot")
    require(mailbox.get("state") in (0, 1, 2), f"{kind} diagnostic save is invalid: {mailbox}")
    require(mailbox.get("mismatch") == 0xFFFFFFFF, f"{kind} backup is inconsistent")
    require(state["core"].get("board_keys") == 0 and mailbox.get("keys") == 0,
            "release physical game buttons before running this automatic test")
    require(idle_backup(state), f"{kind} boot unexpectedly dirtied or saved backup")
    if mailbox["state"] == 0:
        require(mailbox.get("marker") == 0xFFFFFFFF and mailbox.get("count") == 0xFFFFFFFF,
                f"{kind} blank-save marker/count mismatch")
    else:
        choice = "A" if mailbox["state"] == 1 else "B"
        require(mailbox.get("marker") == _roms.MARKERS[choice] and
                mailbox.get("boot_marker") == _roms.MARKERS[choice] and
                mailbox.get("boot_count") == mailbox.get("count") and
                1 <= mailbox.get("count", 0) < 0xFFFFFFFF,
                f"{kind} boot metadata is inconsistent")
    return state


def assert_guest(state, kind, choice, count, writes):
    mailbox = state["save_test"]
    require(mailbox.get("state") == (1 if choice == "A" else 2) and
            mailbox.get("marker") == _roms.MARKERS[choice] and
            mailbox.get("count") == count and mailbox.get("writes") == writes and
            mailbox.get("expected_marker") == _roms.MARKERS[choice] and
            mailbox.get("expected_count") == count and mailbox.get("mismatch") == 0xFFFFFFFF,
            f"{kind} {choice}/{count} guest verification failed: {mailbox}")


def press(console, choice):
    console.send("keys " + ("001" if choice == "A" else "002"))
    console.idle(0.1)
    console.send("keys 000")


def wait_guest(console, kind, choice, count, args):
    target = _roms.MARKERS[choice]

    def finished(state):
        mailbox = state.get("save_test", {})
        return (mailbox.get("marker") == target and mailbox.get("count") == count and
                mailbox.get("writes") == 1 and mailbox.get("keys") == 0 and
                mailbox.get("state") == (1 if choice == "A" else 2))

    state = wait_state(console, finished, args.operation_timeout, kind + " " + choice + " guest write")
    assert_guest(state, kind, choice, count, 1)
    return state


def expected_crc(kind, choice, count):
    image = _roms.expected_save(kind, choice, count)
    return len(image), zlib.crc32(image) & 0xFFFFFFFF


def check_card(console, kind, choice, count, args):
    path = "/GBA/SAVES/__save_" + kind + ".sav"
    size, crc = expected_crc(kind, choice, count)
    console.send("crc " + path)
    checked = console.wait(r"CRC32=([0-9a-fA-F]+) bytes=(\d+) " + re.escape(path),
                           args.operation_timeout)
    actual = {"path": path, "bytes": int(checked[2]), "crc32": int(checked[1], 16),
              "expected_bytes": size, "expected_crc32": crc}
    require(actual["bytes"] == size and actual["crc32"] == crc,
            f"{kind} {choice}/{count} full TF image CRC/size mismatch: {actual}")
    return actual


def wait_saved(console, kind, choice, count, baseline, args):
    size, crc = expected_crc(kind, choice, count)
    path = "/GBA/SAVES/__save_" + kind + ".sav"

    def finished(state):
        save = state["save"]
        return save.get("completed", 0) > baseline["completed"] and idle_backup(state)

    state = wait_state(console, finished, args.operation_timeout, kind + " battery-file completion")
    save = state["save"]
    require(save.get("queued") == baseline["queued"] + 1 and
            save.get("completed") == baseline["completed"] + 1 and
            save.get("last_result") == 0 and save.get("bytes") == size and
            save.get("crc32") == crc and save.get("path") == path,
            f"{kind} save worker result differs from expected image: {save}")
    assert_guest(state, kind, choice, count, 1)
    return state


def assert_no_save(before, after, description):
    require(after["save"].get("queued") == before["save"].get("queued") and
            after["save"].get("completed") == before["save"].get("completed"),
            description + " unexpectedly wrote a battery file")


def test_kind(console, kind, args, report):
    before = console.status()
    boot = boot_rom(console, kind, args)
    report["initial"] = boot
    assert_no_save(before, boot, kind + " initial boot")
    mailbox = boot["save_test"]
    if args.verify_only:
        require(mailbox["state"] in (1, 2), f"{kind} has no persisted diagnostic save to verify")
        choice = "A" if mailbox["state"] == 1 else "B"
        report["verified_crc"] = check_card(console, kind, choice, mailbox["count"], args)
        stopped = stop_core(console, args.operation_timeout)
        assert_no_save(boot, stopped, kind + " read-only persistence verification")
        report["stopped"] = stopped
        return

    count = 0 if mailbox["state"] == 0 else mailbox["boot_count"]
    require(count < 0xFFFFFFFC, "diagnostic counter has insufficient room for three increments")
    press(console, "A")
    report["auto_guest"] = wait_guest(console, kind, "A", count + 1, args)
    # This wait deliberately leaves the emulator running: stop must not be the
    # operation that flushes the file in the autosave test.
    console.idle(2.2)
    after_auto = wait_saved(console, kind, "A", count + 1, boot["save"], args)
    report["auto_saved"] = after_auto
    report["auto_crc"] = check_card(console, kind, "A", count + 1, args)
    stop_core(console, args.operation_timeout)
    a_reload = boot_rom(console, kind, args)
    report["auto_reload"] = a_reload
    assert_guest(a_reload, kind, "A", count + 1, 0)
    assert_no_save(after_auto, a_reload, kind + " A reload")

    press(console, "B")
    b_guest = wait_guest(console, kind, "B", count + 2, args)
    report["manual_guest"] = b_guest
    assert_no_save(a_reload, b_guest, kind + " B before manual save")
    # Send immediately after the guest completed its native backup transaction,
    # before the 2s idle autosave can submit a snapshot of its own.
    console.send("save")
    after_manual = wait_saved(console, kind, "B", count + 2, a_reload["save"], args)
    report["manual_saved"] = after_manual
    report["manual_crc"] = check_card(console, kind, "B", count + 2, args)
    stop_core(console, args.operation_timeout)
    b_reload = boot_rom(console, kind, args)
    report["manual_reload"] = b_reload
    assert_guest(b_reload, kind, "B", count + 2, 0)
    assert_no_save(after_manual, b_reload, kind + " B reload")
    # Stop before the idle autosave deadline. This specifically exercises the
    # stop path's flush rather than stopping an already saved game.
    press(console, "A")
    report["flush_guest"] = wait_guest(console, kind, "A", count + 3, args)
    assert_no_save(b_reload, report["flush_guest"], kind + " before stop flush")
    after_flush = stop_core(console, args.operation_timeout)
    report["flush_saved"] = after_flush
    size, crc = expected_crc(kind, "A", count + 3)
    require(after_flush["save"].get("completed") == b_reload["save"]["completed"] + 1 and
            after_flush["save"].get("queued") == b_reload["save"]["queued"] + 1 and
            after_flush["save"].get("last_result") == 0 and
            after_flush["save"].get("bytes") == size and
            after_flush["save"].get("crc32") == crc,
            kind + " stop flush did not complete the expected snapshot")
    report["flush_crc"] = check_card(console, kind, "A", count + 3, args)
    flush_reload = boot_rom(console, kind, args)
    report["flush_reload"] = flush_reload
    assert_guest(flush_reload, kind, "A", count + 3, 0)
    assert_no_save(after_flush, flush_reload, kind + " stop-flush reload")
    report["stopped"] = stop_core(console, args.operation_timeout)


def run_tests(console, args, result):
    initial = console.status()
    result["initial"] = initial
    require(initial["core"].get("running") == 0 and initial["save"].get("busy") == 0,
            "start from a stopped emulator and idle save worker")
    require(initial["lcd"].get("active") == 1, "LCD must already be enabled")
    console.send("keys 000")
    console.send("audio silent")
    ready = console.status()
    quiet(ready)
    require(ready["save"].get("errors") == 0, "save worker already reports errors")
    result["devices"] = []
    for kind in args.kind or _roms.KINDS:
        report = {"kind": kind, "pass": False}
        result["devices"].append(report)
        print(f"SAVE {kind}: {'verify persisted image' if args.verify_only else 'autosave + manual + reload'}",
              flush=True)
        test_kind(console, kind, args, report)
        report["pass"] = True
        print(f"SAVE {kind} PASS", flush=True)


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM52")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--kind", action="append", choices=_roms.KINDS)
    parser.add_argument("--verify-only", action="store_true")
    parser.add_argument("--load-timeout", type=float, default=60)
    parser.add_argument("--operation-timeout", type=float, default=30)
    parser.add_argument("--log", type=Path)
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    for name in ("load_timeout", "operation_timeout"):
        if not 0 < getattr(args, name) < float("inf"):
            parser.error("--" + name.replace("_", "-") + " must be finite and positive")
    root = Path(__file__).resolve().parent.parent
    path = args.log or root / "build" / ("regba-save-roundtrip-" + datetime.now().strftime("%Y%m%d-%H%M%S") + ".log")
    json_path = args.json or path.with_suffix(".json")
    path.parent.mkdir(parents=True, exist_ok=True)
    json_path.parent.mkdir(parents=True, exist_ok=True)
    uart = serial.Serial()
    uart.port, uart.baudrate, uart.timeout = args.port, args.baud, 0.1
    uart.dtr = uart.rts = False
    result = {"timestamp": datetime.now().astimezone().isoformat(), "verify_only": args.verify_only,
              "pass": False, "port": args.port, "log": str(path)}
    with path.open("ab") as output:
        console = Console(uart, output)
        try:
            uart.open()
            print(f"UART {args.port} {args.baud}; raw log={path}", flush=True)
            console.idle(0.2)
            run_tests(console, args, result)
            result["pass"] = True
        except (Exception, KeyboardInterrupt) as error:
            result["error"] = f"{type(error).__name__}: {error}"
        finally:
            if uart.is_open:
                console.ignore_failures = True
                try:
                    console.send("keys 000")
                    console.send("stop")
                    console.send("audio silent")
                    console.send("audio off")
                    console.idle(0.5)
                    stopped = wait_state(
                        console, lambda state: state["core"].get("running") == 0 and
                        state["audio"].get("running") == 0 and idle_backup(state),
                        args.operation_timeout, "final stopped and muted state")
                    quiet(stopped)
                    result["stopped"] = stopped
                except Exception as error:
                    result["pass"] = False
                    result["stop_error"] = f"{type(error).__name__}: {error}"
                finally:
                    uart.close()
        message = "SAVE ROUNDTRIP " + ("PASS" if result["pass"] else "FAIL")
        console.record(("\n[host " + message + "]\n").encode())
        print(message + f"; JSON={json_path}", flush=True)
        for key in ("error", "stop_error"):
            if key in result:
                print(result[key], flush=True)
    json_path.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

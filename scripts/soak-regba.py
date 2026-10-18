"""Run a ReGBA/PCM-DMA/TF soak (silent by default) on already booted, mounted firmware."""
import argparse
import codecs
from collections import deque
from datetime import datetime
from pathlib import Path
import re
import time

import serial


class Console:
    def __init__(self, uart, output):
        self.uart, self.output = uart, output
        self.buffer = ""
        self.decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")
        self.pending = deque()
        self.ignore_failures = False

    def record(self, data):
        self.output.write(data)
        self.output.flush()

    def send(self, command):
        if not command.strip() or "\r" in command or "\n" in command:
            raise ValueError("Console commands must be nonempty single lines")
        encoded = command.encode("utf-8")
        self.record(f"\n[host {datetime.now().isoformat(timespec='seconds')}: {command}]\n".encode())
        self.uart.write(encoded + b"\r")
        self.uart.flush()

    def lines(self, seconds):
        deadline = time.monotonic() + seconds
        while self.pending or time.monotonic() < deadline:
            if self.pending:
                yield self.pending.popleft()
                continue
            data = self.uart.read(self.uart.in_waiting or 1)
            if not data:
                continue
            self.record(data)
            self.buffer += self.decoder.decode(data)
            while "\n" in self.buffer:
                line, self.buffer = self.buffer.split("\n", 1)
                self.pending.append(line.rstrip("\r"))
            for line in self.pending:
                if not self.ignore_failures and re.search(
                        r"\bPANIC\b|\bFAIL(?:ED)?\b|core bad jump|(?:^|\s)result=-\d+", line):
                    raise RuntimeError("board failure: " + line)

    def wait(self, pattern, timeout):
        for line in self.lines(timeout):
            match = re.search(pattern, line)
            if match:
                return match
        raise RuntimeError(f"timeout waiting for {pattern!r}")

    def idle(self, seconds):
        for _ in self.lines(seconds):
            pass

    def status(self):
        self.send("status")
        state = {}
        for line in self.lines(8):
            if line.startswith("time_us="):
                state = {"clock": self.fields(line)}
            elif line.startswith("core running=") and "clock" in state:
                state["core"] = self.fields(line)
            elif line.startswith("BTPAD initialized=") and "core" in state:
                state["btpad"] = self.fields(line, hex_fields=("keys",))
            elif line.startswith("btpad_rx ") and "core" in state:
                state["btpad_rx"] = self.fields(line)
            elif line.startswith("lcd active=") and "core" in state:
                state["lcd"] = self.fields(line)
                print("LCD " + line, flush=True)
            elif line.startswith("audio running=") and "core" in state:
                state["audio"] = self.fields(line)
                print("STATUS " + line + " frames=" + str(state["core"].get("frames")) +
                      " tick=" + str(state["clock"].get("tick")), flush=True)
                return state
        raise RuntimeError("incomplete status response")

    @staticmethod
    def fields(line, hex_fields=()):
        fields = {key: int(value) for key, value in re.findall(r"([A-Za-z_]+)=(\d+)(?=\s|$)", line)}
        for key in hex_fields:
            match = re.search(rf"\b{re.escape(key)}=([0-9a-fA-F]+)(?=\s|$)", line)
            if match:
                fields[key] = int(match[1], 16)
        return fields


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def check_running(state, previous=None, require_display=False, require_btpad=False, expected_muted=1):
    core, audio = state["core"], state["audio"]
    require(core.get("running") == 1 and core.get("audio_requested") == 1,
            "emulator/audio request is not running")
    require(audio.get("running") == 1 and audio.get("muted") == expected_muted,
            f"DMA is stopped or audio muted={audio.get('muted')} (expected {expected_muted})")
    for counter in ("queue_under", "FIFO_under", "errors", "timeouts"):
        require(audio.get(counter) == 0, f"audio {counter}={audio.get(counter)}")
    for counter in ("in", "out", "DMA_done", "nonzero", "peak"):
        require(audio.get(counter, 0) > 0, f"audio {counter} has no activity")
    if require_display:
        lcd = state.get("lcd", {})
        require(lcd.get("active") == 1, "display is inactive or LCD status is missing")
        require(lcd.get("underrun") == 0, f"display underrun={lcd.get('underrun')}")
        require(lcd.get("allocated") == 7372800,
                f"display framebuffer allocation={lcd.get('allocated')}")
    if require_btpad:
        btpad, bt_rx = state.get("btpad", {}), state.get("btpad_rx", {})
        require(btpad.get("initialized") == 1, "Bluetooth is uninitialized or BTPAD status is missing")
        require(bt_rx.get("data_frames", 0) > 0, "Bluetooth has no valid data frames or RX status is missing")
        for counter in ("checksum_errors", "length_errors", "uart_errors"):
            require(bt_rx.get(counter) == 0, f"Bluetooth {counter}={bt_rx.get(counter)}")
        for counter in ("bytes", "data_frames", "link_frames"):
            require(bt_rx.get(counter, -1) >= 0, f"Bluetooth {counter} is missing")
            if previous:
                require(bt_rx[counter] >= previous["btpad_rx"][counter],
                        f"Bluetooth {counter} went backwards")
    if previous:
        for group, counter in (("clock", "time_us"), ("clock", "tick"), ("core", "frames"),
                               ("audio", "in"), ("audio", "out"), ("audio", "DMA_done")):
            require(state[group].get(counter, 0) > previous[group].get(counter, 0),
                    f"{group} {counter} did not grow")
        require(audio["nonzero"] >= previous["audio"]["nonzero"], "PCM nonzero counter went backwards")
        if require_display:
            for counter in ("vblank", "presented"):
                require(lcd.get(counter, 0) > previous["lcd"].get(counter, 0),
                        f"display {counter} did not grow")
            require(lcd.get("skipped", -1) >= previous["lcd"].get("skipped", 0),
                    "display skipped counter went backwards or is missing")
        microseconds = state["clock"]["time_us"] - previous["clock"]["time_us"]
        ticks = state["clock"]["tick"] - previous["clock"]["tick"]
        require(abs(microseconds - ticks * 1000) < 100000 + microseconds // 1000,
                "OST microseconds and FreeRTOS ticks diverged")


def run_soak(console, args):
    expected_muted = 0 if args.audible else 1
    mode = "Audible" if args.audible else "Silent"
    state = console.status()
    require(state["core"].get("running") == 0 and state["audio"].get("running") == 0,
            "start from stopped emulator and audio")
    console.send("audio on" if args.audible else "audio silent")
    state = console.status()
    require(state["audio"].get("muted") == expected_muted and state["audio"].get("running") == 0,
            f"{mode.lower()} output not confirmed before ROM load")
    if args.skip_load:
        print("Reusing stopped ROM; caller is responsible for it being loaded", flush=True)
    else:
        print(f"Loading {args.rom}; waiting up to {args.load_timeout:g} seconds", flush=True)
        console.send("load " + args.rom)
        loaded = console.wait(r"load result=(-?\d+)", args.load_timeout)
        require(int(loaded[1]) == 0, "ROM load failed")
    state = console.status()
    require(state["core"].get("running") == 0 and state["audio"].get("muted") == expected_muted,
            "core/output state changed before run")
    console.send("run 0")
    console.idle(2.5)
    if args.press_start:
        console.send("keys 008")
        console.idle(0.1)
        console.send("keys 000")
        console.idle(2.5)
    previous = console.status()
    check_running(previous, require_display=args.require_display, require_btpad=args.require_btpad,
                  expected_muted=expected_muted)
    initial = previous
    started = time.monotonic()
    deadline = started + args.duration
    next_status, next_write, next_crc = started + 60, started + 300, started + 600
    print(f"{mode} soak started: target={args.duration:g}s (warm-up excluded)", flush=True)
    while time.monotonic() < deadline:
        now = time.monotonic()
        if now >= next_write:
            console.send("rwtest")
            written = console.wait(r"TF test OK: (\S+) bytes=(\d+) crc32=([0-9a-fA-F]+)", 60)
            require(int(written[2]) == 131072 and int(written[3], 16) == 0x29DF376D,
                    "TF write/read CRC mismatch")
            print(f"TF rwtest verified {written[1]} at {time.monotonic() - started:.1f}s", flush=True)
            next_write += 300
        if now >= next_crc:
            console.send("crc /GBA/TEST/SDTEST00.BIN")
            checked = console.wait(r"CRC32=([0-9a-fA-F]+) bytes=(\d+) /GBA/TEST/SDTEST00\.BIN", 60)
            require(int(checked[1], 16) == 0x29DF376D and int(checked[2]) == 131072,
                    "persistent TF test file CRC mismatch")
            print(f"TF persistent read verified at {time.monotonic() - started:.1f}s", flush=True)
            next_crc += 600
        if now >= next_status:
            current = console.status()
            check_running(current, previous, require_display=args.require_display, require_btpad=args.require_btpad,
                          expected_muted=expected_muted)
            previous = current
            print(f"Soak elapsed={time.monotonic() - started:.1f}s", flush=True)
            next_status += 60
        console.idle(min(0.5, max(0, deadline - time.monotonic())))
    # Sample before stop: intentional final draining must not hide underruns
    # that occurred during emulation. All running error counters must be zero.
    current = console.status()
    # Bluetooth counters must also remain monotonic after the last periodic sample.
    check_running(current, previous if args.require_btpad else initial,
                  require_display=args.require_display, require_btpad=args.require_btpad,
                  expected_muted=expected_muted)
    elapsed = time.monotonic() - started
    board_seconds = (current["clock"]["time_us"] - initial["clock"]["time_us"]) / 1000000
    require(abs(board_seconds - elapsed) < elapsed * 0.02 + 0.2,
            f"board/host time mismatch: board={board_seconds:.3f}s host={elapsed:.3f}s")
    print(f"Clock comparison: board={board_seconds:.3f}s host={elapsed:.3f}s", flush=True)
    if args.require_btpad:
        bt_rx = current["btpad_rx"]
        message = "Bluetooth soak: " + " ".join(
            f"{counter}={bt_rx[counter]}" for counter in
            ("bytes", "data_frames", "link_frames", "checksum_errors", "length_errors", "uart_errors"))
        console.record(("\n[host " + message + "]\n").encode())
        print(message, flush=True)
    return elapsed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM52")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--duration", type=float, default=1800, help="Measured soak seconds, excluding warm-up")
    parser.add_argument("--rom", default="/gba_game/test1.gba")
    parser.add_argument("--load-timeout", type=float, default=600, help="Maximum ROM load seconds")
    parser.add_argument("--skip-load", action="store_true",
                        help="Reuse stopped ROM; caller must ensure a ROM is already loaded")
    parser.add_argument("--press-start", action="store_true",
                        help="Press Start for 100 ms after 2.5s, then wait another 2.5s before checks")
    parser.add_argument("--audible", action="store_true",
                        help="Enable audible game audio and require unmuted output (default: silent)")
    parser.add_argument("--require-display", action="store_true",
                        help="Require an active portrait display with advancing video counters and no underruns")
    parser.add_argument("--require-btpad", action="store_true",
                        help="Require initialized Bluetooth with valid input history, no RX errors, and monotonic counters")
    parser.add_argument("--log", type=Path)
    args = parser.parse_args()
    if not 0 < args.duration < float("inf"):
        parser.error("--duration must be finite and positive")
    if not 0 < args.load_timeout < float("inf"):
        parser.error("--load-timeout must be finite and positive")
    try:
        rom_bytes = args.rom.encode("utf-8")
    except UnicodeEncodeError:
        parser.error("--rom must be a valid UTF-8 console path")
    if not args.rom.strip() or "\r" in args.rom or "\n" in args.rom or len(rom_bytes) > 500:
        parser.error("--rom must be a nonempty single-line UTF-8 path of at most 500 bytes")
    root = Path(__file__).resolve().parent.parent
    path = args.log or root / "build" / ("regba-soak-" + datetime.now().strftime("%Y%m%d-%H%M%S") + ".log")
    path.parent.mkdir(parents=True, exist_ok=True)
    uart = serial.Serial()
    uart.port, uart.baudrate, uart.timeout = args.port, args.baud, 0.1
    uart.dtr = uart.rts = False
    result, elapsed = 0, None
    with path.open("ab") as output:
        console = Console(uart, output)
        try:
            uart.open()
            print(f"UART {args.port} {args.baud}; raw log={path}", flush=True)
            console.idle(0.2)
            elapsed = run_soak(console, args)
        except (Exception, KeyboardInterrupt) as error:
            result = 1
            message = f"SOAK FAIL: {type(error).__name__}: {error}"
            console.record(("\n[host " + message + "]\n").encode())
            print(message, flush=True)
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
                    require(stopped["core"].get("running") == 0 and
                            stopped["audio"].get("running") == 0,
                            "core/audio did not stop")
                    print("Core and audio stopped", flush=True)
                except Exception as error:
                    result = 1
                    message = f"SOAK FAIL: stop confirmation: {error}"
                    console.record(("\n[host " + message + "]\n").encode())
                    print(message, flush=True)
                finally:
                    uart.close()
        if result == 0:
            label = "30min PASS" if elapsed >= 1800 else "short-run PASS"
            message = (f"SOAK {label}: actual={elapsed:.3f}s; requested={args.duration:g}s; "
                       f"audio={'audible' if args.audible else 'silent'}")
            console.record(("\n[host " + message + "]\n").encode())
            print(message, flush=True)
    return result


if __name__ == "__main__":
    raise SystemExit(main())

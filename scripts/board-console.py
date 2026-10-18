"""Capture the board UART; optionally send explicit console commands."""
import argparse
from datetime import datetime
from pathlib import Path
import time
import sys

import serial


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM52")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--command", action="append", default=[])
    parser.add_argument("--stop-autoboot", action="store_true",
                        help="Send a space when the U-Boot autoboot prompt appears; exit capture")
    parser.add_argument("--duration", type=float, default=5,
                        help="Capture seconds after each command (or with no command)")
    parser.add_argument("--log", type=Path)
    args = parser.parse_args()
    if args.duration <= 0:
        parser.error("--duration must be positive")
    root = Path(__file__).resolve().parent.parent
    path = args.log or root / "build" / ("board-console-" + datetime.now().strftime("%Y%m%d-%H%M%S") + ".log")
    path.parent.mkdir(parents=True, exist_ok=True)
    uart = serial.Serial()
    uart.port, uart.baudrate, uart.timeout = args.port, args.baud, 0.1
    uart.dtr = uart.rts = False
    uart.open()
    with uart, path.open("ab") as output:
        boot_text = bytearray()
        stopped = False
        def emit(data):
            output.write(data)
            output.flush()
            print(data.decode("utf-8", errors="replace"), end="", flush=True)

        def capture(seconds):
            nonlocal stopped
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                data = uart.read(uart.in_waiting or 1)
                if data:
                    emit(data)
                    if args.stop_autoboot:
                        boot_text.extend(data)
                        del boot_text[:-4096]
                        if not stopped and any(marker in bytes(boot_text).lower() for marker in
                                               (b"hit any key", b"press any key", b"autoboot")):
                            uart.write(b" ")
                            uart.flush()
                            stopped = True
                            emit(b"\n[autoboot interrupted]\n")
                    if stopped and b"# " in bytes(boot_text)[-160:]:
                        return

        print(f"UART {args.port} {args.baud}; log={path}", flush=True)
        capture(0.2)
        for command in args.command:
            if "\r" in command or "\n" in command:
                parser.error("Each --command must be one line")
            emit(f"\n[host command: {command}]\n".encode())
            uart.write(command.encode("ascii") + b"\r")
            uart.flush()
            capture(args.duration)
        if not args.command:
            capture(args.duration)


if __name__ == "__main__":
    main()

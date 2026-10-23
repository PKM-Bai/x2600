"""Probe U-Boot or load ReGBA into RAM via YMODEM, with length and CRC checks.

Default/--probe sends only help loady and read-only clock register commands.
--execute uploads to 0x80600000, verifies filesize and U-Boot CRC32, then goes.
--no-go leaves the verified image in RAM. No NOR or environment save command
is sent. --self-test uses an in-memory serial peer and never opens a port.
"""
from __future__ import annotations

import argparse
import binascii
from datetime import datetime
import hashlib
from pathlib import Path
import re
import time
import uuid
import zlib

ROOT = Path(__file__).resolve().parent.parent
ADDRESS = 0x80600000
MAX_IMAGE = 0x600000
SOH, STX, EOT, ACK, NAK, CAN, CRC = 1, 2, 4, 6, 21, 24, 67
PROMPT = re.compile(rb"(?:^|\n)[A-Za-z0-9_./:@ -]{1,80}# ?$")
YMODEM_RESTART_BANNER = re.compile(
    rb"[\r\n\0](U-Boot(?: SPL)? [0-9]{4}\.[0-9]{2}|(?:PLAYER|AI)_BOOT core=mips)(?=[ (\r\n-])"
)


class Log:
    def __init__(self, path: Path):
        path.parent.mkdir(parents=True, exist_ok=True)
        self.file = path.open("x", encoding="utf-8", buffering=1)
        self.path = path

    def __call__(self, text: str):
        print(text, flush=True)
        self.file.write(datetime.now().isoformat(timespec="seconds") + " " + text + "\n")

    def close(self):
        self.file.close()


class Console:
    def __init__(self, serial, log):
        self.serial, self.log = serial, log

    def write(self, data: bytes):
        count = self.serial.write(data)
        if count != len(data):
            raise RuntimeError(f"Short serial write: {count}/{len(data)}")
        self.serial.flush()

    def drain(self, quiet=0.25, limit=2.0):
        data = bytearray()
        start = last = time.monotonic()
        while time.monotonic() - start < limit and time.monotonic() - last < quiet:
            chunk = self.serial.read(4096)
            if chunk:
                data.extend(chunk)
                last = time.monotonic()
        if data:
            self.log("RX " + bytes(data).decode("ascii", errors="backslashreplace"))
        return bytes(data)

    def read_prompt(self, timeout=15.0):
        data = bytearray()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            chunk = self.serial.read(1)
            if chunk:
                data.extend(chunk)
                if PROMPT.search(data):
                    # The prompt may end '# ' and the space may arrive later;
                    # an optional trailing space is harmless to the next read.
                    self.log("RX " + bytes(data).decode("ascii", errors="backslashreplace"))
                    return bytes(data)
        self.log("RX (timeout) " + bytes(data).decode("ascii", errors="backslashreplace"))
        raise RuntimeError("U-Boot prompt timeout; no further command was sent")

    def command(self, command: str, timeout=15.0):
        self.log("TX " + command)
        self.write(command.encode("ascii") + b"\r")
        result = self.read_prompt(timeout)
        if b"Unknown command" in result:
            raise RuntimeError(f"U-Boot does not support {command.split()[0]}")
        return result

    def wait_boot(self, timeout=3600.0):
        """Hold the UART across reset, autoboot interruption and upload."""
        self.log("Waiting for normal board reset; will interrupt U-Boot autoboot")
        data = bytearray()
        interrupted = False
        reconnect = False
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if reconnect:
                try:
                    # Reuse the same object/port/settings, with modem lines
                    # inactive before open. Re-enumeration never authorizes a
                    # command or upload, only another boot-banner observation.
                    self.serial.dtr = self.serial.rts = False
                    self.serial.open()
                except OSError as exc:
                    self.log(f"Serial boot reopen pending: {exc}")
                    remaining = deadline - time.monotonic()
                    if remaining > 0:
                        time.sleep(min(0.1, remaining))
                    continue
                self.log("Serial boot port reopened; waiting for a fresh autoboot banner")
                reconnect = False
            try:
                chunk = self.serial.read(1)
            except OSError as exc:  # pyserial SerialException also derives from OSError.
                self.log(f"Serial boot disconnected: {exc}")
                # Neither a partial old banner nor an old interruption may
                # match bytes from a different boot after the port reopens.
                data.clear()
                interrupted = False
                try:
                    self.serial.close()
                except OSError as close_error:
                    self.log(f"Serial boot close after disconnect: {close_error}")
                reconnect = True
                continue
            if not chunk:
                continue
            data.extend(chunk)
            if not interrupted and b"hit any key" in data[-200:].lower():
                self.write(b" ")
                interrupted = True
            if interrupted and PROMPT.search(data):
                self.log("RX boot " + bytes(data).decode("ascii", errors="backslashreplace"))
                return
        self.log("RX boot (timeout) " + bytes(data).decode("ascii", errors="backslashreplace"))
        raise RuntimeError("No interrupted U-Boot prompt after reset; no upload sent")

    def reboot_linux(self):
        """Identify the running shell before rebooting on this same UART."""
        self.drain()
        self.log("TX uname -s (identify Linux before software reboot)")
        self.write(b"\runame -s\r")
        data = bytearray()
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            chunk = self.serial.read(1)
            if chunk:
                data.extend(chunk)
                if re.search(rb"(?:^|\n)Linux\r*\n", data):
                    self.log("RX " + bytes(data).decode("ascii", errors="backslashreplace"))
                    self.log("TX reboot (Linux software reset; no NOR programming)")
                    self.write(b"reboot\r")
                    self.wait_boot()
                    return
        self.log("RX identify " + bytes(data).decode("ascii", errors="backslashreplace"))
        raise RuntimeError("Linux shell not confirmed; no reboot sent")

    def reboot_player(self):
        """Identify our RAM player diagnostics before its watchdog reboot."""
        self.drain()
        self.log("TX status (identify player before software reboot)")
        self.write(b"\rstatus\r")
        data = bytearray()
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            chunk = self.serial.read(1)
            if chunk:
                data.extend(chunk)
                if re.search(rb"(?:^|\n)AMP_STATUS active=", data):
                    self.log("RX " + bytes(data).decode("ascii", errors="backslashreplace"))
                    self.log("TX reboot (player watchdog reset; no NOR programming)")
                    self.write(b"reboot\r")
                    self.wait_boot(timeout=30.0)
                    return
        self.log("RX identify " + bytes(data).decode("ascii", errors="backslashreplace"))
        raise RuntimeError("Player diagnostics not confirmed; no reboot sent")

    def reboot_ai(self):
        """Identify the independent AI firmware before its watchdog reboot."""
        self.drain()
        self.log("TX status (identify AI firmware before software reboot)")
        self.write(b"\rstatus\r")
        data = bytearray()
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            chunk = self.serial.read(1)
            if chunk:
                data.extend(chunk)
                if re.search(rb"(?:^|\n)(?:ai> )?AI_STATUS stage=foundation ", data):
                    self.log("RX " + bytes(data).decode("ascii", errors="backslashreplace"))
                    self.log("TX reboot (AI watchdog reset; no NOR programming)")
                    self.write(b"reboot\r")
                    self.wait_boot(timeout=30.0)
                    return
        self.log("RX identify " + bytes(data).decode("ascii", errors="backslashreplace"))
        raise RuntimeError("AI diagnostics not confirmed; no reboot sent")


def memory_words(output: bytes) -> dict[int, int]:
    words = {}
    for line in output.decode("ascii", errors="replace").splitlines():
        match = re.match(r"\s*([0-9a-fA-F]{8}):\s+(.*)", line)
        if match:
            address = int(match[1], 16)
            for value in match[2].split():
                if not re.fullmatch(r"[0-9a-fA-F]{8}", value):
                    break
                words[address] = int(value, 16)
                address += 4
    return words


def pll_rate(value: int) -> int:
    m, n = (value >> 20) & 0xfff, (value >> 14) & 0x3f
    od0, od1 = (value >> 8) & 7, (value >> 11) & 7
    if not n or not od0 or not od1 or not value & 8:
        raise RuntimeError(f"PLL is unlocked or has an invalid divisor: {value:08x}")
    return 24_000_000 * m // n // od0 // od1


def probe(console: Console):
    help_text = console.command("help loady")
    if b"ymodem" not in help_text.lower():
        raise RuntimeError("U-Boot help did not confirm YMODEM support")
    values = memory_words(console.command("md.l b0000000 6"))
    values.update(memory_words(console.command("md.l b000002c 1")))
    try:
        cpccr, apll, mpll, ddrcdr = (values[a] for a in
            (0xb0000000, 0xb0000010, 0xb0000014, 0xb000002c))
    except KeyError as exc:
        raise RuntimeError(f"Missing clock register in U-Boot output: {exc}") from exc
    rates = {1: pll_rate(apll), 2: pll_rate(mpll)}
    cpu = rates.get((cpccr >> 28) & 3, 0) // ((cpccr & 15) + 1)
    ddr = rates.get(ddrcdr >> 30, 0) // ((ddrcdr & 255) + 1)
    console.log(f"Clock CPCCR={cpccr:08x} APLL={apll:08x} MPLL={mpll:08x} DDRCDR={ddrcdr:08x}")
    console.log(f"Clock CPU={cpu} APLL={rates[1]} MPLL={rates[2]} DDR={ddr} Hz")
    return (cpu, rates[1], rates[2], ddr)


def packet(number: int, payload: bytes, length: int, pad=0):
    if length not in (128, 1024) or len(payload) > length:
        raise ValueError("Invalid YMODEM payload length")
    data = payload.ljust(length, bytes((pad,)))
    checksum = binascii.crc_hqx(data, 0)
    number &= 255
    return bytes((SOH if length == 128 else STX, number, number ^ 255)) + data + checksum.to_bytes(2, "big")


class YModem:
    def __init__(self, console: Console, retries=10, timeout=8.0):
        self.console, self.retries, self.timeout = console, retries, timeout
        self.retry_count = 0
        # Keep raw bytes across wait/retry boundaries. Filtering ASCII 'C' as
        # a CRC request must not corrupt a new boot banner, and a partial
        # banner may arrive immediately before one packet wait expires.
        self._rx_window = bytearray(b"\n")

    def wait(self, accepted, timeout=None):
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        noise = bytearray()
        while time.monotonic() < deadline:
            value = self.console.serial.read(1)
            if value:
                self._rx_window.extend(value)
                del self._rx_window[:-256]
                restarted = YMODEM_RESTART_BANNER.search(self._rx_window)
                if restarted:
                    self.console.log("YMODEM RX restart " + bytes(self._rx_window).decode(
                        "ascii", errors="backslashreplace"))
                    raise RuntimeError("Board restarted during YMODEM (" +
                                       restarted.group(1).decode("ascii") +
                                       "); stopped without sending further packets or go")
                code = value[0]
                if code == CAN:
                    raise RuntimeError("Receiver cancelled YMODEM")
                if code in accepted:
                    if noise:
                        self.console.log("YMODEM RX " + bytes(noise).decode("ascii", errors="backslashreplace"))
                    return code
                if code not in (ACK, NAK, CRC):
                    noise.append(code)
        if noise:
            self.console.log("YMODEM RX " + bytes(noise).decode("ascii", errors="backslashreplace"))
        return None

    def acknowledged(self, data: bytes, label: str):
        for attempt in range(self.retries):
            self.console.write(data)
            result = self.wait((ACK, NAK))
            if result == ACK:
                return
            self.retry_count += 1
            self.console.log(f"YMODEM retry {label}, attempt {attempt + 1}: " + ("NAK" if result == NAK else "timeout"))
        raise RuntimeError(f"YMODEM retry limit reached: {label}")

    def send(self, name: str, data: bytes):
        metadata = name.encode("ascii") + b"\0" + str(len(data)).encode("ascii") + b"\0"
        if len(metadata) > 128:
            raise ValueError("YMODEM metadata exceeds 128 bytes")
        if self.wait((CRC, NAK), 30.0) != CRC:
            raise RuntimeError("Receiver did not request CRC-mode YMODEM")
        self.acknowledged(packet(0, metadata, 128), "header")
        # This U-Boot ACKs block 0 then immediately waits for block 1. It does
        # not send another 'C' until a timeout. Do not require a second 'C'.
        for index, offset in enumerate(range(0, len(data), 1024), 1):
            self.acknowledged(packet(index, data[offset:offset + 1024], 1024, 0x1a), f"block {index}")
            if index % 64 == 0 or offset + 1024 >= len(data):
                self.console.log(f"YMODEM {min(offset + 1024, len(data))}/{len(data)} bytes")
        # U-Boot ACKs EOT immediately (sometimes twice); other YMODEM peers
        # NAK the first EOT and ACK the second. Both use the same retry loop.
        self.acknowledged(bytes((EOT,)), "EOT")
        if self.wait((CRC, NAK)) != CRC:
            raise RuntimeError("Receiver did not request final YMODEM header")
        self.acknowledged(packet(0, b"", 128), "final header")
        self.console.log(f"YMODEM complete, retries={self.retry_count}")


def verify_download(console: Console, data: bytes, transfer_output: bytes):
    sizes = re.findall(rb"## Total Size\s*=\s*0x([0-9a-fA-F]+)\s*=\s*(\d+) Bytes", transfer_output)
    if not sizes or int(sizes[-1][0], 16) != len(data) or int(sizes[-1][1]) != len(data):
        raise RuntimeError("U-Boot reported a missing or incorrect received length; refusing go")
    environment = console.command("printenv filesize")
    found = re.findall(rb"(?:^|[\r\n])filesize=([0-9a-fA-F]+)\s*(?:\r?\n|$)", environment)
    if not found or int(found[-1], 16) != len(data):
        raise RuntimeError("U-Boot filesize does not match the binary; refusing go")
    output = console.command(f"crc32 {ADDRESS:x} {len(data):x}", 30.0)
    result = re.findall(rb"CRC32 for [^\r\n]*?==>\s*([0-9a-fA-F]{8})", output, re.IGNORECASE)
    expected = zlib.crc32(data) & 0xffffffff
    if not result or int(result[-1], 16) != expected:
        raise RuntimeError(f"Board CRC32 mismatch/missing, PC expected {expected:08x}; refusing go")
    console.log(f"VERIFIED length={len(data)} CRC32={expected:08x} SHA256={hashlib.sha256(data).hexdigest()}")


def upload(console: Console, path: Path, data: bytes, no_go=False):
    console.log(f"TX loady {ADDRESS:x}")
    console.write(f"loady {ADDRESS:x}\r".encode("ascii"))
    YModem(console).send("halley7m_regba.bin", data)
    output = console.read_prompt(30.0)
    verify_download(console, data, output)
    if no_go:
        console.log("Verified image left in RAM; --no-go selected")
    else:
        console.log(f"TX go {ADDRESS:x}")
        console.write(f"go {ADDRESS:x}\r".encode("ascii"))
        # The firmware never returns to U-Boot. Preserve its initial banner.
        console.drain(quiet=2.0, limit=10.0)


def self_test():
    class BootClock:
        def __init__(self):
            self.now = 0.0

        def monotonic(self):
            return self.now

        def sleep(self, seconds):
            self.now += seconds

    class BootSerial:
        def __init__(self, clock, events, fail_opens=1):
            self.clock, self.events = clock, list(events)
            self.fail_opens = fail_opens
            self.open_attempts = self.close_count = 0
            self.writes, self.open_settings = [], []
            self.port, self.baudrate, self.timeout = "FAKE52", 115200, 0.1
            self.write_timeout = 10
            self.xonxoff = self.rtscts = self.dsrdtr = False
            self.dtr = self.rts = False
            self.is_open = True
            self.received_since_open = bytearray()

        def read(self, size=1):
            assert self.is_open
            self.clock.now += 0.01
            if not self.events:
                return b""
            event = self.events[0]
            if isinstance(event, OSError):
                self.events.pop(0)
                raise event
            result = event[:size]
            self.events[0] = event[size:]
            if not self.events[0]:
                self.events.pop(0)
            self.received_since_open.extend(result)
            return result

        def write(self, data):
            assert self.is_open
            self.writes.append((data, bytes(self.received_since_open)))
            return len(data)

        def flush(self):
            pass

        def close(self):
            self.close_count += 1
            self.is_open = False

        def open(self):
            assert not self.is_open
            self.open_attempts += 1
            self.open_settings.append((self.port, self.baudrate, self.timeout,
                                       self.write_timeout, self.xonxoff, self.rtscts,
                                       self.dsrdtr, self.dtr, self.rts))
            if self.open_attempts <= self.fail_opens:
                raise PermissionError(13, "fake port is still re-enumerating")
            self.received_since_open.clear()
            self.is_open = True

    original_monotonic, original_sleep = time.monotonic, time.sleep
    try:
        # Exercise both a previously interrupted boot and a partial old banner.
        # A post-reopen prompt/suffix must not authorize any transmission.
        for old_banner, new_prefix, old_spaces in (
            (b"Hit any key", b"\nstale# \n", 1),
            (b"Hit any", b" key\nstale# \n", 0),
        ):
            clock = BootClock()
            time.monotonic, time.sleep = clock.monotonic, clock.sleep
            peer = BootSerial(clock, [old_banner, PermissionError(13, "ClearCommError"),
                                     new_prefix, b"new boot: Hit any key\nx2600# "])
            Console(peer, lambda _: None).wait_boot(timeout=5.0)
            assert peer.close_count == 1 and peer.open_attempts == 2
            assert len(peer.writes) == old_spaces + 1
            assert all(data == b" " for data, _ in peer.writes)
            assert b"new boot: Hit any key" in peer.writes[-1][1]
            assert all(settings == ("FAKE52", 115200, 0.1, 10, False,
                                    False, False, False, False)
                       for settings in peer.open_settings)

        # Reopen failures consume the original deadline; they never extend it
        # or fall through to an upload when no fresh boot banner was observed.
        clock = BootClock()
        time.monotonic, time.sleep = clock.monotonic, clock.sleep
        peer = BootSerial(clock, [OSError("disconnected")], fail_opens=1000)
        try:
            Console(peer, lambda _: None).wait_boot(timeout=0.35)
        except RuntimeError as exc:
            assert "no upload sent" in str(exc)
        else:
            raise AssertionError("Disconnected boot wait did not time out")
        assert not peer.writes and 0.35 <= clock.now <= 0.36

        # Reconnection belongs only to wait_boot. A command or an in-flight
        # YMODEM packet must propagate a disconnect without reopen/retransmit.
        for during_command in (True, False):
            peer = BootSerial(clock, [OSError("transfer disconnected")])
            console = Console(peer, lambda _: None)
            try:
                if during_command:
                    console.command("help loady", timeout=0.1)
                else:
                    YModem(console).acknowledged(packet(1, b"x", 128), "disconnect")
            except OSError:
                pass
            else:
                raise AssertionError("Command/transfer disconnect was hidden")
            assert peer.open_attempts == 0 and len(peer.writes) == 1

        # A boot marker can straddle packet wait/retry boundaries. One retry
        # before the marker is complete is allowed; recognition stops the loop
        # immediately, without cancellation/cleanup writes or another packet.
        clock = BootClock()
        time.monotonic, time.sleep = clock.monotonic, clock.sleep
        peer = BootSerial(clock, [], fail_opens=0)
        original_write = peer.write

        def fragmented_restart(data):
            count = original_write(data)
            if len(peer.writes) == 1:
                peer.events.append(b"\r\nU-Boot SPL 20")
            elif len(peer.writes) == 2:
                peer.events.append(b"13.07-00006 boot\r\n")
            else:
                raise AssertionError("Packet sent after complete restart banner")
            return count

        peer.write = fragmented_restart
        modem = YModem(Console(peer, lambda _: None), timeout=0.3)
        try:
            modem.acknowledged(packet(1, b"x", 128), "restart split across retry")
        except RuntimeError as exc:
            assert "Board restarted during YMODEM" in str(exc)
        else:
            raise AssertionError("Restart banner split across retries was ignored")
        assert len(peer.writes) == 2 and modem.retry_count == 1 and not peer.open_attempts

        # The rolling detector sees unfiltered CRC 'C' bytes and stays bounded;
        # normal CRC/ACK processing and the existing loady noise remain valid.
        clock = BootClock()
        time.monotonic, time.sleep = clock.monotonic, clock.sleep
        peer = BootSerial(clock, [b"loady noise:" + b"C" * 1024 + bytes((ACK,))])
        modem = YModem(Console(peer, lambda _: None))
        assert modem.wait((ACK,), timeout=20.0) == ACK
        assert len(modem._rx_window) == 256 and modem._rx_window[-2:] == bytes((CRC, ACK))
        assert not peer.writes
    finally:
        time.monotonic, time.sleep = original_monotonic, original_sleep

    class FakeSerial:
        def __init__(self, eot_nak=False, reject_first=True, header_c=False, restart_banner=None):
            self.pending = bytearray()
            self.state, self.next_block = "console", 1
            self.received, self.commands = bytearray(), []
            self.advertised = 0
            self.reject_first, self.eot_nak, self.header_c = reject_first, eot_nak, header_c
            self.rejected = self.eot_seen = False
            self.bad_crc = self.bad_length = False
            self.restart_banner = restart_banner
            self.writes = []

        def read(self, size=1):
            result = bytes(self.pending[:size])
            del self.pending[:size]
            return result

        def flush(self):
            pass

        def write(self, data):
            self.writes.append(data)
            if self.state == "restarted":
                raise AssertionError("Data/command sent after receiver restart")
            if self.state == "console":
                command = data.decode("ascii").strip()
                self.commands.append(command)
                response = command + "\r\n"
                if command.startswith("loady "):
                    self.state = "header"
                    response += "## Ready for binary (ymodem) download\r\n"
                    self.pending.extend(response.encode() + bytes((CRC,)))
                    return len(data)
                if command == "help loady":
                    response += "loady - load binary file over serial line (ymodem mode)\r\n"
                elif command == "md.l b0000000 6":
                    response += "b0000000: 9a0b5410 00000000 00000000 00000000    ....\r\nb0000010: 0300490d 04b0490d    ....\r\n"
                elif command == "md.l b000002c 1":
                    response += "b000002c: 80000002    ....\r\n"
                elif command == "printenv filesize":
                    response += f"filesize={self.advertised + int(self.bad_length):x}\r\n"
                elif command.startswith("crc32 "):
                    value = zlib.crc32(self.received[:self.advertised]) ^ int(self.bad_crc)
                    response += f"CRC32 for 80600000 ... 80600001 ==> {value:08x}\r\n"
                elif command.startswith("go "):
                    response += "ReGBA started\r\n"
                else:
                    raise AssertionError(f"Unexpected fake command {command}")
                self.pending.extend((response + "x2600m_halley7# ").encode())
                return len(data)
            if data == bytes((EOT,)):
                if self.eot_nak and not self.eot_seen:
                    self.pending.append(NAK)
                else:
                    self.pending.extend((ACK, ACK, CRC))
                    self.state = "final"
                self.eot_seen = True
                return 1
            length = 128 if data[0] == SOH else 1024
            assert len(data) == length + 5
            assert data[1] ^ data[2] == 255
            assert binascii.crc_hqx(data[3:-2], 0) == int.from_bytes(data[-2:], "big")
            if self.state == "header":
                assert data[1] == 0 and length == 128
                name, size = data[3:-2].split(b"\0")[:2]
                assert name == b"halley7m_regba.bin"
                self.advertised = int(size)
                self.state = "data"
                self.pending.append(ACK)
                if self.header_c:
                    self.pending.append(CRC)
            elif self.state == "data":
                if self.restart_banner is not None:
                    self.pending.extend(self.restart_banner)
                    self.state = "restarted"
                elif self.reject_first and not self.rejected:
                    self.rejected = True
                    self.pending.append(NAK)
                elif data[1] == (self.next_block & 255):
                    self.received.extend(data[3:-2])
                    self.next_block += 1
                    self.pending.append(ACK)
                else:
                    assert data[1] == ((self.next_block - 1) & 255)
                    self.pending.append(ACK)
            else:
                assert self.state == "final" and data[1] == 0 and not any(data[3:-2])
                self.state = "console"
                self.pending.append(ACK)
                self.pending.extend(f"\r\n## Total Size      = 0x{self.advertised:08x} = {self.advertised} Bytes\r\nx2600m_halley7# ".encode())
            return len(data)

    assert binascii.crc_hqx(b"123456789", 0) == 0x31c3
    assert packet(256, b"abc", 128)[1:3] == b"\0\xff"
    for eot_nak, header_c, length in ((False, False, 2065), (True, True, 257 * 1024 + 3)):
        fake = FakeSerial(eot_nak=eot_nak, header_c=header_c)
        console = Console(fake, lambda _: None)
        assert probe(console) == (1152000000, 1152000000, 1800000000, 600000000)
        data = bytes((i * 37 + 11) & 255 for i in range(length))
        console.write(f"loady {ADDRESS:x}\r".encode())
        modem = YModem(console, timeout=0.01)
        modem.send("halley7m_regba.bin", data)
        output = console.read_prompt(0.01)
        assert fake.received[:length] == data
        assert fake.received[length:] == bytes((0x1a,)) * ((-length) % 1024)
        assert modem.retry_count == 1 + int(eot_nak)
        verify_download(console, data, output)
        for field in ("bad_crc", "bad_length"):
            setattr(fake, field, True)
            try:
                verify_download(console, data, output)
            except RuntimeError:
                pass
            else:
                raise AssertionError(f"{field} was accepted")
            setattr(fake, field, False)
        try:
            verify_download(console, data, output.replace(str(length).encode(), b"1"))
        except RuntimeError:
            pass
        else:
            raise AssertionError("Incorrect transfer length was accepted")
        assert not any(command.startswith("go ") for command in fake.commands)

    # Restart during a real upload path must stop at the first data packet:
    # do not retransmit to the new shell, send EOT/CAN, verify, or run go.
    for banner in (
        b"\r\nU-Boot SPL 2013.07-00006-g96302ff087 (test)\r\n",
        b"\r\nU-Boot 2013.07-00006-g96302ff087 (test)\r\n",
        b"\r\nPLAYER_BOOT core=mips rtos=FreeRTOS entry=80600000\r\n",
        b"\r\nAI_BOOT core=mips rtos=FreeRTOS entry=80600000\r\n",
    ):
        fake = FakeSerial(reject_first=False, restart_banner=banner)
        try:
            upload(Console(fake, lambda _: None), Path("fake.bin"), b"x" * 2048)
        except RuntimeError as exc:
            assert "Board restarted during YMODEM" in str(exc)
        else:
            raise AssertionError("Receiver restart during upload was ignored")
        assert len(fake.writes) == 3  # loady, metadata, first data packet only.
        assert fake.commands == [f"loady {ADDRESS:x}"] and not fake.received
        assert not any(data in (bytes((EOT,)), bytes((CAN,))) or data.startswith(b"go ")
                       for data in fake.writes)
    print("PASS: boot disconnect/re-enumeration, failed reopen/recovery with original settings and inactive DTR/RTS, stale banner/prompt rejection, fixed deadline/no upload, command/YMODEM disconnect propagation; YMODEM restart abort/no retransmit/go, raw bounded banner window across reads/retries; CRC16, 128/1K framing, header with/without C, NAK retry, one/two EOT, sequence wrap, exact length/padding, clocks, board CRC32/filesize rejection; no serial port opened.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--probe", action="store_true", help="Read-only U-Boot command/clock probe (default)")
    mode.add_argument("--execute", action="store_true", help="Upload to RAM, verify, and go unless --no-go")
    mode.add_argument("--self-test", action="store_true", help="Fake serial regression; no hardware")
    parser.add_argument("--port", default="COM52")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--file", type=Path, default=ROOT / "build/regba-Release/halley7m_regba.bin")
    parser.add_argument("--no-go", action="store_true")
    parser.add_argument("--wait-boot", action="store_true",
                        help="Wait for a physical normal reset and interrupt autoboot before probing")
    parser.add_argument("--reboot-linux", action="store_true",
                        help="Verify a running Linux shell, reboot it and interrupt U-Boot on the same UART")
    parser.add_argument("--reboot-player", action="store_true",
                        help="Verify player diagnostics, watchdog reboot and interrupt U-Boot on the same UART")
    parser.add_argument("--reboot-ai", action="store_true",
                        help="Verify AI diagnostics, watchdog reboot and interrupt U-Boot on the same UART")
    parser.add_argument("--log", type=Path)
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if args.no_go and not args.execute:
        parser.error("--no-go requires --execute")
    if sum((args.reboot_linux, args.reboot_player, args.reboot_ai, args.wait_boot)) > 1:
        parser.error("Choose --reboot-linux, --reboot-player, --reboot-ai or --wait-boot")
    data = args.file.read_bytes() if args.execute else b""
    if args.execute and not 0 < len(data) <= MAX_IMAGE:
        parser.error("BIN must be non-empty and fit the fixed 6MiB RAM upload window")
    log_path = args.log or ROOT / "build" / ("ramload-" + datetime.now().strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:6] + ".log")
    log = Log(log_path)
    serial_port = None
    try:
        import serial
        log(f"Serial {args.port} {args.baud} 8N1; log={log_path}")
        serial_port = serial.Serial(port=None, baudrate=args.baud, timeout=0.1, write_timeout=10,
                                    xonxoff=False, rtscts=False, dsrdtr=False)
        serial_port.dtr = serial_port.rts = False
        serial_port.port = args.port
        serial_port.open()
        console = Console(serial_port, log)
        if args.reboot_linux:
            console.reboot_linux()
        elif args.reboot_player:
            console.reboot_player()
        elif args.reboot_ai:
            console.reboot_ai()
        elif args.wait_boot:
            console.wait_boot()
        else:
            console.drain()
        rates = probe(console)
        if args.execute:
            if rates != (1152000000, 1152000000, 1800000000, 600000000):
                raise RuntimeError("Board clocks do not match the ReGBA normal boot profile; no upload sent")
            log(f"BIN={args.file.resolve()} bytes={len(data)} SHA256={hashlib.sha256(data).hexdigest()}")
            upload(console, args.file, data, args.no_go)
        else:
            log("Probe complete; no RAM upload, go, NOR write, or saveenv sent")
        return 0
    except Exception as exc:
        log(f"ERROR: {exc}")
        return 1
    finally:
        if serial_port is not None:
            serial_port.close()
        log.close()


if __name__ == "__main__":
    raise SystemExit(main())

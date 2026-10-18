"""Exercise the real soak Console with byte chunks; never open a serial port."""
from collections import deque
import importlib.util
import io
from pathlib import Path


spec = importlib.util.spec_from_file_location(
    "regba_soak_console_test", Path(__file__).resolve().parents[1] / "scripts/soak-regba.py")
soak = importlib.util.module_from_spec(spec)
spec.loader.exec_module(soak)


class FakeUART:
    def __init__(self, chunks=()):
        self.chunks = deque(chunks)
        self.writes = []
        self.flushes = 0

    @property
    def in_waiting(self):
        return len(self.chunks[0]) if self.chunks else 0

    def read(self, size):
        if not self.chunks:
            raise AssertionError("Console read past the expected input")
        chunk = self.chunks.popleft()
        if len(chunk) > size:
            self.chunks.appendleft(chunk[size:])
        return chunk[:size]

    def write(self, data):
        self.writes.append(data)

    def flush(self):
        self.flushes += 1


def check_send():
    uart = FakeUART()
    console = soak.Console(uart, io.BytesIO())
    command = "load /GBA/ROMS/口袋妖怪 绿宝石.gba"
    console.send(command)
    assert uart.writes == [command.encode("utf-8") + b"\r"]
    assert uart.flushes == 1


def check_chunks():
    expected = ["游戏列表", "/GBA/ROMS/口袋妖怪 绿宝石.gba", "读取完成 bytes=8388608"]
    raw = ("\r\n".join(expected) + "\r\n").encode("utf-8")
    for chunks in ([raw], [raw[i:i + 1] for i in range(len(raw))],
                   [raw[i:i + 5] for i in range(0, len(raw), 5)]):
        output = io.BytesIO()
        console = soak.Console(FakeUART(chunks), output)
        lines = console.lines(10)
        assert [next(lines) for _ in expected] == expected
        lines.close()
        assert output.getvalue() == raw

    # Finish one lines() call while the decoder retains the first byte of
    # the next Chinese character, as can happen at a polling boundary.
    text = "汉字路径"
    encoded = text.encode("utf-8")
    console = soak.Console(FakeUART([b"first\r\n" + encoded[:1], encoded[1:] + b"\n"]), io.BytesIO())
    first = console.lines(10)
    assert next(first) == "first"
    first.close()
    following = console.lines(10)
    assert next(following) == text
    following.close()


if __name__ == "__main__":
    check_send()
    check_chunks()
    print("console UTF-8 PASS: exact Chinese command bytes, single/multiple-byte RX chunks and decoder state across polls")

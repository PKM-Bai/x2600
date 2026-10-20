#!/usr/bin/env python3
"""Exercise the actual runner with fragmented fake UART and a virtual clock."""
import importlib.util
import io
import json
import math
from pathlib import Path
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("runner", ROOT / "scripts/run-player-board.py")
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)


class FakeBoard:
    def __init__(self, fault=None):
        self.now = 0.0
        self.output = bytearray()
        self.commands = []
        self.play_started = self.measure_started = self.ui_started = None
        self.seconds, self.label, self.request, self.fault = 600, "test", 0, fault
        self.media = "0:/multimedia/video/acceptance-30min.avi"
        self.stopped = True
        self.ui_stopped = False
        self.measure_stopped = False
        self.fault_emitted = False
        self.summary_induced_drops = 0
        self.summary_audio_active = []
        self.input = bytearray()

    def pause(self, seconds):
        self.now += seconds

    def clock(self):
        return self.now

    @property
    def in_waiting(self):
        return min(len(self.output), 97)

    def read(self, count):
        if self.fault == "serial" and self.now > 90:
            raise OSError("synthetic disconnected UART")
        if self.output:
            count = min(count, 17)  # Split UTF-8, marker names and player prompt.
            result = bytes(self.output[:count])
            del self.output[:count]
            self.now += 0.0001
            return result
        self.now += 0.1
        if self.fault == "async-gui" and self.now > 90 and not self.fault_emitted:
            self.output.extend(b"GUI_ERROR result=-5 surfaces=pinned\r\n")
            self.fault_emitted = True
        return b""

    def flush(self):
        pass

    def playback_seconds(self):
        return 0 if self.play_started is None else self.now - self.play_started

    def media_ended(self):
        return self.playback_seconds() >= (100 if self.fault == "early-eof" else 1800)

    def measure_elapsed(self):
        if self.measure_started is None:
            return 0
        elapsed = self.now - self.measure_started
        if self.play_started is not None:
            elapsed = min(elapsed, self.play_started + 1800 - self.measure_started)
        return max(0, min(self.seconds, elapsed))

    def summary(self):
        elapsed = self.measure_elapsed()
        ended = self.media_ended()
        reason = "eof" if ended else "duration" if elapsed >= self.seconds else "manual" if self.measure_stopped else "none"
        active = int(reason == "none")
        records = math.ceil(elapsed)
        dropped = int(self.fault == "drop" and self.playback_seconds() > 90)
        late = int(self.fault == "late" and self.playback_seconds() > 90)
        return (f"MEASURE_SUMMARY label={self.label} active={active} reason={reason} records={records}/1860 "
            f"elapsed_ms={round(elapsed * 1000)} target_s={self.seconds} samples={round(elapsed*48000)} "
            f"decoded={round(elapsed*30)} displayed={round(elapsed*30)-dropped} dropped={dropped} late={late} underruns=0 dma_errors=0 dma_timeouts=0")

    def sequence(self):
        elapsed = 0 if self.ui_started is None else max(0, self.now - self.ui_started)
        aborted = self.fault == "physical" and elapsed >= 90
        complete = self.ui_started is not None and elapsed >= self.seconds
        reason = 4 if aborted else 2 if complete else 3 if self.ui_stopped else 1 if self.ui_started is not None else 0
        active = int(reason == 1)
        cycles = len([second for second in range(30, self.seconds, 120) if second + 4 <= elapsed])
        return (f"UI_SEQUENCE active={active} reason={reason} phase=0 cycles_started={cycles} cycles_completed={cycles} "
                f"elapsed_ms={round(min(elapsed,self.seconds)*1000)} duration_ms={self.seconds*1000} keys=000 aborted={int(aborted)} cancel_may_emit_short=0")

    def write(self, payload):
        self.input.extend(payload)
        while b"\r" in self.input:
            end = self.input.index(b"\r") + 1
            command = bytes(self.input[:end]); del self.input[:end]
            self.write_command(command)
        return len(payload)

    def write_command(self, payload):
        command = payload.decode("utf-8").strip()
        self.commands.append((self.now, command))
        response = ""
        if command in ("stop", "play " + self.media):
            self.request += 1
            rejected = command.startswith("play") and self.fault == "reject-play"
            response = f"SESSION_POST request={self.request} kind={1 if command.startswith('play') else 5} result={-16 if rejected else 0}"
            if command == "stop":
                if self.fault == "stop-drop" and not self.stopped and self.measure_elapsed() >= self.seconds:
                    self.summary_induced_drops += 1
                self.stopped = True
            elif not rejected:
                self.stopped = False
                self.play_started = self.now
        elif command == "session":
            elapsed = min(1800, self.playback_seconds())
            state = 0 if self.stopped else 4 if self.media_ended() else 2
            dropped = int(self.fault == "drop" and elapsed > 90) + self.summary_induced_drops
            late = int(self.fault == "late" and elapsed > 90)
            request = self.request - int(self.fault == 'stale-stop-status')
            response = (f"SESSION_STATUS session=1 state={state} kind=2 samples={int(elapsed*48000)}/86400000 decoded={int(elapsed*30)} "
                f"displayed={int(elapsed*30)-dropped} dropped={dropped} underruns=0 volume=7 pinned=0 error=0 "
                f"request={request} result=0 path={self.media} detail=\r\n"
                f"SESSION_QUEUES late={late} max_late_us=1000 video_packets=2 audio_packets=3 pcm_queued=3200")
        elif command == "audio":
            volume = 10 if self.fault == "volume" else 7
            completed = min(86400000, round(self.playback_seconds() * 48000))
            response = (f"AUDIO_STATUS session=1 state=2 active={int(not self.stopped)} total=86400000 written={completed} completed={completed} "
                f"underruns=0 volume={volume} clock_valid=1 error=0 dma_errors=0 dma_timeouts=0 queue_underruns=0 fifo_underruns=0")
        elif command == "gui":
            page = 2 if self.fault == "wrong-page" else 1
            response = f"GUI_STATUS page={page} menu=0 generation=3 rendered=30 presented=30 dropped_repeats=0 dropped_actions=0 error=0 stack_free_words=3000"
        elif command.startswith("measure begin "):
            _, _, seconds, self.label = command.split(" ")
            self.seconds = int(seconds)
            response = f"MEASURE_BEGIN result={-16 if self.fault == 'reject-measure' else 0}"
            if self.fault != "reject-measure":
                self.measure_started = self.now
        elif command.startswith("ui sequence start "):
            self.ui_started = self.now
            self.ui_stopped = False
            response = "no response marker" if self.fault == "timeout" else "UI_SEQUENCE_START result=0"
        elif command == "ui sequence stop":
            self.ui_stopped = True
            response = self.sequence()
        elif command == "ui sequence":
            response = self.sequence()
        elif command in ("measure summary", "measure stop"):
            self.summary_audio_active.append(not self.stopped)
            if self.fault == "summary-drop" and not self.stopped and not self.media_ended() and self.measure_elapsed() >= self.seconds:
                self.summary_induced_drops += 1
            if command == "measure stop":
                self.measure_stopped = True
            response = self.summary()
        elif command == "measure dump":
            count = math.ceil(self.measure_elapsed())
            actual = count - int(self.fault == "truncated-csv")
            lines = ["label,elapsed_ms,session,audio_session,amp_session,flags,state"]
            for index in range(actual):
                flags = 15 if index + 1 == count else 14
                milliseconds = min((index + 1) * 1000, round(self.measure_elapsed() * 1000))
                lines.append(f'"{self.label}",{milliseconds},1,1,7,{flags},2')
            response = "\r\n".join(lines) + "\r\nMEASURE_DUMP result=0"
        else:
            response = "Commands: unknown"
        # Actual newlib UART: printf("player> ") remains buffered until the
        # next command's newline. No trailing prompt reaches the wire now.
        self.output.extend(("player> " + command + "\r\n" + response + "\r\n").encode("utf-8"))
        return len(payload)


def exercise(seconds=600, fault=None):
    board, log, events = FakeBoard(fault), io.BytesIO(), []
    args = SimpleNamespace(seconds=seconds, label="amp30" if seconds == 1800 else "mips10", media=board.media)
    console = RUNNER.Console(board, log, board.clock, board.pause)
    runner = RUNNER.Runner(console, args, events.append)
    error = None
    try:
        runner.run()
    except (RUNNER.BoardError, OSError) as caught:
        error = caught
        runner.cleanup()
    console.close_log()
    return board, log.getvalue().decode(), events, error


class RunnerTests(unittest.TestCase):
    def test_long_utf8_command_survives_realistic_64_byte_fifo_and_one_byte_ticks(self):
        class SlowUART:
            def __init__(self):
                self.now, self.next_tick = 0.0, 0.001
                self.fifo, self.line, self.output = bytearray(), bytearray(), bytearray()
                self.writes, self.pauses, self.commands, self.dropped = [], [], [], 0

            @property
            def in_waiting(self):
                return len(self.output)

            def advance(self, seconds):
                end = self.now + seconds
                while self.next_tick <= end:
                    if self.fifo:
                        byte = self.fifo.pop(0)
                        if byte == 13:
                            command = self.line.decode('utf-8')
                            self.commands.append(command)
                            self.output.extend((command + '\r\nSESSION_POST request=1 kind=1 result=0\r\n').encode('utf-8'))
                            self.line.clear()
                        else:
                            self.line.append(byte)
                    self.next_tick += 0.001
                self.now = end

            def pause(self, seconds):
                self.pauses.append(seconds)
                self.advance(seconds)

            def write(self, payload):
                self.writes.append(bytes(payload))
                accepted = min(len(payload), 64 - len(self.fifo))
                self.fifo.extend(payload[:accepted]); self.dropped += len(payload) - accepted
                return len(payload)  # Host write succeeds even when board RX overflows.

            def flush(self):
                pass

            def read(self, count):
                self.advance(0.001)
                data = bytes(self.output[:min(count, 9)]); del self.output[:len(data)]
                return data

        path = 'play 0:/multimedia/video/片源_测试_' + '长路径' * 12 + '.mp4'
        uart, log = SlowUART(), io.BytesIO()
        console = RUNNER.Console(uart, log, lambda: uart.now, uart.pause)
        record, transcript = console.command(path, 'SESSION_POST')
        self.assertEqual(record['kind'], '1')
        self.assertEqual(uart.commands, [path])
        self.assertEqual(uart.dropped, 0)
        self.assertTrue(all(len(block) <= 16 for block in uart.writes))
        self.assertTrue(all(0.020 <= pause <= 0.032 for pause in uart.pauses))
        self.assertEqual(len(uart.pauses), len(uart.writes) - 1)
        self.assertEqual(b''.join(uart.writes), path.encode('utf-8') + b'\r')
        self.assertEqual(b''.join(uart.writes).count(b'\r'), 1)
        self.assertIn('SESSION_POST request=1 kind=1 result=0', transcript)
        self.assertIn(path, log.getvalue().decode('utf-8'))

    def test_cleanup_stop_ignores_wrong_kind_and_stale_request(self):
        class ReplyUART:
            def __init__(self):
                self.now, self.output = 0.0, bytearray()

            @property
            def in_waiting(self):
                return len(self.output)

            def write(self, payload):
                self.output.extend(b'SESSION_POST request=7 kind=1 result=0\r\n'
                                   b'SESSION_POST request=6 kind=5 result=0\r\n'
                                   b'SESSION_POST request=8 kind=5 result=0\r\n')
                return len(payload)

            def flush(self):
                pass

            def read(self, count):
                self.now += 0.001
                data = bytes(self.output[:count]); del self.output[:count]
                return data

        uart = ReplyUART()
        console = RUNNER.Console(uart, io.BytesIO(), lambda: uart.now)
        record, _ = console.command('stop', 'SESSION_POST')
        self.assertEqual(record['request'], '8')
        self.assertEqual(console.last_stop_request, 8)
        # The original failure concatenated STOP onto a partial PLAY path,
        # producing only a PLAY(kind=1) reply. Never report that as STOP ACK.
        bad = ReplyUART()
        def concatenated_play(payload):
            bad.output.extend(b'SESSION_POST request=7 kind=1 result=0\r\n')
            return len(payload)
        bad.write = concatenated_play
        console = RUNNER.Console(bad, io.BytesIO(), lambda: bad.now)
        with self.assertRaises(RUNNER.BoardError):
            console.command('stop', 'SESSION_POST', timeout=0.02)
        self.assertIsNone(console.last_stop_request)

    def test_cleanup_does_not_accept_old_idle_request(self):
        board, _, events, error = exercise(fault='stale-stop-status')
        self.assertIsNotNone(error)
        self.assertFalse(events[-1]['idle_confirmed'])
        self.assertFalse(any(command.startswith('play ') for _, command in board.commands))

    def test_actual_idle_uart_fixture_has_no_trailing_prompt(self):
        fixture = json.loads((ROOT / "tests/player_runner_uart_fixture.json").read_text(encoding="utf-8"))

        class RecordedUART:
            def __init__(self):
                self.now, self.output, self.next = 0.0, bytearray(), 0
                self.input = bytearray()

            @property
            def in_waiting(self):
                return len(self.output)

            def write(self, data):
                self.input.extend(data)
                if b"\r" not in self.input:
                    return len(data)
                command = bytes(self.input); self.input.clear()
                recorded = fixture["responses"][self.next]
                if command.decode().strip() != recorded["command"]:
                    raise AssertionError(command)
                self.output.extend(recorded["wire"].encode("utf-8"))
                self.next += 1
                return len(data)

            def flush(self):
                pass

            def read(self, count):
                self.now += 0.005
                result = bytes(self.output[:1])
                del self.output[:1]
                return result

        uart, output = RecordedUART(), io.BytesIO()
        console = RUNNER.Console(uart, output, lambda: uart.now)
        for recorded in fixture["responses"]:
            with self.subTest(command=recorded["command"]):
                self.assertFalse(recorded["wire"].endswith("player> "))
                result, transcript = console.command(recorded["command"], recorded["marker"])
                self.assertEqual(transcript, recorded["wire"])
                if recorded["marker"] == "SESSION_STATUS":
                    self.assertEqual(result["queues"]["late"], "0")
                    self.assertIn("SESSION_QUEUES", transcript)
        self.assertEqual(uart.next, 4)

    def test_ten_minutes_order_quiet_checks_and_raw_csv(self):
        board, log, events, error = exercise()
        self.assertIsNone(error, str(error))
        commands = [command for _, command in board.commands]
        self.assertEqual(commands[:6], ["stop", "ui sequence stop", "session", "audio", "play " + board.media, "session"])
        self.assertLess(commands.index("measure begin 600 mips10"), commands.index("ui sequence start 600"))
        self.assertGreater(commands.index("measure summary"), len(commands) - 1 - commands[::-1].index("stop"))
        self.assertEqual(commands[-5:], ["stop", "session", "audio", "measure summary", "measure dump"])
        self.assertEqual(board.summary_audio_active, [False])
        self.assertFalse(any(command in ("metrics", "display", "volume 7", "reboot", "tone") for command in commands))
        self.assertEqual(sum(event["event"] == "progress" for event in events), 9)
        self.assertIn('\nlabel,elapsed_ms,session,', log)
        self.assertIn('\n"mips10",1000,', log)
        self.assertIn('[host rx ', log)
        self.assertEqual(events[-1]["event"], "complete")

    def test_full_media_eof_about_three_seconds_before_measurement_target(self):
        board, log, events, error = exercise(1800)
        self.assertIsNone(error, str(error))
        self.assertIn("state=4 kind=2 samples=86400000/86400000", log)
        self.assertIn("reason=eof", log)
        self.assertEqual(events[-1]["event"], "complete")
        self.assertEqual(sum(event["event"] == "progress" for event in events), 29)

    def test_rejected_play_never_retries_media_or_starts_measurement(self):
        board, _, _, error = exercise(fault="reject-play")
        self.assertIsNotNone(error)
        commands = [command for _, command in board.commands]
        self.assertEqual(sum(command.startswith("play ") for command in commands), 1)
        self.assertFalse(any(command.startswith("measure begin") for command in commands))
        self.assertIn("stop", commands[-7:])

    def test_invalid_preconditions_abort_without_changing_volume_or_page(self):
        for fault in ("volume", "wrong-page", "reject-measure"):
            with self.subTest(fault=fault):
                board, _, _, error = exercise(fault=fault)
                self.assertIsNotNone(error)
                self.assertFalse(any(command.startswith("volume ") for _, command in board.commands))
                self.assertFalse(any(command.startswith("ui sequence start") for _, command in board.commands))

    def test_physical_key_async_fault_drop_and_early_eof_cleanup(self):
        for fault in ("physical", "async-gui", "early-eof"):
            with self.subTest(fault=fault):
                board, _, events, error = exercise(1800, fault)
                self.assertIsNotNone(error)
                self.assertFalse(any(event["event"] == "complete" for event in events))
                commands = [command for _, command in board.commands]
                self.assertIn("measure stop", commands)
                self.assertEqual(commands[-3:], ["session", "audio", "measure stop"])
                self.assertTrue(events[-1]["idle_confirmed"])

    def test_serial_disconnect_attempts_cleanup_without_reopen(self):
        board, _, events, error = exercise(fault="serial")
        self.assertIsNotNone(error)
        self.assertIn("stop", [command for _, command in board.commands[-5:]])
        self.assertEqual(events[-1]["event"], "cleanup-complete")

    def test_truncated_csv_is_failure_even_with_dump_result_zero(self):
        _, _, events, error = exercise(fault="truncated-csv")
        self.assertIsNotNone(error)
        self.assertIn("incomplete measurement dump", str(error))
        self.assertFalse(any(event["event"] == "complete" for event in events))

    def test_timeout_attempts_stop_and_reports_invalid(self):
        board, _, events, error = exercise(fault="timeout")
        self.assertIsNotNone(error)
        self.assertIn("timeout", str(error))
        self.assertTrue(events[-1]["idle_confirmed"])
        self.assertIn("measure stop", [command for _, command in board.commands])

    def test_drop_or_late_preserves_full_run_for_video_review(self):
        for fault in ("drop", "late"):
            with self.subTest(fault=fault):
                _, _, events, error = exercise(1800, fault)
                self.assertIsNone(error, str(error))
                self.assertTrue(events[-1]["valid"])
                self.assertEqual(events[-1]["video_acceptance"], "needs-review")
                self.assertEqual(events[-1]["dropped" if fault == "drop" else "late"], 1)

    def test_completed_window_summary_does_not_stall_playing_audio(self):
        # This model makes the old ordering visibly wrong: a long summary
        # while PLAYING causes a real drop after the measurement has frozen.
        old = FakeBoard("summary-drop")
        old.play_started, old.measure_started, old.now, old.stopped = 0, 3, 604, False
        old.write(b"measure summary\r")
        self.assertEqual(old.summary_induced_drops, 1)
        self.assertIn("dropped=0", old.summary())  # Frozen measurement remains valid.
        board, _, events, error = exercise(fault="summary-drop")
        self.assertIsNone(error, str(error))
        self.assertEqual(board.summary_audio_active, [False])
        self.assertEqual(board.summary_induced_drops, 0)
        self.assertEqual(events[-1]["measurement_dropped"], 0)

    def test_post_window_stop_drop_is_retained_separately(self):
        _, _, events, error = exercise(fault="stop-drop")
        self.assertIsNone(error, str(error))
        self.assertTrue(events[-1]["collection_complete"])
        self.assertEqual(events[-1]["measurement_dropped"], 0)
        self.assertEqual(events[-1]["dropped"], 0)
        self.assertEqual(events[-1]["post_stop_dropped"], 1)


if __name__ == "__main__":
    unittest.main()

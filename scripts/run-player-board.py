#!/usr/bin/env python3
"""Run one explicit 600/1800s player measurement; no reboot, flash or media retry."""
import argparse
import csv
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import sys
import time


class BoardError(RuntimeError):
    pass


def fields(line):
    matches = list(re.finditer(r'(?:^|\s)([A-Za-z_][A-Za-z_0-9]*)=', line))
    return {match[1]: line[match.end():matches[index + 1].start() if index + 1 < len(matches) else len(line)].strip()
            for index, match in enumerate(matches)}


def value(record, name):
    try:
        return int(record[name])
    except (ValueError, KeyError) as error:
        raise BoardError(f"missing/invalid {name} in {record}") from error


class Console:
    def __init__(self, uart, log, clock=time.monotonic):
        self.uart, self.log, self.clock = uart, log, clock
        self.started = clock()
        self.pending = bytearray()

    def stamp(self):
        return f"[{datetime.now(timezone.utc).isoformat(timespec='milliseconds')} +{self.clock() - self.started:.3f}s] "

    def host(self, text):
        self.log.write((self.stamp() + "[host " + text + "]\n").encode("utf-8"))
        self.log.flush()

    def receive(self, ignore_faults=False):
        data = self.uart.read(min(self.uart.in_waiting or 1, 65536))
        if not data:
            return b""
        self.pending.extend(data)
        while b"\n" in self.pending:
            index = self.pending.index(b"\n") + 1
            line = bytes(self.pending[:index])
            del self.pending[:index]
            self.log.write(("[host rx " + self.stamp().strip() + "]\n").encode("ascii"))
            self.log.write(line)  # Preserve the actual UART line, including CSV.
            self.log.flush()
            text = line.decode("utf-8", errors="replace")
            if not ignore_faults and any(marker in text for marker in
                ("AMP_FAIL ", "AMP_FORCE_SILENCE ", "AMP_RECOVERY_REQUIRED ", "GUI_ERROR ", "PANIC:", "assertion failed")):
                raise BoardError("asynchronous board fault: " + text.strip())
        return data

    def collect_until(self, deadline, ignore_faults=False):
        while self.clock() < deadline:
            self.receive(ignore_faults)

    def command(self, command, marker, timeout=5, ignore_faults=False):
        self.host("command: " + command)
        self.uart.write(command.encode("utf-8") + b"\r")
        self.uart.flush()
        deadline, received, tail = self.clock() + timeout, bytearray(), b""
        # newlib stdout is line buffered: the trailing "player> " can remain
        # unsent until the NEXT command's newline. A complete response line,
        # not a prompt, is the protocol boundary. Never accept partial fields.
        pattern = re.compile(r"\b" + re.escape(marker) + r" ([^\r\n]*)\r?\n")
        while self.clock() < deadline:
            data = self.receive(ignore_faults)
            received.extend(data)
            tail = (tail + data)[-8192:]
            text = tail.decode("utf-8", errors="replace")
            if "PROBE_BUSY " in text or "Commands:" in text:
                raise BoardError(f"board refused {command!r}: {text.strip()}")
            match = pattern.search(text)
            if match:
                result = fields(match[1])
                if marker == "SESSION_STATUS":
                    queue = re.search(r"\bSESSION_QUEUES ([^\r\n]*)\r?\n", text[match.end():])
                    if not queue:
                        continue
                    result["queues"] = fields(queue[1])
                return result, received.decode("utf-8", errors="replace")
        raise BoardError(f"timeout waiting for complete {marker} response after {command!r}")

    def close_log(self):
        if self.pending:
            self.log.write(("[host rx-partial " + self.stamp().strip() + "]\n").encode("ascii"))
            self.log.write(bytes(self.pending) + b"\n")
            self.pending.clear()
        self.log.flush()


class Runner:
    def __init__(self, console, args, progress=None):
        self.console, self.args = console, args
        self.progress = progress or (lambda event: print(json.dumps(event, ensure_ascii=False), flush=True))
        self.measure_started = self.sequence_started = False
        self.session_id = None
        self.video_observed_issue = False

    def event(self, name, **details):
        self.console.host("event: " + json.dumps(dict(event=name, **details), ensure_ascii=False))
        self.progress(dict(event=name, host_elapsed_s=round(self.console.clock() - self.console.started, 3), **details))

    def query(self, command, marker, **kwargs):
        return self.console.command(command, marker, **kwargs)[0]

    def post(self, command):
        record = self.query(command, "SESSION_POST")
        if value(record, "result"):
            raise BoardError(f"command rejected: {command}: {record}")

    def idle(self, ignore_faults=False):
        deadline = self.console.clock() + 12
        while self.console.clock() < deadline:
            state = self.query("session", "SESSION_STATUS", ignore_faults=ignore_faults)
            if value(state, "state") == 0:
                audio = self.query("audio", "AUDIO_STATUS", ignore_faults=ignore_faults)
                if value(audio, "active") == 0 and not value(state, "pinned"):
                    return state, audio
            self.console.collect_until(min(deadline, self.console.clock() + 0.5), ignore_faults)
        raise BoardError("STOP did not confirm IDLE and inactive audio")

    @staticmethod
    def clean_session(state):
        for name in ("error", "pinned", "underruns"):
            if value(state, name):
                raise BoardError(f"session {name} is nonzero: {state}")
        if value(state, "volume") != 7:
            raise BoardError("session volume changed from the approved 7%")

    @staticmethod
    def clean_audio(audio):
        for name in ("error", "underruns", "dma_errors", "dma_timeouts", "queue_underruns", "fifo_underruns"):
            if value(audio, name):
                raise BoardError(f"audio {name} is nonzero: {audio}")
        if value(audio, "volume") != 7:
            raise BoardError("audio volume changed from the approved 7%")
        if value(audio, "active") != 1:
            raise BoardError("audio session became inactive before STOP")

    @staticmethod
    def clean_gui(gui, require_playback=False):
        if value(gui, "error") or (require_playback and (value(gui, "page") != 1 or value(gui, "menu"))):
            raise BoardError(f"GUI not in the required clean playback state: {gui}")

    @staticmethod
    def clean_sequence(sequence):
        if value(sequence, "aborted") or value(sequence, "cancel_may_emit_short"):
            raise BoardError(f"UI sequence invalidated (physical key/GUI/timing abort): {sequence}")
        if value(sequence, "reason") not in (1, 2):
            raise BoardError(f"UI sequence is not running or normally complete: {sequence}")

    def check_running(self, state, elapsed):
        self.clean_session(state)
        if value(state, "dropped") or value(state.get("queues", {}), "late"):
            self.video_observed_issue = True
        session = value(state, "session")
        if self.session_id is None:
            self.session_id = session
        elif session != self.session_id:
            raise BoardError("media session changed during the formal run")
        if value(state, "kind") != 2 or state.get("path") != self.args.media:
            raise BoardError(f"unexpected media/session path: {state}")
        if value(state, "state") == 2:
            return
        if value(state, "state") == 4 and self.args.seconds == 1800 and elapsed >= 1795:
            return
        raise BoardError(f"unexpected/early media state at {elapsed:.2f}s: {state}")

    def dump(self, summary):
        self.event("dump-start")
        record, text = self.console.command("measure dump", "MEASURE_DUMP", timeout=180)
        if value(record, "result"):
            raise BoardError(f"measurement dump rejected: {record}")
        header, rows = None, []
        for line in text.splitlines():
            if line.startswith("label,elapsed_ms,session,"):
                if header is not None:
                    raise BoardError("duplicate CSV header in one dump")
                header = next(csv.reader([line]))
            elif line.startswith('"') and header:
                values = next(csv.reader([line], strict=True))
                if len(values) != len(header) or values[0] != self.args.label:
                    raise BoardError("damaged or unexpected measurement CSV row")
                rows.append(dict(zip(header, values)))
        expected = int(summary.get("records", "invalid").split("/")[0])
        if not rows or len(rows) != expected or not int(rows[-1]["flags"]) & 1:
            raise BoardError(f"incomplete measurement dump: rows={len(rows)}, declared={expected}")
        times = [int(row["elapsed_ms"]) for row in rows]
        if any(right <= left for left, right in zip(times, times[1:])) or any(int(row["flags"]) & 1 for row in rows[:-1]):
            raise BoardError("measurement CSV has duplicate/backward times or an intermediate FINAL")
        self.event("dump-complete", records=len(rows))

    def run(self):
        self.event("prepare", seconds=self.args.seconds, label=self.args.label, media=self.args.media)
        self.post("stop")
        self.query("ui sequence stop", "UI_SEQUENCE")
        self.idle()
        self.post("play " + self.args.media)
        self.console.collect_until(self.console.clock() + 3)
        state = self.query("session", "SESSION_STATUS")
        audio = self.query("audio", "AUDIO_STATUS")
        gui = self.query("gui", "GUI_STATUS")
        self.check_running(state, 0)
        self.clean_audio(audio)
        self.clean_gui(gui, require_playback=True)
        begin = self.query(f"measure begin {self.args.seconds} {self.args.label}", "MEASURE_BEGIN")
        if value(begin, "result"):
            raise BoardError(f"measurement start rejected: {begin}")
        self.measure_started = True
        sequence = self.query(f"ui sequence start {self.args.seconds}", "UI_SEQUENCE_START")
        if value(sequence, "result"):
            raise BoardError(f"UI sequence start rejected: {sequence}")
        self.sequence_started = True
        started = self.console.clock()
        self.event("running")
        next_status = 60
        # The media started ~3s before the recorder; 1800s EOF may precede the
        # measurement/UI target. Keep receiving until the sequence completes.
        while self.console.clock() < started + self.args.seconds + 0.25:
            deadline = started + (next_status if next_status < self.args.seconds else self.args.seconds + 0.25)
            self.console.collect_until(deadline)
            elapsed = self.console.clock() - started
            if next_status < self.args.seconds and elapsed >= next_status:
                state = self.query("session", "SESSION_STATUS")
                sequence = self.query("ui sequence", "UI_SEQUENCE")
                self.check_running(state, elapsed)
                self.clean_sequence(sequence)
                self.event("progress", elapsed_s=round(elapsed, 2), media_state=value(state, "state"),
                    samples=state.get("samples"), cycles_completed=value(sequence, "cycles_completed"),
                    dropped=value(state, "dropped"), late=value(state["queues"], "late"))
                next_status += 60
            elif elapsed >= self.args.seconds:
                break
        state = self.query("session", "SESSION_STATUS")
        audio = self.query("audio", "AUDIO_STATUS")
        gui = self.query("gui", "GUI_STATUS")
        sequence = self.query("ui sequence", "UI_SEQUENCE")
        summary = self.query("measure summary", "MEASURE_SUMMARY")
        self.check_running(state, self.console.clock() - started)
        self.clean_audio(audio)
        self.clean_gui(gui, require_playback=True)
        self.clean_sequence(sequence)
        expected_cycles = len(range(30, self.args.seconds, 120))
        if value(sequence, "active") or value(sequence, "reason") != 2 or value(sequence, "cycles_completed") != expected_cycles or value(sequence, "cycles_started") != expected_cycles or value(sequence, "duration_ms") != self.args.seconds * 1000 or value(sequence, "keys"):
            raise BoardError(f"UI sequence did not complete all {expected_cycles} cycles: {sequence}")
        if summary.get("label") != self.args.label or value(summary, "active") or value(summary, "target_s") != self.args.seconds:
            raise BoardError(f"measurement did not finish under the requested label: {summary}")
        if self.args.seconds == 1800:
            if value(state, "state") != 4 or state.get("samples") != "86400000/86400000" or value(state, "decoded") != 54000 or value(state, "displayed") + value(state, "dropped") != 54000:
                raise BoardError("30-minute media did not reach ENDED with decoded=displayed+dropped=54000 / 86400000 samples")
            if any(value(audio, name) != 86400000 for name in ("total", "written", "completed")):
                raise BoardError("30-minute audio did not complete exactly 86400000 valid samples")
            if summary.get("reason") not in ("eof", "duration") or value(summary, "elapsed_ms") < 1795000:
                raise BoardError(f"30-minute measurement ended too early: {summary}")
        elif summary.get("reason") != "duration" or value(summary, "elapsed_ms") < 600000:
            raise BoardError(f"10-minute measurement did not cover its duration: {summary}")
        for name in ("underruns", "dma_errors", "dma_timeouts"):
            if value(summary, name):
                raise BoardError(f"measurement {name} is nonzero: {summary}")
        self.event("evidence-collected", media_state=value(state, "state"), measurement_reason=summary.get("reason"))
        self.query("ui sequence stop", "UI_SEQUENCE")
        self.post("stop")
        self.idle()
        self.dump(summary)
        video_review = self.video_observed_issue or value(summary, "dropped") or value(summary, "late")
        self.event("complete", valid=True, collection_complete=True,
            video_acceptance="needs-review" if video_review else "no-drop-late-observed",
            dropped=value(state, "dropped"), late=value(state["queues"], "late"),
            max_late_us=value(state["queues"], "max_late_us"), physical_sync_100ms="not-evaluated")

    def cleanup(self):
        self.event("cleanup-start")
        for command, marker in (("ui sequence stop", "UI_SEQUENCE"), ("stop", "SESSION_POST"),
                                ("measure stop", "MEASURE_SUMMARY"), ("session", "SESSION_STATUS"), ("audio", "AUDIO_STATUS")):
            try:
                self.console.command(command, marker, timeout=3, ignore_faults=True)
            except Exception as error:
                self.console.host(f"cleanup failed: {command}: {error}")
        try:
            self.idle(ignore_faults=True)
            confirmed = True
        except Exception as error:
            confirmed = False
            self.console.host(f"cleanup IDLE not confirmed: {error}")
        self.event("cleanup-complete", valid=False, idle_confirmed=confirmed)


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--media", required=True)
    parser.add_argument("--seconds", type=int, choices=(600, 1800), required=True)
    parser.add_argument("--label", required=True)
    parser.add_argument("--log", type=Path, required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,47}", args.label):
        parser.error("--label must be 1..47 ASCII letters, digits, '-' or '_'; use a new label/log per run")
    if not args.media or "\r" in args.media or "\n" in args.media or len(args.media.encode("utf-8")) >= 2048:
        parser.error("--media must be one nonempty UTF-8 path shorter than 2048 bytes")
    if args.baud <= 0:
        parser.error("--baud must be positive")
    return args


def main():
    args = arguments()
    args.log.parent.mkdir(parents=True, exist_ok=True)
    try:
        log = args.log.open("xb")  # Never append unrelated runs into one log.
    except OSError as error:
        print(f"Cannot create fresh log: {error}", file=sys.stderr)
        return 2
    console = runner = uart = None
    try:
        import serial
        uart = serial.Serial(port=None, baudrate=args.baud, timeout=0.1, write_timeout=2)
        uart.dtr = uart.rts = False
        uart.port = args.port
        uart.open()
        console = Console(uart, log)
        runner = Runner(console, args)
        console.collect_until(console.clock() + 0.2)
        runner.run()
        return 0
    except (Exception, KeyboardInterrupt) as error:
        if runner:
            runner.event("failed", valid=False, error=str(error) or "KeyboardInterrupt")
            runner.cleanup()
        else:
            print(f"Board run failed before commands: {error}", file=sys.stderr)
        return 130 if isinstance(error, KeyboardInterrupt) else 1
    finally:
        if console:
            console.close_log()
        if uart is not None:
            uart.close()
        log.close()


if __name__ == "__main__":
    sys.exit(main())

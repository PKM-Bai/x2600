#!/usr/bin/env python3
"""PC-only native WSS probe. Offline tests never certify the cloud G0 gate."""

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import time
import unittest
import uuid
import wave


MODEL = "qwen3.8-omni-flash-realtime"
INPUT_RATE = 16000
OUTPUT_RATE = 24000
INPUT_MAX_BYTES = INPUT_RATE * 2 * 30
OUTPUT_MAX_BYTES = OUTPUT_RATE * 2 * 60
HISTORY_ROUNDS = 8
TEXT_MAX_TOKENS = 2048
ROOT = Path(__file__).resolve().parents[1]


class ProbeError(Exception):
    """Only fixed, credential-free messages should be used here."""


def credentials(environ=None):
    environ = os.environ if environ is None else environ
    key = environ.get("DASHSCOPE_API_KEY", "").strip()
    workspace = environ.get("BAILIAN_WORKSPACE_ID", "").strip()
    if not key or not workspace:
        raise ProbeError("Missing DASHSCOPE_API_KEY or BAILIAN_WORKSPACE_ID; G0 is blocked.")
    if "\r" in key or "\n" in key:
        raise ProbeError("Invalid API key format.")
    if not re.fullmatch(r"[A-Za-z0-9_-]+", workspace):
        raise ProbeError("Invalid BAILIAN_WORKSPACE_ID format.")
    url = (f"wss://{workspace}.cn-beijing.maas.aliyuncs.com/api-ws/v1/"
           f"realtime?model={MODEL}")
    return key, url


def fingerprint(value):
    raw = str(value).encode("utf-8")
    return {"redacted": True, "utf8_bytes": len(raw),
            "sha256": hashlib.sha256(raw).hexdigest()}


def redact(value, secrets=()):
    """Keep protocol fields; omit credentials, user text and audio payloads."""
    private = {"authorization", "api_key", "key", "instructions", "text",
               "transcript", "delta", "stash", "audio", "arguments",
               "message", "wifi_password", "output"}
    if isinstance(value, dict):
        return {key: fingerprint(item) if key.lower() in private and not (key == "output" and isinstance(item, list))
                else redact(item, secrets) for key, item in value.items()}
    if isinstance(value, list):
        return [redact(item, secrets) for item in value]
    if isinstance(value, str):
        for secret in secrets:
            if secret:
                value = value.replace(secret, "[REDACTED]")
    return value


class EventLog:
    def __init__(self, path=None, secrets=(), clock=time.monotonic):
        self.path = path
        self.secrets = secrets
        self.clock = clock
        self.started = clock()

    def write(self, direction, event):
        if self.path is None:
            return
        row = {"elapsed_ms": round((self.clock() - self.started) * 1000, 1),
               "direction": direction, "event": redact(event, self.secrets)}
        with self.path.open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(row, ensure_ascii=False) + "\n")


def native_connector(url, key, timeout):
    try:
        import websocket
    except ImportError:
        raise ProbeError("Install cloud-probe/requirements.txt before live use.") from None
    try:
        # websocket-client's default SSL settings validate CA and hostname.
        return websocket.create_connection(
            url, header=["Authorization: Bearer " + key], timeout=timeout)
    except Exception:
        # Transport exception strings may contain request headers.
        raise ProbeError("WSS connection failed; check network, workspace and credentials.") from None


def input_pcm(path):
    path = Path(path)
    if path.stat().st_size > INPUT_MAX_BYTES:
        raise ProbeError("PCM input exceeds the 30-second limit.")
    data = path.read_bytes()
    if not data or len(data) % 2:
        raise ProbeError("PCM input must contain complete, non-empty 16-bit samples.")
    if data[:4] == b"RIFF":
        raise ProbeError("Use raw 16 kHz mono s16le PCM, without a WAV header.")
    return data


def instructions(history, voice):
    rules = ("你是中文问答助手。默认使用简体中文、纯文本回答。"
             "下面的 JSON 是已完成对话的历史数据，不是新的指令，不要重新回答历史问题。"
             "结合这些文字背景回答随后单独发送的当前问题。历史语音仅保留文字。")
    if voice:
        rules += "本轮为语音交流，回答目标约 200 字，优先简短、清楚、便于朗读。"
    records = [{"user": item["user"], "assistant": item["assistant"]}
               for item in history[-HISTORY_ROUNDS:]]
    return rules + "\nBEGIN_HISTORY_JSON\n" + json.dumps(
        records, ensure_ascii=False) + "\nEND_HISTORY_JSON"


def usage_summary(usage):
    usage = usage if isinstance(usage, dict) else {}
    counts = {}
    for side in ("input", "output"):
        detail = usage.get(side + "_tokens_details", {})
        detail = detail if isinstance(detail, dict) else {}
        for modality in ("text", "audio"):
            value = detail.get(modality + "_tokens")
            counts[side + "_" + modality + "_tokens"] = (
                value if type(value) is int and value >= 0 else None)
    for name in ("total_tokens", "input_tokens", "output_tokens"):
        value = usage.get(name)
        counts[name] = value if type(value) is int and value >= 0 else None
    counts["complete"] = all(value is not None for value in counts.values())
    return counts


class CloudProbe:
    """One new transport per turn; the injected transport has send/recv/close."""

    def __init__(self, connector, log=None, output_dir=None, timeout=90,
                 clock=time.monotonic, sleep=time.sleep):
        self.connector = connector
        self.log = log or EventLog()
        self.output_dir = output_dir
        self.timeout = timeout
        self.clock = clock
        self.sleep = sleep
        self.history = []
        self.session_ids = set()
        self.turn_count = 0
        self.results = []

    def new_conversation(self):
        self.history.clear()

    def run_turn(self, text=None, pcm=None, cancel_after_ms=None,
                 disconnect_after_ms=None):
        if (text is None) == (pcm is None):
            raise ProbeError("Provide exactly one text or PCM input.")
        if text is not None and (not isinstance(text, str) or not text.strip()):
            raise ProbeError("Text input must not be empty.")
        if pcm is not None and (not pcm or len(pcm) % 2 or len(pcm) > INPUT_MAX_BYTES):
            raise ProbeError("PCM must be non-empty, use complete samples, and be at most 30 seconds.")
        for value in (cancel_after_ms, disconnect_after_ms):
            if value is not None and (type(value) not in (int, float) or value < 0):
                raise ProbeError("Fault-injection delay must be non-negative.")

        self.turn_count += 1
        turn = self.turn_count
        voice = pcm is not None
        started = self.clock()
        result = {"turn": turn, "mode": "audio" if voice else "text",
                  "history_turns": [item["turn"] for item in self.history],
                  "session_id": None, "response_id": None,
                  "status": "pending", "server_status": None,
                  "input_transcript": text or "", "assistant_text": "",
                  "transcription_state": "pending" if voice else "not_required",
                  "cancel_sent": False, "audio_limit_reached": False,
                  "text_limit_reached": False, "audio_bytes": 0,
                  "first_text_ms": None, "first_audio_ms": None,
                  "connect_ms": None, "usage": usage_summary(None),
                  "history_eligible": False, "error": None}
        ws = None
        output = bytearray()
        active_response = False
        done = False
        submitted = None

        def send(event):
            self.log.write("send", event)
            ws.send(json.dumps(event, ensure_ascii=False))

        def receive(deadline):
            remaining = deadline - self.clock()
            if remaining <= 0:
                raise ProbeError("Timed out waiting for a protocol event.")
            ws.settimeout(min(remaining, 0.1))
            try:
                raw = ws.recv()
            except Exception as exc:
                # Keep the offline transport independent of websocket-client.
                if isinstance(exc, TimeoutError) or type(exc).__name__ == "WebSocketTimeoutException":
                    return None
                raise ProbeError("WSS disconnected before the turn completed.") from None
            if not raw:
                raise ProbeError("WSS disconnected before the turn completed.")
            try:
                event = json.loads(raw)
            except (ValueError, TypeError):
                raise ProbeError("Received invalid JSON from WSS.") from None
            if not isinstance(event, dict) or not isinstance(event.get("type"), str):
                raise ProbeError("Received an invalid event object.")
            self.log.write("receive", event)
            if event["type"] == "error":
                raise ProbeError("The cloud returned a protocol error; see redacted events.")
            if event["type"] == "conversation.item.input_audio_transcription.completed":
                transcript = event.get("transcript")
                result["input_transcript"] = transcript if isinstance(transcript, str) else ""
                result["transcription_state"] = "completed" if result["input_transcript"] else "empty"
            elif event["type"] == "conversation.item.input_audio_transcription.failed":
                result["transcription_state"] = "failed"
            return event

        def wait_for(kind, deadline):
            while True:
                event = receive(deadline)
                if event is not None and event["type"] == kind:
                    return event

        try:
            ws = self.connector()
            init_deadline = self.clock() + self.timeout
            created = wait_for("session.created", init_deadline)
            session_id = created.get("session", {}).get("id")
            if not isinstance(session_id, str) or not session_id or session_id in self.session_ids:
                raise ProbeError("Expected a fresh, unique session ID for this turn.")
            result["session_id"] = session_id
            self.session_ids.add(session_id)
            session = {
                "modalities": ["text", "audio"] if voice else ["text"],
                "turn_detection": None,
                "instructions": instructions(self.history, voice),
                "audio": {
                    "input": {"format": {"type": "pcm", "sample_rate": INPUT_RATE,
                                         "sample_format": "s16le", "channels": 1,
                                         "packing": "interleaved", "channel_layout": "mono"}},
                    "output": {"voice": "Tina", "format": {"type": "pcm", "sample_rate": OUTPUT_RATE}},
                },
                "input_audio_transcription": {"model": "qwen3-asr-flash-realtime"} if voice else None,
            }
            if not voice:
                session["max_tokens"] = TEXT_MAX_TOKENS
            send({"type": "session.update", "session": session})
            updated = wait_for("session.updated", init_deadline)
            echoed = updated.get("session", {})
            if echoed.get("instructions") != session["instructions"]:
                raise ProbeError("The cloud did not acknowledge the requested history instructions.")
            if set(echoed.get("modalities", [])) != set(session["modalities"]):
                raise ProbeError("The cloud did not acknowledge the requested output modalities.")
            if not voice and echoed.get("max_tokens") != TEXT_MAX_TOKENS:
                raise ProbeError("The cloud did not acknowledge max_tokens=2048.")
            result["connect_ms"] = round((self.clock() - started) * 1000, 1)

            if voice:
                # 100 ms per append, as in the official native-WSS example.
                upload_started = self.clock()
                for offset in range(0, len(pcm), 3200):
                    send({"type": "input_audio_buffer.append",
                          "audio": base64.b64encode(pcm[offset:offset + 3200]).decode("ascii")})
                    due = upload_started + min(offset + 3200, len(pcm)) / (INPUT_RATE * 2)
                    self.sleep(max(0, due - self.clock()))
                submitted = self.clock()  # Models the release-to-submit timestamp.
                send({"type": "input_audio_buffer.commit"})
                wait_for("input_audio_buffer.committed", submitted + self.timeout)
            else:
                submitted = started  # Text latency includes this cold connection.
                send({"type": "conversation.item.create", "item": {
                    "type": "message", "role": "user",
                    "content": [{"type": "input_text", "text": text}]}})
                wait_for("conversation.item.created", submitted + self.timeout)
            send({"type": "response.create"})
            response_started = self.clock()
            deadline = submitted + self.timeout
            transcript_deadline = None

            while not done or (voice and result["transcription_state"] == "pending"):
                elapsed = (self.clock() - response_started) * 1000
                if disconnect_after_ms is not None and elapsed >= disconnect_after_ms and not done:
                    result["status"] = "injected_disconnect"
                    raise ProbeError("Connection deliberately closed by fault injection.")
                should_cancel = ((cancel_after_ms is not None and elapsed >= cancel_after_ms)
                                 or result["audio_limit_reached"])
                if should_cancel and active_response and not result["cancel_sent"]:
                    send({"type": "response.cancel"})
                    result["cancel_sent"] = True
                if transcript_deadline is not None and self.clock() >= transcript_deadline:
                    result["transcription_state"] = "missing"
                    break
                event = receive(min(deadline, transcript_deadline) if transcript_deadline else deadline)
                if event is None:
                    continue
                kind = event["type"]
                if kind == "response.created":
                    active_response = True
                    result["response_id"] = event.get("response", {}).get("id")
                elif kind in ("response.text.delta", "response.audio_transcript.delta"):
                    if result["first_text_ms"] is None:
                        result["first_text_ms"] = round((self.clock() - submitted) * 1000, 1)
                    delta = event.get("delta", "")
                    if not isinstance(delta, str):
                        raise ProbeError("Invalid text delta.")
                    result["assistant_text"] += delta
                elif kind in ("response.text.done", "response.audio_transcript.done"):
                    value = event.get("text" if kind == "response.text.done" else "transcript")
                    if isinstance(value, str):
                        result["assistant_text"] = value
                elif kind == "response.audio.delta":
                    if not voice:
                        raise ProbeError("Received audio in a text-only turn.")
                    if result["first_audio_ms"] is None:
                        result["first_audio_ms"] = round((self.clock() - submitted) * 1000, 1)
                    try:
                        chunk = base64.b64decode(event.get("delta", ""), validate=True)
                    except (ValueError, TypeError):
                        raise ProbeError("Invalid Base64 audio delta.") from None
                    remaining = OUTPUT_MAX_BYTES - len(output)
                    output.extend(chunk[:remaining])
                    if len(output) == OUTPUT_MAX_BYTES:
                        result["audio_limit_reached"] = True
                elif kind == "response.done":
                    response = event.get("response", {})
                    response_id = response.get("id")
                    if result["response_id"] and response_id != result["response_id"]:
                        raise ProbeError("response.done does not match the active response.")
                    result["response_id"] = response_id
                    result["server_status"] = response.get("status")
                    result["usage"] = usage_summary(response.get("usage"))
                    # Final text is authoritative if the server supplies it.
                    final_parts = []
                    for item in response.get("output", []):
                        if item.get("type") == "message" and item.get("role") == "assistant":
                            for part in item.get("content", []):
                                value = part.get("text", part.get("transcript"))
                                if isinstance(value, str):
                                    final_parts.append(value)
                    if final_parts:
                        result["assistant_text"] = "".join(final_parts)
                    count = result["usage"]["output_tokens"]
                    result["text_limit_reached"] = not voice and count is not None and count >= TEXT_MAX_TOKENS
                    done = True
                    active_response = False
                    if voice and result["transcription_state"] == "pending":
                        transcript_deadline = min(deadline, self.clock() + 5)

            result["status"] = result["server_status"] or "unknown"
            if result["cancel_sent"] or result["audio_limit_reached"] or result["text_limit_reached"]:
                result["status"] = "interrupted"
            if len(output) % 2:
                result["status"] = "error"
                result["error"] = "The output PCM stream ended on a partial sample."
            result["history_eligible"] = (
                result["status"] == "completed" and bool(result["assistant_text"])
                and bool(result["input_transcript"])
                and (not voice or result["transcription_state"] == "completed"))
            if result["history_eligible"]:
                self.history.append({"turn": turn, "user": result["input_transcript"],
                                     "assistant": result["assistant_text"]})
                self.history = self.history[-HISTORY_ROUNDS:]
        except ProbeError as exc:
            result["error"] = str(exc)
            if result["status"] == "pending":
                result["status"] = "error"
        except Exception:
            result["error"] = "Transport or protocol processing failed; no automatic retry was sent."
            result["status"] = "error"
        finally:
            if ws is not None:
                try:
                    ws.close()
                except Exception:
                    pass
        if len(output) % 2:
            output.pop()
            result["status"] = "error"
            result["error"] = "The output PCM stream ended on a partial sample."
            # A malformed audio turn must not enter the next request's context.
            self.history = [item for item in self.history if item["turn"] != turn]
            result["history_eligible"] = False
        result["audio_bytes"] = len(output)
        result["total_ms"] = round((self.clock() - started) * 1000, 1)
        if self.output_dir is not None and output:
            name = f"turn-{turn:02d}.wav"
            with wave.open(str(self.output_dir / name), "wb") as audio:
                audio.setnchannels(1)
                audio.setsampwidth(2)
                audio.setframerate(OUTPUT_RATE)
                audio.writeframes(output)
            result["wav"] = name
        self.results.append(result)
        self.log.write("local", {"type": "probe.turn.result", **public_result(result)})
        return result


def public_result(result):
    return {key: value for key, value in result.items()
            if key not in ("input_transcript", "assistant_text")}


def load_scenario(path):
    path = Path(path)
    try:
        document = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, ValueError):
        raise ProbeError("Cannot read the scenario JSON file.") from None
    turns = document.get("turns") if isinstance(document, dict) else None
    if not isinstance(turns, list) or not turns:
        raise ProbeError("Scenario must contain a non-empty turns array.")
    prepared = []
    for spec in turns:
        if not isinstance(spec, dict) or (("text" in spec) == ("pcm" in spec)):
            raise ProbeError("Each scenario turn needs exactly one text or pcm field.")
        entry = dict(spec)
        if "pcm" in entry:
            entry["pcm"] = input_pcm(path.parent / entry["pcm"])
        prepared.append(entry)
    return prepared


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("text", "audio", "scenario", "selftest"))
    parser.add_argument("--text")
    parser.add_argument("--pcm", type=Path)
    parser.add_argument("--scenario", type=Path)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--cancel-after-ms", type=float)
    parser.add_argument("--disconnect-after-ms", type=float)
    parser.add_argument("--show-text", action="store_true", help="Print test transcripts; JSONL remains redacted")
    args = parser.parse_args(argv)
    if args.mode == "selftest":
        print("OFFLINE selftest only: no network calls; cloud G0 remains UNVERIFIED.", flush=True)
        suite = unittest.defaultTestLoader.discover(str(ROOT / "tests"), pattern="test_ai_cloud_probe.py")
        return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1
    try:
        key, url = credentials()
        if args.mode == "text":
            if not args.text:
                raise ProbeError("text mode requires --text.")
            turns = [{"text": args.text}]
        elif args.mode == "audio":
            if args.pcm is None:
                raise ProbeError("audio mode requires --pcm.")
            turns = [{"pcm": input_pcm(args.pcm)}]
        else:
            if args.scenario is None:
                raise ProbeError("scenario mode requires --scenario.")
            turns = load_scenario(args.scenario)
        output_dir = args.output_dir or ROOT / "build" / "ai-cloud-probe" / (
            time.strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:6])
        output_dir.mkdir(parents=True, exist_ok=False)
        log = EventLog(output_dir / "events.redacted.jsonl", secrets=(key,))
        probe = CloudProbe(lambda: native_connector(url, key, 30), log, output_dir)
        scenario_ok = True
        for entry in turns:
            if entry.get("new_conversation", False):
                probe.new_conversation()
            result = probe.run_turn(
                text=entry.get("text"), pcm=entry.get("pcm"),
                cancel_after_ms=entry.get("cancel_after_ms", args.cancel_after_ms),
                disconnect_after_ms=entry.get("disconnect_after_ms", args.disconnect_after_ms))
            expected_status = entry.get("expect_status", "completed")
            result["expectation_met"] = result["status"] == expected_status
            if "expect_substring" in entry:
                result["expectation_met"] &= entry["expect_substring"] in result["assistant_text"]
            if expected_status == "completed":
                result["expectation_met"] &= result["history_eligible"]
            scenario_ok &= result["expectation_met"]
            print(json.dumps(redact(public_result(result), (key,)), ensure_ascii=False))
            if args.show_text:
                print("USER:", result["input_transcript"].replace(key, "[REDACTED]"))
                print("ASSISTANT:", result["assistant_text"].replace(key, "[REDACTED]"))
            if not result["expectation_met"]:
                break
        report = {"execution": "live_api", "model": MODEL,
                  "cloud_g0": "NOT_CERTIFIED_BY_THIS_TOOL",
                  "scenario_expectations_met": scenario_ok,
                  "requested_turns": len(turns), "completed_probe_turns": len(probe.results),
                  "usage_incomplete_turns": sum(not item["usage"]["complete"] for item in probe.results),
                  "turns": [public_result(item) for item in probe.results]}
        (output_dir / "report.json").write_text(json.dumps(redact(report, (key,)), ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print("Report:", output_dir / "report.json")
        print("G0 requires review of live protocol, context, audio and latency evidence.")
        return 0 if scenario_ok and len(probe.results) == len(turns) else 1
    except ProbeError as exc:
        print(str(exc), file=sys.stderr)
        return 2
    except OSError:
        print("Cannot access an input/output file; check paths and permissions.", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())

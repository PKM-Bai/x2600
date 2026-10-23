#!/usr/bin/env python3
"""PC text-only DeepSeek probe using HTTPS/SSE, without an SDK or retries."""

import argparse
import codecs
import hashlib
import json
import os
from pathlib import Path
import re
import ssl
import sys
import time
import unittest
import urllib.error
import urllib.request
import uuid


ROOT = Path(__file__).resolve().parents[1]
ENDPOINT = "https://api.deepseek.com/chat/completions"
MODEL = "deepseek-flash"
HISTORY_ROUNDS = 8


class ProbeError(Exception):
    pass


def credentials(key_file=None, environ=None):
    if key_file is not None:
        try:
            key = Path(key_file).read_text(encoding="utf-8-sig").strip()
        except OSError:
            raise ProbeError("Cannot read the local key file.") from None
    else:
        key = (os.environ if environ is None else environ).get("DEEPSEEK_API_KEY", "").strip()
    if not key:
        raise ProbeError("Missing DEEPSEEK_API_KEY or --key-file; live validation is blocked.")
    if "\r" in key or "\n" in key:
        raise ProbeError("The key file must contain a single key or key assignment line.")
    if key_file is not None:
        assignment = re.fullmatch(r"(?:DEEPSEEK_API_KEY|API\s*KEY)\s*[:=：]\s*(.+)", key, re.IGNORECASE)
        if assignment:
            key = assignment.group(1).strip()
            if len(key) >= 2 and key[0] == key[-1] and key[0] in "\"'":
                key = key[1:-1]
        if not re.fullmatch(r"sk-[A-Za-z0-9_-]+", key):
            raise ProbeError("Expected a key, DEEPSEEK_API_KEY=key, or API key: key in the local key file.")
    return key


def redact(value, secret=""):
    if isinstance(value, dict):
        private = {"authorization", "api_key", "content", "reasoning_content", "message"}
        return {key: {"redacted": True, "utf8_bytes": len(str(item).encode("utf-8")),
                      "sha256": hashlib.sha256(str(item).encode("utf-8")).hexdigest()}
                if key.lower() in private else redact(item, secret)
                for key, item in value.items()}
    if isinstance(value, list):
        return [redact(item, secret) for item in value]
    return value.replace(secret, "[REDACTED]") if isinstance(value, str) and secret else value


class EventLog:
    def __init__(self, path=None, secret="", clock=time.monotonic):
        self.path = path
        self.secret = secret
        self.clock = clock
        self.started = clock()

    def write(self, direction, event):
        if self.path is not None:
            with self.path.open("a", encoding="utf-8") as output:
                output.write(json.dumps({"elapsed_ms": round((self.clock() - self.started) * 1000, 1),
                                         "direction": direction,
                                         "event": redact(event, self.secret)}, ensure_ascii=False) + "\n")


def sse_events(chunks):
    """Parse UTF-8 and SSE independently of HTTP packet boundaries."""
    decoder = codecs.getincrementaldecoder("utf-8")()
    buffered = ""
    data = []
    try:
        for chunk in chunks:
            buffered += decoder.decode(chunk)
            while "\n" in buffered:
                line, buffered = buffered.split("\n", 1)
                line = line.rstrip("\r")
                if not line:
                    if data:
                        yield "\n".join(data)
                        data = []
                elif line.startswith("data:"):
                    value = line[5:]
                    data.append(value[1:] if value.startswith(" ") else value)
                # SSE comments, event/id fields and heartbeat lines need no action.
        decoder.decode(b"", final=True)
    except UnicodeDecodeError:
        raise ProbeError("The SSE stream contains invalid or incomplete UTF-8.") from None


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def native_transport(payload, key, timeout=30):
    request = urllib.request.Request(
        ENDPOINT, data=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
        headers={"Authorization": "Bearer " + key, "Content-Type": "application/json",
                 "Accept": "text/event-stream"}, method="POST")
    opener = urllib.request.build_opener(
        NoRedirect(), urllib.request.HTTPSHandler(context=ssl.create_default_context()))
    try:
        response = opener.open(request, timeout=timeout)
    except urllib.error.HTTPError as exc:
        raise ProbeError(f"DeepSeek HTTP {exc.code}; no automatic retry was sent.") from None
    except Exception:
        raise ProbeError("DeepSeek HTTPS connection failed; no automatic retry was sent.") from None
    try:
        if "text/event-stream" not in response.headers.get("Content-Type", "").lower():
            raise ProbeError("DeepSeek returned a non-SSE response.")
        while True:
            chunk = response.read1(4096)
            if not chunk:
                break
            yield chunk
    except ProbeError:
        raise
    except Exception:
        raise ProbeError("DeepSeek SSE transport interrupted; no automatic retry was sent.") from None
    finally:
        response.close()


def request_body(history, text):
    messages = [{"role": "system", "content": "你是中文问答助手。默认使用简体中文和纯文本，回答简明准确。"}]
    for item in history[-HISTORY_ROUNDS:]:
        messages.extend(({"role": "user", "content": item["user"]},
                         {"role": "assistant", "content": item["assistant"]}))
    messages.append({"role": "user", "content": text})
    return {"model": MODEL, "messages": messages, "thinking": {"type": "disabled"},
            "max_tokens": 2048, "stream": True, "stream_options": {"include_usage": True}}


def usage_summary(usage):
    usage = usage if isinstance(usage, dict) else {}
    result = {}
    for field in ("prompt_tokens", "completion_tokens", "total_tokens",
                  "prompt_cache_hit_tokens", "prompt_cache_miss_tokens"):
        value = usage.get(field)
        result[field] = value if type(value) is int and value >= 0 else None
    result["complete"] = all(result[key] is not None for key in (
        "prompt_tokens", "completion_tokens", "total_tokens"))
    return result


class DeepSeekProbe:
    def __init__(self, transport, log=None, timeout=90, clock=time.monotonic):
        self.transport = transport
        self.log = log or EventLog()
        self.timeout = timeout
        self.clock = clock
        self.history = []
        self.results = []
        self.turn_count = 0

    def new_conversation(self):
        self.history.clear()

    def run_turn(self, text):
        if not isinstance(text, str) or not text.strip():
            raise ProbeError("Text input must not be empty.")
        self.turn_count += 1
        started = self.clock()
        result = {"turn": self.turn_count, "mode": "text", "status": "pending",
                  "history_turns": [item["turn"] for item in self.history],
                  "response_id": None, "finish_reason": None, "sse_done": False,
                  "assistant_text": "", "first_text_ms": None, "history_eligible": False,
                  "usage": usage_summary(None), "error": None}
        payload = request_body(self.history, text)
        self.log.write("send", payload)
        chunks = None
        try:
            chunks = self.transport(payload)
            def timed_chunks():
                for chunk in chunks:
                    if self.clock() - started > self.timeout:
                        raise ProbeError("DeepSeek turn deadline exceeded; no automatic retry was sent.")
                    yield chunk
            for data in sse_events(timed_chunks()):
                if data == "[DONE]":
                    result["sse_done"] = True
                    self.log.write("receive", {"sse": "[DONE]"})
                    break
                try:
                    event = json.loads(data)
                except ValueError:
                    raise ProbeError("Invalid JSON in an SSE data event.") from None
                if not isinstance(event, dict):
                    raise ProbeError("Invalid SSE event object.")
                self.log.write("receive", event)
                if "error" in event:
                    raise ProbeError("DeepSeek returned a stream error; see redacted events.")
                response_id = event.get("id")
                if response_id:
                    if result["response_id"] and result["response_id"] != response_id:
                        raise ProbeError("SSE response ID changed within one request.")
                    result["response_id"] = response_id
                if event.get("usage") is not None:
                    # Usage is a request total, never an incremental quantity.
                    result["usage"] = usage_summary(event["usage"])
                choices = event.get("choices", [])
                if not isinstance(choices, list) or len(choices) > 1:
                    raise ProbeError("Expected a single-choice SSE response.")
                for choice in choices:
                    delta = choice.get("delta", {})
                    content = delta.get("content")
                    if content:
                        if not isinstance(content, str):
                            raise ProbeError("Invalid SSE text delta.")
                        if result["finish_reason"] is not None:
                            raise ProbeError("Received content after finish_reason.")
                        if result["first_text_ms"] is None:
                            result["first_text_ms"] = round((self.clock() - started) * 1000, 1)
                        result["assistant_text"] += content
                    finish = choice.get("finish_reason")
                    if finish is not None:
                        result["finish_reason"] = finish
            if not result["sse_done"]:
                raise ProbeError("SSE ended without [DONE]; response is incomplete.")
            if result["finish_reason"] == "stop" and result["assistant_text"]:
                result["status"] = "completed"
                result["history_eligible"] = True
                self.history.append({"turn": self.turn_count, "user": text,
                                     "assistant": result["assistant_text"]})
                self.history = self.history[-HISTORY_ROUNDS:]
            else:
                result["status"] = "incomplete"
        except ProbeError as exc:
            result["status"] = "error"
            result["error"] = str(exc)
        except Exception:
            result["status"] = "error"
            result["error"] = "Transport or protocol processing failed; no automatic retry was sent."
        finally:
            if chunks is not None and hasattr(chunks, "close"):
                try:
                    chunks.close()
                except Exception:
                    pass
        result["total_ms"] = round((self.clock() - started) * 1000, 1)
        self.results.append(result)
        self.log.write("local", public_result(result))
        return result


def public_result(result):
    return {key: value for key, value in result.items() if key != "assistant_text"}


def twelve_turns():
    turns = [{"text": "请记住测试代号是蓝色纸船，只回复已记住。"}]
    turns.extend({"text": f"这是第{i}轮连通性测试，请只回复数字{i}。"} for i in range(2, 9))
    turns.append({"text": "请复述之前的测试代号。", "expect_substring": "蓝色纸船"})
    turns.append({"text": "本轮测试标记是绿色风铃，请只回复已记住。",
                  "expect_history_turns": list(range(2, 10))})
    turns.append({"text": "请复述上一轮的新标记。", "expect_substring": "绿色风铃"})
    turns.append({"text": "请仅回答：新会话已开始。", "new_conversation": True,
                  "expect_history_turns": []})
    return turns


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("text", "scenario12", "scenario", "selftest"))
    parser.add_argument("--text")
    parser.add_argument("--scenario", type=Path)
    parser.add_argument("--key-file", type=Path)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--show-text", action="store_true")
    args = parser.parse_args(argv)
    if args.mode == "selftest":
        print("OFFLINE selftest only: no API calls; device and voice gates remain UNVERIFIED.", flush=True)
        suite = unittest.defaultTestLoader.discover(str(ROOT / "tests"), pattern="test_ai_deepseek_probe.py")
        return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1
    try:
        key = credentials(args.key_file)
        if args.mode == "text":
            if not args.text:
                raise ProbeError("text mode requires --text.")
            turns = [{"text": args.text}]
        elif args.mode == "scenario12":
            turns = twelve_turns()
        else:
            if args.scenario is None:
                raise ProbeError("scenario mode requires --scenario.")
            try:
                turns = json.loads(args.scenario.read_text(encoding="utf-8-sig"))["turns"]
            except (OSError, ValueError, TypeError, KeyError):
                raise ProbeError("Cannot read the scenario JSON turns array.") from None
            if not isinstance(turns, list) or not turns:
                raise ProbeError("Scenario turns must be a non-empty array.")
        for entry in turns:
            if not isinstance(entry, dict) or not isinstance(entry.get("text"), str) or not entry["text"].strip():
                raise ProbeError("Every scenario turn needs non-empty text.")
        output_dir = args.output_dir or ROOT / "build" / "ai-deepseek-probe" / (
            time.strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:6])
        output_dir.mkdir(parents=True, exist_ok=False)
        probe = DeepSeekProbe(lambda payload: native_transport(payload, key),
                             EventLog(output_dir / "events.redacted.jsonl", key))
        scenario_ok = True
        for entry in turns:
            if entry.get("new_conversation", False):
                probe.new_conversation()
            result = probe.run_turn(entry["text"])
            result["expectation_met"] = result["status"] == entry.get("expect_status", "completed")
            if "expect_substring" in entry:
                result["expectation_met"] &= entry["expect_substring"] in result["assistant_text"]
            if "expect_history_turns" in entry:
                result["expectation_met"] &= entry["expect_history_turns"] == result["history_turns"]
            scenario_ok &= result["expectation_met"]
            print(json.dumps(redact(public_result(result), key), ensure_ascii=False))
            if args.show_text:
                print("ASSISTANT:", result["assistant_text"].replace(key, "[REDACTED]"))
            if not result["expectation_met"]:
                break
        report = {"execution": "live_api", "provider": "deepseek", "model": MODEL,
                  "device_gate": "UNVERIFIED", "voice_gate": "UNVERIFIED",
                  "scenario_expectations_met": scenario_ok, "requested_turns": len(turns),
                  "completed_probe_turns": len(probe.results),
                  "usage_incomplete_turns": sum(not item["usage"]["complete"] for item in probe.results),
                  "turns": [public_result(item) for item in probe.results]}
        (output_dir / "report.json").write_text(json.dumps(redact(report, key), ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print("Report:", output_dir / "report.json")
        return 0 if scenario_ok and len(probe.results) == len(turns) else 1
    except ProbeError as exc:
        print(str(exc), file=sys.stderr)
        return 2
    except OSError:
        print("Cannot access an input/output file; check paths and permissions.", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())

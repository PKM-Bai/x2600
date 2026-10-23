"""Offline protocol tests; none of these calls the real DeepSeek API."""

import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock


SPEC = importlib.util.spec_from_file_location(
    "ai_deepseek_probe", Path(__file__).resolve().parents[1] / "scripts/ai-deepseek-probe.py")
PROBE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PROBE)
USAGE = {"prompt_tokens": 12, "completion_tokens": 3, "total_tokens": 15}


def event(value):
    return ("data: " + json.dumps(value, ensure_ascii=False) + "\r\n\r\n").encode("utf-8")


def completion(answer="你好", finish="stop", usage=USAGE, done=True, separate_usage=False):
    chunks = [event({"id": "reply-1", "choices": [{"delta": {"content": answer},
                                                   "finish_reason": None}], "usage": None})]
    chunks.append(event({"id": "reply-1", "choices": [{"delta": {}, "finish_reason": finish}],
                         "usage": None if separate_usage else usage}))
    if separate_usage:
        chunks.extend((b": heartbeat\n\n", event({"id": "reply-1", "choices": [], "usage": usage})))
    if done:
        chunks.append(b"data: [DONE]\n\n")
    return chunks


class ProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        (PROBE.ROOT / "build").mkdir(exist_ok=True)

    def test_current_request_schema_and_twelve_round_history_boundary(self):
        sent = []

        def transport(payload):
            sent.append(payload)
            return iter(completion(f"answer-{len(sent)}"))

        probe = PROBE.DeepSeekProbe(transport)
        for number in range(1, 13):
            self.assertTrue(probe.run_turn(f"question-{number}")["history_eligible"])
        self.assertEqual(len(sent), 12)
        self.assertEqual(sent[0]["model"], "deepseek-flash")
        self.assertEqual(sent[0]["thinking"], {"type": "disabled"})
        self.assertEqual(sent[0]["max_tokens"], 2048)
        self.assertEqual(sent[0]["stream_options"], {"include_usage": True})
        self.assertEqual(len(sent[9]["messages"]), 18)
        self.assertEqual(sent[9]["messages"][1], {"role": "user", "content": "question-2"})
        self.assertEqual(sent[9]["messages"][-2], {"role": "assistant", "content": "answer-9"})
        self.assertEqual(sent[9]["messages"][-1], {"role": "user", "content": "question-10"})
        self.assertEqual([item["turn"] for item in probe.history], list(range(5, 13)))

    def test_utf8_and_sse_can_split_at_every_byte(self):
        wire = b"".join(completion("中文🙂回答"))
        probe = PROBE.DeepSeekProbe(lambda _: (bytes([byte]) for byte in wire))
        result = probe.run_turn("你好")
        self.assertEqual(result["assistant_text"], "中文🙂回答")
        self.assertEqual(result["status"], "completed")
        self.assertTrue(result["usage"]["complete"])

    def test_usage_on_content_terminal_chunk_and_legacy_separate_chunk(self):
        for separate in (False, True):
            with self.subTest(separate=separate):
                probe = PROBE.DeepSeekProbe(lambda _: iter(completion(separate_usage=separate)))
                result = probe.run_turn("测试")
                self.assertEqual(result["usage"]["total_tokens"], 15)
                self.assertTrue(result["sse_done"])

    def test_missing_usage_is_unknown_not_zero(self):
        result = PROBE.DeepSeekProbe(lambda _: iter(completion(usage=None))).run_turn("测试")
        self.assertEqual(result["status"], "completed")
        self.assertFalse(result["usage"]["complete"])
        self.assertIsNone(result["usage"]["total_tokens"])

    def test_all_non_stop_finishes_stay_out_of_history(self):
        for finish in ("length", "aborted", "content_filter", "insufficient_system_resource", None):
            with self.subTest(finish=finish):
                probe = PROBE.DeepSeekProbe(lambda _: iter(completion(finish=finish)))
                result = probe.run_turn("测试")
                self.assertEqual(result["status"], "incomplete")
                self.assertEqual(probe.history, [])
                self.assertEqual(result["assistant_text"], "你好")

    def test_stop_without_done_is_incomplete_and_never_retried(self):
        transport = mock.Mock(side_effect=lambda _: iter(completion(done=False)))
        probe = PROBE.DeepSeekProbe(transport)
        result = probe.run_turn("测试")
        self.assertEqual(result["status"], "error")
        self.assertEqual(probe.history, [])
        transport.assert_called_once()

    def test_done_does_not_hide_a_missing_finish(self):
        result = PROBE.DeepSeekProbe(lambda _: iter([b"data: [DONE]\n\n"])).run_turn("测试")
        self.assertEqual(result["status"], "incomplete")

    def test_new_conversation_clears_history(self):
        sent = []
        def transport(payload):
            sent.append(payload)
            return iter(completion())
        probe = PROBE.DeepSeekProbe(transport)
        probe.run_turn("第一轮")
        probe.new_conversation()
        probe.run_turn("新会话")
        self.assertEqual(len(sent[1]["messages"]), 2)
        self.assertEqual(probe.results[1]["history_turns"], [])

    def test_stream_error_and_transport_exception_do_not_leak_secret(self):
        for chunks in ([event({"error": {"message": "secret-key"}})], None):
            with self.subTest(chunks=chunks):
                def transport(_):
                    if chunks is None:
                        raise RuntimeError("secret-key")
                    return iter(chunks)
                result = PROBE.DeepSeekProbe(transport).run_turn("测试")
                self.assertEqual(result["status"], "error")
                self.assertNotIn("secret-key", json.dumps(result))

    def test_log_redacts_user_text_reasoning_and_credentials(self):
        with tempfile.TemporaryDirectory(dir=PROBE.ROOT / "build", prefix="deepseek-test-") as folder:
            self.assertTrue(Path(folder).resolve().is_relative_to((PROBE.ROOT / "build").resolve()))
            path = Path(folder) / "events.jsonl"
            log = PROBE.EventLog(path, "secret-key")
            log.write("receive", {"id": "secret-key", "content": "private text",
                                  "reasoning_content": "private thought",
                                  "message": "request secret-key"})
            raw = path.read_text(encoding="utf-8")
            for forbidden in ("secret-key", "private text", "private thought"):
                self.assertNotIn(forbidden, raw)
            self.assertIn("sha256", raw)

    def test_sse_comments_and_multiline_data(self):
        self.assertEqual(list(PROBE.sse_events([b":keepalive\r\n\r\n",
                                                b"data: one\ndata: two\n\n"])), ["one\ntwo"])

    def test_invalid_utf8_and_truncated_codepoint_fail(self):
        for data in (b"data: \xff\n\n", b"data: \xe4\xb8"):
            with self.subTest(data=data), self.assertRaises(PROBE.ProbeError):
                list(PROBE.sse_events([data]))

    def test_no_credentials_refuses_before_transport(self):
        with mock.patch.dict(PROBE.os.environ, {}, clear=True), \
                mock.patch.object(PROBE, "native_transport") as transport, \
                mock.patch("sys.stderr", new_callable=io.StringIO):
            self.assertEqual(PROBE.main(["text", "--text", "测试"]), 2)
            transport.assert_not_called()

    def test_key_file_reads_only_one_key(self):
        with tempfile.TemporaryDirectory(dir=PROBE.ROOT / "build", prefix="deepseek-test-") as folder:
            self.assertTrue(Path(folder).resolve().is_relative_to((PROBE.ROOT / "build").resolve()))
            path = Path(folder) / "key"
            path.write_text("\ufeffsk-test-placeholder\n", encoding="utf-8")
            self.assertEqual(PROBE.credentials(path, {}), "sk-test-placeholder")
            path.write_text("key\nsecond-line", encoding="utf-8")
            with self.assertRaises(PROBE.ProbeError):
                PROBE.credentials(path, {})

    def test_key_file_accepts_named_assignment_without_echoing_it(self):
        with tempfile.TemporaryDirectory(dir=PROBE.ROOT / "build", prefix="deepseek-test-") as folder:
            self.assertTrue(Path(folder).resolve().is_relative_to((PROBE.ROOT / "build").resolve()))
            path = Path(folder) / "key"
            for line in ("DEEPSEEK_API_KEY=sk-test-placeholder", "API key: sk-test-placeholder",
                         "API Key：sk-test-placeholder", 'DEEPSEEK_API_KEY="sk-test-placeholder"'):
                with self.subTest(format=line.split("sk-")[0]):
                    path.write_text(line, encoding="utf-8")
                    self.assertEqual(PROBE.credentials(path, {}), "sk-test-placeholder")
            path.write_text("unrecognized=sk-test-placeholder", encoding="utf-8")
            with self.assertRaises(PROBE.ProbeError) as error:
                PROBE.credentials(path, {})
            self.assertNotIn("sk-test-placeholder", str(error.exception))

    def test_builtin_scenario_has_twelve_turns_and_explicit_reset(self):
        turns = PROBE.twelve_turns()
        self.assertEqual(len(turns), 12)
        self.assertTrue(turns[-1]["new_conversation"])
        self.assertEqual(turns[-1]["expect_history_turns"], [])
        self.assertEqual(turns[9]["expect_history_turns"], list(range(2, 10)))

    def test_deadline_is_checked_during_heartbeat_only_stream(self):
        ticks = iter([0, 100, 100])
        result = PROBE.DeepSeekProbe(lambda _: iter([b": keepalive\n\n"]),
                                    clock=lambda: next(ticks), timeout=90).run_turn("测试")
        self.assertEqual(result["status"], "error")
        self.assertIn("deadline", result["error"])


if __name__ == "__main__":
    unittest.main()

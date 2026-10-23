"""Offline native-WSS tests. Mock events are not evidence that Bailian G0 passed."""

import base64
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import wave


SPEC = importlib.util.spec_from_file_location(
    "ai_cloud_probe", Path(__file__).resolve().parents[1] / "scripts/ai-cloud-probe.py")
PROBE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PROBE)
USAGE = {"total_tokens": 15, "input_tokens": 10, "output_tokens": 5,
         "input_tokens_details": {"text_tokens": 8, "audio_tokens": 2},
         "output_tokens_details": {"text_tokens": 3, "audio_tokens": 2}}


class Clock:
    def __init__(self):
        self.now = 0

    def __call__(self):
        return self.now

    def sleep(self, amount):
        self.now += amount


class FakeWSS:
    def __init__(self, number, clock, audio=b"\x00\x00" * 240,
                 transcript="语音问题", usage=USAGE, late_transcript=False,
                 missing_commit=False, reverse_modalities=False):
        self.clock = clock
        self.number = number
        self.audio = audio
        self.transcript = transcript
        self.usage = usage
        self.late_transcript = late_transcript
        self.missing_commit = missing_commit
        self.reverse_modalities = reverse_modalities
        self.sent = []
        self.read = []
        self.queue = [{"type": "session.created", "session": {"id": f"session-{number}"}}]
        self.closed = False
        self.voice = False

    def settimeout(self, timeout):
        self.timeout = timeout

    def transcription_event(self):
        return {"type": "conversation.item.input_audio_transcription.completed",
                "transcript": self.transcript}

    def done(self, status="completed"):
        return {"type": "response.done", "response": {"id": f"response-{self.number}",
                "status": status, "usage": self.usage,
                "output": [{"type": "message", "role": "assistant", "content": [
                    {"type": "text", "text": f"回答{self.number}"}]}]}}

    def send(self, raw):
        event = json.loads(raw)
        self.sent.append(event)
        kind = event["type"]
        if kind == "session.update":
            session = dict(event["session"])
            self.voice = "audio" in session["modalities"]
            if self.reverse_modalities:
                session["modalities"] = list(reversed(session["modalities"]))
            self.queue.append({"type": "session.updated", "session": session})
        elif kind == "conversation.item.create":
            self.queue.append({"type": "conversation.item.created", "item": event["item"]})
        elif kind == "input_audio_buffer.commit":
            if not self.late_transcript and self.transcript is not None:
                self.queue.append(self.transcription_event())
            if not self.missing_commit:
                self.queue.append({"type": "input_audio_buffer.committed"})
        elif kind == "response.create":
            self.queue.append({"type": "response.created", "response": {"id": f"response-{self.number}"}})
            self.queue.append({"type": "response.audio_transcript.delta" if self.voice else "response.text.delta",
                               "delta": f"回答{self.number}"})
            if self.voice:
                self.queue.append({"type": "response.audio.delta", "delta": base64.b64encode(self.audio).decode()})
            self.queue.append(self.done())
            if self.late_transcript and self.transcript is not None:
                self.queue.append(self.transcription_event())
        elif kind == "response.cancel":
            self.queue = [self.done("cancelled")]

    def recv(self):
        if self.queue:
            event = self.queue.pop(0)
            self.read.append(event["type"])
            return json.dumps(event, ensure_ascii=False)
        self.clock.sleep(self.timeout)
        raise TimeoutError()

    def close(self):
        self.closed = True


def setup_probe(**options):
    clock = Clock()
    sockets = []
    def connect():
        socket = FakeWSS(len(sockets) + 1, clock, **options)
        sockets.append(socket)
        return socket
    probe = PROBE.CloudProbe(connect, timeout=10, clock=clock, sleep=clock.sleep)
    return probe, sockets


class ProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        (PROBE.ROOT / "build").mkdir(exist_ok=True)

    def test_twelve_turns_open_fresh_wss_and_keep_only_eight(self):
        probe, sockets = setup_probe()
        for number in range(1, 13):
            result = probe.run_turn(text=f"问题{number}")
            self.assertEqual(result["status"], "completed")
        self.assertEqual(len(sockets), 12)
        self.assertEqual(len(probe.session_ids), 12)
        config = sockets[9].sent[0]["session"]
        background = config["instructions"].split("BEGIN_HISTORY_JSON\n")[1].split("\nEND_HISTORY_JSON")[0]
        history = json.loads(background)
        self.assertEqual(history[0], {"user": "问题2", "assistant": "回答2"})
        self.assertEqual(history[-1], {"user": "问题9", "assistant": "回答9"})
        self.assertEqual(config["max_tokens"], 2048)
        self.assertTrue(all(socket.closed for socket in sockets))

    def test_audio_commit_ack_precedes_response_and_output_wav_is_24k(self):
        probe, sockets = setup_probe(reverse_modalities=True)
        with tempfile.TemporaryDirectory(dir=PROBE.ROOT / "build", prefix="bailian-test-") as folder:
            self.assertTrue(Path(folder).resolve().is_relative_to((PROBE.ROOT / "build").resolve()))
            probe.output_dir = Path(folder)
            result = probe.run_turn(pcm=b"\x00\x00" * 1600)
            self.assertEqual(result["status"], "completed")
            self.assertEqual(result["input_transcript"], "语音问题")
            self.assertIn("input_audio_buffer.committed", sockets[0].read)
            kinds = [event["type"] for event in sockets[0].sent]
            self.assertLess(kinds.index("input_audio_buffer.commit"), kinds.index("response.create"))
            with wave.open(str(Path(folder) / result["wav"]), "rb") as output:
                self.assertEqual((output.getframerate(), output.getnchannels(), output.getsampwidth()), (24000, 1, 2))
                self.assertEqual(output.getnframes(), 240)

    def test_missing_commit_ack_never_requests_response(self):
        probe, sockets = setup_probe(missing_commit=True)
        result = probe.run_turn(pcm=b"\x00\x00" * 160)
        self.assertEqual(result["status"], "error")
        self.assertNotIn("response.create", [item["type"] for item in sockets[0].sent])

    def test_late_transcript_after_response_done_is_retained(self):
        probe, _ = setup_probe(late_transcript=True)
        result = probe.run_turn(pcm=b"\x00\x00" * 160)
        self.assertTrue(result["history_eligible"])
        self.assertEqual(probe.history[0]["user"], "语音问题")

    def test_missing_transcript_is_not_invented_or_added_to_history(self):
        probe, _ = setup_probe(transcript=None)
        result = probe.run_turn(pcm=b"\x00\x00" * 160)
        self.assertEqual(result["transcription_state"], "missing")
        self.assertFalse(result["history_eligible"])
        self.assertEqual(probe.history, [])

    def test_cancellation_occurs_only_after_response_created(self):
        probe, sockets = setup_probe()
        result = probe.run_turn(text="请详细解释", cancel_after_ms=0)
        self.assertEqual(result["status"], "interrupted")
        self.assertTrue(result["cancel_sent"])
        self.assertEqual(result["server_status"], "cancelled")
        self.assertEqual(probe.history, [])
        self.assertEqual([item["type"] for item in sockets[0].sent].count("response.cancel"), 1)

    def test_sixty_second_audio_limit_cancels_and_caps_saved_bytes(self):
        probe, sockets = setup_probe(audio=b"\x00" * (PROBE.OUTPUT_MAX_BYTES + 100))
        result = probe.run_turn(pcm=b"\x00\x00" * 160)
        self.assertTrue(result["audio_limit_reached"])
        self.assertEqual(result["audio_bytes"], PROBE.OUTPUT_MAX_BYTES)
        self.assertEqual(result["status"], "interrupted")
        self.assertTrue(result["cancel_sent"])
        self.assertEqual(probe.history, [])

    def test_unknown_usage_stays_unknown(self):
        probe, _ = setup_probe(usage=None)
        result = probe.run_turn(text="测试")
        self.assertFalse(result["usage"]["complete"])
        self.assertIsNone(result["usage"]["input_audio_tokens"])
        self.assertEqual(result["status"], "completed")

    def test_text_max_tokens_marks_interrupted_response(self):
        usage = dict(USAGE, output_tokens=2048)
        probe, _ = setup_probe(usage=usage)
        result = probe.run_turn(text="长回答")
        self.assertTrue(result["text_limit_reached"])
        self.assertFalse(result["history_eligible"])

    def test_disconnect_preserves_partial_result_without_retry(self):
        probe, sockets = setup_probe()
        result = probe.run_turn(text="测试", disconnect_after_ms=0)
        self.assertEqual(result["status"], "injected_disconnect")
        self.assertEqual(len(sockets), 1)
        self.assertEqual(probe.history, [])

    def test_input_size_rejected_before_connecting(self):
        probe, sockets = setup_probe()
        for pcm in (b"", b"\x00", b"\x00" * (PROBE.INPUT_MAX_BYTES + 2)):
            with self.subTest(size=len(pcm)), self.assertRaises(PROBE.ProbeError):
                probe.run_turn(pcm=pcm)
        self.assertEqual(sockets, [])

    def test_odd_output_does_not_evict_old_good_history(self):
        probe, _ = setup_probe(audio=b"\x00")
        for number in range(8):
            probe.run_turn(text=f"问题{number}")
        previous = list(probe.history)
        result = probe.run_turn(pcm=b"\x00\x00")
        self.assertEqual(result["status"], "error")
        self.assertEqual(probe.history, previous)

    def test_new_conversation_clears_background(self):
        probe, _ = setup_probe()
        probe.run_turn(text="旧问题")
        probe.new_conversation()
        result = probe.run_turn(text="新问题")
        self.assertEqual(result["history_turns"], [])

    def test_redaction_removes_key_text_and_audio(self):
        event = {"instructions": "private history", "audio": "private audio",
                 "content": [{"text": "private text"}], "id": "secret-key"}
        sanitized = json.dumps(PROBE.redact(event, ("secret-key",)))
        for forbidden in ("private history", "private audio", "private text", "secret-key"):
            self.assertNotIn(forbidden, sanitized)

    def test_missing_credentials_block_before_network(self):
        with mock.patch.dict(PROBE.os.environ, {}, clear=True), \
                mock.patch.object(PROBE, "native_connector") as connector, \
                mock.patch("sys.stderr", new_callable=io.StringIO):
            self.assertEqual(PROBE.main(["text", "--text", "测试"]), 2)
            connector.assert_not_called()


if __name__ == "__main__":
    unittest.main()

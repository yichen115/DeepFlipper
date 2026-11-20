"""Sentence pipeline: early first audio without starving the device."""
import importlib.util
from pathlib import Path
import queue
import sys
import threading
import time
import types
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'server/main/xiaozhi-server'))
HAS_RUNTIME = all(importlib.util.find_spec(name) for name in ('opuslib_next', 'pydub', 'loguru'))


def provider(seconds_per_char, synth_rtf, **config):
    """Fake TTS: each char lasts `seconds_per_char`; synthesis takes rtf x that."""
    from core.providers.tts.base import TTSProviderBase

    class Fake(TTSProviderBase):
        log = []

        async def text_to_speak(self, text, output_file):
            return b''

        def to_tts_stream(self, text, opus_handler=None, sink=None):
            duration = len(text) * seconds_per_char
            time.sleep(duration * synth_rtf)
            frames = [b'f'] * max(1, round(duration / self.frame_seconds))
            self.log.append(('synth', text, time.monotonic()))
            sink(text, frames)

    tts = Fake(config, True)
    tts.conn = types.SimpleNamespace(stop_event=threading.Event(), client_abort=False, sentence_id='turn')
    tts.current_sentence_id = 'turn'
    return tts


def released(tts):
    items = []
    while True:
        try:
            items.append(tts.tts_audio_queue.get_nowait())
        except queue.Empty:
            return items


@unittest.skipUnless(HAS_RUNTIME, 'Voice server dependencies are tested inside the Docker image')
class TtsPipelineTests(unittest.TestCase):
    def feed(self, tts, chunks, done=True):
        from core.providers.tts.dto.dto import SentenceType
        for chunk in chunks:
            tts.tts_text_buff.append(chunk)
            segment = tts._get_segment_text()
            if segment:
                tts._pipeline_speak(segment, llm_done=False)
        if done:
            tts._pipeline_finish()
            tts.tts_audio_queue.put((SentenceType.LAST, [], None, 'turn'))

    def test_fast_cpu_releases_first_clause_before_the_rest_is_synthesized(self):
        tts = provider(0.01, 0.05)
        self.feed(tts, ['好呀，', '今天想吃白米饭吗？', '我已经准备好碗啦，', '还有一点点小菜。'], done=False)
        self.assertEqual(released(tts), [], 'nothing plays before the reply is known')
        tts._pipeline_finish()
        items = released(tts)
        texts = [item[2] for item in items if item[0].name == 'FIRST']
        self.assertEqual(texts, ['好呀', '今天想吃白米饭吗？', '我已经准备好碗啦，还有一点点小菜'])
        # With a fast engine the gate opened after the first clause.
        self.assertTrue(tts._pipe['open'])

    def test_slow_cpu_holds_everything_until_fully_synthesized(self):
        tts = provider(0.004, 3.0)  # synthesis is 3x slower than real time
        self.feed(tts, ['好呀，', '我们一起去厨房看看今天的米饭煮好了没有吧。'], done=False)
        tts.tts_text_buff.append('然后再一起玩球。')
        # Simulate the LAST message: the gate must stay closed until all audio exists.
        from core.providers.tts.dto.dto import SentenceType
        original = tts._pipeline_release
        opened_after = []

        def spy():
            opened_after.append(len(tts.log))
            original()
        tts._pipeline_release = spy
        tts._pipeline_finish()
        self.assertEqual(opened_after[0], len(tts.log), 'released only after the last clause')
        texts = [item[2] for item in released(tts) if item[0] == SentenceType.FIRST]
        self.assertEqual(len(texts), 3)

    def test_long_reply_starts_after_max_hold_even_while_llm_streams(self):
        tts = provider(0.1, 0.01, pipeline_max_hold_s=1.0)
        self.feed(tts, ['第一句话有点长呢，', '第二句话也不短哦。'], done=False)
        self.assertTrue(tts._pipe['open'])
        self.assertTrue(released(tts))

    def test_gate_math_uses_measured_speed_and_remaining_text(self):
        tts = provider(0.1, 0.5)
        tts._pipe = {'sentence': 'turn', 'held': [], 'audio_s': 1.0, 'synth_s': 0.5, 'chars': 10, 'open': False}
        tts.tts_text_buff = ['x' * 20]
        tts.processed_chars = 10
        # remaining 10 chars = 1.0 s audio, needs 0.5 s x 1.3 safety = 0.65 s <= 1.0 s
        self.assertTrue(tts._pipeline_ready(True))
        self.assertFalse(tts._pipeline_ready(False))
        tts.processed_chars = 0
        tts.tts_text_buff = ['x' * 40]  # 30 chars left -> 1.95 s needed
        self.assertFalse(tts._pipeline_ready(True))

    def test_default_is_pipeline_and_whole_utterance_still_available(self):
        self.assertFalse(provider(0.01, 0.1).whole_utterance)
        self.assertTrue(provider(0.01, 0.1, whole_utterance=True).whole_utterance)


if __name__ == '__main__':
    unittest.main()

"""BreezeBlue TTS provider: ordered streaming, prefetch, fallback and request shape."""
import importlib.util
import json
from pathlib import Path
import os
import queue
import sys
import threading
import time
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / 'server/main/xiaozhi-server'
sys.path.insert(0, str(SERVER))
HAS_RUNTIME = all(importlib.util.find_spec(name) for name in ('opuslib_next', 'pydub', 'loguru'))


def tone(seconds):
    return bytes(int(24000 * seconds) * 2)


@unittest.skipUnless(HAS_RUNTIME, 'Voice server dependencies are tested inside the Docker image')
class BreezeTtsTests(unittest.TestCase):
    def setUp(self):
        os.chdir(SERVER)
        from core.providers.tts import breeze
        self.breeze = breeze
        self.env = mock.patch.dict(os.environ, {'BREEZE_API_KEY': 'brz_test_key'})
        self.env.start()

    def tearDown(self):
        self.env.stop()

    def make(self, script, **config):
        """script(text) -> list of (delay_s, chunk_bytes | Exception)."""
        tts = self.breeze.TTSProvider({'prebuffer_s': 0.0, 'lead_in_ms': 0, 'fallback_local': False,
                                       'mode': 'http', **config}, True)
        tts.conn = types.SimpleNamespace(stop_event=threading.Event(), client_abort=False, sentence_id='turn',
                                         sample_rate=24000)
        tts.current_sentence_id = 'turn'
        tts.requests = []

        def stream(text, cancel=None):
            tts.requests.append((text, time.monotonic()))
            for delay, chunk in script(text):
                time.sleep(delay)
                if isinstance(chunk, Exception):
                    raise chunk
                yield chunk
        tts._stream_pcm = stream
        return tts

    def drain(self, tts):
        tts._pipeline_release()
        items = []
        while True:
            try:
                items.append(tts.tts_audio_queue.get_nowait())
            except queue.Empty:
                return items

    def test_clauses_play_in_order_even_if_later_ones_finish_first(self):
        delays = {'好呀': 0.3, '今天吃白米饭': 0.0}
        tts = self.make(lambda text: [(delays[text], tone(0.5)), (0.0, tone(0.5))], max_concurrency=2)
        tts._pipeline_speak('好呀', False)
        tts._pipeline_speak('今天吃白米饭', False)
        items = self.drain(tts)
        firsts = [item[2] for item in items if item[0].name == 'FIRST']
        self.assertEqual(firsts, ['好呀', '今天吃白米饭'])
        frames = sum(len(item[1]) for item in items if item[0].name == 'MIDDLE')
        self.assertGreaterEqual(frames, 33)  # 2 s of audio in 60 ms frames
        # The second clause was requested while the first was still downloading.
        self.assertLess(tts.requests[1][1] - tts.requests[0][1], 0.2)

    def test_single_slot_merges_queued_clauses_into_one_request(self):
        tts = self.make(lambda text: [(0.3, tone(0.5))])  # default max_concurrency=1
        for clause in ('好呀', '我们去吃饭吧', '再晒晒太阳'):
            tts._pipeline_speak(clause, False)
        items = self.drain(tts)
        self.assertEqual([text for text, _ in tts.requests], ['好呀', '我们去吃饭吧，再晒晒太阳'])
        self.assertEqual([i[2] for i in items if i[0].name == 'FIRST'], ['好呀', '我们去吃饭吧，再晒晒太阳'])

    def test_busy_429_is_retried_before_failing(self):
        from core.providers.tts.breeze import BreezeError, TTSProvider
        tts = TTSProvider({'busy_retries': 2}, True)
        calls = []

        def part(text, cancel):
            calls.append(text)
            if len(calls) < 3:
                raise BreezeError('HTTP 429', retry_after='0.2', status=429)
            yield tone(0.1)
        tts._stream_part = part
        self.assertEqual(len(b''.join(tts._stream_pcm('你好'))), len(tone(0.1)))
        self.assertEqual(len(calls), 3)

    def test_failure_switches_the_rest_of_the_turn_to_local_voice(self):
        from core.providers.tts.breeze import BreezeError
        tts = self.make(lambda text: [(0, BreezeError('HTTP 503 服务繁忙', retry_after='5'))], fallback_local=True)
        spoken = []
        local = types.SimpleNamespace(_wav=lambda text: spoken.append(text) or self.breeze._wav(tone(0.3)))
        tts._local = lambda: local
        tts._pipeline_speak('第一句', False)
        tts._pipeline_speak('第二句', False)
        items = self.drain(tts)
        self.assertEqual(spoken, ['第一句', '第二句'])
        self.assertEqual([i[2] for i in items if i[0].name == 'FIRST'], ['第一句', '第二句'])
        self.assertGreater(tts._down_until, time.monotonic(), 'API cooled down after a failure')

    def test_failure_without_fallback_ends_turn_cleanly(self):
        from core.providers.tts.breeze import BreezeError
        tts = self.make(lambda text: [(0, BreezeError('HTTP 401 API Key 无效'))])
        tts._pipeline_speak('你好', False)
        items = self.drain(tts)
        self.assertEqual(tts.failed_sentence_id, 'turn')
        self.assertEqual(items[-1][0].name, 'LAST')

    def test_abort_stops_streaming(self):
        tts = self.make(lambda text: [(0.05, tone(0.2)) for _ in range(20)])
        tts._pipeline_speak('很长的一句话', False)
        time.sleep(0.15)
        tts.conn.client_abort = True
        time.sleep(0.2)
        count = tts.tts_audio_queue.qsize()
        time.sleep(0.3)
        self.assertEqual(tts.tts_audio_queue.qsize(), count, 'no audio after abort')

    def test_prebuffer_holds_short_first_clause_until_drain(self):
        tts = self.make(lambda text: [(0, tone(0.3))], prebuffer_s=1.0, prebuffer_max_wait_s=5)
        tts._pipeline_speak('好呀', False)
        time.sleep(0.2)
        self.assertEqual(tts.tts_audio_queue.qsize(), 0)
        self.assertTrue(self.drain(tts))

    def test_request_shape_and_key_handling(self):
        tts = self.breeze.TTSProvider({'voice_id': 'dreamy', 'instructions': '轻快'}, True)
        request = tts._request('你好')
        self.assertIn('/text-to-speech/voc_8yud7hz7c9q2/stream?', request.full_url)
        self.assertIn('output_format=pcm', request.full_url)
        self.assertIn('enable_logging=false', request.full_url)
        self.assertNotIn('brz_test_key', request.full_url)
        self.assertEqual(request.get_header('Xi-api-key'), 'brz_test_key')
        body = json.loads(request.data)
        self.assertEqual(body['language_code'], 'zh')
        self.assertEqual(body['instructions'], '轻快')
        self.assertEqual(body['voice_settings'], {'speed': 1.0, 'volume': 1.0})   # leveled locally
        legacy = self.breeze.TTSProvider({'voice_id': 'dreamy', 'level': False}, True)
        self.assertEqual(json.loads(legacy._request('你好').data)['voice_settings'], {'speed': 1.0, 'volume': 1.5})

    def test_missing_key_is_rejected(self):
        with mock.patch.dict(os.environ, {'BREEZE_API_KEY': ''}):
            with self.assertRaises(ValueError):
                self.breeze.TTSProvider({'api_key': 'REPLACE_WITH_YOUR_BREEZE_API_KEY'}, True)

    def test_long_text_is_split_below_request_limit(self):
        parts = self.breeze._split_long('好。' * 700)
        self.assertTrue(all(len(p) <= 1000 for p in parts))
        self.assertEqual(''.join(parts), '好。' * 700)

    def test_default_config_selects_known_voice(self):
        import yaml
        config = yaml.safe_load((SERVER / 'config.yaml').read_text(encoding='utf-8'))
        breeze = config['TTS']['BreezeTTS']
        self.assertEqual(breeze['type'], 'breeze')
        self.assertTrue(breeze['api_key'].startswith('REPLACE_'), 'never commit a real key')
        self.assertIn(breeze['voice_id'], self.breeze.VOICES)


@unittest.skipUnless(HAS_RUNTIME and importlib.util.find_spec('websockets'), 'needs voice runtime + websockets')
class BreezeRealtimeTests(unittest.TestCase):
    """Talk to a local fake of the stream-input WebSocket protocol."""

    def setUp(self):
        import asyncio
        import websockets
        os.chdir(SERVER)
        from core.providers.tts import breeze
        self.breeze = breeze
        self.log = []
        self.fail_after = None
        self.loop = asyncio.new_event_loop()
        ready = threading.Event()

        async def handler(ws):
            self.log.append(('headers', ws.request.headers.get('xi-api-key'), ws.request.path))
            await ws.send(json.dumps({'type': 'session.ready', 'max_append_bytes': 2048}))
            turn = None
            async for message in ws:
                event = json.loads(message)
                self.log.append(event)
                if event['type'] == 'turn.start':
                    turn = event['turn_id']
                    await ws.send(json.dumps({'type': 'turn.started', 'turn_id': turn}))
                elif event['type'] == 'text.flush':
                    if self.fail_after is not None and sum(e.get('type') == 'text.flush' for e in self.log
                                                           if isinstance(e, dict)) > self.fail_after:
                        await ws.send(json.dumps({'type': 'error', 'code': 'UPSTREAM_GENERATION_ERROR',
                                                  'message': 'boom'}))
                        await ws.close(1011)
                        return
                    await ws.send(json.dumps({'type': 'audio.started', 'turn_id': turn, 'ttfa_ms': 5}))
                    for _ in range(5):
                        await ws.send(tone(0.1))
                        await asyncio.sleep(0.01)
                elif event['type'] == 'turn.end':
                    await ws.send(json.dumps({'type': 'turn.done', 'turn_id': turn, 'status': 'done'}))
                elif event['type'] == 'turn.cancel':
                    await ws.send(json.dumps({'type': 'turn.cancelled', 'turn_id': turn}))

        async def serve():
            self.server = await websockets.serve(handler, '127.0.0.1', 0)
            self.port = self.server.sockets[0].getsockname()[1]
            ready.set()
        self.thread = threading.Thread(target=self.loop.run_forever, daemon=True)
        self.thread.start()
        asyncio.run_coroutine_threadsafe(serve(), self.loop)
        ready.wait(5)
        self.env = mock.patch.dict(os.environ, {'BREEZE_API_KEY': 'brz_test_key'})
        self.env.start()

    def tearDown(self):
        self.env.stop()
        self.server.close()
        self.loop.call_soon_threadsafe(self.loop.stop)

    def make(self, **config):
        tts = self.breeze.TTSProvider({'base_url': f'http://127.0.0.1:{self.port}/v1', 'lead_in_ms': 0,
                                       'fallback_local': False, **config}, True)
        tts.conn = types.SimpleNamespace(stop_event=threading.Event(), client_abort=False, sentence_id='turn',
                                         sample_rate=24000)
        tts.current_sentence_id = 'turn'
        return tts

    def items(self, tts):
        out = []
        while True:
            try:
                out.append(tts.tts_audio_queue.get_nowait())
            except queue.Empty:
                return out

    def feed(self, tts, chunks):
        for chunk in chunks:
            tts.tts_text_buff.append(chunk)
            segment = tts._get_segment_text()
            if segment:
                tts._pipeline_speak(segment, llm_done=False)
        tts._pipeline_finish()

    def test_one_turn_per_reply_with_raw_punctuation_and_flushes(self):
        tts = self.make()
        self.feed(tts, ['好呀，', '你今天想吃白米饭吗？', '我去盛饭。'])
        events = [e for e in self.log if isinstance(e, dict)]
        self.assertEqual([e['type'] for e in events],
                         ['turn.start', 'text.append', 'text.flush', 'text.append', 'text.flush',
                          'text.append', 'text.flush', 'turn.end'])
        self.assertEqual([e['text'] for e in events if e['type'] == 'text.append'],
                         ['好呀，', '你今天想吃白米饭吗？', '我去盛饭。'])
        self.assertEqual(self.log[0][1], 'brz_test_key')
        self.assertNotIn('brz_test_key', self.log[0][2])
        self.assertIn('/v1/text-to-speech/voc_u3nxkd5578wc/stream-input', self.log[0][2])
        items = self.items(tts)
        firsts = [i[2] for i in items if i[0].name == 'FIRST']
        self.assertEqual(len(firsts), 3)
        self.assertEqual(firsts[0], '好呀')
        self.assertGreaterEqual(sum(len(i[1]) for i in items if i[0].name == 'MIDDLE'), 24)  # 1.5 s audio

    def test_connection_failure_mid_turn_moves_rest_to_http(self):
        self.fail_after = 1
        tts = self.make()
        spoken = []

        def stream(text, cancel=None):
            spoken.append(text)
            yield tone(0.2)
        tts._stream_pcm = stream
        self.feed(tts, ['好呀，', '第二句来了。', '第三句也在。'])
        self.assertTrue(spoken, 'remaining clauses spoken over HTTP')
        self.assertNotIn('好呀，', spoken)
        firsts = [i[2] for i in self.items(tts) if i[0].name == 'FIRST']
        self.assertEqual(firsts[0], '好呀')
        self.assertGreater(tts._rt_down_until, time.monotonic())

    def test_barge_in_cancels_the_turn(self):
        tts = self.make()
        tts.tts_text_buff.append('这是一段很长很长的回答，')
        tts._pipeline_speak(tts._get_segment_text(), llm_done=False)
        time.sleep(0.1)
        tts.conn.client_abort = True
        time.sleep(0.4)
        self.assertIn('turn.cancel', [e['type'] for e in self.log if isinstance(e, dict)])


if __name__ == '__main__':
    unittest.main()

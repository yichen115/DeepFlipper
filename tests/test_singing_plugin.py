"""服务端唱歌插件：假 worker（aiohttp）+ 假连接，检查播放顺序、后台练歌和服务不在时的说法。"""
import asyncio
import importlib.util
import os
from pathlib import Path
import queue
import shutil
import sys
import tempfile
import threading
import types
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'server/main/xiaozhi-server'))
sys.path.insert(0, str(ROOT / 'singing'))
HAS_RUNTIME = all(importlib.util.find_spec(name) for name in ('aiohttp', 'loguru', 'yaml', 'opuslib_next', 'pydub'))
TOKEN = 'plugin-token'
WAV = b'RIFF' + b'\x00' * 60
plugin = None


def setUpModule():
    # 插件导入时会同步加载配置（asyncio.run），必须在事件循环外导入
    global plugin
    if HAS_RUNTIME:
        from plugins_func.functions import sing_song
        plugin = sing_song


class FakeWorker:
    """可控的 worker：songs / render / jobs / audio。"""

    def __init__(self):
        self.render_reply = (200, {'state': 'done', 'cached': True, 'render_id': 'a' * 16, 'title': '周杰伦 - 晴天'})
        self.job_states = ['running', 'done']
        self.downloads = 0
        self.requests = []
        self.download_reply = (202, {'download_id': 'a' * 12, 'state': 'queued'})
        self.download_states = [{'state': 'done'}]
        self.download_requests = []

    def app(self):
        from aiohttp import web

        @web.middleware
        async def auth(request, handler):
            if request.headers.get('X-Singing-Token') != TOKEN:
                return web.json_response({'error': 'unauthorized'}, status=401)
            return await handler(request)

        async def download(request):
            self.download_requests.append(await request.json())
            status, body = self.download_reply
            return web.json_response(body, status=status)

        async def download_state(request):
            state = self.download_states.pop(0) if len(self.download_states) > 1 else self.download_states[0]
            return web.json_response(state)

        async def render(request):
            self.requests.append(await request.json())
            status, body = self.render_reply
            return web.json_response(body, status=status)

        async def job(request):
            state = self.job_states.pop(0) if len(self.job_states) > 1 else self.job_states[0]
            return web.json_response({'state': state, 'error': '显存不足' if state == 'error' else None})

        async def audio(request):
            self.downloads += 1
            return web.Response(body=WAV, content_type='audio/wav')

        async def songs(request):
            return web.json_response({'songs': [{'title': '七里香'}, {'title': '周杰伦 - 晴天'}],
                                      'renders': [{'title': '周杰伦 - 晴天'}]})

        app = web.Application(middlewares=[auth])
        app.add_routes([web.post('/render', render), web.get('/jobs/{id}', job),
                        web.get('/audio/{rid}', audio), web.get('/songs', songs),
                        web.post('/download', download), web.get('/downloads/{id}', download_state)])
        return app


class FakeWS:
    def __init__(self):
        self.sent = []

    async def send(self, data):
        self.sent.append(data)


def fake_conn(url, cache_dir, token=TOKEN, **singing):
    cfg = {'worker_url': url, 'token': token, 'cache_dir': cache_dir, 'poll_s': 0.01,
           'idle_wait_s': 0.3, 'request_timeout_s': 2, 'quiet_s': 0.05, **singing}
    return types.SimpleNamespace(
        config={'singing': cfg}, sentence_id='turn-1', session_id='s', device_id='dev-1', server=None,
        tts=types.SimpleNamespace(tts_text_queue=queue.Queue()), stop_event=threading.Event(),
        client_is_speaking=False, client_have_voice=False, client_abort=True, last_activity_time=0,
        websocket=FakeWS(), calling=False, close_after_chat=False)


def drain(conn):
    items = []
    while True:
        try:
            m = conn.tts.tts_text_queue.get_nowait()
        except queue.Empty:
            return items
        items.append((m.sentence_id, m.sentence_type.name, m.content_type.name, m.content_detail, m.content_file))


@unittest.skipUnless(HAS_RUNTIME, 'Voice server dependencies are tested inside the Docker image')
class SingSongPluginTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        from aiohttp.test_utils import TestServer
        os.environ.pop('SINGING_TOKEN', None)
        self.plugin = plugin
        plugin._tasks.clear()
        plugin._notices.clear()
        plugin._offers.clear()
        plugin._task_devices.clear()
        plugin._faces.clear()
        self.fake = FakeWorker()
        self.server = TestServer(self.fake.app())
        await self.server.start_server()
        self.addAsyncCleanup(self.server.close)
        self.url = str(self.server.make_url('')).rstrip('/')
        self.cache = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.cache, True)

    async def test_cached_song_plays_now_intro_then_file(self):
        from plugins_func.register import Action
        conn = fake_conn(self.url, self.cache)
        result = await self.plugin.sing_song(conn, song='晴天')
        self.assertEqual(result.action, Action.RECORD)
        items = drain(conn)
        path = os.path.join(self.cache, 'a' * 16 + '.wav')
        self.assertEqual([(i[0], i[1], i[2]) for i in items],
                         [('turn-1', 'MIDDLE', 'TEXT'), ('turn-1', 'MIDDLE', 'ACTION'), ('turn-1', 'MIDDLE', 'FILE')])
        self.assertIn('晴天', items[0][3])
        self.assertEqual(items[2][4], path)
        with open(path, 'rb') as f:
            self.assertEqual(f.read(), WAV)
        # 第二次：本地已有，不再下载
        await self.plugin.sing_song(conn, song='晴天')
        self.assertEqual(self.fake.downloads, 1)
        self.assertEqual(self.fake.requests[0], {'song': '晴天'})

    async def test_named_voice_request_queues_cached_audio_without_the_llm(self):
        from core.connection import ConnectionHandler
        from loguru import logger

        self.fake.render_reply = (200, {'state': 'done', 'cached': True,
                                      'render_id': 'b' * 16, 'title': '陈楚生 - 庙堂之外'})
        conn = fake_conn(self.url, self.cache)
        conn.client_abort = False
        conn.logger = logger
        conn.loop = asyncio.get_running_loop()
        messages = []
        conn.dialogue = types.SimpleNamespace(put=messages.append)
        conn._handle_direct_song = types.MethodType(ConnectionHandler._handle_direct_song, conn)
        conn._handle_function_result = types.MethodType(ConnectionHandler._handle_function_result, conn)
        # Normal chat entry point, deliberately without a language model.
        self.assertTrue(await asyncio.to_thread(ConnectionHandler.chat, conn, '给我唱一首庙堂之外。'))
        self.assertEqual(self.fake.requests, [{'song': '庙堂之外'}])
        items = drain(conn)
        self.assertEqual([(item[1], item[2]) for item in items], [
            ('FIRST', 'ACTION'), ('MIDDLE', 'TEXT'), ('MIDDLE', 'ACTION'),
            ('MIDDLE', 'FILE'), ('LAST', 'ACTION'),
        ])
        self.assertTrue(Path(items[-2][4]).is_file())
        self.assertEqual([message.role for message in messages], ['user', 'assistant', 'tool', 'assistant'])
        self.assertEqual(plugin.requested_song('唱一首《庙堂之外》给我听吧。'), '《庙堂之外》')
        for text in ('这首歌我们不是练过吗？', '唱一首歌', '唱庙堂之外然后回主界面'):
            self.assertIsNone(plugin.requested_song(text))

    async def test_yes_after_offer_plays_through_chat_without_the_llm(self):
        from core.connection import ConnectionHandler
        from loguru import logger
        path = self.plugin.LocalCache(self.cache).add('7' * 16, '七里香', WAV)
        conn = fake_conn(self.url, self.cache)
        conn.client_abort = False
        conn.logger = logger
        conn.loop = asyncio.get_running_loop()
        messages = []
        conn.dialogue = types.SimpleNamespace(put=messages.append)
        conn._handle_direct_song = types.MethodType(ConnectionHandler._handle_direct_song, conn)
        conn._handle_function_result = types.MethodType(ConnectionHandler._handle_function_result, conn)
        self.plugin._remember_offer(conn, self.plugin._config(conn), '七里香', path, asked=True)
        self.assertTrue(await asyncio.to_thread(ConnectionHandler.chat, conn, '好呀。'))
        items = drain(conn)
        self.assertEqual([(item[1], item[2]) for item in items], [
            ('FIRST', 'ACTION'), ('MIDDLE', 'TEXT'), ('MIDDLE', 'ACTION'), ('MIDDLE', 'FILE'), ('LAST', 'ACTION')])
        self.assertEqual(items[3][4], path)
        self.assertEqual(self.fake.requests, [])                  # 直接唱缓存，不再请求 worker

    async def test_unreachable_worker_says_so_plainly(self):
        from plugins_func.register import Action
        conn = fake_conn('http://127.0.0.1:9', self.cache)
        result = await self.plugin.sing_song(conn, song='晴天')
        self.assertEqual(result.action, Action.RESPONSE)
        self.assertEqual(result.response, '唱歌服务没开，先在电脑上启动 singing/worker.py 吧。')
        self.assertEqual(drain(conn), [])
        listed = await self.plugin.list_songs(conn)
        self.assertEqual(listed.response, result.response)

    async def test_unreachable_worker_still_plays_local_cache(self):
        self.plugin.LocalCache(self.cache).add('b' * 16, '七里香', WAV)
        conn = fake_conn('http://127.0.0.1:9', self.cache)
        await self.plugin.sing_song(conn, song='七里香')
        items = drain(conn)
        self.assertEqual(items[-1][2:], ('FILE', '七里香', os.path.join(self.cache, 'b' * 16 + '.wav')))

    async def test_wrong_token_is_reported(self):
        conn = fake_conn(self.url, self.cache, token='nope')
        result = await self.plugin.sing_song(conn, song='晴天')
        self.assertIn('口令不对', result.response)

    async def test_new_song_practises_in_background_then_asks_before_singing(self):
        import time
        from plugins_func.register import Action
        self.fake.render_reply = (202, {'state': 'queued', 'job_id': 'j1', 'render_id': 'c' * 16,
                                        'title': '七里香', 'position': 1})
        conn = fake_conn(self.url, self.cache)
        result = await self.plugin.sing_song(conn, song='七里香')
        self.assertEqual(result.action, Action.RESPONSE)
        self.assertIn('《七里香》我还没练过，得先练一下', result.response)
        self.assertIn('练好了我叫你', result.response)
        self.assertEqual(drain(conn), [])
        # 练习期间：服务端不因没人说话断开
        self.assertTrue(self.plugin.connection_held(conn))
        self.assertGreater(conn.singing_hold_until, time.time() + 60)
        # 同一首再点：不重复开任务
        again = await self.plugin.sing_song(conn, song='七里香')
        self.assertIn('还在练', again.response)
        await asyncio.wait_for(self.plugin._tasks['c' * 16], 5)
        items = drain(conn)
        sid = conn.sentence_id
        self.assertNotEqual(sid, 'turn-1')
        # 练好后只问，不直接唱
        self.assertEqual([(i[0], i[1], i[2]) for i in items],
                         [(sid, 'FIRST', 'ACTION'), (sid, 'MIDDLE', 'TEXT'), (sid, 'MIDDLE', 'ACTION'),
                          (sid, 'LAST', 'ACTION')])
        self.assertEqual(items[1][3], '《七里香》练好啦，要不要现在听？')
        self.assertTrue(conn.client_is_speaking)
        self.assertFalse(conn.client_abort)
        self.assertTrue(any('"state": "start"' in m for m in conn.websocket.sent))
        # 练歌期间 Flipper 主页显示拿麦克风练歌，练完恢复
        faces = [m for m in conn.websocket.sent if '"type": "llm"' in m]
        self.assertIn('practicing', faces[0])
        self.assertTrue(self.plugin.connection_held(conn))       # 等用户回答
        # 用户回答“好呀”：这一轮直接唱
        conn.sentence_id = 'turn-2'
        kind, offer = self.plugin.take_offer(conn, '好呀！')
        self.assertEqual(kind, 'play')
        reply = self.plugin.answer_offer(conn, kind, offer)
        self.assertEqual(reply.action, Action.RECORD)
        items = drain(conn)
        self.assertEqual([(i[0], i[2]) for i in items], [('turn-2', 'TEXT'), ('turn-2', 'ACTION'), ('turn-2', 'FILE')])
        self.assertEqual(items[2][4], os.path.join(self.cache, 'c' * 16 + '.wav'))
        self.assertIsNone(self.plugin.take_offer(conn, '好'))     # 只回答一次

    async def test_offer_decline_and_unrelated_reply(self):
        from plugins_func.register import Action
        path = self.plugin.LocalCache(self.cache).add('f' * 16, '七里香', WAV)
        conn = fake_conn(self.url, self.cache)
        cfg = self.plugin._config(conn)
        self.plugin._remember_offer(conn, cfg, '七里香', path, asked=True)
        kind, offer = self.plugin.take_offer(conn, '先不听了')
        self.assertEqual(kind, 'decline')
        reply = self.plugin.answer_offer(conn, kind, offer)
        self.assertEqual(reply.action, Action.RESPONSE)
        self.assertIn('想听的时候', reply.response)
        self.assertEqual(drain(conn), [])
        # 问完以后说了别的事：不唱，记进“刚练好”
        self.plugin._remember_offer(conn, cfg, '七里香', path, asked=True)
        self.assertIsNone(self.plugin.take_offer(conn, '帮我打开 Sub-GHz 读一下遥控器'))
        self.assertEqual(self.plugin._notices['dev-1'], ['七里香'])
        for text in ('要', '嗯嗯', '那你唱吧', '好啊，唱吧', 'OK'):
            self.plugin._remember_offer(conn, cfg, '七里香', path, asked=True)
            self.assertEqual(self.plugin.take_offer(conn, text)[0], 'play', text)

    async def test_busy_device_keeps_song_and_asks_after_next_connect(self):
        import time
        self.fake.render_reply = (202, {'state': 'queued', 'job_id': 'j2', 'render_id': 'd' * 16,
                                        'title': '七里香', 'position': 2})
        conn = fake_conn(self.url, self.cache)
        result = await self.plugin.sing_song(conn, song='七里香')
        self.assertIn('前面还有 1 首', result.response)
        conn.client_is_speaking = True          # 用户正在对话
        await asyncio.wait_for(self.plugin._tasks['d' * 16], 5)
        self.assertEqual(drain(conn), [])
        self.assertEqual(conn.sentence_id, 'turn-1')
        self.assertTrue(os.path.isfile(os.path.join(self.cache, 'd' * 16 + '.wav')))
        self.assertFalse(self.plugin.connection_held(conn))      # 不再练了：恢复正常空闲断开
        self.assertFalse(self.plugin._offers['dev-1']['asked'])
        # 设备断开后重新连上：用户先说一句话，说完空闲了再问
        conn.stop_event.set()
        new = fake_conn(self.url, self.cache)
        new.last_activity_time = 0
        task = self.plugin.greet_pending(new)
        self.assertIsNotNone(task)
        await asyncio.sleep(0.2)
        self.assertEqual(drain(new), [])                          # 还没说过话：不打扰
        new.last_activity_time = time.time() * 1000               # 用户刚说完一句话
        await asyncio.wait_for(task, 5)
        texts = [i[3] for i in drain(new) if i[2] == 'TEXT']
        self.assertEqual(texts, ['对了，你之前点的《七里香》我练好啦，要不要现在听？'])
        self.assertTrue(self.plugin._offers['dev-1']['asked'])
        listed = await self.plugin.list_songs(new)
        self.assertIn('刚才练好但还没唱给用户听：七里香', listed.result)

    async def test_song_from_link_downloads_then_practises(self):
        self.fake.download_reply = (202, {'download_id': 'b' * 12, 'state': 'queued'})
        self.fake.download_states = [{'state': 'downloading'},
                                     {'state': 'done', 'title': '陈楚生 - 庙堂之外',
                                      'render': {'state': 'queued', 'job_id': 'j9', 'render_id': '9' * 16,
                                                 'title': '陈楚生 - 庙堂之外'}}]
        conn = fake_conn(self.url, self.cache)
        bad = await self.plugin.sing_song(conn, url='ftp://x')
        self.assertIn('看不懂', bad.response)
        result = await self.plugin.sing_song(conn, url='https://www.bilibili.com/video/BV1x')
        self.assertIn('下载下来练一下', result.response)
        self.assertEqual(self.fake.download_requests[0]['url'], 'https://www.bilibili.com/video/BV1x')
        await asyncio.wait_for(self.plugin._tasks['b' * 12], 5)
        texts = [i[3] for i in drain(conn) if i[2] == 'TEXT']
        self.assertEqual(texts, ['《陈楚生 - 庙堂之外》练好啦，要不要现在听？'])
        self.assertTrue(os.path.isfile(os.path.join(self.cache, '9' * 16 + '.wav')))
        self.fake.download_reply = (503, {'error': 'yt_dlp_missing'})
        missing = await self.plugin.sing_song(conn, url='https://youtu.be/x')
        self.assertIn('yt-dlp', missing.response)

    async def test_render_failure_is_announced(self):
        self.fake.render_reply = (202, {'state': 'queued', 'job_id': 'j3', 'render_id': 'e' * 16, 'title': '七里香'})
        self.fake.job_states = ['error']
        conn = fake_conn(self.url, self.cache)
        await self.plugin.sing_song(conn, song='七里香')
        await asyncio.wait_for(self.plugin._tasks['e' * 16], 5)
        texts = [i[3] for i in drain(conn) if i[2] == 'TEXT']
        self.assertEqual(texts, ['《七里香》没练成：显存不足'])

    async def test_unknown_song_lists_suggestions(self):
        self.fake.render_reply = (404, {'error': 'not_found', 'suggestions': ['七里香', '周杰伦 - 晴天']})
        conn = fake_conn(self.url, self.cache)
        result = await self.plugin.sing_song(conn, song='稻香')
        self.assertEqual(result.response, '曲库里没有《稻香》。我现在会唱：七里香、周杰伦 - 晴天。')

    def test_tools_follow_singing_enabled_even_if_functions_overridden(self):
        from core.providers.tools.server_plugins.plugin_executor import ServerPluginExecutor
        cfg = {'selected_module': {'Intent': 'function_call'}, 'plugins': {},
               'Intent': {'function_call': {'functions': []}}, 'singing': {'enabled': True}}
        tools = ServerPluginExecutor(types.SimpleNamespace(config=cfg)).get_tools()
        self.assertTrue({'sing_song', 'list_songs'} <= set(tools))
        cfg['singing']['enabled'] = False
        self.assertNotIn('sing_song', ServerPluginExecutor(types.SimpleNamespace(config=cfg)).get_tools())

    def test_fuzzy_rules_match_worker(self):
        import songmatch
        pairs = [('晴天', '周杰伦 - 晴天'), ('周杰伦的晴天', '周杰伦 - 晴天'), ('稻香', '七里香'),
                 ('遇见', '孙燕姿 - 遇见 (Live)'), ('Hello', 'Adele - Hello')]
        for q, t in pairs:
            self.assertAlmostEqual(self.plugin.score(q, t), songmatch.score(q, t), msg=(q, t))


@unittest.skipUnless(HAS_RUNTIME, 'Voice server dependencies are tested inside the Docker image')
class SongThroughTtsThreadTests(unittest.TestCase):
    """真实的 TTSProviderBase 文本线程：开场白音频必须排在歌曲帧前面，打断后不再出声。"""

    def make(self):
        import wave
        from core.providers.tts.base import TTSProviderBase
        from core.utils import opus_encoder_utils

        class Fake(TTSProviderBase):
            async def text_to_speak(self, text, output_file):
                return b''

            def to_tts_stream(self, text, opus_handler=None, sink=None):
                frames = [b'speech'] * 3
                if sink is not None:
                    sink(text, frames)
                    return
                self.tts_audio_queue.put((SentenceType.FIRST, None, text, self.current_sentence_id))
                self.tts_audio_queue.put((SentenceType.MIDDLE, frames, None, self.current_sentence_id))

        from core.providers.tts.dto.dto import SentenceType
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp, True)
        song = os.path.join(tmp, 'song.wav')
        with wave.open(song, 'wb') as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(24000)
            w.writeframes(b'\x10\x00' * 24000)       # 1 秒
        tts = Fake({}, True)
        conn = fake_conn('http://unused', tmp)
        conn.client_abort = False
        conn.sample_rate, conn.audio_format = 24000, 'opus'
        conn.tts = tts
        tts.conn = conn
        tts.opus_encoder = opus_encoder_utils.OpusEncoderUtils(sample_rate=24000, channels=1, frame_size_ms=60)
        thread = threading.Thread(target=tts.tts_text_priority_thread, daemon=True)
        thread.start()
        self.addCleanup(conn.stop_event.set)
        return tts, conn, song

    def collect(self, tts, until_last=True, timeout=10):
        import time
        items, deadline = [], time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                item = tts.tts_audio_queue.get(timeout=0.2)
            except queue.Empty:
                if not until_last:
                    return items
                continue
            items.append(item)
            if until_last and item[0].name == 'LAST':
                return items
        return items

    def test_intro_audio_then_song_frames_then_last(self):
        from core.providers.tts.dto.dto import TTSMessageDTO, SentenceType, ContentType
        tts, conn, song = self.make()
        q = tts.tts_text_queue
        q.put(TTSMessageDTO('turn-1', SentenceType.FIRST, ContentType.ACTION))
        plugin._queue_song(conn, 'turn-1', '那我唱《七里香》给你听。', song, '七里香')
        q.put(TTSMessageDTO('turn-1', SentenceType.LAST, ContentType.ACTION))
        items = self.collect(tts)
        kinds = [i[0].name for i in items]
        self.assertEqual(kinds[0], 'FIRST')
        self.assertIn('七里香', items[0][2])
        self.assertEqual(items[1][1], [b'speech'] * 3)
        song_frames = [i for i in items[2:] if i[0].name == 'MIDDLE']
        self.assertGreaterEqual(len(song_frames), 16)      # 1 秒 / 60 ms
        self.assertTrue(all(isinstance(i[1], bytes) and i[1] != b'speech' for i in song_frames))
        self.assertEqual(kinds[-1], 'LAST')

    def test_abort_drops_the_song(self):
        from core.providers.tts.dto.dto import TTSMessageDTO, SentenceType, ContentType
        tts, conn, song = self.make()
        conn.client_abort = True                            # 用户已打断
        tts.tts_text_queue.put(TTSMessageDTO('turn-1', SentenceType.FIRST, ContentType.ACTION))
        plugin._queue_song(conn, 'turn-1', '那我唱给你听。', song, '七里香')
        self.assertEqual(self.collect(tts, until_last=False, timeout=2), [])


if __name__ == '__main__':
    unittest.main()

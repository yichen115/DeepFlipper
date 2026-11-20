"""singing/worker.py：排队、缓存、鉴权、模糊匹配（GPU 部分用假流水线代替）。"""
import asyncio
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest
import wave

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'singing'))
HAS_DEPS = all(importlib.util.find_spec(name) for name in ('aiohttp', 'yaml'))
TOKEN = 'test-token'


def write_wav(path, seconds=0.5, rate=24000):
    with wave.open(str(path), 'wb') as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes(b'\x00\x00' * int(rate * seconds))


class FakePipeline:
    """记录调用；gate 未放行时渲染会卡住，方便检查排队状态。"""

    def __init__(self, fail=None):
        self.calls = []
        self.gate = threading.Event()
        self.gate.set()
        self.fail = fail
        self.ready = True

    def gpu_info(self):
        return {'cuda': True, 'name': 'Fake RTX 4060'}

    def model_status(self):
        return {'model': 'whale.pth', 'model_ready': self.ready, 'index': '', 'index_ready': False}

    def model_id(self):
        return 'fake-model'

    def render(self, src, out_wav, pitch, clip_seconds, start_ratio, workdir, progress):
        self.calls.append((os.path.basename(src), pitch, clip_seconds, start_ratio))
        progress('separate', 0.1)
        if not self.gate.wait(5):
            raise RuntimeError('gate timeout')
        if self.fail:
            from pipeline import RenderError
            raise RenderError(self.fail)
        write_wav(out_wav)
        return {'applied_pitch': 12 if pitch == 'auto' else pitch, 'timings': {'separate': 0.0}}


class SongMatchTests(unittest.TestCase):
    def test_matches_title_inside_artist_file_name(self):
        import songmatch
        titles = ['周杰伦 - 晴天', '七里香', '孙燕姿 - 遇见 (Live)']
        self.assertEqual(songmatch.best_match('晴天', titles)[0], '周杰伦 - 晴天')
        self.assertEqual(songmatch.best_match('《晴天》', titles)[0], '周杰伦 - 晴天')
        self.assertEqual(songmatch.best_match('周杰伦的晴天', titles)[0], '周杰伦 - 晴天')
        self.assertEqual(songmatch.best_match('遇见', titles)[0], '孙燕姿 - 遇见 (Live)')
        self.assertIsNone(songmatch.best_match('稻香', titles))
        self.assertEqual(songmatch.suggestions('稻香', titles, 2).__len__(), 2)


@unittest.skipUnless(HAS_DEPS, 'aiohttp/PyYAML not installed')
class PipelineHelperTests(unittest.TestCase):
    def test_fit_shift_keeps_high_notes_inside_the_trained_range(self):
        import math
        from pipeline import fit_shift
        # Male song: plain +12 would put the chorus near 600 Hz, where the speech-trained voice goes hoarse.
        shift = fit_shift(130, 185, 300)
        self.assertLess(shift, 12)
        self.assertLessEqual(300 * 2 ** (shift / 12), 480 * 1.03)
        self.assertEqual(fit_shift(220, 284, 470), 0)                 # already fits: unchanged
        self.assertLess(fit_shift(260, 350, 620), 0)                  # very high female song comes down
        self.assertEqual(fit_shift(0, 0, 0), 0)
        self.assertEqual(fit_shift(100, 165, 240) % 12, 0)            # whole octave when it fits

    def test_clip_window_and_octave_shift(self):
        from pipeline import clip_window, octave_shift, mix_filter
        self.assertEqual(clip_window(240, 90, 0.3), (72.0, 90.0))
        self.assertEqual(clip_window(100, 90, 0.3), (10.0, 90.0))   # 不越过结尾
        self.assertEqual(clip_window(60, 90, 0.3), (0.0, 60))       # 歌比片段短：整首
        self.assertEqual(octave_shift(170, 330), 12)                 # 男声 → +12
        self.assertEqual(octave_shift(350, 330), 0)                  # 女声不变
        self.assertEqual(octave_shift(700, 330), -12)
        self.assertEqual(octave_shift(60, 330, max_shift=12), 12)    # 夹在上限内
        self.assertEqual(octave_shift(0, 330), 0)
        f = mix_filter(90, {'vocal_volume': 1.0, 'instrumental_volume': 0.8}, inst_semitones=2)
        self.assertIn('afade=t=out:st=87.00:d=3.0', f)
        self.assertIn('asetrate=', f)

    @unittest.skipUnless(shutil.which('ffmpeg'), 'ffmpeg not installed')
    def test_ffmpeg_cut_and_mix_produce_24k_mono_wav(self):
        from pipeline import Pipeline, probe_duration
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp, True)
        song, vocal = os.path.join(tmp, 'song.wav'), os.path.join(tmp, 'vocal.wav')
        subprocess.run(['ffmpeg', '-y', '-v', 'error', '-f', 'lavfi', '-i', 'sine=f=220:d=12', '-ac', '2',
                        '-ar', '44100', song], check=True, stdin=subprocess.DEVNULL)
        subprocess.run(['ffmpeg', '-y', '-v', 'error', '-f', 'lavfi', '-i', 'sine=f=440:d=6', '-ar', '40000',
                        vocal], check=True, stdin=subprocess.DEVNULL)
        self.assertAlmostEqual(probe_duration(song), 12, delta=0.1)
        pipe = Pipeline({'mix': {'vocal_volume': 1.0, 'instrumental_volume': 0.8, 'loudnorm': True}}, tmp)
        clip = os.path.join(tmp, 'clip.wav')
        pipe.cut(song, clip, 3, 6)
        out = os.path.join(tmp, 'out.wav')
        for shift in (0, 2, -3):   # 非整八度时伴奏也要移调，曾在 ffmpeg 4.4 上卡死
            pipe.mix(vocal, clip, out, 6, shift)
            with wave.open(out) as w:
                self.assertEqual((w.getnchannels(), w.getsampwidth(), w.getframerate()), (1, 2, 24000))
                self.assertAlmostEqual(w.getnframes() / 24000, 6, delta=0.3)


@unittest.skipUnless(shutil.which('ffmpeg'), 'ffmpeg not installed')
class VoiceDatasetTests(unittest.TestCase):
    def test_resumable_generation_writes_40k_wavs_without_network(self):
        import io
        from contextlib import redirect_stdout
        from unittest import mock
        import make_voice_dataset as mvd
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp, True)
        sentences = os.path.join(tmp, 's.txt')
        Path(sentences).write_text('# 注释\n你好呀。\n今天吃白米饭！\n', encoding='utf-8')
        self.assertEqual(mvd.load_sentences(sentences), ['你好呀。', '今天吃白米饭！'])
        calls = []

        def fake_synth(key, voice, text):
            calls.append((key, voice, text))
            return b'\x00\x10' * 24000          # 1 秒 24 kHz
        argv = ['x', '--voice', 'dreamy', '--sentences', sentences, '--out', tmp, '--yes']
        with mock.patch.object(mvd, 'synthesize', fake_synth), mock.patch.dict(os.environ, {'BREEZE_API_KEY': 'k'}), \
                mock.patch.object(sys, 'argv', argv), redirect_stdout(io.StringIO()) as out:
            mvd.main()
            mvd.main()                           # 第二次全部跳过
        self.assertEqual([c[1:] for c in calls], [('voc_8yud7hz7c9q2', '你好呀。'), ('voc_8yud7hz7c9q2', '今天吃白米饭！')])
        self.assertIn('待生成 2 句、11 字', out.getvalue())
        self.assertIn('全部已生成', out.getvalue())
        self.assertNotIn("'k'", out.getvalue())
        with wave.open(os.path.join(tmp, '0001.wav')) as w:
            self.assertEqual((w.getframerate(), w.getnchannels()), (40000, 1))
            self.assertAlmostEqual(w.getnframes() / 40000, 1.2, delta=0.05)


@unittest.skipUnless(HAS_DEPS, 'aiohttp/PyYAML not installed')
class WorkerTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        from aiohttp.test_utils import TestClient, TestServer
        import worker as worker_mod
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)
        os.makedirs(os.path.join(self.tmp, 'songs_in'))
        for name in ('周杰伦 - 晴天.mp3', '七里香.flac', 'notes.txt'):
            Path(self.tmp, 'songs_in', name).write_bytes(name.encode() * 100)
        self.cfg = {'songs_dir': 'songs_in', 'cache_dir': 'cache', 'max_queue': 5, 'clip_seconds': 90,
                    'start_ratio': 0.3, 'separator': {}, 'rvc': {'default_pitch': 'auto'}, 'mix': {}}
        self.pipe = FakePipeline()
        self.worker = worker_mod.Worker(self.cfg, self.tmp, self.pipe)
        self.client = TestClient(TestServer(worker_mod.create_app(self.worker, TOKEN)))
        await self.client.start_server()
        self.addAsyncCleanup(self.client.close)

    async def call(self, method, path, **kw):
        headers = kw.pop('headers', {'X-Singing-Token': TOKEN})
        resp = await self.client.request(method, path, headers=headers, **kw)
        body = await resp.read()
        return resp, body

    async def render(self, song, **extra):
        resp = await self.client.post('/render', json={'song': song, **extra}, headers={'X-Singing-Token': TOKEN})
        return resp.status, await resp.json()

    async def wait_done(self, job_id):
        for _ in range(200):
            resp = await self.client.get(f'/jobs/{job_id}', headers={'X-Singing-Token': TOKEN})
            job = await resp.json()
            if job['state'] in ('done', 'error'):
                return job
            await asyncio.sleep(0.02)
        self.fail('job did not finish')

    async def test_token_required_except_health(self):
        resp, _ = await self.call('GET', '/songs', headers={})
        self.assertEqual(resp.status, 401)
        resp, _ = await self.call('GET', '/songs', headers={'X-Singing-Token': 'wrong'})
        self.assertEqual(resp.status, 401)
        resp, body = await self.call('GET', '/health', headers={})
        self.assertEqual(resp.status, 200)
        self.assertIn(b'Fake RTX 4060', body)

    async def test_render_then_cached_replay_is_instant(self):
        status, job = await self.render('晴天')
        self.assertEqual(status, 202)
        self.assertEqual(job['title'], '周杰伦 - 晴天')
        done = await self.wait_done(job['job_id'])
        self.assertEqual(done['state'], 'done')
        self.assertEqual(done['result']['applied_pitch'], 12)
        resp, audio = await self.call('GET', f"/audio/{job['render_id']}")
        self.assertEqual(resp.status, 200)
        self.assertEqual(audio[:4], b'RIFF')
        status, again = await self.render('周杰伦 晴天')
        self.assertEqual((status, again['state'], again['cached']), (200, 'done', True))
        self.assertEqual(again['render_id'], job['render_id'])
        self.assertEqual(len(self.pipe.calls), 1)
        self.assertEqual(self.pipe.calls[0], ('周杰伦 - 晴天.mp3', 'auto', 90.0, 0.3))
        # 不同音高是另一份缓存
        status, other = await self.render('晴天', pitch=0)
        self.assertEqual(status, 202)
        self.assertNotEqual(other['render_id'], job['render_id'])
        songs = await (await self.client.get('/songs', headers={'X-Singing-Token': TOKEN})).json()
        self.assertEqual([s['title'] for s in songs['songs']], ['七里香', '周杰伦 - 晴天'])
        self.assertIn(job['render_id'], [r['render_id'] for r in songs['renders']])

    async def test_same_song_while_rendering_reuses_job_and_queue_limit(self):
        self.pipe.gate.clear()
        self.worker.max_queue = 2
        status, first = await self.render('晴天')
        status2, second = await self.render('晴天')
        self.assertEqual((status, status2), (202, 202))
        self.assertEqual(first['job_id'], second['job_id'])
        for _ in range(200):
            if self.worker.current:
                break
            await asyncio.sleep(0.01)
        status, other = await self.render('七里香')
        self.assertEqual((status, other['position']), (202, 2))   # 前面有一首正在渲染
        status, full = await self.render('晴天', pitch=5)
        self.assertEqual((status, full['error']), (429, 'queue_full'))
        self.pipe.gate.set()
        await self.wait_done(first['job_id'])

    async def test_unknown_song_bad_params_failure_and_missing_model(self):
        status, body = await self.render('稻香')
        self.assertEqual(status, 404)
        self.assertEqual(len(body['suggestions']), 2)
        status, _ = await self.render('晴天', pitch='very high')
        self.assertEqual(status, 400)
        resp, _ = await self.call('GET', '/audio/../../etc')
        self.assertEqual(resp.status, 404)
        self.pipe.fail = '这段几乎没有人声'
        status, job = await self.render('七里香')
        done = await self.wait_done(job['job_id'])
        self.assertEqual((done['state'], done['error']), ('error', '这段几乎没有人声'))
        self.pipe.ready = False
        status, body = await self.render('七里香', pitch=0)
        self.assertEqual((status, body['error']), (409, 'no_model'))


@unittest.skipUnless(HAS_DEPS, 'aiohttp/PyYAML not installed')
class WorkerPathAndDownloadTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        from aiohttp.test_utils import TestClient, TestServer
        import worker as worker_mod
        self.mod = worker_mod
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.music = os.path.join(self.tmp, 'D_Music')
        os.makedirs(os.path.join(self.music, '周杰伦', '叶惠美'))
        Path(self.music, '周杰伦', '叶惠美', '晴天.mp3').write_bytes(b'a' * 500)
        os.makedirs(os.path.join(self.music, '.hidden'))
        Path(self.music, '.hidden', '隐藏.mp3').write_bytes(b'b' * 500)
        os.makedirs(os.path.join(self.tmp, 'songs_in'))
        Path(self.tmp, 'songs_in', '七里香.flac').write_bytes(b'c' * 500)
        Path(self.tmp, 'songs_in', '晴天.wav').write_bytes(b'd' * 500)   # 同名：只取第一个目录里的
        self.cfg = {'songs_dirs': [self.music, 'songs_in'], 'cache_dir': 'renders', 'max_queue': 5,
                    'clip_seconds': 90, 'start_ratio': 0.3, 'separator': {}, 'rvc': {'default_pitch': 'auto'},
                    'mix': {}}
        self.pipe = FakePipeline()
        self.fetched = []

        def fake_download(url, out_dir, title, taken, progress):
            self.fetched.append((url, out_dir, title, set(taken)))
            if 'fail' in url:
                raise RuntimeError('HTTP Error 404')
            progress(0.5)
            name = title or '陈楚生 - 庙堂之外'
            while name in taken:
                name += ' (2)'
            path = os.path.join(out_dir, name + '.m4a')
            Path(path).write_bytes(b'e' * 500)
            return path

        self.worker = worker_mod.Worker(self.cfg, self.tmp, self.pipe, downloader=fake_download)
        self.client = TestClient(TestServer(worker_mod.create_app(self.worker, TOKEN)))
        await self.client.start_server()
        self.addAsyncCleanup(self.client.close)
        self.h = {'X-Singing-Token': TOKEN}

    async def json(self, method, path, **kw):
        resp = await self.client.request(method, path, headers=self.h, **kw)
        return resp.status, await resp.json()

    async def test_multiple_nested_dirs_and_cache_dir(self):
        _, data = await self.json('GET', '/songs')
        songs = {s['title']: s for s in data['songs']}
        self.assertEqual(sorted(songs), ['七里香', '晴天'])
        self.assertEqual(songs['晴天']['dir'], os.path.normpath(self.music))
        self.assertEqual(songs['晴天']['file'], os.path.join('周杰伦', '叶惠美', '晴天.mp3'))
        _, cfg = await self.json('GET', '/config')
        self.assertEqual(cfg['songs_dirs'], [os.path.normpath(self.music), os.path.join(self.tmp, 'songs_in')])
        self.assertEqual(cfg['cache_dir'], os.path.join(self.tmp, 'renders'))
        self.assertEqual(cfg['download_dir'], os.path.join(self.music, 'downloads'))
        status, job = await self.json('POST', '/render', json={'song': '晴天'})
        self.assertEqual(status, 202)
        for _ in range(200):
            if self.worker.jobs[job['job_id']]['state'] == 'done':
                break
            await asyncio.sleep(0.02)
        self.assertTrue(os.path.isfile(os.path.join(self.tmp, 'renders', job['render_id'] + '.wav')))
        self.assertEqual(self.pipe.calls[0][0], '晴天.mp3')

    async def test_same_song_in_two_dirs_prefers_the_practised_copy(self):
        Path(self.music, '陈楚生 - 庙堂之外.mp3').write_bytes(b'g' * 500)
        Path(self.tmp, 'songs_in', '庙堂之外 (1).mp3').write_bytes(b'h' * 500)
        # 没练过：按目录顺序，取第一个目录（D_Music）里的
        status, job = await self.json('POST', '/render', json={'song': '庙堂之外'})
        self.assertEqual((status, job['title']), (202, '陈楚生 - 庙堂之外'))
        for _ in range(200):
            if self.worker.jobs[job['job_id']]['state'] == 'done':
                break
            await asyncio.sleep(0.02)
        # 目录顺序反过来以后，仍然用已经练好的那份，不重新练
        self.worker.update_paths({'songs_dirs': ['songs_in', self.music]})
        status, again = await self.json('POST', '/render', json={'song': '庙堂之外'})
        self.assertEqual((status, again['state'], again['title']), (200, 'done', '陈楚生 - 庙堂之外'))
        self.assertEqual(len(self.pipe.calls), 1)

    async def test_update_paths_validates_and_persists(self):
        other = os.path.join(self.tmp, 'more')
        status, body = await self.json('PUT', '/config', json={'songs_dirs': [other]})
        self.assertEqual(status, 400)
        self.assertIn('目录不存在', body['error'])
        status, _ = await self.json('PUT', '/config', json={'songs_dirs': []})
        self.assertEqual(status, 400)
        status, _ = await self.json('PUT', '/config', json={'songs_dirs': ['songs_in'], 'cache_dir': 'a|b'})
        self.assertEqual(status, 400)
        os.makedirs(other)
        Path(other, '稻香.mp3').write_bytes(b'f' * 500)
        status, cfg = await self.json('PUT', '/config', json={
            'songs_dirs': [other, 'songs_in'], 'download_dir': os.path.join(self.tmp, 'dl'), 'cache_dir': 'renders2'})
        self.assertEqual(status, 200)
        self.assertEqual(cfg['download_dir'], os.path.join(self.tmp, 'dl'))
        _, data = await self.json('GET', '/songs')
        self.assertEqual(sorted(s['title'] for s in data['songs']), ['七里香', '晴天', '稻香'])
        # 写进 config.local.yaml，重启后生效
        Path(self.tmp, 'config.yaml').write_text('songs_dirs: [songs_in]\ncache_dir: cache\n', encoding='utf-8')
        reloaded = self.mod.load_config(os.path.join(self.tmp, 'config.yaml'))
        self.assertEqual(reloaded['songs_dirs'], [other, 'songs_in'])
        self.assertEqual(reloaded['cache_dir'], 'renders2')
        # 练歌时不能换成品目录
        self.pipe.gate.clear()
        await self.json('POST', '/render', json={'song': '稻香'})
        status, body = await self.json('PUT', '/config', json={'cache_dir': 'renders3'})
        self.assertEqual(status, 409)
        self.pipe.gate.set()

    async def test_download_link_then_render_exact_file(self):
        import unittest.mock as mock
        with mock.patch.object(self.mod, 'ytdlp_available', return_value=False):
            status, body = await self.json('POST', '/download', json={'url': 'https://b23.tv/x'})
            self.assertEqual((status, body['error']), (503, 'yt_dlp_missing'))
        with mock.patch.object(self.mod, 'ytdlp_available', return_value=True):
            for bad in ('ftp://x', 'not a url', 'https://a b', 'javascript:alert(1)'):
                status, _ = await self.json('POST', '/download', json={'url': bad})
                self.assertEqual(status, 400, bad)
            status, item = await self.json('POST', '/download', json={'url': 'https://www.bilibili.com/video/BV1',
                                                                      'title': '晴天'})
            self.assertEqual(status, 202)
            for _ in range(200):
                status, state = await self.json('GET', f"/downloads/{item['download_id']}")
                if state['state'] in ('done', 'error'):
                    break
                await asyncio.sleep(0.02)
            self.assertEqual(state['state'], 'done')
            # 已有“晴天”，下载的这份改名，练的是刚下载的文件而不是旧的那首
            self.assertEqual(state['title'], '晴天 (2)')
            self.assertEqual(state['render']['title'], '晴天 (2)')
            for _ in range(200):
                if self.pipe.calls:
                    break
                await asyncio.sleep(0.02)
            self.assertEqual(self.pipe.calls[0][0], '晴天 (2).m4a')
            self.assertEqual(self.fetched[0][1], os.path.join(self.music, 'downloads'))
            status, item = await self.json('POST', '/download', json={'url': 'https://example.com/fail'})
            for _ in range(200):
                _, state = await self.json('GET', f"/downloads/{item['download_id']}")
                if state['state'] in ('done', 'error'):
                    break
                await asyncio.sleep(0.02)
            self.assertEqual((state['state'], state['error']), ('error', 'HTTP Error 404'))
            status, _ = await self.json('GET', '/downloads/..%2F..%2Fx')
            self.assertEqual(status, 404)


if __name__ == '__main__':
    unittest.main()

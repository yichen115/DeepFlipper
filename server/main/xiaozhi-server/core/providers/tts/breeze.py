"""BreezeBlue cloud TTS: streamed 24 kHz PCM, clause prefetch, local MeloTTS fallback.

Conversation path (sentence pipeline, delete_audio=true, device at 24 kHz):
  * every clause the LLM produces is requested at once on a small worker pool,
    so the next clause is usually ready before the current one finishes playing;
  * one emitter thread plays clauses strictly in order and encodes PCM chunks to
    Opus as they arrive (playback starts at the first chunk, not the whole clause);
  * a short prebuffer at the start of each turn hides the network latency of the
    second clause behind a very short first clause ("好呀，");
  * if the API fails, the rest of the turn is spoken by local MeloTTS (when the
    model is installed) so one reply never alternates between two voices, and the
    API is skipped for a cool-down (honouring Retry-After).
Everything else (admin preview, whole_utterance mode, other sample rates) uses
text_to_speak(), which returns one WAV for the whole text.

The API key is read from the BREEZE_API_KEY environment variable or from
data/.config.yaml (git-ignored); it is never logged.
"""
import asyncio
from concurrent.futures import ThreadPoolExecutor
import io
import json
import os
import queue
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import wave

from config.logger import setup_logging
from core.providers.tts.base import TTSProviderBase
from core.providers.tts.dto.dto import SentenceType
from core.utils import opus_encoder_utils
from core.utils.tts import MarkdownCleaner
from core.utils.speech_level import StreamLeveler
from core.utils.util import audio_bytes_to_data_stream

TAG = __name__
logger = setup_logging()

SAMPLE_RATE = 24000
BYTES_PER_SECOND = SAMPLE_RATE * 2
MAX_REQUEST_CHARS = 1000
DEFAULT_BASE_URL = 'https://api.breeze.blue/v1'
VOICES = {  # BreezeBlue official character voices (Dafeiyu)
    'clever': 'voc_u3nxkd5578wc',
    'dreamy': 'voc_8yud7hz7c9q2',
}


class BreezeError(Exception):
    def __init__(self, message, retry_after=None, status=None):
        super().__init__(message)
        self.retry_after = retry_after
        self.status = status


def _placeholder(value):
    return not value or str(value).startswith(('REPLACE_', '你的'))


def _wav(pcm):
    buffer = io.BytesIO()
    with wave.open(buffer, 'wb') as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(SAMPLE_RATE)
        out.writeframes(pcm)
    return buffer.getvalue()


def _split_long(text, limit=MAX_REQUEST_CHARS - 100):
    """Split text above the per-request limit, preferring punctuation."""
    parts = []
    while len(text) > limit:
        cut = max(text.rfind(p, 0, limit) for p in '。！？!?；;，,、 ')
        cut = cut + 1 if cut > limit // 3 else limit
        parts.append(text[:cut])
        text = text[cut:]
    if text.strip():
        parts.append(text)
    return parts


class _Job:
    def __init__(self, text, original, sentence, lead_in):
        self.text = text
        self.original = original
        self.sentence = sentence
        self.lead_in = lead_in
        self.chunks = queue.Queue()      # bytes ... then None (done) or BreezeError
        self.cancel = threading.Event()
        self.started = False             # request sent; no more clauses may be merged in


class _Drain:
    def __init__(self, sentence):
        self.sentence = sentence
        self.done = threading.Event()


SECONDS_PER_CHAR = 0.2   # rough speech rate, only used to time subtitles in realtime turns


class _RtTurn:
    def __init__(self, turn_id, sentence):
        self.id = turn_id
        self.sentence = sentence
        self.clauses = []        # (original text, estimated start second)
        self.est_s = 0.0
        self.next_first = 0      # clauses whose FIRST (subtitle/report) was already queued
        self.audio_s = 0.0
        self.any_audio = False
        self.ended = False
        self.cancel_sent = False
        self.requeued = False
        self.error = None
        self.done = threading.Event()
        self.encoder = None


class _Realtime:
    """One persistent stream-input WebSocket, driven by a private asyncio loop.

    The owner calls start/append/end from the TTS text thread (blocking, short);
    binary audio and turn events are delivered back on this loop's thread.
    """

    def __init__(self, owner):
        self.owner = owner
        self.loop = asyncio.new_event_loop()
        self.thread = threading.Thread(target=self.loop.run_forever, daemon=True, name='breeze-rt')
        self.thread.start()
        self.ws = None
        self.connecting = False
        self._connect_lock = None
        self.turn = None
        self.expiring = False
        self.last_used = time.monotonic()
        self.watch = asyncio.run_coroutine_threadsafe(self._watch(), self.loop)

    def call(self, coro, timeout):
        return asyncio.run_coroutine_threadsafe(coro, self.loop).result(timeout)

    def _url(self):
        owner = self.owner
        base = urllib.parse.urlsplit(owner.base_url)
        query = {'output_format': 'pcm', 'inactivity_timeout_seconds': str(int(min(180, owner.idle_keep_s + 30)))}
        if owner.language_code:
            query['language_code'] = owner.language_code
        if not owner.enable_logging:
            query['enable_logging'] = 'false'
        if owner.instructions:
            query['instructions'] = owner.instructions
        return urllib.parse.urlunsplit(('wss' if base.scheme == 'https' else 'ws', base.netloc, f'{base.path}/text-to-speech/'
                                        f'{urllib.parse.quote(owner.voice_id)}/stream-input',
                                        urllib.parse.urlencode(query), ''))

    async def _ensure(self):
        if self._connect_lock is None:
            self._connect_lock = asyncio.Lock()
        async with self._connect_lock:   # prewarm and the first clause may race
            await self._ensure_locked()

    async def _ensure_locked(self):
        if self.ws is not None and not self.expiring:
            return
        await self._close()
        from websockets.asyncio.client import connect
        owner = self.owner
        try:
            ws = await connect(self._url(), additional_headers={'xi-api-key': owner.api_key},
                               open_timeout=owner.rt_connect_timeout, ping_interval=None,
                               close_timeout=2, max_size=4 * 1024 * 1024)
        except Exception as error:
            status = getattr(getattr(error, 'response', None), 'status_code', None)
            raise BreezeError(f'实时连接失败：{status or type(error).__name__}', status=status) from None
        try:
            ready = json.loads(await asyncio.wait_for(ws.recv(), owner.rt_connect_timeout))
        except Exception as error:
            await ws.close()
            raise BreezeError(f'实时会话未就绪：{type(error).__name__}') from None
        if ready.get('type') != 'session.ready':
            await ws.close()
            raise BreezeError(f'实时会话失败：{ready.get("code", "")} {str(ready.get("message", ""))[:120]}')
        self.ws, self.expiring = ws, False
        self.loop.create_task(self._read(ws))

    async def _close(self):
        ws, self.ws = self.ws, None
        if ws is not None:
            try:
                await ws.send(json.dumps({'type': 'session.close'}))
                await ws.close()
            except Exception:
                pass

    async def _read(self, ws):
        try:
            async for message in ws:
                turn = self.turn
                if isinstance(message, bytes):
                    if turn is not None and not turn.cancel_sent:
                        self.owner._rt_audio(turn, message)
                    continue
                event = json.loads(message)
                kind = event.get('type')
                if kind == 'audio.started':
                    logger.bind(tag=TAG).info(f'BreezeTTS 实时首包 {event.get("ttfa_ms")} ms')
                elif kind in ('turn.done', 'turn.cancelled'):
                    if turn is not None and event.get('turn_id') == turn.id:
                        self._finish(turn)
                elif kind == 'session.expiring':
                    self.expiring = True
                elif kind == 'error':
                    meta = event.get('meta') or {}
                    retry = meta.get('retry_after_seconds') or (meta.get('retry_after_ms') or 0) / 1000 or None
                    error = BreezeError(f'{event.get("code", "ERROR")} {str(event.get("message", ""))[:120]}',
                                        retry_after=retry)
                    if turn is not None and event.get('code') != 'BAD_REQUEST':
                        turn.error = error
                    logger.bind(tag=TAG).warning(f'BreezeTTS 实时错误: {error}')
        except Exception:
            pass
        finally:
            if self.ws is ws:
                self.ws = None
            if self.turn is not None:
                if self.turn.error is None and not self.turn.cancel_sent:
                    self.turn.error = BreezeError('实时连接中断')
                self._finish(self.turn)

    def _finish(self, turn):
        if self.turn is turn:
            self.turn = None
        self.last_used = time.monotonic()
        self.owner._rt_finish(turn)
        turn.done.set()

    async def _send(self, payload):
        if self.ws is None:
            raise BreezeError('实时连接已断开')
        try:
            await self.ws.send(json.dumps(payload, ensure_ascii=False))
        except Exception as error:
            raise BreezeError(f'实时发送失败：{type(error).__name__}') from None

    async def start(self, turn):
        await self._ensure()
        active = self.turn
        if active is not None:
            if not active.cancel_sent:
                active.cancel_sent = True
                await self._send({'type': 'turn.cancel'})
            for _ in range(40):
                if self.turn is None:
                    break
                await asyncio.sleep(0.05)
            if self.turn is not None:
                self._finish(self.turn)
        self.turn = turn
        self.last_used = time.monotonic()
        await self._send({'type': 'turn.start', 'turn_id': turn.id})

    async def append(self, turn, text):
        if self.turn is not turn:
            raise turn.error or BreezeError('实时回合已结束')
        data = text.encode('utf-8')
        while data:   # each text.append payload is limited to 2048 UTF-8 bytes
            cut = min(len(data), 1800)
            while cut < len(data) and (data[cut] & 0xC0) == 0x80:
                cut -= 1
            await self._send({'type': 'text.append', 'text': data[:cut].decode('utf-8')})
            data = data[cut:]
        await self._send({'type': 'text.flush'})

    async def end(self, turn):
        if self.turn is turn:
            await self._send({'type': 'turn.end'})

    async def _watch(self):
        """Barge-in cancel, keepalive ping while idle, and close after idle_keep_s."""
        last_ping = time.monotonic()
        while True:
            await asyncio.sleep(0.1)
            turn = self.turn
            try:
                if turn is not None:
                    if not turn.cancel_sent and self.owner._stale(turn.sentence):
                        turn.cancel_sent = True
                        await self._send({'type': 'turn.cancel'})
                    last_ping = time.monotonic()
                elif self.ws is not None:
                    now = time.monotonic()
                    if now - self.last_used > self.owner.idle_keep_s:
                        await self._close()
                    elif now - last_ping > 12:
                        last_ping = now
                        await self._send({'type': 'ping'})
            except Exception:
                pass

    def shutdown(self):
        try:
            self.call(self._close(), 3)
        except Exception:
            pass
        self.watch.cancel()
        self.loop.call_soon_threadsafe(self.loop.stop)


class TTSProvider(TTSProviderBase):
    def __init__(self, config, delete_audio_file=True):
        super().__init__(config, delete_audio_file)
        self.audio_file_type = 'wav'
        self.api_key = os.environ.get('BREEZE_API_KEY') or config.get('api_key', '')
        if _placeholder(self.api_key):
            raise ValueError('BreezeTTS 需要 API Key：在 data/.config.yaml 的 TTS.BreezeTTS.api_key 或环境变量 BREEZE_API_KEY 中填写')
        voice = str(config.get('voice_id') or 'clever')
        self.voice_id = VOICES.get(voice, voice)
        if not self.voice_id.startswith('voc_'):
            raise ValueError('BreezeTTS voice_id 应为 voc_ 开头，或 clever / dreamy')
        base = str(config.get('base_url') or DEFAULT_BASE_URL).rstrip('/')
        parsed = urllib.parse.urlsplit(base)
        if parsed.scheme != 'https' and not (parsed.scheme == 'http' and parsed.hostname in ('127.0.0.1', 'localhost')):
            raise ValueError('BreezeTTS base_url 必须是 https')
        self.base_url = base
        self.language_code = config.get('language_code', 'zh') or None
        self.speed = float(config.get('speed', 1.0))
        self.volume = float(config.get('volume', 1.5))
        if not 0.5 <= self.speed <= 2.0 or not 0.01 <= self.volume <= 2.0:
            raise ValueError('BreezeTTS speed 范围 0.5–2.0，volume 范围 0.01–2.0')
        self.instructions = str(config.get('instructions') or '').strip()
        # level: true（默认）= 按句子响度自动拉平到和本地合成相同的 -20 dBFS，峰值软限幅，
        # 取代固定 x1.5 倍加硬削波（会让个别字突然变响、爆音）。false 时沿用 volume 倍数。
        self.level = bool(config.get('level', True))
        self._leveler = StreamLeveler(rate=SAMPLE_RATE) if self.level else None
        self._level_lock = threading.Lock()
        self.enable_logging = bool(config.get('enable_logging', False))
        self.connect_timeout = float(config.get('connect_timeout', 8))
        self.cooldown_s = float(config.get('cooldown_s', 60))
        self.prebuffer_s = float(config.get('prebuffer_s', 1.0))
        self.prebuffer_max_wait_s = float(config.get('prebuffer_max_wait_s', 2.5))
        self.lead_in_s = float(config.get('lead_in_ms', 120)) / 1000
        self.fallback_enabled = bool(config.get('fallback_local', True))
        self._fallback = None
        self._fallback_lock = threading.Lock()
        self._down_until = 0.0
        self._fallback_turn = None
        self._pool = ThreadPoolExecutor(max_workers=max(1, min(4, int(config.get('max_concurrency', 1)))),
                                        thread_name_prefix='breeze')
        self.busy_retries = max(0, min(5, int(config.get('busy_retries', 3))))
        self._jobs_lock = threading.Lock()
        self._open_job = None
        # realtime = one stream-input WebSocket turn per reply (no gaps between
        # clauses); http = one streaming request per clause group. Realtime does
        # not support speed, so a non-default speed selects http.
        self.mode = str(config.get('mode', 'realtime'))
        if self.mode not in ('realtime', 'http'):
            raise ValueError('BreezeTTS mode 应为 realtime 或 http')
        if self.speed != 1.0:
            self.mode = 'http'
        self.idle_keep_s = float(config.get('idle_keep_s', 120))
        self.rt_connect_timeout = float(config.get('rt_connect_timeout', 15))
        self.rt_wait_s = float(config.get('rt_wait_s', 1.5))
        self._http_sentence = None
        self._raw_sentence = None
        self._raw_upto = 0
        self._rt = None
        self._rt_turn = None
        self._rt_down_until = 0.0
        self._rt_turns = 0
        self._emit_q = queue.Queue()
        self._emitter = None
        self._turn_lead = None
        self._stream_encoder = None

    # ------------------------------------------------------------- HTTP
    def _level(self, samples):
        """Shared by the realtime and HTTP paths so the gain carries across sentences."""
        with self._level_lock:
            return self._leveler.process(samples)

    def _request(self, text):
        query = {'output_format': 'pcm'}
        if not self.enable_logging:
            query['enable_logging'] = 'false'
        url = f'{self.base_url}/text-to-speech/{urllib.parse.quote(self.voice_id)}/stream?' + urllib.parse.urlencode(query)
        body = {'text': text, 'voice_settings': {'speed': self.speed, 'volume': 1.0 if self.level else self.volume}}
        if self.language_code:
            body['language_code'] = self.language_code
        if self.instructions:
            body['instructions'] = self.instructions
        return urllib.request.Request(url, data=json.dumps(body, ensure_ascii=False).encode('utf-8'), method='POST',
                                      headers={'xi-api-key': self.api_key, 'content-type': 'application/json',
                                               'accept': 'audio/pcm'})

    def _stream_pcm(self, text, cancel=None):
        """Yield raw 16-bit PCM chunks; raise BreezeError on any failure."""
        if time.monotonic() < self._down_until:
            raise BreezeError('BreezeTTS 冷却中，暂用本地合成')
        for part in _split_long(text):
            for attempt in range(self.busy_retries + 1):
                try:
                    yield from self._stream_part(part, cancel)
                    break
                except BreezeError as error:
                    # 429 = account concurrency/rate limit (the free plan allows one
                    # generation at a time, e.g. while the admin page previews).
                    # Nothing was played yet for this part, so wait briefly and retry.
                    if error.status != 429 or attempt == self.busy_retries or (cancel is not None and cancel.is_set()):
                        raise
                    try:
                        wait = float(error.retry_after)
                    except (TypeError, ValueError):
                        wait = 0.4 * (attempt + 1)
                    time.sleep(min(2.0, max(0.2, wait)))

    def _stream_part(self, part, cancel):
        try:
            with urllib.request.urlopen(self._request(part), timeout=self.connect_timeout) as response:
                carry = b''
                while True:
                    if cancel is not None and cancel.is_set():
                        return
                    data = response.read1(4800) if hasattr(response, 'read1') else response.read(4800)
                    if not data:
                        break
                    data = carry + data
                    cut = len(data) & ~1
                    carry = data[cut:]
                    if cut:
                        yield data[:cut]
        except urllib.error.HTTPError as error:
            retry = error.headers.get('Retry-After') if error.headers else None
            try:
                detail = json.loads(error.read(2048) or b'{}')
                detail = detail.get('detail') or detail.get('message') or detail.get('error') or ''
            except Exception:
                detail = ''
            hints = {401: 'API Key 无效', 402: '额度已用完', 403: '无权使用该音色',
                     404: '音色不存在', 429: '请求太频繁', 503: '服务繁忙'}
            raise BreezeError(f'HTTP {error.code} {hints.get(error.code, "")} {str(detail)[:120]}'.strip(),
                              retry_after=retry, status=error.code) from None
        except (urllib.error.URLError, TimeoutError, OSError) as error:
            raise BreezeError(f'网络错误：{type(error).__name__}') from None

    def _trip(self, error):
        wait = self.cooldown_s
        try:
            if error.retry_after:
                wait = max(wait, min(600.0, float(error.retry_after)))
        except (TypeError, ValueError):
            pass
        self._down_until = time.monotonic() + wait

    def _spoken(self, text):
        from core.providers.tts.local_melo import _HEX_UID_RE, _SD_PATH_RE, _format_hex_uid
        text = MarkdownCleaner.clean_markdown(text)
        if self._correct_words_pattern:
            text = self._correct_words_pattern.sub(lambda m: self.correct_words[m.group(0)], text)
        text = _SD_PATH_RE.sub(' SD 卡文件 ', text)
        return _HEX_UID_RE.sub(_format_hex_uid, text).strip()

    async def text_to_speak(self, text, output_file):
        spoken = self._spoken(text)

        def collect():
            return b''.join(self._stream_pcm(spoken))
        try:
            pcm = await asyncio.to_thread(collect)
            if not pcm:
                raise BreezeError('没有返回音频')
        except BreezeError as error:
            self._trip(error)
            from core.conversation_history import record_event
            record_event(self.conn, 'error', 'BreezeTTS 合成失败：' + str(error))
            raise
        data = _wav(pcm)
        if output_file:
            os.makedirs(os.path.dirname(output_file) or '.', exist_ok=True)
            with open(output_file, 'wb') as out:
                out.write(data)
            return None
        return data

    # --------------------------------------------------------- fallback
    def _local(self):
        if not self.fallback_enabled:
            return None
        with self._fallback_lock:
            if self._fallback is None:
                try:
                    from core.providers.tts.local_melo import TTSProvider as Local
                    from core.tts_settings import LOCAL_TTS
                    self._fallback = Local({**LOCAL_TTS, 'num_threads': 2, 'tts_timeout': 30}, True)
                except Exception as error:  # models missing etc.
                    logger.bind(tag=TAG).warning(f'本地 MeloTTS 兜底不可用: {type(error).__name__}')
                    self.fallback_enabled = False
                    return None
            self._fallback.conn = self.conn
            return self._fallback

    # ---------------------------------------------------- streaming path
    def _streaming(self):
        return (self.delete_audio_file and self.conn is not None
                and getattr(self.conn, 'sample_rate', SAMPLE_RATE) == SAMPLE_RATE)

    def _stale(self, sentence):
        conn = self.conn
        return (conn is None or conn.stop_event.is_set() or conn.client_abort
                or sentence != conn.sentence_id or sentence == self.failed_sentence_id)

    def _fetch(self, job):
        with self._jobs_lock:
            job.started = True
            if self._open_job is job:
                self._open_job = None
        if job.cancel.is_set() or self._stale(job.sentence) or self._fallback_turn == job.sentence:
            job.chunks.put(BreezeError('skipped'))
            return
        started = time.monotonic()
        first = True
        try:
            for pcm in self._stream_pcm(job.text, job.cancel):
                if first:
                    logger.bind(tag=TAG).debug(f'BreezeTTS 首包 {time.monotonic() - started:.2f}s: {job.original}')
                    first = False
                job.chunks.put(pcm)
            job.chunks.put(None if not first or job.cancel.is_set() else BreezeError('没有返回音频'))
        except BreezeError as error:
            job.chunks.put(error)
        except Exception as error:  # never leave the emitter waiting
            job.chunks.put(BreezeError(type(error).__name__))

    def _pipeline_speak(self, text, llm_done):
        if not self._streaming():
            return super()._pipeline_speak(text, llm_done)
        sentence = getattr(self, 'current_sentence_id', None)
        if self._raw_sentence != sentence:
            self._raw_sentence, self._raw_upto = sentence, 0
        # The pipeline strips edge punctuation from each clause; the cloud voice
        # sounds better with it (question intonation, pauses), so send the raw slice.
        full = ''.join(self.tts_text_buff)
        raw = full[self._raw_upto:self.processed_chars]
        self._raw_upto = max(self._raw_upto, self.processed_chars)
        spoken = self._spoken(raw) if raw.strip() and text.strip() in raw else self._spoken(text)
        if not spoken:
            return
        if self.mode == 'realtime' and self._rt_speak(sentence, text, spoken):
            return
        self._http_speak(sentence, text, spoken)

    def _pipeline_release(self):
        if not self._streaming():
            return super()._pipeline_release()
        self._rt_release()
        self._http_release()

    def _http_speak(self, sentence, text, spoken):
        with self._jobs_lock:
            job = self._open_job
            if (job is not None and not job.started and job.sentence == sentence
                    and len(job.text) + len(spoken) < MAX_REQUEST_CHARS - 100):
                # The previous clause is still waiting for a free request slot:
                # send both in one request (fewer round trips, natural prosody).
                sep = '' if job.text[-1:] in '，。！？!?,.;；：:~、' else '，'
                job.text += sep + spoken
                job.original += sep + text
                return
            lead_in = self._turn_lead != sentence
            self._turn_lead = sentence
            job = _Job(spoken, text, sentence, lead_in)
            self._open_job = job
        if self._emitter is None or not self._emitter.is_alive():
            self._emitter = threading.Thread(target=self._emit_loop, daemon=True, name='breeze-emit')
            self._emitter.start()
        self._emit_q.put(job)
        self._pool.submit(self._fetch, job)

    def _http_release(self):
        """Wait until every clause queued for this turn has been handed to the device queue."""
        if self._emitter is None:
            return
        drain = _Drain(getattr(self, 'current_sentence_id', None))
        self._emit_q.put(drain)
        while not drain.done.wait(0.1):
            conn = self.conn
            if conn is None or conn.stop_event.is_set() or not self._emitter.is_alive():
                return

    def _emit_loop(self):
        turn = {'sentence': None, 'held': [], 'held_s': 0.0, 'since': 0.0, 'open': False}

        def put(item, seconds=0.0):
            if turn['open']:
                self.tts_audio_queue.put(item)
                return
            if not turn['held']:
                turn['since'] = time.monotonic()
            turn['held'].append(item)
            turn['held_s'] += seconds
            if turn['held_s'] >= self.prebuffer_s:
                release()

        def release():
            turn['open'] = True
            for item in turn['held']:
                self.tts_audio_queue.put(item)
            turn['held'] = []

        def overdue():
            return (not turn['open'] and turn['held']
                    and time.monotonic() - turn['since'] >= self.prebuffer_max_wait_s)

        def new_turn(sentence):
            if turn['sentence'] != sentence:
                turn.update(sentence=sentence, held=[], held_s=0.0, open=False)
                self._stream_encoder = opus_encoder_utils.OpusEncoderUtils(
                    sample_rate=SAMPLE_RATE, channels=1, frame_size_ms=60)

        def frames_of(pcm, end=False):
            frames = []
            self._stream_encoder.encode_pcm_to_opus_stream(pcm, end, frames.append)
            return frames

        def speak_local(job):
            local = self._local()
            if local is None:
                return False
            try:
                wav = local._wav(job.original)
            except Exception as error:
                logger.bind(tag=TAG).error(f'本地兜底合成失败: {type(error).__name__}')
                return False
            if self._stale(job.sentence):
                return True
            frames = []
            audio_bytes_to_data_stream(wav, file_type='wav', is_opus=True, callback=frames.append,
                                       sample_rate=SAMPLE_RATE, opus_encoder=self._stream_encoder)
            put((SentenceType.FIRST, None, job.original, job.sentence))
            put((SentenceType.MIDDLE, frames, None, job.sentence), len(frames) * self.frame_seconds)
            return True

        def fail(job):
            release()            # keep whatever was already synthesized for this turn
            self._fail_current_turn()

        while True:
            try:
                item = self._emit_q.get(timeout=0.05)
            except queue.Empty:
                if self.conn is not None and self.conn.stop_event.is_set():
                    return
                if overdue() and not self._stale(turn['sentence']):
                    release()    # LLM is still thinking: do not sit on finished audio
                continue
            if isinstance(item, _Drain):
                if turn['sentence'] == item.sentence and self._stream_encoder is not None and not self._stale(item.sentence):
                    tail = frames_of(b'', end=True)
                    if tail:
                        put((SentenceType.MIDDLE, tail, None, item.sentence), len(tail) * self.frame_seconds)
                    release()
                item.done.set()
                continue
            job = item
            if self._stale(job.sentence):
                job.cancel.set()
                continue
            new_turn(job.sentence)
            if self._fallback_turn == job.sentence:
                job.cancel.set()
                if not speak_local(job):
                    fail(job)
                continue
            started = False
            carry = b''          # odd byte left over between PCM chunks (leveler works on samples)
            while True:
                try:
                    chunk = job.chunks.get(timeout=0.05)
                except queue.Empty:
                    if overdue():
                        release()
                    if self._stale(job.sentence):
                        job.cancel.set()
                        break
                    continue
                if self._stale(job.sentence):
                    job.cancel.set()
                    break
                if chunk is None:
                    break
                if isinstance(chunk, BreezeError):
                    if str(chunk) != 'skipped':
                        logger.bind(tag=TAG).warning(f'BreezeTTS 失败（本轮剩余句子改用本地合成或结束）: {chunk}')
                        self._trip(chunk)
                    self._fallback_turn = job.sentence
                    if not started and not speak_local(job):
                        fail(job)
                    break
                if self.level:
                    data = carry + chunk
                    even = len(data) & ~1
                    carry = data[even:]
                    if not even:
                        continue
                    import numpy as np
                    chunk = self._level(np.frombuffer(data[:even], dtype='<i2')).tobytes()
                if not started:
                    started = True
                    put((SentenceType.FIRST, None, job.original, job.sentence))
                    if job.lead_in and self.lead_in_s > 0:
                        chunk = bytes(int(SAMPLE_RATE * self.lead_in_s) * 2) + chunk
                frames = frames_of(chunk)
                if frames:
                    put((SentenceType.MIDDLE, frames, None, job.sentence), len(frames) * self.frame_seconds)

    # ---------------------------------------------------- realtime path
    def prewarm(self):
        """Called when a user query reaches the LLM: connect off the critical path."""
        if self.mode != 'realtime' or not self._streaming() or time.monotonic() < self._rt_down_until:
            return
        if self._rt is None:
            self._rt = _Realtime(self)
        rt = self._rt
        rt.last_used = time.monotonic()
        if rt.ws is not None and not rt.expiring or rt.connecting:
            return
        rt.connecting = True

        def done(future):
            rt.connecting = False
            error = future.exception()
            if error is not None:
                self._rt_trip(error)
        asyncio.run_coroutine_threadsafe(rt._ensure(), rt.loop).add_done_callback(done)

    def _rt_speak(self, sentence, text, spoken):
        """Append one clause to the reply's realtime turn. False = use HTTP instead."""
        turn = self._rt_turn
        if turn is not None and (turn.sentence != sentence or turn.ended):
            turn = None
        if turn is not None and turn.error is not None:
            self._rt_requeue(turn)
            return False
        if turn is None:
            if self._http_sentence == sentence or time.monotonic() < self._rt_down_until:
                return False
            self.prewarm()     # no-op when already connected or connecting
            rt = self._rt
            # Never stall a reply on a slow WebSocket handshake: wait briefly, else
            # speak this whole reply over HTTP and let the connection finish in the
            # background for the next one.
            deadline = time.monotonic() + self.rt_wait_s
            while rt.ws is None and time.monotonic() < deadline and not self._stale(sentence):
                time.sleep(0.02)
            if rt.ws is None:
                self._http_sentence = sentence
                logger.bind(tag=TAG).info('BreezeTTS 实时连接尚未就绪，本次回复改用逐句 HTTP')
                return False
            self._rt_turns += 1
            turn = _RtTurn(f'df-{self._rt_turns}', sentence)
            try:
                self._rt.call(self._rt.start(turn), self.rt_connect_timeout + 4)
            except Exception as error:
                self._rt_trip(error)
                return False
            self._rt_turn = turn
        turn.clauses.append((text, turn.est_s))
        turn.est_s += len(spoken) * SECONDS_PER_CHAR
        try:
            self._rt.call(self._rt.append(turn, spoken), 5)
        except Exception as error:
            turn.error = turn.error or (error if isinstance(error, BreezeError) else BreezeError(type(error).__name__))
            self._rt_trip(turn.error)
            self._rt_requeue(turn)
            return True     # requeue already handed this clause to HTTP
        return True

    def _rt_release(self):
        turn = self._rt_turn
        if turn is None:
            return
        self._rt_turn = None
        turn.ended = True
        if turn.error is None and not self._stale(turn.sentence):
            try:
                self._rt.call(self._rt.end(turn), 5)
            except Exception as error:
                turn.error = BreezeError(f'实时结束失败：{type(error).__name__}')
        # Long replies stream at faster than real time; allow generous time.
        deadline = time.monotonic() + 20 + turn.est_s
        while not turn.done.wait(0.1):
            if self._stale(turn.sentence) or time.monotonic() > deadline:
                if not turn.cancel_sent and self._rt is not None:
                    turn.cancel_sent = True
                    try:
                        self._rt.call(self._rt._send({'type': 'turn.cancel'}), 2)
                    except Exception:
                        pass
                if time.monotonic() > deadline:
                    turn.error = turn.error or BreezeError('实时合成超时')
                    turn.done.wait(2)
                break
        if turn.error is not None and not self._stale(turn.sentence):
            self._rt_trip(turn.error)
            self._rt_requeue(turn)

    def _rt_trip(self, error):
        wait = self.cooldown_s
        try:
            if getattr(error, 'retry_after', None):
                wait = max(wait, min(600.0, float(error.retry_after)))
        except (TypeError, ValueError):
            pass
        self._rt_down_until = time.monotonic() + wait
        logger.bind(tag=TAG).warning(f'BreezeTTS 实时通道暂停 {wait:.0f}s，改用逐句 HTTP：{error}')

    def _rt_requeue(self, turn):
        """Hand clauses that never started playing to the HTTP path, in order."""
        if turn.requeued:
            return
        turn.requeued = True
        if turn.done.is_set() is False and turn.any_audio:
            turn.done.wait(1)
        for original, _ in turn.clauses[turn.next_first:]:
            turn.next_first += 1
            self._http_speak(turn.sentence, original, self._spoken(original))

    def _rt_put_firsts(self, turn, upto_s):
        while turn.next_first < len(turn.clauses) and turn.clauses[turn.next_first][1] <= upto_s:
            self.tts_audio_queue.put((SentenceType.FIRST, None, turn.clauses[turn.next_first][0], turn.sentence))
            turn.next_first += 1

    def _rt_audio(self, turn, pcm):
        """Realtime loop thread: encode PCM as it arrives and queue it for the device."""
        if self._stale(turn.sentence) or len(pcm) < 2:
            return
        import numpy as np
        samples = np.frombuffer(pcm[:len(pcm) & ~1], dtype='<i2')
        if self.level:
            samples = self._level(samples)
        elif self.volume != 1.0:   # realtime has no server-side volume
            samples = np.clip(samples.astype(np.float32) * self.volume, -32768, 32767).astype('<i2')
        if not turn.any_audio:
            turn.any_audio = True
            turn.encoder = opus_encoder_utils.OpusEncoderUtils(sample_rate=SAMPLE_RATE, channels=1, frame_size_ms=60)
            if self.lead_in_s > 0:
                samples = np.concatenate([np.zeros(int(SAMPLE_RATE * self.lead_in_s), dtype='<i2'), samples])
        self._rt_put_firsts(turn, turn.audio_s)
        turn.audio_s += len(samples) / SAMPLE_RATE
        frames = []
        turn.encoder.encode_pcm_to_opus_stream(samples.tobytes(), False, frames.append)
        if frames:
            self.tts_audio_queue.put((SentenceType.MIDDLE, frames, None, turn.sentence))

    def _rt_finish(self, turn):
        if turn.error is not None or self._stale(turn.sentence) or not turn.any_audio:
            return
        self._rt_put_firsts(turn, float('inf'))   # report every clause even if timing was off
        frames = []
        turn.encoder.encode_pcm_to_opus_stream(b'', True, frames.append)
        if frames:
            self.tts_audio_queue.put((SentenceType.MIDDLE, frames, None, turn.sentence))

    async def close(self):
        await super().close()
        self._pool.shutdown(wait=False, cancel_futures=True)
        if self._rt is not None:
            await asyncio.to_thread(self._rt.shutdown)
            self._rt = None

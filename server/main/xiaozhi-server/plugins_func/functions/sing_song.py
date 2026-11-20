"""唱歌：让鲸鱼娘用 RVC 翻唱用户曲库里的歌。

渲染在用户 Windows 电脑上的 singing/worker.py（GPU）里完成，这里只负责：
  * 模糊匹配歌名，向 worker 请求渲染；成品缓存在 data/songs/<render_id>.wav；
  * 有缓存：本轮先说一句开场白，再把 WAV 作为 FILE 消息放进 TTS 队列播放；
  * 没练过：先回一句“这首要先练一下，练好了叫你”，后台轮询 worker。练习期间不因为没人说话
    就断开语音连接；练好后等设备空闲，主动问“练好啦，要不要现在听？”，用户说要/好再唱；
    设备已断开就留着，下次连上、说完第一句话后再问；
  * 用户给了链接（url）：worker 先用 yt-dlp 下载到下载目录，再按上面的流程练；
  * worker 连不上：直说服务没开（已缓存的歌照样能唱）。
播放走现有 TTS 队列，用户打断（client_abort）会照常停止。
"""
import asyncio
from difflib import SequenceMatcher
import json
import os
import re
import time
import unicodedata
import uuid
from typing import TYPE_CHECKING

import aiohttp

from config.logger import setup_logging
from plugins_func.register import register_function, ToolType, ActionResponse, Action

if TYPE_CHECKING:
    from core.connection import ConnectionHandler

TAG = __name__
logger = setup_logging()

DEFAULTS = {
    "enabled": True,
    "worker_url": "http://host.docker.internal:8790",
    "token": "",
    "cache_dir": "data/songs",
    "max_wait_s": 900,
    "poll_s": 3,
    "idle_wait_s": 120,
    "answer_wait_s": 120,      # 问完“要不要听”后，等用户回答的这段时间不断开
    "offer_keep_s": 86400,     # 设备断开时，练好的歌留多久等下次连上再问
    "greet_wait_s": 300,
    "quiet_s": 3,              # 主动开口前，设备要安静这么久（秒）
    "request_timeout_s": 8,
}

MSG_OFFLINE = "唱歌服务没开，先在电脑上启动 singing/worker.py 吧。"
MSG_TOKEN = "唱歌服务连上了，但口令不对：把 singing/token.txt 里的口令填到 data/server.env 的 SINGING_TOKEN，再重启语音服务。"
MSG_NO_MODEL = "我的唱歌音色模型还没放好，先按 singing/README.zh-CN.md 训练一个放进 singing/models 吧。"
MSG_EMPTY = "曲库还是空的，先把歌曲文件放进电脑上的原曲目录，或者在管理页“唱歌”里贴一个歌曲链接吧。"
MSG_NO_YTDLP = "唱歌服务还没装下载工具，在电脑上运行 singing\\venv\\Scripts\\python -m pip install yt-dlp 再试。"

# 后台练歌任务（render_id 或 download_id -> Task）及其设备
_tasks = {}
_task_devices = {}
# 练好但还没唱给用户听的歌（device_id -> [歌名]）
_notices = {}
# 练好后问过/要问“要不要听”的歌：device_id -> {"title", "path", "asked", "expires"}
_offers = {}


# ---------------------------------------------------------------- 配置
def _config(conn):
    cfg = dict(DEFAULTS)
    cfg.update((getattr(conn, "config", None) or {}).get("singing") or {})
    cfg["token"] = os.environ.get("SINGING_TOKEN") or str(cfg.get("token") or "")
    path = str(cfg["cache_dir"])
    if not os.path.isabs(path):
        from config.config_loader import get_project_dir
        path = os.path.join(get_project_dir(), path)
    cfg["cache_dir"] = path
    return cfg


# ---------------------------------------------------------------- 歌名匹配（与 singing/songmatch.py 相同规则）
_DROP = re.compile(r"[\s\-_·•.,，。!！?？'\"“”‘’《》<>()（）\[\]【】~～&+]+")
_TAGS = re.compile(r"[(\[（【](live|伴奏|纯音乐|cover|翻唱|现场|remix|inst\w*|official.*?|高品质|无损|hq|sq|mqms\d*|\d{1,2})[)\]）】]", re.I)


# Require an explicit request at the start so conversation about a song stays conversational.
_SONG_REQUEST = re.compile(
    r"(?:请(?:你)?|能不能|可以)?(?:给我|帮我|为我)?"
    r"(?:唱(?:一首|首|一下)?|来一首|播放(?:一首)?)\s*([^，,。！？!?；;\n]{1,64})"
)


def requested_song(text):
    if not isinstance(text, str):
        return None
    text = unicodedata.normalize("NFKC", text).strip().rstrip("。.!?！？~～ ")
    match = _SONG_REQUEST.fullmatch(text)
    if not match:
        return None
    name = match[1].strip().removesuffix("给我听").strip().rstrip("吧呀啊呗嘛").strip()
    name = name.removesuffix("给我听").strip()
    if not name or name in ("歌", "首歌", "一首歌", "什么歌", "一下", "一首"):
        return None
    # Requests with extra instructions and questions can still use the LLM tools.
    if re.search(r"[吗么]|怎么|能否|为什么|是不是|升调|降调|一遍|刚才|然后|再唱", name):
        return None
    return name


def normalize(text):
    text = unicodedata.normalize("NFKC", str(text or "")).lower()
    return _DROP.sub("", _TAGS.sub("", text))


def score(query, title):
    q = normalize(query)
    if not q:
        return 0.0
    best = 0.0
    for part in [title] + [p for p in re.split(r"\s+-\s+|_-_|–|—", str(title)) if p.strip()]:
        t = normalize(part)
        if not t:
            continue
        if q == t:
            return 1.0
        if q in t:
            s = 0.7 + 0.3 * len(q) / len(t)
        elif t in q:
            s = 0.65 + 0.3 * len(t) / len(q)
        else:
            s = 0.8 * SequenceMatcher(None, q, t).ratio()
        best = max(best, s)
    return best


def best_match(query, titles, threshold=0.5):
    ranked = sorted(((score(query, t), t) for t in titles), key=lambda x: -x[0])
    if ranked and ranked[0][0] >= threshold:
        return ranked[0][1]
    return None


# ---------------------------------------------------------------- 本地缓存 data/songs/
class LocalCache:
    def __init__(self, directory):
        self.dir = directory
        self.index_path = os.path.join(directory, "index.json")

    def _index(self):
        try:
            with open(self.index_path, "r", encoding="utf-8") as f:
                data = json.load(f)
            return data if isinstance(data, dict) else {}
        except (OSError, ValueError):
            return {}

    def path(self, render_id):
        return os.path.join(self.dir, f"{render_id}.wav")

    def get(self, render_id):
        return self.path(render_id) if os.path.isfile(self.path(render_id)) else None

    def items(self):
        return {rid: meta for rid, meta in self._index().items() if self.get(rid)}

    def match(self, song):
        """worker 不在时按歌名找已缓存的翻唱：返回 (title, path)。"""
        items = sorted(self.items().items(), key=lambda kv: -kv[1].get("created", 0))
        title = best_match(song, [meta.get("title", "") for _, meta in items])
        if title is None:
            return None
        rid = next(rid for rid, meta in items if meta.get("title") == title)
        return title, self.path(rid)

    def add(self, render_id, title, data):
        os.makedirs(self.dir, exist_ok=True)
        tmp = self.path(render_id) + ".part"
        with open(tmp, "wb") as f:
            f.write(data)
        os.replace(tmp, self.path(render_id))
        index = self._index()
        index[render_id] = {"title": title, "created": time.time()}
        tmp = self.index_path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(index, f, ensure_ascii=False, indent=1)
        os.replace(tmp, self.index_path)
        return self.path(render_id)


# ---------------------------------------------------------------- worker 客户端
class WorkerError(Exception):
    def __init__(self, message, kind="offline"):
        super().__init__(message)
        self.kind = kind


class WorkerClient:
    def __init__(self, cfg):
        self.url = str(cfg["worker_url"]).rstrip("/")
        self.headers = {"X-Singing-Token": cfg["token"]}
        self.timeout = float(cfg["request_timeout_s"])

    async def _call(self, method, path, body=None, raw=False, timeout=None):
        try:
            async with aiohttp.ClientSession(
                    timeout=aiohttp.ClientTimeout(total=timeout or self.timeout)) as session:
                async with session.request(method, self.url + path, json=body, headers=self.headers) as resp:
                    if resp.status == 401:
                        raise WorkerError("unauthorized", "token")
                    if raw:
                        if resp.status != 200:
                            raise WorkerError(f"HTTP {resp.status}", "http")
                        return await resp.read()
                    try:
                        data = await resp.json(content_type=None)
                    except ValueError:
                        data = {}
                    return resp.status, data
        except WorkerError:
            raise
        except (aiohttp.ClientError, asyncio.TimeoutError, OSError) as error:
            raise WorkerError(type(error).__name__) from None

    async def songs(self):
        status, data = await self._call("GET", "/songs")
        if status != 200:
            raise WorkerError(f"HTTP {status}", "http")
        return data

    async def render(self, song, pitch=None):
        body = {"song": song}
        if pitch is not None:
            body["pitch"] = pitch
        return await self._call("POST", "/render", body)

    async def job(self, job_id):
        return await self._call("GET", f"/jobs/{job_id}")

    async def audio(self, render_id):
        return await self._call("GET", f"/audio/{render_id}", raw=True, timeout=max(30.0, self.timeout))

    async def download(self, url, title="", pitch=None):
        body = {"url": url, "title": title or "", "render": True}
        if pitch is not None:
            body["pitch"] = pitch
        return await self._call("POST", "/download", body)

    async def download_state(self, download_id):
        return await self._call("GET", f"/downloads/{download_id}")

    async def get_config(self):
        return await self._call("GET", "/config")

    async def put_config(self, body):
        return await self._call("PUT", "/config", body)

    async def health(self):
        return await self._call("GET", "/health")


def _plain(error):
    return MSG_TOKEN if getattr(error, "kind", "") == "token" else MSG_OFFLINE


# ---------------------------------------------------------------- 播放
def _queue_song(conn, sentence_id, intro, path, title):
    """开场白 → 立即合成 → 歌曲文件，全部按顺序进入 TTS 文本队列。"""
    from core.providers.tts.dto.dto import TTSMessageDTO, SentenceType, ContentType
    q = conn.tts.tts_text_queue
    if intro:
        q.put(TTSMessageDTO(sentence_id, SentenceType.MIDDLE, ContentType.TEXT, intro))
        q.put(TTSMessageDTO(sentence_id, SentenceType.MIDDLE, ContentType.ACTION))
    if path:
        q.put(TTSMessageDTO(sentence_id, SentenceType.MIDDLE, ContentType.FILE, title, path))


def _intro(title):
    return f"那我唱《{title}》给你听。"


def _is_idle(conn, quiet_s=3.0):
    if conn.stop_event.is_set() or getattr(conn, "tts", None) is None:
        return False
    if conn.client_is_speaking or getattr(conn, "client_have_voice", False):
        return False
    last = float(getattr(conn, "last_activity_time", 0) or 0)
    return time.time() * 1000 - last >= quiet_s * 1000


def _live_conn(conn):
    """原连接断了就找同一台设备的新连接。"""
    if not conn.stop_event.is_set():
        return conn
    device = getattr(conn, "device_id", None)
    for other in list(getattr(getattr(conn, "server", None), "connections", ()) or ()):
        if (device and getattr(other, "device_id", None) == device and not other.stop_event.is_set()
                and getattr(other, "tts", None) is not None):
            return other
    return None


async def _start_turn(conn, text, path=None, title=None):
    """主动开一轮：和管理页面的分句测试一样，start → FIRST → 内容 → LAST。"""
    from core.handle.sendAudioHandle import send_tts_message
    from core.providers.tts.dto.dto import TTSMessageDTO, SentenceType, ContentType
    sentence_id = uuid.uuid4().hex
    conn.sentence_id = sentence_id
    conn.client_abort = False
    conn.client_is_speaking = True
    await send_tts_message(conn, "start")
    q = conn.tts.tts_text_queue
    q.put(TTSMessageDTO(sentence_id, SentenceType.FIRST, ContentType.ACTION))
    _queue_song(conn, sentence_id, text, path, title)
    q.put(TTSMessageDTO(sentence_id, SentenceType.LAST, ContentType.ACTION))
    try:
        from core.conversation_history import record_event
        record_event(conn, "status", f"唱歌：{text}")
    except Exception:
        pass


async def _announce(conn, cfg, text, path=None, title=None, offer=False, wait_s=None, after_ms=0):
    """等设备空闲再主动开口；超时或断线就留到下次。

    offer=True：只问“要不要听”，记下待回答的歌，不直接唱。after_ms>0：要等这之后用户先说过话。
    """
    deadline = time.monotonic() + float(cfg["idle_wait_s"] if wait_s is None else wait_s)
    while True:
        target = _live_conn(conn)
        if target is None:
            break
        spoke = float(getattr(target, "last_activity_time", 0) or 0) > after_ms
        if (not after_ms or spoke) and _is_idle(target, float(cfg["quiet_s"])):
            if offer:
                target.listen_after_turn = True      # 问完就听回答，不用再按键
                await _start_turn(target, text)
                _remember_offer(target, cfg, title, path, asked=True)
                # 给用户留出回答的时间，期间不因为没说话而断开
                hold_connection(target, float(cfg["answer_wait_s"]))
                target.last_activity_time = time.time() * 1000
            else:
                await _start_turn(target, text, path, title)
            return True
        if time.monotonic() > deadline:
            break
        await asyncio.sleep(1)
    if path and title:
        device = getattr(conn, "device_id", None) or ""
        if title not in _notices.setdefault(device, []):
            _notices[device].append(title)
        if offer:
            _remember_offer(conn, cfg, title, path, asked=False)
    return False


# ---------------------------------------------------------------- 练习期间保持连接
def hold_connection(conn, seconds):
    """接下来 seconds 秒内，服务端不因“没人说话”关闭这条语音连接（只延长，不缩短）。"""
    until = time.time() + max(0.0, float(seconds))
    conn.singing_hold_until = max(float(getattr(conn, "singing_hold_until", 0) or 0), until)


def connection_held(conn):
    return float(getattr(conn, "singing_hold_until", 0) or 0) > time.time()


def _device(conn):
    return getattr(conn, "device_id", None) or ""


def _practising(device):
    return any(d == device for d in _task_devices.values())


def _track(key, conn, coro, cfg):
    _task_devices[key] = _device(conn)
    hold_connection(conn, float(cfg["max_wait_s"]) + 60)
    _tasks[key] = asyncio.create_task(coro)
    device = _device(conn)
    glow = _faces.get(device)
    if glow is None or glow.done():
        _faces[device] = asyncio.create_task(_practice_face(conn, device))


_faces = {}


async def _send_face(conn, emotion):
    try:
        await conn.websocket.send(json.dumps({"type": "llm", "text": emotion, "emotion": emotion,
                                              "session_id": getattr(conn, "session_id", "")}))
    except Exception:
        pass


async def _practice_face(conn, device, period=3.0):
    """练歌期间设备空闲时，Flipper 主页显示拿着麦克风练歌（设备回到待机会把表情重置，所以定时重发）。"""
    shown = False
    try:
        while _practising(device):
            target = _live_conn(conn)
            # 设备在听或在说时 Flipper 自己显示对应动画，这里只在空闲时换成练歌
            if target is not None and not target.client_is_speaking and not getattr(target, "client_have_voice", False):
                await _send_face(target, "practicing")
                shown = True
            await asyncio.sleep(period)
    finally:
        _faces.pop(device, None)
        target = _live_conn(conn)
        if shown and target is not None and not target.client_is_speaking:
            await _send_face(target, "neutral")


def _untrack(key, conn):
    _tasks.pop(key, None)
    device = _task_devices.pop(key, None)
    if device is None or _practising(device):
        return
    if (_offers.get(device) or {}).get("asked"):
        return                  # 刚问过“要不要听”，等待回答的保持时间已经设好
    # 没有在练的歌了：恢复正常的空闲断开，从现在重新计时
    target = _live_conn(conn) or conn
    target.singing_hold_until = 0
    target.last_activity_time = time.time() * 1000


# ---------------------------------------------------------------- 练好后问“要不要听”
_YES = re.compile(r"(?:嗯+|好+|好的|好呀|好啊|好吧|行|可以|要|要的|要听|想听|听|听听|听吧|唱|唱吧|唱呀|唱给我听|你唱|来|来吧|"
                  r"来一首|当然|当然要|是的|对|ok|okay|yes|现在|现在听|现在唱|放吧|快唱|快唱吧)+")
_NO = re.compile(r"不要|不用|不听|不想|先不|等会|等一会|待会|晚点|以后|下次|不了|算了|别唱|别了|\bno\b")
_FILLER = re.compile(r"[\s,，。.!！?？~～、…]+|^(那|嗯那|那就|那你)|(吧|呀|啊|呢|哦|喔|嘛|啦)+$")


def _remember_offer(conn, cfg, title, path, asked):
    if not title or not path:
        return
    keep = float(cfg["answer_wait_s"] if asked else cfg["offer_keep_s"])
    _offers[_device(conn)] = {"title": title, "path": path, "asked": asked, "expires": time.time() + keep}


def take_offer(conn, text):
    """用户回答“要不要听”：返回 ("play"|"decline", offer)；不是回答就返回 None 并作废这次询问。"""
    device = _device(conn)
    offer = _offers.get(device)
    if not offer or not offer.get("asked"):
        return None
    _offers.pop(device, None)
    if offer["expires"] < time.time() or not os.path.isfile(offer["path"]):
        return None
    raw = unicodedata.normalize("NFKC", str(text or "")).strip().lower()
    short = _FILLER.sub("", raw)
    if len(raw) <= 16 and _NO.search(raw):
        return "decline", offer
    if len(raw) <= 16 and short and _YES.fullmatch(short):
        return "play", offer
    # 说了别的事：不当成回答，歌记在“刚练好”里，问“会唱什么”时会提到
    if offer["title"] not in _notices.setdefault(device, []):
        _notices[device].append(offer["title"])
    return None


def answer_offer(conn, kind, offer):
    if kind == "play":
        return _play_now(conn, offer["title"], offer["path"])
    _consume_notice(conn, offer["title"])
    text = f"好，《{offer['title']}》我留着，想听的时候跟我说“唱{offer['title']}”就行。"
    return ActionResponse(Action.RESPONSE, result=text, response=text)


def greet_pending(conn):
    """设备新连上：如果上次练好的歌还没问过，等用户先说完一句话，再问要不要听。"""
    cfg = _config(conn)
    device = _device(conn)
    offer = _offers.get(device)
    if not cfg.get("enabled", True) or not offer or offer.get("asked") or offer["expires"] < time.time():
        return None
    if not os.path.isfile(offer["path"]):
        _offers.pop(device, None)
        return None
    _offers.pop(device, None)
    text = f"对了，你之前点的《{offer['title']}》我练好啦，要不要现在听？"
    return asyncio.create_task(_announce(conn, cfg, text, offer["path"], offer["title"], offer=True,
                                         wait_s=float(cfg["greet_wait_s"]),
                                         after_ms=time.time() * 1000))


async def _finish_render(conn, cfg, client, job, render_id, title):
    data = await client.audio(render_id)
    path = await asyncio.to_thread(LocalCache(cfg["cache_dir"]).add, render_id, title, data)
    logger.bind(tag=TAG).info(f"《{title}》练好了：{path}")
    await _announce(conn, cfg, f"《{title}》练好啦，要不要现在听？", path, title, offer=True)


async def _follow(conn, cfg, client, job_id, render_id, title, key=None):
    """后台轮询渲染进度，练好后下载成品并问用户要不要听。"""
    deadline = time.monotonic() + float(cfg["max_wait_s"])
    misses = 0
    try:
        while time.monotonic() < deadline:
            await asyncio.sleep(float(cfg["poll_s"]))
            try:
                status, job = await client.job(job_id)
            except WorkerError:
                misses += 1
                if misses >= 10:
                    logger.bind(tag=TAG).warning(f"唱歌服务断开，放弃等待《{title}》")
                    return
                continue
            misses = 0
            if status == 404:
                logger.bind(tag=TAG).warning(f"唱歌服务重启过，任务 {job_id} 丢失")
                return
            state = job.get("state")
            if state == "done":
                await _finish_render(conn, cfg, client, job, render_id, title)
                return
            if state == "error":
                logger.bind(tag=TAG).warning(f"《{title}》渲染失败：{job.get('error')}")
                await _announce(conn, cfg, f"《{title}》没练成：{job.get('error') or '渲染出错'}")
                return
        logger.bind(tag=TAG).warning(f"《{title}》超过 {cfg['max_wait_s']} 秒还没练好，停止等待")
    except Exception as error:
        logger.bind(tag=TAG).error(f"唱歌后台任务出错：{type(error).__name__}: {error}")
    finally:
        _untrack(key or render_id, conn)


async def _follow_download(conn, cfg, client, download_id, fallback_title):
    """链接点歌：等 worker 下载完，再接着等它练好。"""
    deadline = time.monotonic() + float(cfg["max_wait_s"])
    misses = 0
    try:
        while time.monotonic() < deadline:
            await asyncio.sleep(float(cfg["poll_s"]))
            try:
                status, item = await client.download_state(download_id)
            except WorkerError:
                misses += 1
                if misses >= 10:
                    return
                continue
            misses = 0
            if status == 404:
                return
            if item.get("state") == "error":
                await _announce(conn, cfg, f"那个链接没下载下来：{item.get('error') or '下载出错'}")
                return
            if item.get("state") != "done":
                continue
            render = item.get("render") or {}
            title = render.get("title") or item.get("title") or fallback_title
            rid = render.get("render_id")
            if not rid:
                await _announce(conn, cfg, f"《{title}》下载好了，但没能开始练：{render.get('error') or '曲库里找不到'}")
                return
            if render.get("state") == "done":
                await _finish_render(conn, cfg, client, render, rid, title)
                return
            job_id = render.get("job_id") or render.get("id")
            if render.get("state") == "error" or not job_id:
                await _announce(conn, cfg, f"《{title}》没练成：{render.get('error') or render.get('message') or '渲染出错'}")
                return
            await _follow(conn, cfg, client, job_id, rid, title, key=download_id)
            return
    except Exception as error:
        logger.bind(tag=TAG).error(f"链接点歌后台任务出错：{type(error).__name__}: {error}")
    finally:
        _untrack(download_id, conn)


def _play_now(conn, title, path):
    intro = _intro(title)
    _queue_song(conn, conn.sentence_id, intro, path, title)
    _consume_notice(conn, title)
    offer = _offers.get(_device(conn))
    if offer and offer.get("title") == title:
        _offers.pop(_device(conn), None)
    return ActionResponse(Action.RECORD, result=f"正在播放鲸鱼娘翻唱的《{title}》", response=intro)


def _consume_notice(conn, title):
    pending = _notices.get(getattr(conn, "device_id", None) or "")
    if pending and title in pending:
        pending.remove(title)


# ---------------------------------------------------------------- 工具
sing_song_desc = {
    "type": "function",
    "function": {
        "name": "sing_song",
        "description": (
            "让鲸鱼娘用自己的声音唱歌（翻唱用户电脑曲库里的歌，或用户给的歌曲链接）。用户说“唱首歌”“唱一下某某”“来一首某某”时调用。"
            "练好的歌会立刻开唱；没练过的歌要先练一两分钟，练好后鲸鱼娘会主动问用户要不要听。"
            "工具会自己说话和播放，调用后不要再重复回复。"
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "song": {"type": "string", "description": "歌名，可以带歌手，例如“晴天”或“周杰伦 晴天”；用户没指定就留空"},
                "pitch": {"type": "integer", "description": "仅当用户明确要求升调/降调时填写的半音数（-24 到 24），否则不要填"},
                "url": {"type": "string", "description": "仅当用户原文给出 http(s) 歌曲链接（B 站、YouTube 等）时原样填写，否则留空"},
            },
            "required": [],
        },
    },
}

list_songs_desc = {
    "type": "function",
    "function": {
        "name": "list_songs",
        "description": "查看鲸鱼娘会唱哪些歌（电脑曲库和已经练好的翻唱）。用户问“你会唱什么歌”“有哪些歌”时调用。",
        "parameters": {"type": "object", "properties": {}, "required": []},
    },
}


@register_function("sing_song", sing_song_desc, ToolType.SYSTEM_CTL)
async def sing_song(conn: "ConnectionHandler", song: str = "", pitch=None, url: str = ""):
    cfg = _config(conn)
    if not cfg.get("enabled", True):
        return ActionResponse(Action.RESPONSE, result="唱歌功能未开启", response="唱歌功能还没打开。")
    song = str(song or "").strip()
    url = str(url or "").strip()
    if url:
        return await _sing_from_url(conn, cfg, url, song, pitch)
    if not song:
        return await list_songs(conn)
    cache = LocalCache(cfg["cache_dir"])
    client = WorkerClient(cfg)
    try:
        status, data = await client.render(song, pitch)
    except WorkerError as error:
        hit = cache.match(song)
        if hit:
            return _play_now(conn, *hit)
        message = _plain(error)
        return ActionResponse(Action.RESPONSE, result=message, response=message)

    if status == 200 and data.get("state") == "done":
        title, rid = data.get("title") or song, data["render_id"]
        path = cache.get(rid)
        if path is None:
            try:
                path = await asyncio.to_thread(cache.add, rid, title, await client.audio(rid))
            except WorkerError as error:
                message = _plain(error)
                return ActionResponse(Action.RESPONSE, result=message, response=message)
        return _play_now(conn, title, path)

    if status == 202:
        title, rid = data.get("title") or song, data["render_id"]
        if rid in _tasks:
            hold_connection(conn, float(cfg["max_wait_s"]) + 60)
            text = f"《{title}》还在练，练好了我马上叫你。"
        else:
            _track(rid, conn, _follow(conn, cfg, client, data["job_id"], rid, title), cfg)
            ahead = int(data.get("position") or 1) - 1
            wait = "大概一两分钟" if ahead <= 0 else f"前面还有 {ahead} 首在排队，要多等一会儿"
            text = f"《{title}》我还没练过，得先练一下，{wait}。练好了我叫你。"
        return ActionResponse(Action.RESPONSE, result=text, response=text)

    if status == 404:
        hit = cache.match(song)
        if hit:
            return _play_now(conn, *hit)
        options = [t for t in data.get("suggestions") or [] if t][:5]
        text = f"曲库里没有《{song}》。" + (f"我现在会唱：{'、'.join(options)}。" if options else MSG_EMPTY)
        return ActionResponse(Action.RESPONSE, result=text, response=text)
    if status == 409:
        return ActionResponse(Action.RESPONSE, result=MSG_NO_MODEL, response=MSG_NO_MODEL)
    if status == 429:
        text = "现在排队的歌太多了，等一会儿再点吧。"
        return ActionResponse(Action.RESPONSE, result=text, response=text)
    text = f"唱歌服务出错了：{data.get('error') or status}"
    return ActionResponse(Action.RESPONSE, result=text, response=text)


def follow_download(conn, download_id, title=""):
    """管理页贴的链接：在连接自己的事件循环里跟进，练好后照常问“要不要听”。"""
    if download_id in _tasks:
        return
    cfg = _config(conn)
    _track(download_id, conn, _follow_download(conn, cfg, WorkerClient(cfg), download_id, title or "这首歌"), cfg)


async def _sing_from_url(conn, cfg, url, title, pitch):
    """用户给了歌曲链接：交给 worker 下载并练习，练好后照常询问。"""
    if len(url) > 500 or not re.match(r"^https?://\S+$", url, re.I):
        text = "这个链接看不懂，要以 http:// 或 https:// 开头。"
        return ActionResponse(Action.RESPONSE, result=text, response=text)
    client = WorkerClient(cfg)
    try:
        status, data = await client.download(url, title, pitch)
    except WorkerError as error:
        message = _plain(error)
        return ActionResponse(Action.RESPONSE, result=message, response=message)
    if status == 503:
        return ActionResponse(Action.RESPONSE, result=MSG_NO_YTDLP, response=MSG_NO_YTDLP)
    if status == 429:
        text = "现在下载的歌太多了，等一会儿再发链接吧。"
        return ActionResponse(Action.RESPONSE, result=text, response=text)
    if status != 202 or not data.get("download_id"):
        text = f"这个链接没法下载：{data.get('error') or status}"
        return ActionResponse(Action.RESPONSE, result=text, response=text)
    did = data["download_id"]
    _track(did, conn, _follow_download(conn, cfg, client, did, title or "这首歌"), cfg)
    name = f"《{title}》" if title else "这首歌"
    text = f"好，我先把{name}下载下来练一下，可能要两三分钟，练好了我叫你。"
    return ActionResponse(Action.RESPONSE, result=text, response=text)


@register_function("list_songs", list_songs_desc, ToolType.SYSTEM_CTL)
async def list_songs(conn: "ConnectionHandler"):
    cfg = _config(conn)
    cached = sorted({m.get("title", "") for m in LocalCache(cfg["cache_dir"]).items().values()} - {""})
    fresh = [t for t in _notices.pop(getattr(conn, "device_id", None) or "", []) if t]
    try:
        data = await WorkerClient(cfg).songs()
    except WorkerError as error:
        if not cached:
            message = _plain(error)
            return ActionResponse(Action.RESPONSE, result=message, response=message)
        text = f"唱歌服务没开，学不了新歌；已经练好、可以直接唱的有：{'、'.join(cached)}。"
        return ActionResponse(Action.REQLLM, result=text, response=None)
    titles = [s.get("title") for s in data.get("songs") or [] if s.get("title")]
    ready = sorted(set(cached) | {r.get("title") for r in data.get("renders") or [] if r.get("title")})
    if not titles and not ready:
        return ActionResponse(Action.RESPONSE, result=MSG_EMPTY, response=MSG_EMPTY)
    lines = [f"曲库（{len(titles)} 首）：{'、'.join(titles[:30])}" + ("等" if len(titles) > 30 else "")]
    if ready:
        lines.append(f"已经练好、马上能唱：{'、'.join(ready[:20])}")
    if fresh:
        lines.append(f"刚才练好但还没唱给用户听：{'、'.join(fresh)}")
    lines.append("新歌第一次唱要先练一两分钟。请用一两句话告诉用户，并问想听哪首。")
    return ActionResponse(Action.REQLLM, result="\n".join(lines), response=None)

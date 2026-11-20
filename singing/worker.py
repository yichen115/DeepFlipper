"""DeepFlipper 唱歌渲染服务（Windows 本机运行，使用 GPU）。

    venv\\Scripts\\python worker.py            # 读取同目录 config.yaml
    venv\\Scripts\\python worker.py --check    # 只检查环境

接口（除 /health 外都需要请求头 X-Singing-Token）：
    GET  /health              GPU、模型、队列状态
    GET  /songs               曲库（各原曲目录里的文件）+ 已渲染的翻唱
    POST /render              {"song","pitch":"auto"|半音,"clip_seconds","start_ratio"} → 任务（有缓存直接 done）
    GET  /jobs/{id}           任务状态/进度/错误
    GET  /audio/{render_id}   成品：24 kHz 单声道 16 位 WAV
    GET  /config              原曲目录、下载目录、成品目录
    PUT  /config              {"songs_dirs":[...],"download_dir","cache_dir"} 修改并写入 config.local.yaml
    POST /download            {"url","title","render":true,"pitch"} 用 yt-dlp 下载链接里的音频到下载目录，可接着练
    GET  /downloads/{id}      下载进度；练歌任务见其中的 render
"""
import argparse
import asyncio
import hashlib
import hmac
import json
import logging
import os
import re
import secrets
import shutil
import sys
import time
import uuid

from aiohttp import web
import yaml

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import songmatch  # noqa: E402
from pipeline import Pipeline, RenderError, write_json  # noqa: E402

log = logging.getLogger("singing")
AUDIO_EXTS = {".mp3", ".flac", ".wav", ".m4a", ".ogg", ".opus", ".aac", ".wma", ".ape", ".webm", ".mka"}
RENDER_ID = re.compile(r"^[0-9a-f]{16}$")
CACHE_VERSION = "v1"
LOCAL_CONFIG = "config.local.yaml"     # 管理页改的路径写在这里，覆盖 config.yaml
PATH_KEYS = ("songs_dirs", "download_dir", "cache_dir")
MAX_SONG_DIRS = 8
SCAN_DEPTH = 3                         # 原曲目录往下找几层子文件夹（歌手/专辑/歌曲）
SCAN_LIMIT = 5000
DOWNLOAD_ID = re.compile(r"^[0-9a-f]{12}$")
URL = re.compile(r"^https?://[^\s/$.?#][^\s]*$", re.I)


def load_config(path):
    with open(path, "r", encoding="utf-8") as f:
        cfg = yaml.safe_load(f) or {}
    local = os.path.join(os.path.dirname(os.path.abspath(path)), LOCAL_CONFIG)
    if os.path.isfile(local):
        with open(local, "r", encoding="utf-8") as f:
            overlay = yaml.safe_load(f) or {}
        cfg.update({k: v for k, v in overlay.items() if k in PATH_KEYS})
    for key in ("separator", "rvc", "mix"):
        cfg.setdefault(key, {})
    return cfg


def song_dirs(cfg):
    """songs_dirs（列表）优先；兼容旧的单个 songs_dir。"""
    value = cfg.get("songs_dirs") or cfg.get("songs_dir") or "songs_in"
    items = [value] if isinstance(value, str) else list(value)
    dirs = [str(item).strip() for item in items if str(item or "").strip()]
    return dirs or ["songs_in"]


def resolve_token(cfg, base_dir):
    token = os.environ.get("SINGING_TOKEN") or str(cfg.get("token") or "").strip()
    token_file = os.path.join(base_dir, "token.txt")
    if not token and os.path.isfile(token_file):
        with open(token_file, "r", encoding="utf-8") as f:
            token = f.read().strip()
    if not token:
        token = secrets.token_urlsafe(24)
        with open(token_file, "w", encoding="utf-8") as f:
            f.write(token)
        log.warning("已生成新的口令 singing/token.txt；把同样的值写到语音服务 data/server.env 的 SINGING_TOKEN=")
    return token


def parse_pitch(value, default="auto"):
    if value is None or value == "":
        value = default
    if isinstance(value, str) and value.strip().lower() in ("auto", "自动"):
        return "auto"
    try:
        pitch = int(round(float(value)))
    except (TypeError, ValueError):
        raise ValueError("pitch 应为 auto 或整数半音")
    if not -24 <= pitch <= 24:
        raise ValueError("pitch 范围 -24..24")
    return pitch


class Library:
    """原曲目录（可多个，含子文件夹）里的歌曲；文件哈希按 (路径, 大小, 修改时间) 缓存。"""

    def __init__(self, songs_dirs, skip=()):
        self.dirs = [songs_dirs] if isinstance(songs_dirs, str) else list(songs_dirs)
        self.skip = {os.path.normcase(os.path.abspath(p)) for p in skip if p}
        self._hash = {}

    @property
    def dir(self):
        return self.dirs[0]

    def _digest(self, path):
        """内容指纹：大小 + 开头和结尾各 1 MB（大曲库首次扫描也很快）。"""
        st = os.stat(path)
        key = (path, st.st_size, int(st.st_mtime))
        if key not in self._hash:
            h = hashlib.sha1(str(st.st_size).encode())
            with open(path, "rb") as f:
                h.update(f.read(1 << 20))
                if st.st_size > 2 << 20:
                    f.seek(-(1 << 20), os.SEEK_END)
                    h.update(f.read(1 << 20))
            self._hash[key] = h.hexdigest()[:16]
        return self._hash[key]

    def _walk(self, root):
        root_depth = root.rstrip("\\/").count(os.sep)
        for current, dirs, files in os.walk(root):
            dirs[:] = sorted(d for d in dirs if not d.startswith(".")
                             and os.path.normcase(os.path.abspath(os.path.join(current, d))) not in self.skip)
            if current.rstrip("\\/").count(os.sep) - root_depth >= SCAN_DEPTH:
                dirs[:] = []
            for name in sorted(files):
                if os.path.splitext(name)[1].lower() in AUDIO_EXTS:
                    yield os.path.join(current, name)

    def scan(self):
        songs, seen_titles, seen_paths = [], set(), set()
        for root in self.dirs:
            if not os.path.isdir(root):
                continue
            for path in self._walk(root):
                key = os.path.normcase(os.path.abspath(path))
                stem = os.path.splitext(os.path.basename(path))[0]
                # 同名歌曲只取第一个目录里的那份，避免点歌时随机命中
                if key in seen_paths or stem in seen_titles or not os.path.isfile(path):
                    continue
                seen_paths.add(key)
                seen_titles.add(stem)
                try:
                    digest = self._digest(path)
                except OSError:
                    continue
                songs.append({"title": stem, "file": os.path.relpath(path, root), "path": path,
                              "root": root, "hash": digest})
                if len(songs) >= SCAN_LIMIT:
                    log.warning("曲库超过 %d 首，后面的不再扫描", SCAN_LIMIT)
                    return sorted(songs, key=lambda s: s["title"])
        return sorted(songs, key=lambda s: s["title"])


class RenderCache:
    """cache/<render_id>.wav + cache/<render_id>.json（标题、参数、耗时）。"""

    def __init__(self, cache_dir, max_items=60):
        self.dir = cache_dir
        self.max_items = max_items
        os.makedirs(self.dir, exist_ok=True)

    def audio_path(self, render_id):
        return os.path.join(self.dir, f"{render_id}.wav")

    def get(self, render_id):
        meta = os.path.join(self.dir, f"{render_id}.json")
        if not (os.path.isfile(self.audio_path(render_id)) and os.path.isfile(meta)):
            return None
        with open(meta, "r", encoding="utf-8") as f:
            return json.load(f)

    def put(self, render_id, meta):
        write_json(os.path.join(self.dir, f"{render_id}.json"), meta)
        self.prune()

    def list(self):
        items = []
        for name in os.listdir(self.dir):
            if name.endswith(".json") and RENDER_ID.match(name[:-5]):
                meta = self.get(name[:-5])
                if meta:
                    items.append(meta)
        return sorted(items, key=lambda m: -m.get("created", 0))

    def prune(self):
        for meta in self.list()[self.max_items:]:
            for ext in (".wav", ".json"):
                try:
                    os.remove(os.path.join(self.dir, meta["render_id"] + ext))
                except OSError:
                    pass


class Worker:
    def __init__(self, cfg, base_dir, pipeline=None, downloader=None):
        self.cfg = cfg
        self.base = base_dir
        self.pipeline = pipeline or Pipeline(cfg, base_dir)
        self.downloader = downloader or download_with_ytdlp
        self.max_queue = int(cfg.get("max_queue", 5))
        self.jobs = {}          # job_id -> job
        self.active = {}        # render_id -> job_id（排队或运行中，避免重复渲染）
        self.downloads = {}     # download_id -> 下载任务
        self.download_lock = asyncio.Lock()
        self.queue = asyncio.Queue()
        self.current = None
        self._apply_paths(song_dirs(cfg), cfg.get("download_dir") or "", cfg.get("cache_dir", "cache"))

    def _path(self, value):
        value = os.path.expandvars(os.path.expanduser(str(value)))
        return os.path.normpath(value if os.path.isabs(value) else os.path.join(self.base, value))

    def _apply_paths(self, dirs, download_dir, cache_dir):
        songs = [self._path(d) for d in dirs]
        download = self._path(download_dir) if download_dir else os.path.join(songs[0], "downloads")
        cache = self._path(cache_dir)
        self.cache = RenderCache(cache, int(self.cfg.get("max_cached", 60)))
        # 下载目录不在任何原曲目录里时，也要算进曲库
        inside = any(os.path.normcase(download).startswith(os.path.normcase(d.rstrip("\\/") + os.sep))
                     or os.path.normcase(download) == os.path.normcase(d) for d in songs)
        self.library = Library(songs if inside else songs + [download], skip=(cache,))
        self.song_dirs = songs
        self.download_dir = download
        for path in songs + [download]:
            os.makedirs(path, exist_ok=True)

    # ------------------------------------------------------------ 路径设置
    def paths(self):
        return {"songs_dirs": list(self.song_dirs), "download_dir": self.download_dir,
                "cache_dir": self.cache.dir, "local_config": os.path.join(self.base, LOCAL_CONFIG),
                "yt_dlp": ytdlp_available()}

    def update_paths(self, body):
        """校验并应用管理页提交的路径；返回新路径。原曲目录必须已存在。"""
        dirs = body.get("songs_dirs", self.song_dirs)
        if isinstance(dirs, str):
            dirs = [line for line in dirs.splitlines()]
        if not isinstance(dirs, list):
            raise ValueError("songs_dirs 应为目录列表")
        dirs = [str(d).strip() for d in dirs if str(d or "").strip()]
        if not 1 <= len(dirs) <= MAX_SONG_DIRS:
            raise ValueError(f"原曲目录需要 1–{MAX_SONG_DIRS} 个")
        download = str(body.get("download_dir", self.download_dir) or "").strip()
        cache = str(body.get("cache_dir", self.cache.dir) or "").strip()
        for value in dirs + [download, cache]:
            if len(value) > 260 or any(c in value for c in "\0\r\n*?\"<>|"):
                raise ValueError(f"路径不合法：{value[:60]}")
        missing = [d for d in dirs if not os.path.isdir(self._path(d))]
        if missing:
            raise ValueError(f"目录不存在：{missing[0]}")
        if not cache:
            raise ValueError("成品目录不能为空")
        if os.path.normcase(self._path(cache)) != os.path.normcase(self.cache.dir) and self.pending():
            raise RuntimeError("正在练歌，等这首练完再改成品目录")
        self._apply_paths(dirs, download, cache)
        write_yaml(os.path.join(self.base, LOCAL_CONFIG),
                   {"songs_dirs": dirs, "download_dir": download, "cache_dir": cache})
        log.info("路径已更新：原曲 %s 下载 %s 成品 %s", self.library.dirs, self.download_dir, self.cache.dir)
        return self.paths()

    def render_id(self, song_hash, pitch, clip_seconds, start_ratio):
        key = "|".join([CACHE_VERSION, song_hash, self.pipeline.model_id(), str(pitch),
                        f"{float(clip_seconds):.1f}", f"{float(start_ratio):.3f}"])
        return hashlib.sha1(key.encode("utf-8")).hexdigest()[:16]

    def pending(self):
        return sum(1 for j in self.jobs.values() if j["state"] in ("queued", "running"))

    def _pick(self, song, songs, pitch, clip, ratio):
        """最像的歌；几首一样像（同一首歌在两个目录里）时，优先已经练好的那份，其次按目录顺序。"""
        best = songmatch.best_match(song, [s["title"] for s in songs])
        if best is None:
            return None
        tied = [s for s in songs if abs(songmatch.score(song, s["title"]) - best[1]) < 1e-9]
        if len(tied) > 1:
            for entry in tied:
                if self.cache.get(self.render_id(entry["hash"], pitch, clip, ratio)):
                    return entry["title"], best[1]
            order = {os.path.normcase(d): i for i, d in enumerate(self.library.dirs)}
            tied.sort(key=lambda s: order.get(os.path.normcase(s.get("root", "")), 99))
            return tied[0]["title"], best[1]
        return best

    # ------------------------------------------------------------ 提交
    async def submit(self, song, pitch=None, clip_seconds=None, start_ratio=None, path=None):
        """返回 (http_status, body)。path 指定具体文件（刚下载的歌），不按歌名模糊匹配。"""
        pitch = parse_pitch(pitch, self.cfg["rvc"].get("default_pitch", "auto"))
        clip = float(self.cfg.get("clip_seconds", 90) if clip_seconds in (None, "") else clip_seconds)
        ratio = float(self.cfg.get("start_ratio", 0.3) if start_ratio in (None, "") else start_ratio)
        if not 0 <= clip <= 600 or not 0 <= ratio <= 0.95:
            raise ValueError("clip_seconds 范围 0–600，start_ratio 范围 0–0.95")
        songs = await asyncio.to_thread(self.library.scan)
        if path:
            key = os.path.normcase(os.path.abspath(path))
            entry = next((s for s in songs if os.path.normcase(os.path.abspath(s["path"])) == key), None)
            if entry is None:
                return 404, {"error": "not_found", "song": song, "suggestions": []}
            match = (entry["title"],)
        else:
            match = self._pick(song, songs, pitch, clip, ratio)
        if match is None:
            # 源文件被删但还有缓存的翻唱：照样能唱
            cached = self.cache.list()
            old = songmatch.best_match(song, [m["title"] for m in cached])
            if old is not None:
                meta = next(m for m in cached if m["title"] == old[0])
                return 200, {"state": "done", "cached": True, "render_id": meta["render_id"], "title": meta["title"]}
            titles = [s["title"] for s in songs] + [m["title"] for m in cached]
            return 404, {"error": "not_found", "song": song, "suggestions": songmatch.suggestions(song, titles)}
        entry = next(s for s in songs if s["title"] == match[0])
        rid = self.render_id(entry["hash"], pitch, clip, ratio)
        if self.cache.get(rid):
            return 200, {"state": "done", "cached": True, "render_id": rid, "title": entry["title"]}
        if rid in self.active:
            return 202, self.public(self.jobs[self.active[rid]])
        if not self.pipeline.model_status()["model_ready"]:
            return 409, {"error": "no_model", "message": "还没有 RVC 音色模型（singing/models/*.pth）"}
        if self.pending() >= self.max_queue:
            return 429, {"error": "queue_full", "message": f"排队已满（{self.max_queue}）"}
        job = {"id": uuid.uuid4().hex[:12], "render_id": rid, "title": entry["title"], "file": entry["file"],
               "path": entry["path"], "hash": entry["hash"], "pitch": pitch, "clip_seconds": clip,
               "start_ratio": ratio, "state": "queued", "stage": "queued", "progress": 0.0, "error": None,
               "created": time.time(), "started": None, "finished": None}
        self.jobs[job["id"]] = job
        self.active[rid] = job["id"]
        self._trim_jobs()
        await self.queue.put(job["id"])
        log.info("排队：%s pitch=%s clip=%.0fs（队列 %d）", entry["title"], pitch, clip, self.pending())
        return 202, self.public(job)

    def public(self, job):
        body = {k: job[k] for k in ("id", "render_id", "title", "pitch", "clip_seconds", "state", "stage",
                                    "progress", "error", "created", "started", "finished")}
        body["job_id"] = job["id"]
        if job["state"] == "queued":
            # 第几个开始渲染：前面排队的 + 正在渲染的那首
            queued = sorted((j for j in self.jobs.values() if j["state"] == "queued"), key=lambda j: j["created"])
            body["position"] = queued.index(job) + 1 + (1 if self.current else 0)
        if job.get("result"):
            body["result"] = job["result"]
        return body

    def _trim_jobs(self, keep=50):
        done = sorted((j for j in self.jobs.values() if j["state"] in ("done", "error")),
                      key=lambda j: j["created"])
        while len(self.jobs) > keep and done:
            self.jobs.pop(done.pop(0)["id"], None)

    # ------------------------------------------------------------ 渲染循环
    async def run(self):
        while True:
            job_id = await self.queue.get()
            job = self.jobs.get(job_id)
            if job is None:
                continue
            await self._render(job)

    async def _render(self, job):
        job.update(state="running", started=time.time())
        self.current = job["id"]
        workdir = os.path.join(self.cache.dir, "tmp", job["render_id"])
        out = self.cache.audio_path(job["render_id"])

        def progress(stage, fraction):
            job["stage"], job["progress"] = stage, round(fraction, 2)

        t0 = time.monotonic()
        try:
            result = await asyncio.to_thread(self.pipeline.render, job["path"], out, job["pitch"],
                                             job["clip_seconds"], job["start_ratio"], workdir, progress)
            meta = {"render_id": job["render_id"], "title": job["title"], "file": job["file"],
                    "song_hash": job["hash"], "pitch": job["pitch"], "clip_seconds": job["clip_seconds"],
                    "start_ratio": job["start_ratio"], "created": time.time(),
                    "total_s": round(time.monotonic() - t0, 1), **result}
            self.cache.put(job["render_id"], meta)
            job.update(state="done", stage="done", progress=1.0, result=meta)
            log.info("完成：%s 总耗时 %.1fs 各阶段 %s 移调 %s", job["title"], meta["total_s"],
                     result.get("timings"), result.get("applied_pitch"))
        except RenderError as error:
            job.update(state="error", error=str(error))
            log.warning("失败：%s：%s", job["title"], error)
        except Exception as error:  # 依赖/显存等问题
            job.update(state="error", error=f"{type(error).__name__}: {str(error)[:300]}")
            log.exception("渲染出错：%s", job["title"])
        finally:
            job["finished"] = time.time()
            self.active.pop(job["render_id"], None)
            self.current = None
            shutil.rmtree(workdir, ignore_errors=True)

    # ------------------------------------------------------------ 查询
    async def songs(self):
        songs = await asyncio.to_thread(self.library.scan)
        cached = self.cache.list()
        by_hash = {}
        for meta in cached:
            by_hash.setdefault(meta.get("song_hash"), []).append(meta["render_id"])
        return {"songs": [{"title": s["title"], "file": s["file"], "dir": s.get("root", ""), "hash": s["hash"],
                           "renders": by_hash.get(s["hash"], [])} for s in songs],
                "renders": [{k: m.get(k) for k in ("render_id", "title", "pitch", "applied_pitch",
                                                   "clip_seconds", "created", "total_s")} for m in cached]}

    def health(self):
        return {"ok": True, "gpu": self.pipeline.gpu_info(), **self.pipeline.model_status(),
                "queue": self.pending(), "running": self.current, "max_queue": self.max_queue,
                "downloads": sum(1 for d in self.downloads.values() if d["state"] in ("queued", "downloading")),
                "yt_dlp": ytdlp_available()}

    # ------------------------------------------------------------ 从链接下载
    def start_download(self, url, title="", render=True, pitch=None):
        url = str(url or "").strip()
        if len(url) > 500 or not URL.match(url):
            raise ValueError("链接应为 http(s) 地址")
        title = clean_title(title)
        if pitch is not None:
            parse_pitch(pitch)
        if sum(1 for d in self.downloads.values() if d["state"] in ("queued", "downloading")) >= 3:
            raise OverflowError("已有 3 个链接在下载，等一会儿再加")
        item = {"id": uuid.uuid4().hex[:12], "url": url, "title": title, "render": bool(render),
                "pitch": pitch, "state": "queued", "progress": 0.0, "file": None, "error": None,
                "render_job": None, "created": time.time(), "finished": None}
        self.downloads[item["id"]] = item
        done = sorted((d for d in self.downloads.values() if d["state"] in ("done", "error")),
                      key=lambda d: d["created"])
        while len(self.downloads) > 30 and done:
            self.downloads.pop(done.pop(0)["id"], None)
        asyncio.get_running_loop().create_task(self._download(item))
        return item

    async def _download(self, item):
        async with self.download_lock:
            item["state"] = "downloading"

            def progress(fraction):
                item["progress"] = round(max(0.0, min(1.0, fraction)), 2)

            try:
                taken = {s["title"] for s in await asyncio.to_thread(self.library.scan)}
                path = await asyncio.to_thread(self.downloader, item["url"], self.download_dir,
                                               item["title"], taken, progress)
                item.update(file=path, title=os.path.splitext(os.path.basename(path))[0], progress=1.0)
                log.info("下载完成：%s → %s", item["url"], path)
                if item["render"]:
                    status, job = await self.submit(item["title"], item["pitch"], path=path)
                    item["render_job"] = {"status": status, **job}
                item["state"] = "done"
            except Exception as error:   # yt-dlp 的各种网络/格式错误
                message = str(error).replace("\n", " ")[:300] or type(error).__name__
                item.update(state="error", error=message)
                log.warning("下载失败：%s：%s", item["url"], message)
            finally:
                item["finished"] = time.time()

    def download_public(self, item):
        body = {k: item[k] for k in ("id", "url", "title", "state", "progress", "file", "error",
                                     "created", "finished")}
        body["download_id"] = item["id"]
        job = item.get("render_job")
        if job:
            live = self.jobs.get(job.get("job_id") or job.get("id") or "")
            body["render"] = self.public(live) if live else job
        return body


def write_yaml(path, data):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write("# 管理页保存的唱歌路径，覆盖 config.yaml 里的同名项。删掉本文件即恢复默认。\n")
        yaml.safe_dump(data, f, allow_unicode=True, sort_keys=False)
    os.replace(tmp, path)


_BAD_NAME = re.compile(r'[\\/:*?"<>|\x00-\x1f]+')


def clean_title(title):
    title = _BAD_NAME.sub(" ", str(title or "")).strip(" .")
    return re.sub(r"\s+", " ", title)[:80]


def ytdlp_available():
    try:
        import importlib.util
        return importlib.util.find_spec("yt_dlp") is not None
    except (ImportError, ValueError):
        return False


def download_with_ytdlp(url, out_dir, title, taken, progress):
    """下载链接里的音频（不转码），返回文件路径。单个视频、≤20 分钟、≤300 MB。"""
    try:
        import yt_dlp
    except ImportError:
        raise RuntimeError("没有安装 yt-dlp：singing\\venv\\Scripts\\python -m pip install yt-dlp") from None

    def too_long(info, *, incomplete=False):
        duration = info.get("duration")
        if duration and duration > 1200:
            return "超过 20 分钟，不像是一首歌"
        return None

    def hook(state):
        total = state.get("total_bytes") or state.get("total_bytes_estimate")
        if state.get("status") == "downloading" and total:
            progress(state.get("downloaded_bytes", 0) / total * 0.95)

    os.makedirs(out_dir, exist_ok=True)
    options = {"format": "bestaudio/best", "noplaylist": True, "quiet": True, "no_warnings": True,
               "noprogress": True, "windowsfilenames": True, "max_filesize": 300 << 20,
               "match_filter": too_long, "progress_hooks": [hook], "retries": 3,
               "paths": {"home": out_dir, "temp": os.path.join(out_dir, ".part")}}
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg:
        options["ffmpeg_location"] = ffmpeg
        # 只有视频流时提取音轨；本来就是音频则原样保存，不重新编码
        options["postprocessors"] = [{"key": "FFmpegExtractAudio", "preferredcodec": "best"}]
    with yt_dlp.YoutubeDL({**options, "skip_download": True}) as probe:
        info = probe.extract_info(url, download=False)
    if info.get("_type") == "playlist":
        raise RuntimeError("这是播放列表，请给单首歌曲的链接")
    name = clean_title(title or info.get("track") or info.get("title") or "下载的歌曲") or "下载的歌曲"
    if info.get("artist") and not title and info.get("track"):
        name = clean_title(f"{info['artist']} - {info['track']}") or name
    base, n = name, 2
    while name in taken:          # 不和曲库里已有的歌同名
        name, n = f"{base} ({n})", n + 1
    options["outtmpl"] = {"default": name + ".%(ext)s"}
    with yt_dlp.YoutubeDL(options) as ydl:
        info = ydl.extract_info(url, download=True)
        files = [d.get("filepath") for d in info.get("requested_downloads") or [] if d.get("filepath")]
        path = files[0] if files else ydl.prepare_filename(info)
    if not os.path.isfile(path) or os.path.splitext(path)[1].lower() not in AUDIO_EXTS:
        raise RuntimeError("下载到的不是可用的音频文件")
    return path


def create_app(worker, token):
    @web.middleware
    async def auth(request, handler):
        if request.path != "/health":
            given = request.headers.get("X-Singing-Token", "")
            if not hmac.compare_digest(given.encode(), token.encode()):
                return web.json_response({"error": "unauthorized"}, status=401)
        return await handler(request)

    async def health(_):
        return web.json_response(await asyncio.to_thread(worker.health))

    async def songs(_):
        return web.json_response(await worker.songs())

    async def render(request):
        try:
            body = await request.json()
        except Exception:
            return web.json_response({"error": "请求体应为 JSON"}, status=400)
        song = str(body.get("song") or "").strip()
        if not song or len(song) > 120:
            return web.json_response({"error": "song 不能为空"}, status=400)
        try:
            status, data = await worker.submit(song, body.get("pitch"), body.get("clip_seconds"),
                                               body.get("start_ratio"))
        except ValueError as error:
            return web.json_response({"error": str(error)}, status=400)
        return web.json_response(data, status=status)

    async def job(request):
        item = worker.jobs.get(request.match_info["job_id"])
        if item is None:
            return web.json_response({"error": "not_found"}, status=404)
        return web.json_response(worker.public(item))

    async def audio(request):
        rid = request.match_info["render_id"]
        if not RENDER_ID.match(rid) or not worker.cache.get(rid):
            return web.json_response({"error": "not_found"}, status=404)
        return web.FileResponse(worker.cache.audio_path(rid), headers={"Content-Type": "audio/wav"})

    async def json_body(request):
        try:
            body = await request.json()
        except Exception:
            raise ValueError("请求体应为 JSON") from None
        if not isinstance(body, dict):
            raise ValueError("请求体应为 JSON 对象")
        return body

    async def get_config(_):
        return web.json_response(worker.paths())

    async def put_config(request):
        try:
            paths = await asyncio.to_thread(worker.update_paths, await json_body(request))
            asyncio.create_task(asyncio.to_thread(worker.library.scan))   # 预热新曲库的指纹
            return web.json_response(paths)
        except ValueError as error:
            return web.json_response({"error": str(error)}, status=400)
        except RuntimeError as error:
            return web.json_response({"error": str(error)}, status=409)
        except OSError as error:
            return web.json_response({"error": f"目录无法使用：{error}"}, status=400)

    async def download(request):
        try:
            body = await json_body(request)
            if not ytdlp_available():
                return web.json_response({"error": "yt_dlp_missing",
                                          "message": "没有安装 yt-dlp：singing\\venv\\Scripts\\python -m pip install yt-dlp"},
                                         status=503)
            item = worker.start_download(body.get("url"), body.get("title", ""), body.get("render", True),
                                         body.get("pitch"))
        except ValueError as error:
            return web.json_response({"error": str(error)}, status=400)
        except OverflowError as error:
            return web.json_response({"error": "queue_full", "message": str(error)}, status=429)
        return web.json_response(worker.download_public(item), status=202)

    async def download_state(request):
        did = request.match_info["download_id"]
        item = worker.downloads.get(did) if DOWNLOAD_ID.match(did) else None
        if item is None:
            return web.json_response({"error": "not_found"}, status=404)
        return web.json_response(worker.download_public(item))

    loop_task = {}

    async def start_loop(_app):
        loop_task["task"] = asyncio.create_task(worker.run())
        # 大曲库第一次扫描要读每首歌的头尾算指纹，先在后台扫一遍，免得第一次点歌超时
        loop_task["scan"] = asyncio.create_task(asyncio.to_thread(worker.library.scan))

    async def stop_loop(_app):
        loop_task["task"].cancel()

    app = web.Application(middlewares=[auth])
    app.add_routes([web.get("/health", health), web.get("/songs", songs), web.post("/render", render),
                    web.get("/jobs/{job_id}", job), web.get("/audio/{render_id}", audio),
                    web.get("/config", get_config), web.put("/config", put_config),
                    web.post("/download", download), web.get("/downloads/{download_id}", download_state)])
    app.on_startup.append(start_loop)
    app.on_cleanup.append(stop_loop)
    return app


def check(worker):
    print(json.dumps(worker.health(), ensure_ascii=False, indent=1))
    print("ffmpeg:", shutil.which("ffmpeg") or "未找到（winget install Gyan.FFmpeg）")
    print("曲库：", [s["title"] for s in worker.library.scan()][:20])


def main():
    parser = argparse.ArgumentParser(description="DeepFlipper 唱歌渲染服务")
    parser.add_argument("--config", default=os.path.join(HERE, "config.yaml"))
    parser.add_argument("--check", action="store_true", help="只检查 GPU/模型/ffmpeg")
    parser.add_argument("--log", default="", help="同时把日志写到文件（开机自启时用）")
    args = parser.parse_args()
    handlers = [logging.StreamHandler()]
    if args.log:
        handlers.append(logging.FileHandler(args.log, encoding="utf-8"))
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s",
                        handlers=handlers)
    cfg = load_config(args.config)
    base = os.path.dirname(os.path.abspath(args.config))
    worker = Worker(cfg, base)
    if args.check:
        check(worker)
        return
    token = resolve_token(cfg, base)
    if not shutil.which("ffmpeg"):
        log.error("没有找到 ffmpeg：winget install Gyan.FFmpeg 后重开终端")
    log.info("唱歌服务 http://%s:%s  原曲 %s  成品 %s", cfg.get("host", "0.0.0.0"), cfg.get("port", 8790),
             "；".join(worker.library.dirs), worker.cache.dir)
    web.run_app(create_app(worker, token), host=cfg.get("host", "0.0.0.0"), port=int(cfg.get("port", 8790)),
                print=None)


if __name__ == "__main__":
    main()

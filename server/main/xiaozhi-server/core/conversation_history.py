"""Bounded local text history, independent of provider/voice connection lifetime."""
from functools import lru_cache
from contextlib import contextmanager
from pathlib import Path
import asyncio
import json
import logging
import queue
import re
import sqlite3
import threading
import time
import uuid

LOG = logging.getLogger(__name__)
KINDS = {'user', 'assistant', 'status', 'error', 'tool'}
IMAGE_NAME = re.compile(r'^[0-9a-f]{32}\.png$')
MAX_IMAGES = 400
# Tool arguments worth showing; anything else (and any value that looks like a secret) is dropped.
SHOWN_ARGS = ('name', 'key', 'keys', 'text', 'press_type', 'bitmap', 'confirm', 'kind', 'path', 'button',
              'action', 'app', 'song', 'url', 'frequency', 'timeout_ms', 'keep_open', 'pin', 'value', 'rpc_mode')
SECRET = re.compile(r'(token|secret|password|api[_-]?key)', re.I)


class ConversationHistory:
    def __init__(self, path, retention=10000):
        self.path = Path(path)
        self.retention = retention
        self.path.parent.mkdir(parents=True, exist_ok=True)
        # Screenshots shown next to tool steps; named by random id, never by device (":" is not NTFS-safe).
        self.images = self.path.parent / 'history_images'
        self.pending = queue.Queue(maxsize=512)
        self.failed = False
        self.lock = threading.RLock()
        self.write_lock = threading.RLock()
        self.generation = 0
        self.device_generations = {}
        with self.connect() as db:
            db.execute('PRAGMA journal_mode=WAL')
            db.execute('''CREATE TABLE IF NOT EXISTS events (
                id INTEGER PRIMARY KEY AUTOINCREMENT, time REAL NOT NULL,
                device TEXT NOT NULL, client TEXT NOT NULL, session TEXT NOT NULL,
                kind TEXT NOT NULL, text TEXT NOT NULL)''')
            db.execute('CREATE INDEX IF NOT EXISTS events_device ON events(device, id)')
        threading.Thread(target=self._writer, name='conversation-history', daemon=True).start()

    @contextmanager
    def connect(self):
        db = sqlite3.connect(str(self.path), timeout=3)
        try:
            with db:
                yield db
        finally:
            db.close()

    def revision(self, device):
        with self.lock:
            return self.generation, self.device_generations.get(str(device or '')[:80], 0)

    def is_current(self, device, revision):
        return revision == self.revision(device)

    def clear(self, device=None):
        """Delete a scope and invalidate queued events/old connections atomically."""
        with self.write_lock:
            with self.connect() as db:
                scope = '' if device is None else ' WHERE device = ?'
                args = () if device is None else (device,)
                images = [self._image_of(text) for (text,) in
                          db.execute("SELECT text FROM events" + (scope + " AND" if scope else " WHERE") +
                                     " kind = 'tool'", args)]
                deleted = db.execute('DELETE FROM events' + scope, args).rowcount
            for name in images:
                self._remove_image(name)
            with self.lock:
                if device is None:
                    self.generation += 1
                    self.device_generations.clear()
                else:
                    self.device_generations[device] = self.device_generations.get(device, 0) + 1
            return deleted

    # ------------------------------------------------------------ screenshots
    @staticmethod
    def _image_of(text):
        try:
            name = json.loads(text).get('image')
        except (ValueError, TypeError, AttributeError):
            return None
        return name if isinstance(name, str) and IMAGE_NAME.match(name) else None

    def _remove_image(self, name):
        if name and IMAGE_NAME.match(name):
            try:
                (self.images / name).unlink()
            except OSError:
                pass

    def save_image(self, png):
        """Store one PNG for a tool event; keep at most MAX_IMAGES, oldest removed first."""
        if not isinstance(png, (bytes, bytearray)) or not png.startswith(b'\x89PNG') or len(png) > 256 * 1024:
            return None
        name = uuid.uuid4().hex + '.png'
        try:
            self.images.mkdir(parents=True, exist_ok=True)
            tmp = self.images / (name + '.tmp')
            tmp.write_bytes(png)
            tmp.replace(self.images / name)
            files = sorted(self.images.glob('*.png'), key=lambda p: p.stat().st_mtime)
            for old in files[:-MAX_IMAGES]:
                old.unlink()
        except OSError:
            LOG.warning('Conversation screenshot write failed')
            return None
        return name

    def image_path(self, name):
        if not isinstance(name, str) or not IMAGE_NAME.match(name):
            return None
        path = self.images / name
        return path if path.is_file() else None

    def append(self, device, client, session, kind, text, revision=None):
        if kind not in KINDS or not isinstance(text, str) or not text.strip():
            return
        event = (time.time(), str(device or '')[:80], str(client or '')[:80],
                 str(session or '')[:80], kind, text[:16000])
        with self.lock:
            current = self.revision(event[1])
            if revision is not None and revision != current:
                return
            try:
                self.pending.put_nowait((event, current))
            except queue.Full:
                self.failed = True
                LOG.warning('Conversation history queue full; event omitted')

    def _writer(self):
        while True:
            item = self.pending.get()
            try:
                if item is None:
                    return
                event, revision = item
                with self.write_lock:
                    if not self.is_current(event[1], revision):
                        continue
                    with self.connect() as db:
                        db.execute('INSERT INTO events(time,device,client,session,kind,text) VALUES(?,?,?,?,?,?)', event)
                        db.execute('DELETE FROM events WHERE id <= (SELECT MAX(id) FROM events) - ?',
                                   (self.retention,))
            except (OSError, sqlite3.Error):
                self.failed = True
                LOG.warning('Conversation history write failed')
            finally:
                self.pending.task_done()

    def close(self):
        self.pending.put(None)
        self.pending.join()

    def read(self, device='', before=0, limit=100):
        limit = max(1, min(200, limit))
        where, args = [], []
        if device:
            where.append('device = ?')
            args.append(device)
        if before:
            where.append('id < ?')
            args.append(before)
        query = 'SELECT id,time,device,client,session,kind,text FROM events'
        if where:
            query += ' WHERE ' + ' AND '.join(where)
        with self.connect() as db:
            db.row_factory = sqlite3.Row
            rows = db.execute(query + ' ORDER BY id DESC LIMIT ?', (*args, limit + 1)).fetchall()
            devices = [r[0] for r in db.execute('SELECT DISTINCT device FROM events ORDER BY device')]
        more = len(rows) > limit
        rows = rows[:limit]
        return {'events': [dict(r) for r in reversed(rows)], 'devices': devices,
                'before': rows[-1]['id'] if more else 0, 'write_error': self.failed}

    def context(self, device, client, turns=6, max_chars=6000):
        if not device or not client or not 1<=turns<=12:return []
        with self.connect() as db:
            rows=db.execute("SELECT kind,text FROM events WHERE device=? AND client=? AND kind IN ('user','assistant') AND time>? ORDER BY id DESC LIMIT 120",
                (device,client,time.time()-86400)).fetchall()
        pairs=[];pending=None
        for role,text in reversed(rows):
            if role=='user':pending=text
            elif pending is not None:
                pairs.append((pending,text));pending=None
        selected=[];size=0
        for user,assistant in reversed(pairs[-turns:]):
            if size+len(user)+len(assistant)>max_chars:break
            selected.insert(0,({'role':'user','content':user},{'role':'assistant','content':assistant}))
            size+=len(user)+len(assistant)
        return [message for pair in selected for message in pair]


def history_for(root):
    # get_project_dir() includes a trailing slash; AdminServer stores a Path.
    # Both must share the same queue and deletion revisions for this database.
    return _history_for(str(Path(root).resolve()))


@lru_cache(maxsize=4)
def _history_for(root):
    return ConversationHistory(Path(root) / 'data/conversations.sqlite3')


def record_event(conn, kind, text):
    history = getattr(conn, 'conversation_history', None)
    if history and not getattr(conn, 'history_cleared', False):
        history.append(getattr(conn, 'device_id', ''), (getattr(conn, 'headers', None) or {}).get('client-id', ''),
                       getattr(conn, 'session_id', ''), kind, text,
                       revision=getattr(conn, 'history_revision', None))


def _shown_args(arguments):
    if isinstance(arguments, str):
        try:
            arguments = json.loads(arguments or '{}')
        except ValueError:
            return {}
    if not isinstance(arguments, dict):
        return {}
    shown = {}
    for key in SHOWN_ARGS:
        value = arguments.get(key)
        if value is None or SECRET.search(key) or isinstance(value, (dict, list)):
            continue
        if isinstance(value, str):
            if SECRET.search(value):
                continue
            value = value[:120]
        shown[key] = value
    return shown


def tool_summary(name, arguments, result, png_name=None):
    """What the transcript keeps for one tool step: name, chosen arguments, outcome, screen."""
    event = {'name': str(name or '')[:64], 'args': _shown_args(arguments)}
    data = None
    if isinstance(result, str):
        try:
            data = json.loads(result)
        except ValueError:
            event['result'] = result[:300]
    elif result is not None:
        event['result'] = str(result)[:300]
    if isinstance(data, dict):
        inner = data.get('result') if isinstance(data.get('result'), dict) else data
        ok = inner.get('ok', data.get('ok'))
        if isinstance(ok, bool):
            event['ok'] = ok
        code = inner.get('code') or inner.get('error') or data.get('code')
        if isinstance(code, str):
            event['code'] = code[:64]
        for key in ('app', 'view', 'selected'):
            if isinstance(inner.get(key), str):
                event[key] = inner[key][:800]
        if isinstance(inner.get('action'), str):
            event['action'] = inner['action'][:64]
        if 'view' not in event and 'code' not in event:
            event['result'] = json.dumps(data, ensure_ascii=False)[:300]
    elif data is not None:
        event['result'] = str(data)[:300]
    if png_name:
        event['image'] = png_name
    return event


def record_tool(conn, name, arguments, result, png=None):
    """Keep one Flipper/plugin step (and its screenshot) in the transcript for the admin page."""
    history = getattr(conn, 'conversation_history', None)
    if not history or getattr(conn, 'history_cleared', False):
        return
    try:
        image = history.save_image(png) if png else None
        event = tool_summary(name, arguments, result, image)
        record_event(conn, 'tool', json.dumps(event, ensure_ascii=False))
    except Exception:
        LOG.warning('Conversation tool event omitted', exc_info=True)


def record_message(conn, message):
    # Provider credentials, system prompts, few-shot examples and raw tool
    # arguments/results are deliberately outside the conversation transcript.
    if message.role in ('user', 'assistant') and not message.is_temporary and message.content:
        text = message.content
        if not isinstance(text, str):
            # Multimodal list content: keep the text parts, never the image data.
            from core.utils.dialogue import content_text
            text = content_text(text, image_placeholder='[截图]')
        if text:
            record_event(conn, message.role, text)


async def clear_conversations(history, runtime, device=None):
    server = getattr(runtime, 'server', None)
    deleted = await asyncio.to_thread(history.clear, device)
    targets = []
    # A settings reload may replace the server while SQLite is being cleared.
    connections = list(getattr(server, 'connections', ()))
    connections += [conn for conn in list(getattr(getattr(runtime, 'server', None), 'connections', ()))
                    if conn not in connections]
    for conn in connections:
        identity = getattr(conn, 'device_id', None) or (getattr(conn, 'headers', None) or {}).get('device-id', '')
        if device is not None and identity != device:
            continue
        # Connections established after the deletion already have fresh context.
        if history.is_current(identity, getattr(conn, 'history_revision', None)):
            continue
        conn.history_cleared = True
        conn.client_abort = True
        conn.stop_event.set()
        conn.dialogue.observer = None
        conn.dialogue.dialogue.clear()
        try:
            conn.clear_queues()
        except Exception:
            LOG.warning('Voice queue cleanup failed while clearing history; closing the connection')
        targets.append(conn)

    async def disconnect(conn):
        try:
            if conn.websocket is not None:
                await asyncio.wait_for(conn.websocket.close(code=1000, reason='history_cleared'), 5)
            return False
        except Exception:
            LOG.warning('History cleared but a voice connection has not finished closing')
            return True

    pending = sum(await asyncio.gather(*(disconnect(conn) for conn in targets)))
    return {'deleted': deleted, 'reset_sessions': len(targets), 'reset_pending': pending}

"""Bounded local text history, independent of provider/voice connection lifetime."""
from functools import lru_cache
from contextlib import contextmanager
from pathlib import Path
import logging
import queue
import sqlite3
import threading
import time

LOG = logging.getLogger(__name__)
KINDS = {'user', 'assistant', 'status', 'error'}


class ConversationHistory:
    def __init__(self, path, retention=10000):
        self.path = Path(path)
        self.retention = retention
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.pending = queue.Queue(maxsize=512)
        self.failed = False
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

    def append(self, device, client, session, kind, text):
        if kind not in KINDS or not isinstance(text, str) or not text.strip():
            return
        event = (time.time(), str(device or '')[:80], str(client or '')[:80],
                 str(session or '')[:80], kind, text[:16000])
        try:
            self.pending.put_nowait(event)
        except queue.Full:
            self.failed = True
            LOG.warning('Conversation history queue full; event omitted')

    def _writer(self):
        while True:
            event = self.pending.get()
            try:
                if event is None:
                    return
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


@lru_cache(maxsize=4)
def history_for(root):
    return ConversationHistory(Path(root) / 'data/conversations.sqlite3')


def record_event(conn, kind, text):
    history = getattr(conn, 'conversation_history', None)
    if history:
        history.append(getattr(conn, 'device_id', ''), (getattr(conn, 'headers', None) or {}).get('client-id', ''),
                       getattr(conn, 'session_id', ''), kind, text)


def record_message(conn, message):
    # Provider credentials, system prompts, few-shot examples and raw tool
    # arguments/results are deliberately outside the conversation transcript.
    if message.role in ('user', 'assistant') and not message.is_temporary and message.content:
        record_event(conn, message.role, message.content)

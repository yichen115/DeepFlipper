from pathlib import Path
import sys
import tempfile
import types
import unittest
import threading
from unittest.mock import patch
from concurrent.futures import ThreadPoolExecutor

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'server/main/xiaozhi-server'))
from core.conversation_history import ConversationHistory, clear_conversations, history_for, record_event, record_message
from core.utils.dialogue import Dialogue, Message


class HistoryTests(unittest.TestCase):
    def test_root_spelling_uses_one_queue_and_clear_revision(self):
        with tempfile.TemporaryDirectory() as directory:
            store = history_for(directory)
            try:
                self.assertIs(store, history_for(directory + '/'))
                self.assertIs(store, history_for(Path(directory) / '.'))
            finally:
                store.close()

    def test_clear_fences_queued_and_late_events_without_erasing_other_devices(self):
        with tempfile.TemporaryDirectory() as directory:
            store = ConversationHistory(Path(directory)/'history.sqlite3')
            try:
                for device in ('a', 'b'):
                    store.append(device, 'c', 'old', 'user', 'question')
                    store.append(device, 'c', 'old', 'assistant', 'answer')
                store.pending.join()
                old = store.revision('a')
                other = store.revision('b')
                with store.write_lock:
                    store.append('a', 'c', 'old', 'assistant', 'queued reply', revision=old)
                    self.assertEqual(store.clear('a'), 2)
                store.append('a', 'c', 'old', 'assistant', 'late reply', revision=old)
                store.pending.join()
                self.assertEqual(store.read('a')['events'], [])
                self.assertEqual(store.context('a', 'c'), [])
                self.assertTrue(store.is_current('b', other))
                self.assertEqual(len(store.context('b', 'c')), 2)
                store.append('a', 'c', 'new', 'user', 'new question', revision=store.revision('a'))
                store.pending.join()
                self.assertEqual(store.read('a')['events'][0]['text'], 'new question')
                with store.write_lock:
                    store.append('b', 'c', 'old', 'assistant', 'queued before global clear')
                    self.assertEqual(store.clear(), 3)
                store.pending.join()
                self.assertEqual(store.read()['events'], [])
                self.assertFalse(store.is_current('b', other))
            finally:
                store.close()
            restarted = ConversationHistory(Path(directory)/'history.sqlite3')
            try:
                self.assertEqual(restarted.read()['events'], [])
                self.assertEqual(restarted.context('b', 'c'), [])
            finally:
                restarted.close()

    def test_context_is_identity_bound_complete_and_bounded(self):
        with tempfile.TemporaryDirectory() as directory:
            store=ConversationHistory(Path(directory)/'history.sqlite3')
            try:
                for role,text in [('assistant','orphan'),('user','one'),('assistant','answer'),('user','unfinished')]:
                    store.append('a','c','old',role,text)
                store.append('a','other','old','user','private');store.append('a','other','old','assistant','private')
                store.pending.join()
                self.assertEqual(store.context('a','c'),[{'role':'user','content':'one'},{'role':'assistant','content':'answer'}])
                self.assertEqual(store.context('a','c',turns=0),[])
                self.assertEqual(store.context('a','c',max_chars=1),[])
            finally:store.close()
    def test_transcript_filters_templates_tools_and_survives_reconnect_restart(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'history.sqlite3'
            store = ConversationHistory(path)
            conn = types.SimpleNamespace(conversation_history=store, device_id='device-a',
                                         headers={'client-id': 'client-a', 'Authorization': 'secret'}, session_id='session-a')
            dialogue = Dialogue(lambda message: record_message(conn, message))
            for message in (Message('system', 'secret prompt'), Message('user', 'example', is_temporary=True),
                            Message('tool', 'secret tool result'), Message('assistant', tool_calls=[{'secret': 'key'}]),
                            Message('user', '你好'), Message('assistant', '<img onerror=alert(1)>')):
                dialogue.put(message)
            conn.session_id='session-b'
            dialogue.put(Message('user', 'reconnected'))
            store.pending.join()
            store.close()
            restarted = ConversationHistory(path)
            try:
                events = restarted.read()['events']
                self.assertEqual([r['text'] for r in events], ['你好', '<img onerror=alert(1)>', 'reconnected'])
                self.assertEqual(events[-1]['session'], 'session-b')
                self.assertNotIn('secret', str(events))
            finally:
                restarted.close()

    def test_bounded_retention_concurrent_writes_device_filter_and_pagination(self):
        with tempfile.TemporaryDirectory() as directory:
            store=ConversationHistory(Path(directory)/'history.sqlite3', retention=150)
            try:
                with ThreadPoolExecutor(4) as executor:
                    list(executor.map(lambda i: store.append('a' if i%2 else 'b', 'c', 's', 'user', str(i)), range(200)))
                store.pending.join()
                latest=store.read(limit=100)
                older=store.read(before=latest['before'],limit=100)
                self.assertEqual(len(latest['events'])+len(older['events']),150)
                self.assertLess(older['events'][-1]['id'],latest['events'][0]['id'])
                self.assertTrue(all(r['device']=='a' for r in store.read(device='a')['events']))
                self.assertEqual(store.read(device="a' OR 1=1 --")['events'],[])
            finally:
                store.close()


class HistoryResetTests(unittest.IsolatedAsyncioTestCase):
    async def test_clear_resets_matching_live_context_and_drops_shutdown_events(self):
        with tempfile.TemporaryDirectory() as directory:
            store = ConversationHistory(Path(directory)/'history.sqlite3')
            class Socket:
                def __init__(self):
                    self.closed = False
                async def close(self, **kwargs):
                    self.closed = True
                    self.reason = kwargs['reason']
            def connection(device):
                conn = types.SimpleNamespace(device_id=device, headers={'client-id': 'c'}, session_id='s',
                    conversation_history=store, history_revision=store.revision(device),
                    stop_event=threading.Event(), client_abort=False, websocket=Socket(),
                    dialogue=Dialogue(), clear_queues=lambda: None)
                conn.dialogue.observer = lambda message: record_message(conn, message)
                conn.dialogue.put(Message('user', 'remember me'))
                return conn
            a, b = connection('a'), connection('b')
            server = types.SimpleNamespace(connections=[a, b])
            runtime = types.SimpleNamespace(server=server)
            try:
                store.pending.join()
                fresh = []
                original_clear = store.clear
                def clear_and_reconnect(device):
                    count = original_clear(device)
                    fresh.append(connection('a'))
                    server.connections.append(fresh[0])
                    return count
                with patch.object(store, 'clear', side_effect=clear_and_reconnect):
                    result = await clear_conversations(store, runtime, 'a')
                self.assertEqual(result, {'deleted': 1, 'reset_sessions': 1, 'reset_pending': 0})
                self.assertTrue(a.stop_event.is_set())
                self.assertTrue(a.client_abort)
                self.assertTrue(a.websocket.closed)
                self.assertEqual(a.websocket.reason, 'history_cleared')
                self.assertEqual(a.dialogue.dialogue, [])
                self.assertFalse(b.websocket.closed)
                self.assertEqual(len(b.dialogue.dialogue), 1)
                self.assertFalse(fresh[0].websocket.closed)
                self.assertFalse(fresh[0].stop_event.is_set())
                record_event(a, 'status', 'old connection closed')
                record_message(a, Message('assistant', 'late old answer'))
                store.pending.join()
                result = await clear_conversations(store, runtime)
                self.assertEqual(result['reset_pending'], 0)
                self.assertTrue(fresh[0].websocket.closed)
                self.assertTrue(b.websocket.closed)
                store.pending.join()
                self.assertEqual(store.read()['events'], [])
                self.assertEqual(store.context('a', 'c'), [])
            finally:
                store.close()

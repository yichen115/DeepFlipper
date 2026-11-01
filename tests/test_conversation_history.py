from pathlib import Path
import sys
import tempfile
import types
import unittest
from concurrent.futures import ThreadPoolExecutor

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'server/main/xiaozhi-server'))
from core.conversation_history import ConversationHistory, record_message
from core.utils.dialogue import Dialogue, Message


class HistoryTests(unittest.TestCase):
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

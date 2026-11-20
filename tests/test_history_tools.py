"""Tool steps and screenshots in the conversation transcript."""
import json
from pathlib import Path
import sys
import tempfile
import time
import types
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'server/main/xiaozhi-server'))
from core.conversation_history import ConversationHistory, record_tool, tool_summary, MAX_IMAGES  # noqa: E402

PNG = b'\x89PNG\r\n\x1a\n' + b'\x00' * 40


class HistoryToolTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.history = ConversationHistory(Path(self.dir.name) / 'data/conversations.sqlite3')
        self.conn = types.SimpleNamespace(conversation_history=self.history, device_id='aa:bb:cc:dd:ee:ff',
                                          headers={'client-id': 'c'}, session_id='s', history_revision=None)

    def wait(self):
        self.history.pending.join()

    def test_summary_keeps_screen_and_outcome_but_drops_secrets(self):
        result = json.dumps({'ok': True, 'pressed': 1, 'app': 'Sub-GHz', 'view': '▶ Read RAW\n[OK REC]',
                             'selected': 'Read RAW'})
        event = tool_summary('flipper_press_keys', '{"keys":"ok","api_key":"sk-1","token":"abc"}', result)
        self.assertEqual(event['args'], {'keys': 'ok'})
        self.assertEqual((event['ok'], event['app'], event['selected']), (True, 'Sub-GHz', 'Read RAW'))
        self.assertIn('[OK REC]', event['view'])
        gated = tool_summary('flipper_press_keys', {'keys': 'ok'},
                             json.dumps({'ok': False, 'code': 'needs_confirmation', 'action': 'Send'}))
        self.assertEqual((gated['code'], gated['action']), ('needs_confirmation', 'Send'))
        self.assertEqual(tool_summary('flipper_app_open', {'name': 'Sub-GHz'}, 'not json')['result'], 'not json')

    def test_tool_event_with_screenshot_is_stored_and_cleared(self):
        record_tool(self.conn, 'flipper_screen', {'bitmap': True}, json.dumps({'ok': True, 'app': '', 'view': 'Ready'}), PNG)
        self.wait()
        events = self.history.read()['events']
        self.assertEqual(events[0]['kind'], 'tool')
        data = json.loads(events[0]['text'])
        path = self.history.image_path(data['image'])
        self.assertEqual(path.read_bytes(), PNG)
        self.assertIsNone(self.history.image_path('../conversations.sqlite3'))
        self.assertIsNone(self.history.save_image(b'not a png'))
        self.history.clear('aa:bb:cc:dd:ee:ff')
        self.assertFalse(path.exists())
        # Context for the model still only replays user/assistant text.
        self.assertEqual(self.history.context('aa:bb:cc:dd:ee:ff', 'c'), [])

    def test_screenshot_count_is_bounded(self):
        names = []
        for i in range(MAX_IMAGES + 3):
            names.append(self.history.save_image(PNG))
        files = list(self.history.images.glob('*.png'))
        self.assertEqual(len(files), MAX_IMAGES)


if __name__ == '__main__':
    unittest.main()

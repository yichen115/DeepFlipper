"""Exercise the actual connection guards with the full voice dependencies in Docker."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import AsyncMock, MagicMock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'server/main/xiaozhi-server'))
HAS_RUNTIME = all(importlib.util.find_spec(name) for name in ('funasr', 'sherpa_onnx', 'opuslib_next'))


@unittest.skipUnless(HAS_RUNTIME, 'Full voice dependencies are tested inside the Docker image')
class ConnectionResetTests(unittest.IsolatedAsyncioTestCase):
    @classmethod
    def setUpClass(cls):
        # This tests connection lifecycle, not logging or the operator's private config.
        with patch('config.logger.setup_logging', return_value=MagicMock()), \
                patch('core.utils.wakeup_word.WakeupWordsConfig._ensure_directories'):
            from core.connection import ConnectionHandler
        cls.handler_class = ConnectionHandler

    async def test_delete_during_context_restore_cannot_restore_old_messages_or_save_memory(self):
        from core.conversation_history import ConversationHistory
        from core.utils.dialogue import Dialogue
        with tempfile.TemporaryDirectory() as directory:
            history = ConversationHistory(Path(directory) / 'history.sqlite3')
            conn = self.handler_class.__new__(self.handler_class)
            conn.logger = MagicMock()
            conn.dialogue = Dialogue()
            conn.config = {'context_turns': 6}
            conn.close = AsyncMock()
            conn.memory = MagicMock()
            ws = types.SimpleNamespace(request=types.SimpleNamespace(
                headers={'device-id': 'a', 'client-id': 'c'}), remote_address=('127.0.0.1', 1234))
            def deleted_while_reading(*args):
                history.clear('a')
                return [{'role': 'user', 'content': 'old question'},
                        {'role': 'assistant', 'content': 'old answer'}]
            try:
                with patch('core.connection.history_for', return_value=history), \
                        patch.object(history, 'context', side_effect=deleted_while_reading):
                    await conn.handle_connection(ws)
                self.assertTrue(conn.history_cleared)
                self.assertEqual(conn.dialogue.dialogue, [])
                conn.close.assert_awaited_once_with(ws)
                conn.memory.save_memory.assert_not_called()
                # Late audio/text callbacks on the old session cannot restart a chat.
                await conn._route_message('ignored')
                self.assertFalse(conn.chat('ignored'))
                conn.history_cleared=False
                conn.diagnostic_playback=True
                await conn._route_message(b'microphone data must not reach ASR during speaker test')
            finally:
                history.close()

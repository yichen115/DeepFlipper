"""Continuous conversation: hello flag, listen-after-reply flag and the quiet follow-up window."""
import asyncio
import json
from pathlib import Path
import sys
import threading
import time
import types
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'server/main/xiaozhi-server'))


class FakeWS:
    def __init__(self):
        self.sent = []

    async def send(self, data):
        self.sent.append(json.loads(data))


def conn(**config):
    return types.SimpleNamespace(config=config, websocket=FakeWS(), stop_event=threading.Event(),
                                 session_id='s', client_listen_mode='auto', client_is_speaking=False,
                                 client_have_voice=False, last_activity_time=0, close_after_chat=False,
                                 client_abort=False, conversation_history=None)


class ContinuousChatTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        from core.utils import continuous_chat
        self.cc = continuous_chat

    async def test_listen_after_reply_only_when_enabled_or_asked(self):
        off, on = conn(), conn(continuous_chat=True)
        self.assertFalse(self.cc.wants_listen(off))
        self.assertTrue(self.cc.wants_listen(on))
        off.listen_after_turn = True                     # "要不要现在听？"
        self.assertTrue(self.cc.wants_listen(off))
        self.assertFalse(self.cc.wants_listen(off))      # one answer only
        on.close_after_chat = True                       # goodbye turn: do not reopen the mic
        self.assertFalse(self.cc.wants_listen(on))

    async def test_quiet_follow_up_window_stops_listening(self):
        c = conn(continuous_chat=True, continuous_follow_up_s=3)
        c.follow_up_pending = True
        original = self.cc.follow_up_seconds
        self.cc.follow_up_seconds = lambda _c: 0.05
        try:
            self.cc.on_listen_start(c, 'auto')
            await asyncio.wait_for(c.follow_up_task, 1)
        finally:
            self.cc.follow_up_seconds = original
        self.assertEqual(c.websocket.sent, [{'type': 'listen', 'state': 'stop', 'session_id': 's'}])
        self.assertEqual(c.client_listen_mode, 'manual')

    async def test_speech_or_button_press_keeps_listening(self):
        from core.utils import continuous_chat as cc
        original = cc.follow_up_seconds
        cc.follow_up_seconds = lambda _c: 0.05
        try:
            spoke = conn(continuous_chat=True)
            spoke.follow_up_pending = True
            cc.on_listen_start(spoke, 'auto')
            spoke.last_activity_time = time.time() * 1000 + 10
            await asyncio.wait_for(spoke.follow_up_task, 1)
            self.assertEqual(spoke.websocket.sent, [])
            pressed = conn(continuous_chat=True)          # a deliberate press has no window
            cc.on_listen_start(pressed, 'auto')
            self.assertIsNone(pressed.follow_up_task)
        finally:
            cc.follow_up_seconds = original
        self.assertEqual(cc.follow_up_seconds(conn(continuous_follow_up_s=100)), 30.0)


if __name__ == '__main__':
    unittest.main()

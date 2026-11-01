import asyncio
import importlib.util
from pathlib import Path
import sys
import types
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'server/main/xiaozhi-server'))
HAS_RUNTIME=all(importlib.util.find_spec(name) for name in ('funasr','opuslib_next'))
if HAS_RUNTIME:
    from core.handle.sendAudioHandle import sendAudio, PRE_BUFFER_COUNT


@unittest.skipUnless(HAS_RUNTIME,'Full audio runtime is tested in Docker')
class AudioPrebufferTests(unittest.IsolatedAsyncioTestCase):
    async def test_initial_burst_is_bounded_and_queued_audio_keeps_order(self):
        class Socket:
            def __init__(self):self.sent=[]
            async def send(self,packet):self.sent.append(packet)
        conn=types.SimpleNamespace(config={},client_abort=False,sentence_id='turn',
                                   conn_from_mqtt_gateway=False,websocket=Socket(),last_activity_time=0)
        packets=[bytes([i]) for i in range(8)]
        await sendAudio(conn,packets,frame_duration=60)
        self.assertEqual(PRE_BUFFER_COUNT,4)
        self.assertEqual(conn.websocket.sent,packets[:4])
        self.assertEqual(len(conn.audio_rate_controller.queue),4)
        await asyncio.wait_for(conn.audio_rate_controller.queue_empty_event.wait(),2)
        self.assertEqual(conn.websocket.sent,packets)
        conn.audio_rate_controller.stop_sending()
        await conn.audio_rate_controller.pending_send_task

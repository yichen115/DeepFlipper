import asyncio
import importlib.util
from pathlib import Path
import sys
import types
import time
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'server/main/xiaozhi-server'))
HAS_RUNTIME=all(importlib.util.find_spec(name) for name in ('funasr','opuslib_next'))
if HAS_RUNTIME:
    from core.handle.sendAudioHandle import sendAudio, PRE_BUFFER_COUNT, _prime_after_pause


@unittest.skipUnless(HAS_RUNTIME,'Full audio runtime is tested in Docker')
class AudioPrebufferTests(unittest.IsolatedAsyncioTestCase):
    async def test_initial_burst_is_bounded_and_queued_audio_keeps_order(self):
        class Socket:
            def __init__(self):self.sent=[]
            async def send(self,packet):self.sent.append(packet)
        conn=types.SimpleNamespace(config={},client_abort=False,sentence_id='turn',
                                   conn_from_mqtt_gateway=False,websocket=Socket(),last_activity_time=0)
        packets=[bytes([i]) for i in range(12)]
        await sendAudio(conn,packets,frame_duration=60)
        self.assertEqual(PRE_BUFFER_COUNT,8)
        self.assertEqual(conn.websocket.sent,packets[:8])
        self.assertEqual(len(conn.audio_rate_controller.queue),4)
        await asyncio.wait_for(conn.audio_rate_controller.queue_empty_event.wait(),2)
        self.assertEqual(conn.websocket.sent,packets)
        conn.audio_rate_controller.stop_sending()
        await conn.audio_rate_controller.pending_send_task

    async def test_resumed_sentence_refills_once_and_never_catches_up_in_one_burst(self):
        class Socket:
            def __init__(self):self.sent=[];self.times=[]
            async def send(self,packet):self.sent.append(packet);self.times.append(time.monotonic())
        conn=types.SimpleNamespace(config={},client_abort=False,sentence_id='turn',
            conn_from_mqtt_gateway=False,websocket=Socket(),last_activity_time=0)
        first=[bytes([i]) for i in range(12)]
        await sendAudio(conn,first)
        rate=conn.audio_rate_controller
        try:
            await asyncio.wait_for(rate.queue_empty_event.wait(),2)
            conn.audio_flow_control['last_send_time']-=2
            rate.start_timestamp=time.monotonic()-10
            rate.play_position=6000
            _prime_after_pause(conn)
            _prime_after_pause(conn) # FIRST metadata must not double-prime the same pause.
            self.assertEqual(conn.audio_flow_control['refill_bursts'],1)
            self.assertEqual(rate.play_position,0)
            self.assertIsNone(rate.start_timestamp)
            rate.add_message(lambda:asyncio.sleep(0))
            await asyncio.wait_for(rate.queue_empty_event.wait(),1)
            second=[bytes([i]) for i in range(12,24)]
            await sendAudio(conn,second)
            self.assertEqual(conn.websocket.sent,first+second[:8])
            self.assertEqual(len(rate.queue),4)
            self.assertEqual(conn.audio_flow_control['refill_bursts'],1)
            await asyncio.wait_for(rate.queue_empty_event.wait(),2)
            self.assertEqual(conn.websocket.sent,first+second)
            self.assertGreater(conn.websocket.times[-1]-conn.websocket.times[-4],0.08)
        finally:
            rate.stop_sending();await rate.pending_send_task

    async def test_custom_send_delay_keeps_its_pacing_and_disables_bursts(self):
        class Socket:
            def __init__(self):self.sent=[]
            async def send(self,packet):self.sent.append(packet)
        conn=types.SimpleNamespace(config={'tts_audio_send_delay':5},client_abort=False,sentence_id='turn',
            conn_from_mqtt_gateway=False,websocket=Socket(),last_activity_time=0)
        packets=[bytes([i]) for i in range(12)]
        await sendAudio(conn,packets)
        self.assertEqual(conn.websocket.sent,[])
        try:
            await asyncio.wait_for(conn.audio_rate_controller.queue_empty_event.wait(),2)
            self.assertEqual(conn.websocket.sent,packets)
            self.assertEqual(conn.audio_flow_control['refill_bursts'],0)
        finally:
            conn.audio_rate_controller.stop_sending();await conn.audio_rate_controller.pending_send_task
